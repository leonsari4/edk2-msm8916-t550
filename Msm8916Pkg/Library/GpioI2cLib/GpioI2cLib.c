/*
 * Minimal bit-banged I2C master on top of the TLMM GPIO protocol.
 *
 * Both lines are used as open-drain outputs: driving low is an output,
 * releasing high is an input with the pull-up enabled by GpioI2cInit().
 * That lets a slave stretch SCL, which the maXTouch does during longer
 * register reads.
 *
 * The repeated START is the fragile part on a bit-banged bus: SDA must
 * actually be high again before SCL is raised, otherwise the slave sees a
 * glitch instead of a START and stops answering.  Every release therefore
 * waits for the line to read high (with a timeout) instead of trusting the
 * pull-up rise time.
 */

#include <Library/LKEnvLib.h>
#include <Library/QcomGpioTlmmLib.h>
#include <Library/TimerLib.h>
#include <Library/DebugLib.h>

#include <Library/GpioI2cLib.h>

#define I2C_STRETCH_TIMEOUT_US 5000
#define I2C_RISE_TIMEOUT_US    1000

STATIC VOID
LineLow (
  IN UINT32  Gpio
  )
{
  gGpioTlmm->DirectionOutput(Gpio, 0);
}

STATIC VOID
LineRelease (
  IN UINT32  Gpio
  )
{
  gGpioTlmm->DirectionInput(Gpio);
}

STATIC UINTN
LineGet (
  IN UINT32  Gpio
  )
{
  return gGpioTlmm->Get(Gpio);
}

STATIC VOID
Delay (
  IN GPIO_I2C_BUS  *Bus
  )
{
  MicroSecondDelay(Bus->HalfPeriodUs);
}

/*
 * Wait for a released line to be pulled high.  A slave that holds SCL low
 * is clock stretching; a slave that holds SDA low at a START boundary is
 * still finishing the previous ACK.  Both are waited out, but a stuck bus
 * is reported instead of being ignored.
 */
STATIC EFI_STATUS
LineWaitHigh (
  IN UINT32  Gpio,
  IN UINTN   TimeoutUs
  )
{
  UINTN Timeout = TimeoutUs;

  while (!LineGet(Gpio)) {
    if (Timeout-- == 0) {
      return EFI_DEVICE_ERROR;
    }
    MicroSecondDelay(1);
  }

  return EFI_SUCCESS;
}

STATIC EFI_STATUS
SclHigh (
  IN GPIO_I2C_BUS  *Bus
  )
{
  EFI_STATUS Status;

  LineRelease(Bus->SclGpio);
  Status = LineWaitHigh(Bus->SclGpio, I2C_STRETCH_TIMEOUT_US);
  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR, "i2c: SCL gpio %u stuck low\n", Bus->SclGpio));
  }
  return Status;
}

STATIC VOID
SclLow (
  IN GPIO_I2C_BUS  *Bus
  )
{
  LineLow(Bus->SclGpio);
}

STATIC EFI_STATUS
I2cStart (
  IN GPIO_I2C_BUS  *Bus
  )
{
  EFI_STATUS Status;

  /* idle -> START: SDA falls while SCL is high */
  LineRelease(Bus->SdaGpio);
  Status = LineWaitHigh(Bus->SdaGpio, I2C_RISE_TIMEOUT_US);
  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR, "i2c: SDA gpio %u stuck low before START\n", Bus->SdaGpio));
    return Status;
  }

  Status = SclHigh(Bus);
  if (EFI_ERROR(Status)) {
    return Status;
  }
  Delay(Bus);

  LineLow(Bus->SdaGpio);
  Delay(Bus);
  SclLow(Bus);
  Delay(Bus);

  return EFI_SUCCESS;
}

STATIC VOID
I2cStop (
  IN GPIO_I2C_BUS  *Bus
  )
{
  /* STOP: SDA rises while SCL is high */
  LineLow(Bus->SdaGpio);
  Delay(Bus);
  (VOID)SclHigh(Bus);
  Delay(Bus);
  LineRelease(Bus->SdaGpio);
  (VOID)LineWaitHigh(Bus->SdaGpio, I2C_RISE_TIMEOUT_US);
  Delay(Bus);
  Delay(Bus);
}

STATIC EFI_STATUS
I2cWriteByte (
  IN  GPIO_I2C_BUS  *Bus,
  IN  UINT8         Byte,
  IN  UINTN         Index
  )
{
  EFI_STATUS Status;
  INTN       Bit;
  UINTN      Nack;

  for (Bit = 7; Bit >= 0; Bit--) {
    if (Byte & (1 << Bit)) {
      LineRelease(Bus->SdaGpio);
    } else {
      LineLow(Bus->SdaGpio);
    }
    Delay(Bus);

    Status = SclHigh(Bus);
    if (EFI_ERROR(Status)) {
      DEBUG((EFI_D_ERROR, "i2c: write byte %u bit %d: SCL stuck\n",
             (UINT32)Index, Bit));
      return Status;
    }
    Delay(Bus);

    SclLow(Bus);
    Delay(Bus);
  }

  /* ACK bit: let the slave pull SDA low */
  LineRelease(Bus->SdaGpio);
  Delay(Bus);

  Status = SclHigh(Bus);
  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR, "i2c: write byte %u ACK: SCL stuck\n", (UINT32)Index));
    return Status;
  }
  Delay(Bus);

  Nack = LineGet(Bus->SdaGpio);
  SclLow(Bus);
  Delay(Bus);
  LineRelease(Bus->SdaGpio);

  if (Nack) {
    DEBUG((EFI_D_ERROR, "i2c: NACK after write byte %u (0x%02x)\n",
           (UINT32)Index, Byte));
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

STATIC EFI_STATUS
I2cReadByte (
  IN  GPIO_I2C_BUS  *Bus,
  OUT UINT8         *Byte,
  IN  BOOLEAN       Ack,
  IN  UINTN         Index
  )
{
  EFI_STATUS Status;
  UINTN      Bit;
  UINT8      Value = 0;

  /* Release SDA so the slave can drive the eight data bits */
  LineRelease(Bus->SdaGpio);

  for (Bit = 0; Bit < 8; Bit++) {
    Delay(Bus);

    Status = SclHigh(Bus);
    if (EFI_ERROR(Status)) {
      DEBUG((EFI_D_ERROR, "i2c: read byte %u bit %u: SCL stuck\n",
             (UINT32)Index, (UINT32)Bit));
      return Status;
    }
    Delay(Bus);

    Value = (UINT8)((Value << 1) | (LineGet(Bus->SdaGpio) ? 1 : 0));
    SclLow(Bus);
    Delay(Bus);
  }

  /* Master drives ACK (low) or NACK (released high) */
  if (Ack) {
    LineLow(Bus->SdaGpio);
  } else {
    LineRelease(Bus->SdaGpio);
  }
  Delay(Bus);

  Status = SclHigh(Bus);
  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR, "i2c: read byte %u ACK: SCL stuck\n", (UINT32)Index));
    return Status;
  }
  Delay(Bus);

  SclLow(Bus);
  Delay(Bus);
  LineRelease(Bus->SdaGpio);

  *Byte = Value;
  return EFI_SUCCESS;
}

VOID
GpioI2cInit (
  IN GPIO_I2C_BUS  *Bus
  )
{
  UINTN Count;

  gGpioTlmm->SetFunction(Bus->SdaGpio, 0);
  gGpioTlmm->SetFunction(Bus->SclGpio, 0);
  gGpioTlmm->SetDriveStrength(Bus->SdaGpio, 2);
  gGpioTlmm->SetDriveStrength(Bus->SclGpio, 2);
  gGpioTlmm->SetPull(Bus->SdaGpio, GPIO_PULL_UP);
  gGpioTlmm->SetPull(Bus->SclGpio, GPIO_PULL_UP);

  LineRelease(Bus->SdaGpio);
  LineRelease(Bus->SclGpio);
  Delay(Bus);

  /*
   * Bus recovery: if a slave is holding SDA low, clock it out while
   * SCL is released (the pull-up makes the high level).
   */
  for (Count = 0; Count < 9 && !LineGet(Bus->SdaGpio); Count++) {
    SclLow(Bus);
    Delay(Bus);
    if (EFI_ERROR(SclHigh(Bus))) {
      break;
    }
    Delay(Bus);
  }

  I2cStop(Bus);
}

STATIC EFI_STATUS
GpioI2cTransferOnce (
  IN  GPIO_I2C_BUS  *Bus,
  IN  UINT8         Address,
  IN  CONST UINT8   *WriteBuf,
  IN  UINTN         WriteLen,
  OUT UINT8         *ReadBuf,
  IN  UINTN         ReadLen
  )
{
  EFI_STATUS Status;
  UINTN      Index;

  Status = I2cStart(Bus);
  if (EFI_ERROR(Status)) {
    goto Out;
  }

  if (WriteLen != 0) {
    Status = I2cWriteByte(Bus, (UINT8)(Address << 1), 0);
    if (EFI_ERROR(Status)) {
      goto Out;
    }

    for (Index = 0; Index < WriteLen; Index++) {
      Status = I2cWriteByte(Bus, WriteBuf[Index], Index + 1);
      if (EFI_ERROR(Status)) {
        goto Out;
      }
    }

    if (ReadLen != 0) {
      /* Repeated START: SDA must be high before SCL rises again */
      Status = I2cStart(Bus);
      if (EFI_ERROR(Status)) {
        goto Out;
      }
    }
  }

  if (ReadLen != 0) {
    Status = I2cWriteByte(Bus, (UINT8)((Address << 1) | 1), 0);
    if (EFI_ERROR(Status)) {
      goto Out;
    }

    for (Index = 0; Index < ReadLen; Index++) {
      Status = I2cReadByte(Bus, &ReadBuf[Index], Index + 1 < ReadLen, Index);
      if (EFI_ERROR(Status)) {
        goto Out;
      }
    }
  }

Out:
  I2cStop(Bus);
  return Status;
}

EFI_STATUS
GpioI2cTransfer (
  IN  GPIO_I2C_BUS  *Bus,
  IN  UINT8         Address,
  IN  CONST UINT8   *WriteBuf,
  IN  UINTN         WriteLen,
  OUT UINT8         *ReadBuf,
  IN  UINTN         ReadLen
  )
{
  EFI_STATUS Status;

  Status = GpioI2cTransferOnce(Bus, Address, WriteBuf, WriteLen, ReadBuf, ReadLen);

  /*
   * Some PMIC-style slaves do not decode a repeated START and only answer
   * a register read if the write phase is terminated by a STOP.  Retry
   * those transfers as two independent bus cycles before giving up.
   */
  if (EFI_ERROR(Status) && WriteLen != 0 && ReadLen != 0) {
    DEBUG((EFI_D_ERROR, "i2c: combined write/read failed, retrying with STOP\n"));
    Status = GpioI2cTransferOnce(Bus, Address, WriteBuf, WriteLen, NULL, 0);
    if (!EFI_ERROR(Status)) {
      MicroSecondDelay(100);
      Status = GpioI2cTransferOnce(Bus, Address, NULL, 0, ReadBuf, ReadLen);
    }
  }

  return Status;
}
