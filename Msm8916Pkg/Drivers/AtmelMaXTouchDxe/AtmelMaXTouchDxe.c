/*
 * Atmel maXTouch touchscreen driver (mXT1664T).
 *
 * Produces EFI_ABSOLUTE_POINTER_PROTOCOL so the UEFI console (ConSplitter)
 * and touch aware boot loaders can use it.
 *
 * Register protocol taken from the Linux kernel driver
 * (drivers/input/touchscreen/atmel_mxt_ts.c).
 */

#include <Library/LKEnvLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/QcomGpioTlmmLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Protocol/AbsolutePointer.h>
#include <Protocol/DevicePath.h>
#include <Protocol/QcomPm8x41.h>

#include <Library/GpioI2cLib.h>

/* PM8916 LDO5 (SID 1, PPID 0x44): touch 1.8V rail on GT510. */
#define PM8916_LDO5                 0x14400
#define LDO_STATUS1                 0x08
#define   LDO_STATUS1_READY         0x80
#define LDO_EN_CTL                  0x46
#define   LDO_EN_CTL_ENABLE         0x80

#define GPIO_NONE 0xFFFFFFFF

#define MXT_INFO_SIZE            7
#define MXT_OBJECT_SIZE          6
#define MXT_OBJECT_START         7
#define MXT_GEN_MESSAGE_T5       5
#define MXT_GEN_COMMAND_T6       6
#define MXT_TOUCH_MULTI_T9       9

#define MXT_CMD_RESET            0x01
#define MXT_RESET_TIME_MS        200

#define TOUCH_POLL_PERIOD_100NS  (20 * 10000)
#define TOUCH_FORCED_POLL_TICKS  5

#define TOUCH_DEV_SIGNATURE SIGNATURE_32('M', 'X', 'T', 'C')

typedef struct {
  VENDOR_DEVICE_PATH       Vendor;
  EFI_DEVICE_PATH_PROTOCOL End;
} TOUCH_DEVICE_PATH;

typedef struct {
  UINT8  Type;
  UINT16 StartAddress;
  UINT8  Size;
  UINT8  Instances;
  UINT8  NumReportIds;
} MXT_OBJECT;

typedef struct {
  UINT32                        Signature;
  EFI_HANDLE                    Handle;
  EFI_ABSOLUTE_POINTER_PROTOCOL AbsPointer;
  EFI_ABSOLUTE_POINTER_MODE     Mode;
  TOUCH_DEVICE_PATH             DevicePath;

  GPIO_I2C_BUS                  Bus;
  UINT8                         Address;
  UINT32                        IntGpio;
  UINT32                        ResetGpio;
  UINT32                        PowerGpio;

  MXT_OBJECT                   *Objects;
  UINT8                         ObjectNum;
  UINT16                        T5Start;
  UINT8                         T9MinId;
  UINT8                         T9MaxId;

  EFI_EVENT                     PollEvent;
  UINTN                         TickCount;

  EFI_ABSOLUTE_POINTER_STATE    State;
  BOOLEAN                       StateChanged;
} TOUCH_DEV;

#define TOUCH_DEV_FROM_ABSPOINTER(a) CR(a, TOUCH_DEV, AbsPointer, TOUCH_DEV_SIGNATURE)

STATIC TOUCH_DEVICE_PATH mTouchDevicePathTemplate = {
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      {
        (UINT8)(sizeof(VENDOR_DEVICE_PATH)),
        (UINT8)((sizeof(VENDOR_DEVICE_PATH)) >> 8),
      },
    },
    EFI_CALLER_ID_GUID
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    {sizeof(EFI_DEVICE_PATH_PROTOCOL), 0},
  },
};

STATIC EFI_STATUS
MxtReadReg (
  IN  TOUCH_DEV  *Dev,
  IN  UINT16     Reg,
  IN  UINTN      Count,
  OUT UINT8      *Buf
  )
{
  UINT8 Addr[2];

  Addr[0] = (UINT8)(Reg & 0xFF);
  Addr[1] = (UINT8)(Reg >> 8);

  return GpioI2cTransfer(&Dev->Bus, Dev->Address, Addr, 2, Buf, Count);
}

STATIC EFI_STATUS
MxtWriteReg (
  IN TOUCH_DEV  *Dev,
  IN UINT16     Reg,
  IN UINT8      Val
  )
{
  UINT8 Buf[3];

  Buf[0] = (UINT8)(Reg & 0xFF);
  Buf[1] = (UINT8)(Reg >> 8);
  Buf[2] = Val;
  return GpioI2cTransfer(&Dev->Bus, Dev->Address, Buf, 3, NULL, 0);
}

STATIC EFI_STATUS
MxtReadRegRetry (
  IN  TOUCH_DEV  *Dev,
  IN  UINT16     Reg,
  IN  UINTN      Count,
  OUT UINT8      *Buf
  )
{
  EFI_STATUS Status = EFI_DEVICE_ERROR;
  UINTN      Retry;

  for (Retry = 0; Retry < 3; Retry++) {
    Status = MxtReadReg(Dev, Reg, Count, Buf);
    if (!EFI_ERROR(Status)) {
      return EFI_SUCCESS;
    }

    DEBUG((EFI_D_ERROR,
           "maXTouch: read %u bytes @0x%04x failed (%r), retry %u\n",
           (UINT32)Count, (UINT32)Reg, Status, (UINT32)(Retry + 1)));
    MicroSecondDelay(5000);
    GpioI2cInit(&Dev->Bus);
  }

  return Status;
}

/*
 * The object table can be several hundred bytes long.  Read it in small
 * chunks: this works around bit-banged transfers that become unreliable
 * once the slave has been clocked for a long time, and it tells us exactly
 * which part of the table the controller refuses.
 */
STATIC EFI_STATUS
MxtReadObjectTable (
  IN  TOUCH_DEV  *Dev,
  OUT UINT8      *Buf,
  IN  UINTN      Size
  )
{
  EFI_STATUS Status;
  UINTN      Offset = 0;

  while (Offset < Size) {
    UINTN Chunk = Size - Offset;

    if (Chunk > 16) {
      Chunk = 16;
    }

    Status = MxtReadRegRetry(Dev, (UINT16)(MXT_OBJECT_START + Offset),
                             Chunk, Buf + Offset);
    if (EFI_ERROR(Status)) {
      DEBUG((EFI_D_ERROR,
             "maXTouch: object table chunk %u/%u @0x%04x failed: %r\n",
             (UINT32)Offset, (UINT32)Size,
             (UINT32)(MXT_OBJECT_START + Offset), Status));
      return Status;
    }

    Offset += Chunk;
    MicroSecondDelay(1000);
  }

  return EFI_SUCCESS;
}

STATIC MXT_OBJECT *
MxtGetObject (
  IN TOUCH_DEV  *Dev,
  IN UINT8      Type
  )
{
  UINTN i;

  for (i = 0; i < Dev->ObjectNum; i++) {
    if (Dev->Objects[i].Type == Type) {
      return &Dev->Objects[i];
    }
  }
  return NULL;
}

STATIC VOID
TransformCoordinates (
  IN OUT UINT64  *X,
  IN OUT UINT64  *Y
  )
{
  UINT64 Tmp;

  if (FixedPcdGetBool(PcdTouchSwapXY)) {
    Tmp = *X;
    *X  = *Y;
    *Y  = Tmp;
  }

  if (*X > (UINT64)FixedPcdGet32(PcdTouchMaxX) - 1) {
    *X = FixedPcdGet32(PcdTouchMaxX) - 1;
  }
  if (*Y > (UINT64)FixedPcdGet32(PcdTouchMaxY) - 1) {
    *Y = FixedPcdGet32(PcdTouchMaxY) - 1;
  }

  if (FixedPcdGetBool(PcdTouchInvertX)) {
    *X = FixedPcdGet32(PcdTouchMaxX) - 1 - *X;
  }
  if (FixedPcdGetBool(PcdTouchInvertY)) {
    *Y = FixedPcdGet32(PcdTouchMaxY) - 1 - *Y;
  }
}

STATIC VOID
MxtPoll (
  IN TOUCH_DEV  *Dev
  )
{
  EFI_STATUS Status;
  UINT8      Msg[8];
  UINT8      ReportId;
  BOOLEAN    Touching = FALSE;
  UINT64     X = Dev->State.CurrentX;
  UINT64     Y = Dev->State.CurrentY;

  if (Dev->T5Start == 0) {
    return;
  }

  Status = MxtReadReg(Dev, Dev->T5Start, 8, Msg);
  if (EFI_ERROR(Status)) {
    return;
  }

  ReportId = Msg[0];
  if (ReportId == 0) {
    return;
  }

  if (Dev->T9MinId != 0 && ReportId >= Dev->T9MinId && ReportId <= Dev->T9MaxId) {
    UINT8 StatusByte = Msg[1];
    X = (UINT64)(Msg[2] | (Msg[3] << 8));
    Y = (UINT64)(Msg[4] | (Msg[5] << 8));
    Touching = (StatusByte != 0);
    TransformCoordinates(&X, &Y);
  }

  if (Touching != ((Dev->State.ActiveButtons & EFI_ABSP_TouchActive) != 0) ||
      X != Dev->State.CurrentX || Y != Dev->State.CurrentY) {
    Dev->State.CurrentX      = X;
    Dev->State.CurrentY      = Y;
    Dev->State.CurrentZ      = 0;
    Dev->State.ActiveButtons = Touching ? EFI_ABSP_TouchActive : 0;
    Dev->StateChanged        = TRUE;
  }
}

STATIC VOID
EFIAPI
TouchPollNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  TOUCH_DEV *Dev     = Context;
  BOOLEAN   Pending  = FALSE;

  Dev->TickCount++;

  if (Dev->IntGpio == GPIO_NONE || gGpioTlmm->Get(Dev->IntGpio) == 0) {
    Pending = TRUE;
  }
  if (Dev->State.ActiveButtons & EFI_ABSP_TouchActive) {
    Pending = TRUE;
  }
  if (Dev->TickCount % TOUCH_FORCED_POLL_TICKS == 0) {
    Pending = TRUE;
  }

  if (Pending) {
    MxtPoll(Dev);
  }
}

STATIC EFI_STATUS
EFIAPI
TouchReset (
  IN EFI_ABSOLUTE_POINTER_PROTOCOL  *This,
  IN BOOLEAN                        ExtendedVerification
  )
{
  TOUCH_DEV *Dev = TOUCH_DEV_FROM_ABSPOINTER(This);
  EFI_TPL   OldTpl;

  OldTpl = gBS->RaiseTPL(TPL_NOTIFY);
  ZeroMem(&Dev->State, sizeof(Dev->State));
  Dev->StateChanged = FALSE;
  gBS->RestoreTPL(OldTpl);

  return EFI_SUCCESS;
}

STATIC EFI_STATUS
EFIAPI
TouchGetState (
  IN     EFI_ABSOLUTE_POINTER_PROTOCOL  *This,
  IN OUT EFI_ABSOLUTE_POINTER_STATE     *State
  )
{
  TOUCH_DEV  *Dev = TOUCH_DEV_FROM_ABSPOINTER(This);
  EFI_TPL    OldTpl;
  EFI_STATUS Status = EFI_NOT_READY;

  if (State == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  OldTpl = gBS->RaiseTPL(TPL_NOTIFY);
  if (Dev->StateChanged) {
    CopyMem(State, &Dev->State, sizeof(*State));
    Dev->StateChanged = FALSE;
    Status            = EFI_SUCCESS;
  }
  gBS->RestoreTPL(OldTpl);

  return Status;
}

STATIC VOID
EFIAPI
TouchWaitForInput (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  TOUCH_DEV *Dev = Context;

  if (Dev->StateChanged) {
    gBS->SignalEvent(Event);
  }
}

STATIC VOID
MxtEnsureTouchRail (
  VOID
  )
{
  QCOM_PM8X41_PROTOCOL  *Pmic = NULL;
  UINT8                  St;
  UINT8                  En;
  UINT8                  St2;

  if (EFI_ERROR(gBS->LocateProtocol(&gQcomPm8x41ProtocolGuid, NULL,
                                    (VOID **)&Pmic)) ||
      (Pmic == NULL)) {
    DEBUG((EFI_D_ERROR, "maXTouch: PMIC protocol missing, cannot check LDO5\n"));
    return;
  }

  St = Pmic->pm8x41_reg_read(PM8916_LDO5 + LDO_STATUS1);
  En = Pmic->pm8x41_reg_read(PM8916_LDO5 + LDO_EN_CTL);
  DEBUG((EFI_D_ERROR, "maXTouch: LDO5 status=%02x en_ctl=%02x\n", St, En));

  if ((St & LDO_STATUS1_READY) != 0) {
    return;
  }

  Pmic->pm8x41_reg_write(PM8916_LDO5 + LDO_EN_CTL,
                         (UINT8)(En | LDO_EN_CTL_ENABLE));
  MicroSecondDelay(2000);
  St2 = Pmic->pm8x41_reg_read(PM8916_LDO5 + LDO_STATUS1);
  DEBUG((EFI_D_ERROR,
         "maXTouch: LDO5 enable attempted, status=%02x en_ctl=%02x\n", St2,
         Pmic->pm8x41_reg_read(PM8916_LDO5 + LDO_EN_CTL)));
}

STATIC VOID
MxtPowerOn (
  IN TOUCH_DEV  *Dev
  )
{
  if (Dev->PowerGpio != GPIO_NONE) {
    gGpioTlmm->SetFunction(Dev->PowerGpio, 0);
    gGpioTlmm->SetDriveStrength(Dev->PowerGpio, 2);
    gGpioTlmm->SetPull(Dev->PowerGpio, GPIO_PULL_NONE);
    gGpioTlmm->DirectionOutput(Dev->PowerGpio, 1);
  }

  if (Dev->ResetGpio != GPIO_NONE) {
    gGpioTlmm->SetFunction(Dev->ResetGpio, 0);
    gGpioTlmm->SetDriveStrength(Dev->ResetGpio, 2);
    gGpioTlmm->SetPull(Dev->ResetGpio, GPIO_PULL_NONE);
    gGpioTlmm->DirectionOutput(Dev->ResetGpio, 0);
    MicroSecondDelay(10000);
    gGpioTlmm->DirectionOutput(Dev->ResetGpio, 1);
  }

  if (Dev->IntGpio != GPIO_NONE) {
    gGpioTlmm->SetFunction(Dev->IntGpio, 0);
    gGpioTlmm->SetDriveStrength(Dev->IntGpio, 2);
    gGpioTlmm->SetPull(Dev->IntGpio, GPIO_PULL_NONE);
    gGpioTlmm->DirectionInput(Dev->IntGpio);
  }

  MicroSecondDelay(100000);
}

EFI_STATUS
EFIAPI
AtmelMaXTouchDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS Status;
  TOUCH_DEV  *Dev;
  UINT8      Info[MXT_INFO_SIZE];
  UINT8      ObjBuf[512];
  UINTN      ObjTableSize;
  UINTN      i;
  MXT_OBJECT *Obj;
  UINT8      Rid;

  Dev = AllocateZeroPool(sizeof(*Dev));
  if (Dev == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Dev->Signature        = TOUCH_DEV_SIGNATURE;
  Dev->Bus.SdaGpio      = FixedPcdGet32(PcdTouchI2cSdaGpio);
  Dev->Bus.SclGpio      = FixedPcdGet32(PcdTouchI2cSclGpio);
  Dev->Bus.HalfPeriodUs = 5;
  Dev->Address          = FixedPcdGet8(PcdTouchI2cAddress);
  Dev->IntGpio          = FixedPcdGet32(PcdTouchIntGpio);
  Dev->PowerGpio        = FixedPcdGet32(PcdTouchPowerEnableGpio);
  Dev->ResetGpio        = FixedPcdGet32(PcdTouchResetGpio);

  MxtEnsureTouchRail();
  MxtPowerOn(Dev);
  DEBUG((EFI_D_ERROR, "maXTouch: pwr gpio=%u rst gpio=%u int gpio=%u\n",
         (UINT32)gGpioTlmm->Get(Dev->PowerGpio),
         (UINT32)gGpioTlmm->Get(Dev->ResetGpio),
         (UINT32)gGpioTlmm->Get(Dev->IntGpio)));

  /* Switch pins to GPIO mode (function 0) before bit-banging */
  gGpioTlmm->SetFunction(Dev->Bus.SdaGpio, 0);
  gGpioTlmm->SetFunction(Dev->Bus.SclGpio, 0);
  gGpioTlmm->SetDriveStrength(Dev->Bus.SdaGpio, 2);
  gGpioTlmm->SetDriveStrength(Dev->Bus.SclGpio, 2);
  gGpioTlmm->SetPull(Dev->Bus.SdaGpio, GPIO_PULL_NONE);
  gGpioTlmm->SetPull(Dev->Bus.SclGpio, GPIO_PULL_NONE);
  gGpioTlmm->DirectionInput(Dev->Bus.SdaGpio);
  gGpioTlmm->DirectionInput(Dev->Bus.SclGpio);
  MicroSecondDelay(100);
  DEBUG((EFI_D_ERROR, "maXTouch: I2C pins SDA=%u SCL=%u (GPIO mode)\n",
         (UINT32)gGpioTlmm->Get(Dev->Bus.SdaGpio), (UINT32)gGpioTlmm->Get(Dev->Bus.SclGpio)));

  GpioI2cInit(&Dev->Bus);
  MicroSecondDelay(100);
  DEBUG((EFI_D_ERROR, "maXTouch: I2C pins after init SDA=%u SCL=%u\n",
         (UINT32)gGpioTlmm->Get(Dev->Bus.SdaGpio), (UINT32)gGpioTlmm->Get(Dev->Bus.SclGpio)));

  /* Release both lines so the external pull-ups can set the idle level.
   * SDA must be high here, otherwise the bus is stuck or the chip is
   * unpowered and clamping the line. */
  gGpioTlmm->DirectionInput(Dev->Bus.SdaGpio);
  gGpioTlmm->DirectionInput(Dev->Bus.SclGpio);
  MicroSecondDelay(200);
  DEBUG((EFI_D_ERROR, "maXTouch: I2C idle SDA=%u SCL=%u\n",
         (UINT32)gGpioTlmm->Get(Dev->Bus.SdaGpio), (UINT32)gGpioTlmm->Get(Dev->Bus.SclGpio)));
  GpioI2cInit(&Dev->Bus);

  Status = MxtReadReg(Dev, 0, MXT_INFO_SIZE, Info);
  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR, "maXTouch: no response at 0x%02x: %r\n", Dev->Address, Status));
    goto Fail;
  }

  Dev->ObjectNum = Info[6];
  DEBUG((EFI_D_ERROR, "maXTouch: INFO %02x %02x %02x %02x %02x %02x %02x\n",
         Info[0], Info[1], Info[2], Info[3], Info[4], Info[5], Info[6]));
  DEBUG((EFI_D_ERROR, "maXTouch: family %u variant %u version %u build %u matrix %ux%u objects %u\n",
         Info[0], Info[1], Info[2], Info[3], Info[4], Info[5], Info[6]));

  if (Dev->ObjectNum == 0 || Dev->ObjectNum > 50) {
    Status = EFI_DEVICE_ERROR;
    goto Fail;
  }

  ObjTableSize = Dev->ObjectNum * MXT_OBJECT_SIZE;
  if (ObjTableSize > sizeof(ObjBuf)) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Fail;
  }

  //
  // Give the controller a moment after the INFO read, then fetch the object
  // table in chunks so a failure points at the exact offset.
  //
  MicroSecondDelay(10000);
  Status = MxtReadObjectTable(Dev, ObjBuf, ObjTableSize);
  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR, "maXTouch: object table read failed: %r\n", Status));
    goto Fail;
  }
  DEBUG((EFI_D_ERROR,
         "maXTouch: object table %02x %02x %02x %02x %02x %02x ...\n",
         ObjBuf[0], ObjBuf[1], ObjBuf[2], ObjBuf[3], ObjBuf[4], ObjBuf[5]));

  Dev->Objects = AllocateZeroPool(Dev->ObjectNum * sizeof(MXT_OBJECT));
  if (Dev->Objects == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Fail;
  }

  for (i = 0; i < Dev->ObjectNum; i++) {
    UINT8 *P = &ObjBuf[i * MXT_OBJECT_SIZE];
    Dev->Objects[i].Type          = P[0];
    Dev->Objects[i].StartAddress  = (UINT16)(P[1] | (P[2] << 8));
    Dev->Objects[i].Size          = P[3];
    Dev->Objects[i].Instances     = P[4];
    Dev->Objects[i].NumReportIds  = P[5];
  }

  Obj = MxtGetObject(Dev, MXT_GEN_MESSAGE_T5);
  if (Obj != NULL) {
    Dev->T5Start = Obj->StartAddress;
  }

  Obj = MxtGetObject(Dev, MXT_TOUCH_MULTI_T9);
  if (Obj != NULL) {
    Rid = 1;
    for (i = 0; i < Dev->ObjectNum; i++) {
      if (&Dev->Objects[i] == Obj) {
        Dev->T9MinId = Rid;
        Dev->T9MaxId = Rid + Obj->NumReportIds - 1;
        break;
      }
      Rid += Dev->Objects[i].NumReportIds;
    }
    DEBUG((EFI_D_ERROR, "maXTouch: T9 report ids %u..%u\n", Dev->T9MinId, Dev->T9MaxId));
  }

  Obj = MxtGetObject(Dev, MXT_GEN_COMMAND_T6);
  if (Obj != NULL) {
    MxtWriteReg(Dev, Obj->StartAddress, MXT_CMD_RESET);
    MicroSecondDelay(MXT_RESET_TIME_MS * 1000);
  }

  Dev->Mode.AbsoluteMinX = 0;
  Dev->Mode.AbsoluteMinY = 0;
  Dev->Mode.AbsoluteMinZ = 0;
  Dev->Mode.AbsoluteMaxX = FixedPcdGet32(PcdTouchMaxX);
  Dev->Mode.AbsoluteMaxY = FixedPcdGet32(PcdTouchMaxY);
  Dev->Mode.AbsoluteMaxZ = 0;
  Dev->Mode.Attributes   = 0;

  Dev->AbsPointer.Reset    = TouchReset;
  Dev->AbsPointer.GetState = TouchGetState;
  Dev->AbsPointer.Mode     = &Dev->Mode;

  CopyMem(&Dev->DevicePath, &mTouchDevicePathTemplate, sizeof(Dev->DevicePath));

  Status = gBS->CreateEvent(
      EVT_NOTIFY_WAIT, TPL_NOTIFY, TouchWaitForInput, Dev,
      &Dev->AbsPointer.WaitForInput);
  if (EFI_ERROR(Status)) {
    goto Fail;
  }

  Status = gBS->CreateEvent(
      EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_NOTIFY, TouchPollNotify, Dev,
      &Dev->PollEvent);
  if (EFI_ERROR(Status)) {
    goto FailWaitEvent;
  }

  Status = gBS->SetTimer(Dev->PollEvent, TimerPeriodic, TOUCH_POLL_PERIOD_100NS);
  if (EFI_ERROR(Status)) {
    goto FailPollEvent;
  }

  Status = gBS->InstallMultipleProtocolInterfaces(
      &Dev->Handle,
      &gEfiAbsolutePointerProtocolGuid, &Dev->AbsPointer,
      &gEfiDevicePathProtocolGuid, &Dev->DevicePath,
      NULL);
  if (EFI_ERROR(Status)) {
    goto FailPollEvent;
  }

  return EFI_SUCCESS;

FailPollEvent:
  gBS->CloseEvent(Dev->PollEvent);
FailWaitEvent:
  gBS->CloseEvent(Dev->AbsPointer.WaitForInput);
Fail:
  if (Dev->Objects != NULL) {
    FreePool(Dev->Objects);
  }
  FreePool(Dev);
  return Status;
}
