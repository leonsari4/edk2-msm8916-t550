#include <PiDxe.h>

#include <Library/DebugLib.h>
#include <Library/LKEnvLib.h>
#include <Library/QcomPm8x41Lib.h>
#include <Library/QcomGpioTlmmLib.h>
#include <Library/KeypadDeviceImplLib.h>
#include <Library/KeypadDeviceHelperLib.h>
#include <Protocol/KeypadDevice.h>

typedef enum {
  KEY_DEVICE_TYPE_UNKNOWN,

  KEY_DEVICE_TYPE_TLMM,
  KEY_DEVICE_TYPE_PM8X41,
  KEY_DEVICE_TYPE_PM8X41_PON,
} KEY_DEVICE_TYPE;

typedef struct {
  KEY_CONTEXT      EfiKeyContext;
  BOOLEAN          IsValid;
  KEY_DEVICE_TYPE  DeviceType;

  // gpio
  UINT8            Gpio;
  BOOLEAN          ActiveLow;

  // pon
  UINT32           PonType;
} KEY_CONTEXT_PRIVATE;

STATIC KEY_CONTEXT_PRIVATE KeyContextPower;
STATIC KEY_CONTEXT_PRIVATE KeyContextVolumeUp;
STATIC KEY_CONTEXT_PRIVATE KeyContextVolumeDown;
STATIC KEY_CONTEXT_PRIVATE KeyContextHome;

STATIC KEY_CONTEXT_PRIVATE* KeyList[] = {
  &KeyContextPower,
  &KeyContextVolumeUp,
  &KeyContextVolumeDown,
  &KeyContextHome
};

STATIC
VOID
KeypadInitializeKeyContextPrivate (
  KEY_CONTEXT_PRIVATE  *Context
  )
{
  Context->IsValid = FALSE;
  Context->Gpio = 0;
  Context->DeviceType = KEY_DEVICE_TYPE_UNKNOWN;
  Context->ActiveLow = FALSE;
}

STATIC
KEY_CONTEXT_PRIVATE*
KeypadKeyCodeToKeyContext (
  UINT32 KeyCode
  )
{
  if (KeyCode == 114){
    return &KeyContextVolumeDown;
  }else if (KeyCode == 115){
    return &KeyContextVolumeUp;
  }else if (KeyCode == 116){
    return &KeyContextPower;
  }else if (KeyCode == 102){
    return &KeyContextHome;
  }else
    return NULL;
}

STATIC BOOLEAN GpioConfigured = FALSE;

STATIC
VOID
KeypadConfigureGpios (
  VOID
  )
{
  if (gGpioTlmm == NULL) {
    gBS->LocateProtocol (&gQcomGpioTlmmProtocolGuid, NULL, (VOID **)&gGpioTlmm);
  }
  if (GpioConfigured || gGpioTlmm == NULL) {
    return;
  }

  // Vol Up: TLMM GPIO 107, active-low, pull-up
  gGpioTlmm->SetFunction(107, 0);
  gGpioTlmm->SetPull(107, GPIO_PULL_UP);
  gGpioTlmm->SetDriveStrength(107, 2);
  gGpioTlmm->DirectionInput(107);

  // Home: TLMM GPIO 109, active-low, pull-up
  gGpioTlmm->SetFunction(109, 0);
  gGpioTlmm->SetPull(109, GPIO_PULL_UP);
  gGpioTlmm->SetDriveStrength(109, 2);
  gGpioTlmm->DirectionInput(109);

  GpioConfigured = TRUE;
}

RETURN_STATUS
EFIAPI
KeypadDeviceImplConstructor (
  VOID
  )
{
  UINTN                          Index;
  KEY_CONTEXT_PRIVATE            *StaticContext;

  // Reset all keys
  for (Index=0; Index<ARRAY_SIZE(KeyList); Index++) {
    KeypadInitializeKeyContextPrivate (KeyList[Index]);
  }

  // Vol Up (115) on TLMM GPIO 107
  StaticContext = KeypadKeyCodeToKeyContext(115);
  StaticContext->DeviceType = KEY_DEVICE_TYPE_TLMM;
  StaticContext->Gpio = 107;
  StaticContext->ActiveLow = TRUE;
  StaticContext->IsValid = TRUE;

  // Home (102) on TLMM GPIO 109
  StaticContext = KeypadKeyCodeToKeyContext(102);
  StaticContext->DeviceType = KEY_DEVICE_TYPE_TLMM;
  StaticContext->Gpio = 109;
  StaticContext->ActiveLow = TRUE;
  StaticContext->IsValid = TRUE;

  // Vol Down (114) on PMIC RESIN
  StaticContext = KeypadKeyCodeToKeyContext(114);
  StaticContext->DeviceType = KEY_DEVICE_TYPE_PM8X41_PON;
  StaticContext->PonType = 1;
  StaticContext->ActiveLow = FALSE;
  StaticContext->IsValid = TRUE;

  // Power (116) on PMIC PON
  StaticContext = KeypadKeyCodeToKeyContext(116);
  StaticContext->DeviceType = KEY_DEVICE_TYPE_PM8X41_PON;
  StaticContext->PonType = 0;
  StaticContext->ActiveLow = FALSE;
  StaticContext->IsValid = TRUE;

  return RETURN_SUCCESS;
}

EFI_STATUS EFIAPI KeypadDeviceImplReset (KEYPAD_DEVICE_PROTOCOL *This)
{
  KeypadConfigureGpios();

  LibKeyInitializeKeyContext(&KeyContextPower.EfiKeyContext);
  KeyContextPower.EfiKeyContext.KeyData.Key.UnicodeChar = CHAR_CARRIAGE_RETURN;

  LibKeyInitializeKeyContext(&KeyContextVolumeUp.EfiKeyContext);
  KeyContextVolumeUp.EfiKeyContext.KeyData.Key.ScanCode = SCAN_UP;

  LibKeyInitializeKeyContext(&KeyContextVolumeDown.EfiKeyContext);
  KeyContextVolumeDown.EfiKeyContext.KeyData.Key.ScanCode = SCAN_DOWN;

  LibKeyInitializeKeyContext(&KeyContextHome.EfiKeyContext);
  KeyContextHome.EfiKeyContext.KeyData.Key.ScanCode = SCAN_ESC;

  return EFI_SUCCESS;
}

EFI_STATUS KeypadDeviceImplGetKeys (KEYPAD_DEVICE_PROTOCOL *This, KEYPAD_RETURN_API *KeypadReturnApi, UINT64 Delta)
{
  UINT8    GpioStatus;
  BOOLEAN  IsPressed;
  INTN     RC;
  UINTN    Index;

  if (gPm8x41 == NULL) {
    gBS->LocateProtocol (&gQcomPm8x41ProtocolGuid, NULL, (VOID **)&gPm8x41);
  }
  if (gGpioTlmm == NULL) {
    gBS->LocateProtocol (&gQcomGpioTlmmProtocolGuid, NULL, (VOID **)&gGpioTlmm);
  }

  for (Index=0; Index<ARRAY_SIZE(KeyList); Index++) {
    KEY_CONTEXT_PRIVATE *Context = KeyList[Index];

    // check if this is a valid key
    if (Context->IsValid == FALSE)
      continue;

    // get status
    if (Context->DeviceType == KEY_DEVICE_TYPE_TLMM) {
      if (gGpioTlmm == NULL) {
        continue;
      }
      GpioStatus = gGpioTlmm->Get(Context->Gpio);
      RC = 0;
    }
    else if (Context->DeviceType == KEY_DEVICE_TYPE_PM8X41) {
      if (gPm8x41 == NULL) {
        continue;
      }
      RC = gPm8x41->pm8x41_gpio_get(Context->Gpio, &GpioStatus);
    }
    else if (Context->DeviceType == KEY_DEVICE_TYPE_PM8X41_PON) {
      if (gPm8x41 == NULL) {
        continue;
      }
      if (Context->PonType == 0x1)
        GpioStatus = gPm8x41->pm8x41_resin_status ();
      else if (Context->PonType == 0x0)
        GpioStatus = gPm8x41->pm8x41_get_pwrkey_is_pressed ();
      else
        continue;

      RC = 0;
    }
    else {
      continue;
    }
    if (RC != 0)
      continue;
    // update key status
    IsPressed = (GpioStatus ? 1 : 0) ^ Context->ActiveLow;
    if (IsPressed) {
      DEBUG((DEBUG_ERROR, "KeypadImpl: key pressed (gpio=%u status=%u)\n", Context->Gpio, GpioStatus));
    }
    LibKeyUpdateKeyStatus(&Context->EfiKeyContext, KeypadReturnApi, IsPressed, Delta);
  }

  return EFI_SUCCESS;
}
