/*
 * Minimal bit-banged I2C master on top of the TLMM GPIO protocol.
 *
 * SCL is driven push-pull so the transfer never depends on the board
 * pull-ups or on the slave releasing clock stretching.  SDA is driven
 * push-pull for master-generated bits/start/stop, and released to an
 * input only while the slave is allowed to drive it (ACK and read data).
 */

#include <Library/LKEnvLib.h>
#include <Library/QcomGpioTlmmLib.h>
#include <Library/TimerLib.h>

#include <Library/GpioI2cLib.h>

#define I2C_STRETCH_TIMEOUT_US 1000

STATIC VOID
LineLow (
  IN UINT32  Gpio
  )
{
  gGpioTlmm->DirectionOutput(Gpio, 0);
}

STATIC VOID
LineHigh (
  IN UINT32  Gpio
  )
{
  gGpioTlmm->DirectionOutput(Gpio, 1);
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

STATIC VOID
SclHigh (
  IN GPIO_I2C_BUS  *Bus
  )
{
  LineHigh(Bus->SclGpio);
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
  /* idle -> START: SDA falls while SCL is high */
  LineHigh(Bus->SdaGpio);
  SclHigh(Bus);
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
  SclHigh(Bus);
  Delay(Bus);
  LineHigh(Bus->SdaGpio);
  Delay(Bus);
}

STATIC EFI_STATUS
I2cWriteByte (
  IN GPIO_I2C_BUS  *Bus,
  IN UINT8         Byte
  )
{
  INTN   Bit;
  UINTN  Nack;

  for (Bit = 7; Bit >= 0; Bit--) {
    if (Byte & (1 << Bit)) {
      LineHigh(Bus->SdaGpio);
    } else {
      LineLow(Bus->SdaGpio);
    }
    Delay(Bus);
    SclHigh(Bus);
    Delay(Bus);
    SclLow(Bus);
  }

  /* ACK bit: let the slave pull SDA low */
  LineRelease(Bus->SdaGpio);
  Delay(Bus);
  SclHigh(Bus);
  Delay(Bus);
  Nack = LineGet(Bus->SdaGpio);
  SclLow(Bus);
  LineHigh(Bus->SdaGpio);

  return Nack ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

STATIC EFI_STATUS
I2cReadByte (
  IN  GPIO_I2C_BUS  *Bus,
  OUT UINT8         *Byte,
  IN  BOOLEAN       Ack
  )
{
  UINTN Bit;
  UINT8 Value = 0;

  /* Release SDA so the slave can drive the eight data bits */
  LineRelease(Bus->SdaGpio);

  for (Bit = 0; Bit < 8; Bit++) {
    Delay(Bus);
    SclHigh(Bus);
    Delay(Bus);
    Value = (Value << 1) | (LineGet(Bus->SdaGpio) ? 1 : 0);
    SclLow(Bus);
  }

  /* Master drives ACK (low) or NACK (high) */
  if (Ack) {
    LineLow(Bus->SdaGpio);
  } else {
    LineHigh(Bus->SdaGpio);
  }
  Delay(Bus);
  SclHigh(Bus);
  Delay(Bus);
  SclLow(Bus);
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

  LineHigh(Bus->SdaGpio);
  SclHigh(Bus);
  Delay(Bus);

  /*
   * Bus recovery: if a slave is holding SDA low, clock it out while
   * SCL is actively driven (no pull-up required).
   */
  LineRelease(Bus->SdaGpio);
  for (Count = 0; Count < 9 && !LineGet(Bus->SdaGpio); Count++) {
    SclLow(Bus);
    Delay(Bus);
    SclHigh(Bus);
    Delay(Bus);
  }

  I2cStop(Bus);
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
  UINTN      Index;

  Status = I2cStart(Bus);
  if (EFI_ERROR(Status)) {
    goto Out;
  }

  if (WriteLen != 0) {
    Status = I2cWriteByte(Bus, (UINT8)(Address << 1));
    if (EFI_ERROR(Status)) {
      goto Out;
    }

    for (Index = 0; Index < WriteLen; Index++) {
      Status = I2cWriteByte(Bus, WriteBuf[Index]);
      if (EFI_ERROR(Status)) {
        goto Out;
      }
    }

    if (ReadLen != 0) {
      Status = I2cStart(Bus);
      if (EFI_ERROR(Status)) {
        goto Out;
      }
    }
  }

  if (ReadLen != 0) {
    Status = I2cWriteByte(Bus, (UINT8)((Address << 1) | 1));
    if (EFI_ERROR(Status)) {
      goto Out;
    }

    for (Index = 0; Index < ReadLen; Index++) {
      Status = I2cReadByte(Bus, &ReadBuf[Index], Index + 1 < ReadLen);
      if (EFI_ERROR(Status)) {
        goto Out;
      }
    }
  }

Out:
  I2cStop(Bus);
  return Status;
}
