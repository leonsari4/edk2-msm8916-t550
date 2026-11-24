#include <PiDxe.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/LKEnvLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/PlatformKeypadLib.h>
#include <Library/QcomPm8x41Lib.h>

#include <Guid/ConsoleInDevice.h>
#include <Protocol/DevicePath.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextInEx.h>
#include <Protocol/QcomGpioTlmm.h>

#define PLATFORM_KEYPAD_QUEUE_SIZE  16
#define PLATFORM_KEYPAD_POLL_MS     20
#define PLATFORM_KEYPAD_POLL_100NS  ((UINT64)PLATFORM_KEYPAD_POLL_MS * 10000ULL)
#define PLATFORM_KEYPAD_LONG_PRESS_MS  600
#define PLATFORM_KEYPAD_COOLDOWN    5  /* polls, ~100ms */

#define KEY_INDEX_VOL_UP    0
#define KEY_INDEX_HOME      1
#define KEY_INDEX_VOL_DOWN  2
#define KEY_INDEX_POWER     3
#define KEY_INDEX_COUNT     4

//
// PM8916 PON registers (PPID 0x8). PON_INT_RT_STS bits:
//   0 KPDPWR_ON, 1 RESIN_ON, 2 CBLPWR_ON, 3 KPDPWR_BARK,
//   4 RESIN_BARK, 5 K_R_BARK, 6 PMIC_WD_BARK, 7 SOFT_RESET
//
#define PON_INT_RT_STS          0x810
#define PON_INT_LATCHED_CLR     0x814
#define PON_INT_EN_SET          0x815
#define PON_INT_LATCHED_STS     0x818
#define PON_INT_PENDING_STS     0x819
#define PON_PULL_CTL            0x870
#define PON_PERPH_TYPE          0x804
#define PON_PERPH_SUBTYPE       0x805

#define PON_KPDPWR_ON           BIT0
#define PON_RESIN_ON            BIT1
#define PON_CBLPWR_ON           BIT2
#define PON_PULL_KPDPWR         BIT1
#define PON_PULL_RESIN          BIT0

#define PLATFORM_KEYPAD_DP_GUID \
  { 0x5d1c2c7c, 0x8e64, 0x4c1b, { 0x9a, 0x8f, 0x2c, 0x5a, 0x1b, 0x3f, 0x6d, 0x40 } }

typedef struct {
  VENDOR_DEVICE_PATH  Keypad;
  EFI_DEVICE_PATH     End;
} PLATFORM_KEYPAD_DEVICE_PATH;

STATIC PLATFORM_KEYPAD_DEVICE_PATH mKeypadDevicePath = {
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      {
        (UINT8)(sizeof(VENDOR_DEVICE_PATH)),
        (UINT8)((sizeof(VENDOR_DEVICE_PATH)) >> 8),
      },
    },
    PLATFORM_KEYPAD_DP_GUID,
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 }
  }
};

typedef struct {
  LIST_ENTRY                         Link;
  EFI_KEY_DATA                       KeyData;
  EFI_KEY_NOTIFY_FUNCTION            Function;
  VOID                               *Context;
} PLATFORM_KEY_NOTIFY;

typedef struct {
  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  TextInEx;
  EFI_SIMPLE_TEXT_INPUT_PROTOCOL     TextIn;
  EFI_EVENT                          WaitForKeyEx;
  EFI_EVENT                          WaitForKey;
  EFI_EVENT                          TimerEvent;
  EFI_EVENT                          PmicTimerEvent;
  EFI_KEY_DATA                       Queue[PLATFORM_KEYPAD_QUEUE_SIZE];
  UINTN                              Head;
  UINTN                              Tail;
  LIST_ENTRY                         NotifyList;

  QCOM_GPIO_TLMM_PROTOCOL            *Gpio;
  QCOM_PM8X41_PROTOCOL               *Pmic;
  BOOLEAN                            PmicKeysEnabled;
  UINT8                              PmicLastRtSts;
  BOOLEAN                            Previous[KEY_INDEX_COUNT];
  BOOLEAN                            Raw[KEY_INDEX_COUNT];
  UINTN                              Stable[KEY_INDEX_COUNT];
  UINTN                              PressStart[KEY_INDEX_COUNT];
  UINTN                              PollCount;
  BOOLEAN                            GpioConfigured;
} PLATFORM_KEYPAD_DEV;

EFI_STATUS EFIAPI PlatformKeypadStartPoll(VOID);
EFI_STATUS EFIAPI PlatformKeypadPollPmicKeys(VOID);

STATIC PLATFORM_KEYPAD_DEV *mKeypadDev = NULL;
STATIC UINTN                mReadKeyLogCount = 0;

STATIC CONST PLATFORM_KEYPAD_POLL_PROTOCOL mKeypadPollProtocol = {
  PlatformKeypadStartPoll,
  PlatformKeypadPollPmicKeys
};

STATIC
BOOLEAN
IsQueueEmpty (
  IN PLATFORM_KEYPAD_DEV *Dev
  )
{
  return (BOOLEAN)(Dev->Head == Dev->Tail);
}

STATIC
EFI_STATUS
PopKey (
  IN  PLATFORM_KEYPAD_DEV *Dev,
  OUT EFI_KEY_DATA        *KeyData
  )
{
  if (IsQueueEmpty(Dev)) {
    return EFI_NOT_READY;
  }
  CopyMem(KeyData, &Dev->Queue[Dev->Head], sizeof(EFI_KEY_DATA));
  Dev->Head = (Dev->Head + 1) % PLATFORM_KEYPAD_QUEUE_SIZE;
  if (IsQueueEmpty(Dev)) {
    // Keep WaitForKey consistent with the queue. CheckEvent clears a pending
    // signal on these EVT_NOTIFY_WAIT events, so a consumer that reads all
    // queued keys does not spin on a stale signaled event.
    gBS->CheckEvent(Dev->WaitForKey);
    gBS->CheckEvent(Dev->WaitForKeyEx);
  }
  return EFI_SUCCESS;
}

STATIC
VOID
PushKey (
  IN PLATFORM_KEYPAD_DEV *Dev,
  IN EFI_KEY_DATA        *KeyData
  )
{
  LIST_ENTRY          *Link;
  PLATFORM_KEY_NOTIFY *Notify;

  if ((Dev->Tail + 1) % PLATFORM_KEYPAD_QUEUE_SIZE == Dev->Head) {
    Dev->Head = (Dev->Head + 1) % PLATFORM_KEYPAD_QUEUE_SIZE;
  }
  CopyMem(&Dev->Queue[Dev->Tail], KeyData, sizeof(EFI_KEY_DATA));
  Dev->Tail = (Dev->Tail + 1) % PLATFORM_KEYPAD_QUEUE_SIZE;

  gBS->SignalEvent(Dev->WaitForKey);
  gBS->SignalEvent(Dev->WaitForKeyEx);

  for (Link = GetFirstNode(&Dev->NotifyList);
       !IsNull(&Dev->NotifyList, Link);
       Link = GetNextNode(&Dev->NotifyList, Link)) {
    Notify = BASE_CR(Link, PLATFORM_KEY_NOTIFY, Link);
    if (Notify->Function != NULL &&
        Notify->KeyData.Key.ScanCode == KeyData->Key.ScanCode &&
        Notify->KeyData.Key.UnicodeChar == KeyData->Key.UnicodeChar) {
      Notify->Function((EFI_KEY_DATA *)KeyData);
    }
  }
}

STATIC
VOID
ConfigureButtons (
  IN PLATFORM_KEYPAD_DEV *Dev
  )
{
  BOOLEAN Level;

  if (Dev->GpioConfigured || Dev->Gpio == NULL) {
    return;
  }

  // Vol-Up: TLMM GPIO 107, active-low, pull-up
  Dev->Gpio->SetFunction(107, 0);
  Dev->Gpio->SetDriveStrength(107, 2);
  Dev->Gpio->SetPull(107, GPIO_PULL_UP);
  Dev->Gpio->DirectionInput(107);

  // Home: TLMM GPIO 109, active-low, pull-up
  Dev->Gpio->SetFunction(109, 0);
  Dev->Gpio->SetDriveStrength(109, 2);
  Dev->Gpio->SetPull(109, GPIO_PULL_UP);
  Dev->Gpio->DirectionInput(109);

  // Do not delay here: this runs from a TPL_CALLBACK timer notification and
  // MicroSecondDelay() hangs the console wait loop on this platform. The
  // 3-sample debounce below handles pull-up settling instead.
  Level = (Dev->Gpio->Get(107) == 0);
  Dev->Raw[KEY_INDEX_VOL_UP] = Level;
  Dev->Previous[KEY_INDEX_VOL_UP] = Level;
  Level = (Dev->Gpio->Get(109) == 0);
  Dev->Raw[KEY_INDEX_HOME] = Level;
  Dev->Previous[KEY_INDEX_HOME] = Level;

  Dev->GpioConfigured = TRUE;
  (void)Dev;
}

STATIC
VOID
PollKeyLongPress (
  IN PLATFORM_KEYPAD_DEV *Dev,
  IN UINTN                Index,
  IN BOOLEAN              Level,
  IN UINT16               ShortScan,
  IN CHAR16               ShortChar,
  IN UINT16               LongScan,
  IN CHAR16               LongChar
  )
{
  UINTN  HeldPolls;
  UINTN  Threshold;
  EFI_KEY_DATA KeyData;

  if (Level != Dev->Raw[Index]) {
    Dev->Raw[Index] = Level;
    Dev->Stable[Index] = 0;
    return;
  }
  if (Dev->Stable[Index] < 3) {
    Dev->Stable[Index]++;
    return;
  }

  if (Level && !Dev->Previous[Index]) {
    Dev->Previous[Index] = TRUE;
    Dev->PressStart[Index] = Dev->PollCount;
    return;
  }

  if (!Level && Dev->Previous[Index]) {
    Dev->Previous[Index] = FALSE;
    HeldPolls = Dev->PollCount - Dev->PressStart[Index];
    Threshold = PLATFORM_KEYPAD_LONG_PRESS_MS / PLATFORM_KEYPAD_POLL_MS;

    SetMem(&KeyData, sizeof(KeyData), 0);
    if (HeldPolls >= Threshold) {
      KeyData.Key.ScanCode    = LongScan;
      KeyData.Key.UnicodeChar = LongChar;
    } else {
      KeyData.Key.ScanCode    = ShortScan;
      KeyData.Key.UnicodeChar = ShortChar;
    }
    PushKey(Dev, &KeyData);
  }
}

STATIC
VOID
PollButtons (
  IN PLATFORM_KEYPAD_DEV *Dev
  )
{
  if (Dev->Gpio == NULL) {
    gBS->LocateProtocol(&gQcomGpioTlmmProtocolGuid, NULL, (VOID **)&Dev->Gpio);
  }

  ConfigureButtons(Dev);
  if (Dev->Gpio == NULL) {
    return;
  }

  // Vol+: tap = Down, hold = Up (Vol- is not readable on T550).
  PollKeyLongPress(Dev, KEY_INDEX_VOL_UP,
                   (Dev->Gpio->Get(107) == 0),
                   SCAN_DOWN, CHAR_NULL,
                   SCAN_UP, CHAR_NULL);
  // Home: tap = Enter, hold = Esc/Back.
  PollKeyLongPress(Dev, KEY_INDEX_HOME,
                   (Dev->Gpio->Get(109) == 0),
                   SCAN_NULL, CHAR_CARRIAGE_RETURN,
                   SCAN_ESC, CHAR_NULL);

  Dev->PollCount++;
}

STATIC
VOID
ConfigurePmicKeys (
  IN PLATFORM_KEYPAD_DEV *Dev
  )
{
  UINT8 Type;
  UINT8 Subtype;
  UINT8 Pull;
  UINT8 RtSts;
  UINT8 Latched;
  UINT8 Pending;
  UINT8 Reason1;

  if (Dev->PmicKeysEnabled) {
    return;
  }

  if (Dev->Pmic == NULL) {
    if (EFI_ERROR(gBS->LocateProtocol(&gQcomPm8x41ProtocolGuid, NULL,
                                      (VOID **)&Dev->Pmic)) ||
        (Dev->Pmic == NULL)) {
      DEBUG((DEBUG_ERROR,
             "PlatformKeypad: PMIC protocol missing, Power/Vol- disabled\n"));
      return;
    }
  }

  Type    = Dev->Pmic->pm8x41_reg_read(PON_PERPH_TYPE);
  Subtype = Dev->Pmic->pm8x41_reg_read(PON_PERPH_SUBTYPE);
  Pull    = Dev->Pmic->pm8x41_reg_read(PON_PULL_CTL);
  RtSts   = Dev->Pmic->pm8x41_reg_read(PON_INT_RT_STS);
  Latched = Dev->Pmic->pm8x41_reg_read(PON_INT_LATCHED_STS);
  Pending = Dev->Pmic->pm8x41_reg_read(PON_INT_PENDING_STS);
  Reason1 = Dev->Pmic->pm8x41_reg_read(0x808);
  DEBUG((DEBUG_ERROR,
         "PlatformKeypad: PON type=%02x subtype=%02x pull=%02x rt=%02x "
         "latched=%02x pending=%02x reason1=%02x\n",
         Type, Subtype, Pull, RtSts, Latched, Pending, Reason1));

  Dev->Pmic->pm8x41_reg_write(
      PON_PULL_CTL, (UINT8)(Pull | PON_PULL_KPDPWR | PON_PULL_RESIN));
  Dev->Pmic->pm8x41_reg_write(PON_INT_LATCHED_CLR, 0xFF);
  Dev->Pmic->pm8x41_reg_write(PON_INT_EN_SET,
                              (UINT8)(PON_KPDPWR_ON | PON_RESIN_ON));

  Pull  = Dev->Pmic->pm8x41_reg_read(PON_PULL_CTL);
  RtSts = Dev->Pmic->pm8x41_reg_read(PON_INT_RT_STS);
  DEBUG((DEBUG_ERROR, "PlatformKeypad: PON configured pull=%02x rt=%02x\n",
         Pull, RtSts));

  Dev->PmicLastRtSts   = RtSts;
  Dev->PmicKeysEnabled = TRUE;
}

STATIC
VOID
PollPmicButtons (
  IN PLATFORM_KEYPAD_DEV *Dev
  )
{
  UINT8 RtSts;

  if (!Dev->PmicKeysEnabled) {
    ConfigurePmicKeys(Dev);
    if (!Dev->PmicKeysEnabled) {
      return;
    }
  }

  RtSts = Dev->Pmic->pm8x41_reg_read(PON_INT_RT_STS);
  if (RtSts != Dev->PmicLastRtSts) {
    UINT8 Latched = Dev->Pmic->pm8x41_reg_read(PON_INT_LATCHED_STS);
    DEBUG((DEBUG_ERROR,
           "PlatformKeypad: PON rt=%02x (was %02x) latched=%02x\n", RtSts,
           Dev->PmicLastRtSts, Latched));
    Dev->PmicLastRtSts = RtSts;
  }

  // Power: tap = Enter, hold = Esc. Vol-Down: tap = Down, hold = Up.
  PollKeyLongPress(Dev, KEY_INDEX_POWER, (RtSts & PON_KPDPWR_ON) != 0, SCAN_NULL,
                   CHAR_CARRIAGE_RETURN, SCAN_ESC, CHAR_NULL);
  PollKeyLongPress(Dev, KEY_INDEX_VOL_DOWN, (RtSts & PON_RESIN_ON) != 0,
                   SCAN_DOWN, CHAR_NULL, SCAN_UP, CHAR_NULL);
}

STATIC
VOID
EFIAPI
KeypadTimerHandler (
  IN EFI_EVENT Event,
  IN VOID      *Context
  )
{
  PollButtons((PLATFORM_KEYPAD_DEV *)Context);
}

STATIC
VOID
EFIAPI
PmicTimerHandler (
  IN EFI_EVENT Event,
  IN VOID      *Context
  )
{
  PollPmicButtons((PLATFORM_KEYPAD_DEV *)Context);
}

STATIC
EFI_STATUS
StartPollTimer (
  IN PLATFORM_KEYPAD_DEV *Dev
  )
{
  EFI_STATUS Status;

  if (Dev->TimerEvent != NULL) {
    return EFI_SUCCESS;
  }
  DEBUG((DEBUG_ERROR, "PlatformKeypad: poll timer started\n"));
  Status = gBS->CreateEvent(EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                            KeypadTimerHandler, Dev, &Dev->TimerEvent);
  if (EFI_ERROR(Status)) {
    DEBUG((DEBUG_ERROR, "PlatformKeypad: timer event failed %r\n", Status));
    return Status;
  }
  Status = gBS->SetTimer(Dev->TimerEvent, TimerPeriodic,
                         PLATFORM_KEYPAD_POLL_100NS);
  if (EFI_ERROR(Status)) {
    DEBUG((DEBUG_ERROR, "PlatformKeypad: SetTimer failed %r\n", Status));
    return Status;
  }
  // Locate the PMIC protocol here at TPL_APPLICATION. Doing it from the
  // TPL_CALLBACK timer notification deadlocks the boot-manager wait loop.
  ConfigurePmicKeys(Dev);

  // PMIC keys live behind SPMI. The protocol is located above at
  // TPL_APPLICATION, so the poll callback itself can safely run in the
  // normal TPL_CALLBACK timer context. (CreateEvent rejects any notification
  // TPL at or below TPL_APPLICATION.)
  if (!Dev->PmicKeysEnabled) {
    return EFI_SUCCESS;
  }
  if (Dev->PmicTimerEvent == NULL) {
    Status = gBS->CreateEvent(EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                              PmicTimerHandler, Dev, &Dev->PmicTimerEvent);
    if (EFI_ERROR(Status)) {
      DEBUG((DEBUG_ERROR, "PlatformKeypad: PMIC timer event failed %r\n",
             Status));
      return EFI_SUCCESS;
    }
  }
  Status = gBS->SetTimer(Dev->PmicTimerEvent, TimerPeriodic,
                         PLATFORM_KEYPAD_POLL_100NS);
  if (EFI_ERROR(Status)) {
    DEBUG((DEBUG_ERROR, "PlatformKeypad: PMIC SetTimer failed %r\n", Status));
  } else {
    DEBUG((DEBUG_ERROR, "PlatformKeypad: PMIC poll timer started\n"));
  }
  return EFI_SUCCESS;
}

STATIC
VOID
EFIAPI
EmptyNotify (
  IN EFI_EVENT Event,
  IN VOID      *Context
  )
{
}

STATIC
EFI_STATUS
EFIAPI
PlatformKeypadReset (
  IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,
  IN BOOLEAN                           ExtendedVerification
  )
{
  PLATFORM_KEYPAD_DEV *Dev;

  Dev = BASE_CR(This, PLATFORM_KEYPAD_DEV, TextInEx);
  Dev->Head = 0;
  Dev->Tail = 0;
  SetMem(Dev->Previous, sizeof(Dev->Previous), 0);
  gBS->CheckEvent(Dev->WaitForKey);
  gBS->CheckEvent(Dev->WaitForKeyEx);
  DEBUG((DEBUG_ERROR, "PlatformKeypad: Reset called\n"));
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
PlatformKeypadReadKeyStrokeEx (
  IN  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,
  OUT EFI_KEY_DATA                      *KeyData
  )
{
  PLATFORM_KEYPAD_DEV *Dev;
  EFI_STATUS           Status;
  EFI_TPL              OldTpl;

  if (KeyData == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  Dev = BASE_CR(This, PLATFORM_KEYPAD_DEV, TextInEx);

  if (mReadKeyLogCount++ == 0) {
    DEBUG((DEBUG_ERROR, "PlatformKeypad: first ReadKeyStrokeEx call\n"));
  }

  OldTpl = gBS->RaiseTPL(TPL_NOTIFY);
  PollButtons(Dev);
  Status = PopKey(Dev, KeyData);
  gBS->RestoreTPL(OldTpl);

  if (!EFI_ERROR(Status)) {
    DEBUG((DEBUG_ERROR,
           "PlatformKeypad: delivered scan=%x char=%x\n",
           KeyData->Key.ScanCode, KeyData->Key.UnicodeChar));
  }
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
PlatformSimpleTextInReset (
  IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
  IN BOOLEAN                         ExtendedVerification
  )
{
  PLATFORM_KEYPAD_DEV *Dev;

  Dev = BASE_CR(This, PLATFORM_KEYPAD_DEV, TextIn);
  return PlatformKeypadReset(&Dev->TextInEx, ExtendedVerification);
}

STATIC
EFI_STATUS
EFIAPI
PlatformSimpleTextInReadKeyStroke (
  IN  EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
  OUT EFI_INPUT_KEY                  *Key
  )
{
  PLATFORM_KEYPAD_DEV *Dev;
  EFI_KEY_DATA         KeyData;
  EFI_STATUS           Status;

  if (Key == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  Dev = BASE_CR(This, PLATFORM_KEYPAD_DEV, TextIn);
  Status = PlatformKeypadReadKeyStrokeEx(&Dev->TextInEx, &KeyData);
  if (EFI_ERROR(Status)) {
    return Status;
  }
  CopyMem(Key, &KeyData.Key, sizeof(EFI_INPUT_KEY));
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
PlatformKeypadSetState (
  IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,
  IN UINT8                             *KeyState
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
PlatformKeypadRegisterKeyNotify (
  IN  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,
  IN  EFI_KEY_DATA                      *KeyData,
  IN  EFI_KEY_NOTIFY_FUNCTION           KeyNotificationFunction,
  OUT VOID                              **NotifyHandle
  )
{
  PLATFORM_KEYPAD_DEV *Dev;
  PLATFORM_KEY_NOTIFY *Notify;

  if (KeyData == NULL || KeyNotificationFunction == NULL || NotifyHandle == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  Dev = BASE_CR(This, PLATFORM_KEYPAD_DEV, TextInEx);
  Notify = AllocateZeroPool(sizeof(PLATFORM_KEY_NOTIFY));
  if (Notify == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  CopyMem(&Notify->KeyData, KeyData, sizeof(EFI_KEY_DATA));
  Notify->Function = KeyNotificationFunction;
  InsertTailList(&Dev->NotifyList, &Notify->Link);
  *NotifyHandle = Notify;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
PlatformKeypadUnregisterKeyNotify (
  IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,
  IN VOID                              *NotifyHandle
  )
{
  PLATFORM_KEYPAD_DEV *Dev;
  LIST_ENTRY          *Link;
  PLATFORM_KEY_NOTIFY *Notify;

  if (NotifyHandle == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  Dev = BASE_CR(This, PLATFORM_KEYPAD_DEV, TextInEx);
  for (Link = GetFirstNode(&Dev->NotifyList);
       !IsNull(&Dev->NotifyList, Link);
       Link = GetNextNode(&Dev->NotifyList, Link)) {
    Notify = BASE_CR(Link, PLATFORM_KEY_NOTIFY, Link);
    if (Notify == NotifyHandle) {
      RemoveEntryList(Link);
      FreePool(Notify);
      return EFI_SUCCESS;
    }
  }
  return EFI_NOT_FOUND;
}

EFI_STATUS
EFIAPI
PlatformKeypadInstall (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle;

  if (mKeypadDev != NULL) {
    return EFI_ALREADY_STARTED;
  }

  // GpioTlmmDxe can be dispatched twice (APRIORI + normal), and each image
  // load gets fresh static globals. Use our private protocol to detect the
  // first instance reliably.
  {
    VOID *Existing = NULL;
    if (!EFI_ERROR(gBS->LocateProtocol(&gPlatformKeypadPollProtocolGuid,
                                       NULL, &Existing))) {
      DEBUG((DEBUG_ERROR, "PlatformKeypad: input already installed, skipping\n"));
      return EFI_SUCCESS;
    }
  }

  mKeypadDev = AllocateZeroPool(sizeof(PLATFORM_KEYPAD_DEV));
  if (mKeypadDev == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  InitializeListHead(&mKeypadDev->NotifyList);
  mKeypadDev->TextInEx.Reset = PlatformKeypadReset;
  mKeypadDev->TextInEx.ReadKeyStrokeEx = PlatformKeypadReadKeyStrokeEx;
  mKeypadDev->TextInEx.SetState = PlatformKeypadSetState;
  mKeypadDev->TextInEx.RegisterKeyNotify = PlatformKeypadRegisterKeyNotify;
  mKeypadDev->TextInEx.UnregisterKeyNotify = PlatformKeypadUnregisterKeyNotify;

  mKeypadDev->TextIn.Reset = PlatformSimpleTextInReset;
  mKeypadDev->TextIn.ReadKeyStroke = PlatformSimpleTextInReadKeyStroke;

  //
  // SimpleTextIn WaitForKey must be EVT_NOTIFY_WAIT. ConSplitter polls it
  // with CheckEvent(); CheckEvent rejects EVT_NOTIFY_SIGNAL events, which
  // silently kept the keypad out of the aggregated console input.
  //
  Status = gBS->CreateEvent(EVT_NOTIFY_WAIT, TPL_CALLBACK, EmptyNotify, NULL,
                            &mKeypadDev->WaitForKey);
  if (EFI_ERROR(Status)) {
    goto Error;
  }
  Status = gBS->CreateEvent(EVT_NOTIFY_WAIT, TPL_CALLBACK, EmptyNotify, NULL,
                            &mKeypadDev->WaitForKeyEx);
  if (EFI_ERROR(Status)) {
    goto Error;
  }
  mKeypadDev->TextIn.WaitForKey = mKeypadDev->WaitForKey;
  mKeypadDev->TextInEx.WaitForKeyEx = mKeypadDev->WaitForKeyEx;

  // The poll timer is started by BDS after console initialization, so early
  // DXE and MMCHS setup are never disturbed.
  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces(
      &Handle,
      &gEfiDevicePathProtocolGuid, &mKeypadDevicePath,
      &gEfiConsoleInDeviceGuid, NULL,
      &gEfiSimpleTextInProtocolGuid, &mKeypadDev->TextIn,
      &gEfiSimpleTextInputExProtocolGuid, &mKeypadDev->TextInEx,
      &gPlatformKeypadPollProtocolGuid, (VOID *)&mKeypadPollProtocol,
      NULL);
  if (EFI_ERROR(Status)) {
    goto Error;
  }

  DEBUG((DEBUG_ERROR,
         "PlatformKeypad: installed ConIn SimpleTextIn(Ex) on handle %p\n",
         Handle));
  return EFI_SUCCESS;

Error:
  DEBUG((DEBUG_ERROR, "PlatformKeypad: install failed %r\n", Status));
  mKeypadDev = NULL;
  return Status;
}

/**
  Start polling the hardware buttons.

  Called by PlatformBootManagerAfterConsole(), after all DXE drivers and
  console connections are complete. Starting the timer earlier interferes
  with MMCHS/console initialization on this platform.
**/
EFI_STATUS
EFIAPI
PlatformKeypadStartPoll (
  VOID
  )
{
  if (mKeypadDev == NULL) {
    return EFI_NOT_READY;
  }
  return StartPollTimer(mKeypadDev);
}

/**
  Poll PMIC RESIN (Vol-) and PON (Power) keys.

  Called from PlatformBootManagerWaitCallback(), i.e. at TPL_APPLICATION.
  Calling into the PMIC protocol from the TPL_CALLBACK key timer deadlocks
  this platform, so PMIC keys are deliberately handled here instead.
**/
EFI_STATUS
EFIAPI
PlatformKeypadPollPmicKeys (
  VOID
  )
{
  PLATFORM_KEYPAD_DEV *Dev;

  Dev = mKeypadDev;
  if (Dev == NULL) {
    return EFI_NOT_READY;
  }

  PollPmicButtons(Dev);
  return Dev->PmicKeysEnabled ? EFI_SUCCESS : EFI_NOT_READY;
}
