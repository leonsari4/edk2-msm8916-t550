/*
 * Bring up the MSM8916 ChipIdea USB 2.0 controller (usb@78d9000) in host
 * mode and leave it for the OS. Windows only has the generic EHCI driver
 * (ACPI PNP0D20, see USB0 in the DSDT), which knows nothing about the
 * Qualcomm clocks, resets or the ULPI PHY, so all of that happens here.
 *
 * Sequence follows Linux drivers/usb/chipidea/ci_hdrc_msm.c and
 * drivers/phy/qualcomm/phy-qcom-usb-hs.c. The PHY tuning values are the
 * msm8916 "qcom,init-seq" from msm8916.dtsi.
 *
 * VBUS: on the T550 the 5V for OTG devices comes from the boost converter in
 * the MAX77849 PMIC (charger I2C address 0x66, MUIC at 0x25, both on the
 * bit-banged I2C bus GPIO 2/3).  EnableOtgVbus() follows Samsung's kernel
 * driver drivers/battery/max77849_charger.c (POWER_SUPPLY_PROP_ONLINE) and
 * drivers/extcon/extcon-max77849.c (PATH_USB_AP).  The boost is only turned
 * on when no external VBUS (PC, charger) is already present.
 *
 * The controller itself is handed to EDK2's standard EhciDxe through
 * NonDiscoverablePciDeviceDxe, which is what makes USB keyboards work in
 * the UEFI Shell.
 */

#include <PiDxe.h>
#include <Library/LKEnvLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/QcomGpioTlmmLib.h>
#include <Library/GpioI2cLib.h>
#include <Library/QcomClockLib.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>
#include <Protocol/Usb2HostController.h>

#define GPIO_NONE                  0xFFFFFFFF

/* PM8916 LDOs feeding the USB HS PHY: SPMI SID 1, peripherals 0x46 / 0x4C */
#define PM8916_LDO7                0x14600 /* 1.8V */
#define PM8916_LDO13               0x14C00 /* 3.075V */
#define LDO_STATUS1                0x08
#define   LDO_STATUS1_READY        0x80
#define LDO_VSET                   0x41
#define LDO_EN_CTL                 0x46
#define   LDO_EN_CTL_ENABLE        0x80

/*
 * Minimal SPMI PMIC arbiter v2 access with timeouts (Linux spmi-pmic-arb.c).
 * The tree's SpmiDxe polls without a timeout and never completes here.
 */
#define SPMI_CORE                  0x0200F000
#define SPMI_CORE_SIZE             0x1000
#define SPMI_CHNLS                 0x02400000
#define SPMI_OBSRVR                0x02C00000
#define SPMI_CNFG                  0x0200A000
#define SPMI_EE                    0 /* apps */
#define SPMI_APID_MAP(n)           (SPMI_CORE + 0x800 + 4 * (n))
#define SPMI_APID_OWNER(n)         (SPMI_CNFG + 0x700 + 4 * (n))
#define SPMI_CMD                   0x00
#define SPMI_STATUS                0x08
#define SPMI_WDATA0                0x10
#define SPMI_RDATA0                0x18
#define SPMI_OP_EXT_WRITEL         0
#define SPMI_OP_EXT_READL          1
#define SPMI_NO_APID               0xFFFF

/*
 * MAX77849 charger (I2C address 0x66).  Register map and sequence from the
 * Samsung kernel: drivers/battery/max77849_charger.c,
 * include/linux/mfd/max77849-private.h.
 */
#define MAX77849_CHG_DTLS_00       0xB3
#define   MAX77849_CHGIN_DTLS_MASK 0x60
#define   MAX77849_CHGIN_DTLS_VALID 0x03 /* UVLO < CHGIN < OVLO */
#define MAX77849_CHG_CNFG_00       0xB7
#define   MAX77849_CNFG00_MASK     0x2F /* CHG, OTG, BUCK, BOOST, DIS_MUIC_CTRL */
#define   MAX77849_CNFG00_OTG_ON   0x2A /* OTG | BOOST | DIS_MUIC_CTRL */
#define MAX77849_CHG_CNFG_02       0xB9
#define   MAX77849_OTG_ILIM_MASK   0x80 /* 1 = 1.2A, 0 = 500mA */
#define MAX77849_CHG_CNFG_06       0xBD
#define   MAX77849_UNLOCK_CHARGER  0x0C
#define MAX77849_CHG_CNFG_11       0xC2
#define   MAX77849_OTG_5V          0x50

/* MAX77849 MUIC (I2C address 0x25), see extcon-max77849.c */
#define MAX77849_MUIC_ID           0x00
#define MAX77849_MUIC_STATUS1      0x04
#define MAX77849_MUIC_ADC          0x07
#define   MAX77849_ADC_GND         0x00 /* OTG adapter: ID pin grounded */
#define MAX77849_MUIC_CTRL1        0x0C
#define   MAX77849_CTRL1_MASK      0x3F /* COMN1SW | COMP2SW */
#define   MAX77849_CTRL1_USB_AP    0x09 /* both switches = BIN_1 (USB AP) */
#define MAX77849_MUIC_CTRL2        0x0D
#define   MAX77849_CTRL2_MASK      0x05 /* CPEn | LOWPWD */
#define   MAX77849_CTRL2_ON        0x04 /* CP enabled, not low power */

#define GCC_USB_HS_BCR             0x01841000
#define GCC_USB2A_PHY_BCR          0x01841028
#define GCC_USB2A_PHY_SLEEP_CBCR   0x0184102C

/* Step logging at ERROR level so a hang shows the last step on screen */
#define STEP(s) DEBUG((EFI_D_ERROR, "UsbHost: " s "\n"))

#define USB_BASE                   0x078D9000
/*
 * The EHCI register file starts at the CAPLENGTH register, not at the
 * ChipIdea vendor block.  EhciDxe maps BAR0 and expects CAPLENGTH at
 * offset 0, so BAR0 has to point here.
 */
#define USB_EHCI_BASE              (USB_BASE + 0x100)
#define USB_EHCI_SIZE              0x0E00
#define USB_ID                     (USB_BASE + 0x000)
#define USB_SBUSCFG                (USB_BASE + 0x090)
#define USB_AHB_MODE               (USB_BASE + 0x098)
#define USB_GENCONFIG              (USB_BASE + 0x09C)
#define   GENCONFIG_TXFIFO_IDLE_FORCE_DIS  BIT4
#define USB_GENCONFIG_2            (USB_BASE + 0x0A0)
#define   GENCONFIG_2_ULPI_TX_PKT_EN_CLR_FIX BIT19
#define   GENCONFIG_2_SESS_VLD_CTRL_EN       BIT7
#define USB_CAP                    (USB_BASE + 0x100)
#define USB_HCCPARAMS              (USB_CAP + 0x08)
#define   HCCPARAMS_LEN            BIT17
#define USB_HS_PHY_CTRL            (USB_BASE + 0x240)
#define   HS_PHY_POR_ASSERT        BIT0
/* From Qualcomm's downstream phy-msm-usb.c (28nm integrated PHY) */
#define   HS_PHY_RETEN             BIT1  /* active low: 0 = PHY in retention */
#define   HS_PHY_CLAMP_DPDMSE_EN   BIT21 /* clamps D+/D- while suspended */
#define USB_HS_PHY_CTRL2           (USB_BASE + 0x278)

/* Operational registers, relative to CAP + CAPLENGTH */
#define OP_USBCMD                  0x00
#define   USBCMD_RS                BIT0
#define   USBCMD_RST               BIT1
#define   USBCMD_SESS_VLD_CTRL     BIT25 /* software session valid (with GENCONFIG_2 bit 7) */
#define OP_ULPI_VIEWPORT           0x30
#define   ULPI_WAKEUP              BIT31
#define   ULPI_RUN                 BIT30
#define   ULPI_WRITE               BIT29
#define OP_PORTSC                  0x44
#define   PORTSC_PP                BIT12
#define   PORTSC_PTS_MASK          (0x3U << 30 | BIT25)
#define   PORTSC_PTS_ULPI          (0x2U << 30)
#define OP_DEVLC                   0x84
#define   DEVLC_PTS_MASK           (0x7U << 29)
#define   DEVLC_PTS_ULPI           (0x2U << 29)
#define OP_USBMODE_NOLPM           0x68
#define OP_USBMODE_LPM             0xC8
#define   USBMODE_CM_MASK          0x3
#define   USBMODE_CM_HOST          0x3

/* ULPI registers */
#define ULPI_FUNC_CTRL             0x04
#define ULPI_FUNC_CTRL_SET         0x05
#define ULPI_FUNC_CTRL_CLR         0x06
#define   ULPI_FUNC_TERMSELECT     BIT2
#define   ULPI_FUNC_XCVR_FS        0x01
#define   ULPI_FUNC_SUSPENDM       BIT6
#define ULPI_IFC_CTRL              0x07
#define ULPI_OTG_CTRL              0x0A
#define ULPI_USB_INT_STS           0x13
#define ULPI_DEBUG                 0x15 /* bits 1:0 = LineState (D+/D-) */
#define ULPI_OTG_CTRL_SET          0x0B
#define   ULPI_OTG_DP_PULLDOWN     BIT1
#define   ULPI_OTG_DM_PULLDOWN     BIT2
#define ULPI_EXT_VENDOR_SPECIFIC   0x80
/* Qualcomm vendor ULPI registers (downstream msm_hsusb_hw.h / phy-msm-usb.c) */
#define ULPI_CHG_DET_CTRL          0x84 /* BC1.2 charger detection, SET +1, CLR +2 */
#define ULPI_CHG_DET_CLR           0x86
#define   ULPI_CHG_DET_ALL         0x3F
#define ULPI_MISC_A                0x96
#define ULPI_MISC_A_SET            0x97
#define   ULPI_MISC_A_VBUSVLDEXT   BIT0
#define   ULPI_MISC_A_VBUSVLDEXTSEL BIT1

STATIC UINTN mOp;
STATIC BOOLEAN mLpm;
STATIC BOOLEAN mExternalVbus;

STATIC BOOLEAN
WaitClear (
  IN UINTN   Reg,
  IN UINT32  Mask,
  IN UINTN   TimeoutUs
  )
{
  while (MmioRead32(Reg) & Mask) {
    if (TimeoutUs-- == 0) {
      return FALSE;
    }
    MicroSecondDelay(1);
  }
  return TRUE;
}

STATIC EFI_STATUS
UlpiWrite (
  IN UINT8  Reg,
  IN UINT8  Val
  )
{
  UINTN Viewport = mOp + OP_ULPI_VIEWPORT;

  /* Make sure the PHY is awake before talking to it */
  MmioWrite32(Viewport, ULPI_WAKEUP);
  if (!WaitClear(Viewport, ULPI_WAKEUP, 10000)) {
    return EFI_TIMEOUT;
  }

  MmioWrite32(Viewport, ULPI_RUN | ULPI_WRITE | ((UINT32)Reg << 16) | Val);
  if (!WaitClear(Viewport, ULPI_RUN, 10000)) {
    return EFI_TIMEOUT;
  }

  return EFI_SUCCESS;
}

STATIC UINT8
UlpiRead (
  IN UINT8  Reg
  )
{
  UINTN Viewport = mOp + OP_ULPI_VIEWPORT;

  MmioWrite32(Viewport, ULPI_WAKEUP);
  if (!WaitClear(Viewport, ULPI_WAKEUP, 10000)) {
    return 0xFF;
  }

  MmioWrite32(Viewport, ULPI_RUN | ((UINT32)Reg << 16));
  if (!WaitClear(Viewport, ULPI_RUN, 10000)) {
    return 0xFF;
  }

  return (UINT8)(MmioRead32(Viewport) >> 8);
}

STATIC UINT16
SpmiFindApid (
  IN UINT32  Addr
  )
{
  UINT16 Ppid = (UINT16)(Addr >> 8) & 0xFFF; /* SID << 8 | peripheral */
  UINT16 Apid;
  UINT32 Val;

  for (Apid = 0; 0x800 + 4 * Apid < SPMI_CORE_SIZE; Apid++) {
    Val = MmioRead32(SPMI_APID_MAP(Apid));
    if (Val != 0 && ((Val >> 8) & 0xFFF) == Ppid) {
      return Apid;
    }
  }

  return SPMI_NO_APID;
}

STATIC EFI_STATUS
SpmiWaitDone (
  IN UINTN  Chan
  )
{
  UINT32 Status;
  UINTN  Timeout;

  for (Timeout = 1000; Timeout > 0; Timeout--) {
    Status = MmioRead32(Chan + SPMI_STATUS);
    if (Status & BIT0) {
      if (Status & BIT2) {
        return EFI_ACCESS_DENIED;
      }
      return (Status & (BIT1 | BIT3)) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
    }
    MicroSecondDelay(1);
  }

  return EFI_TIMEOUT;
}

STATIC EFI_STATUS
SpmiRead (
  IN  UINT32  Addr,
  OUT UINT8   *Val
  )
{
  UINT16     Apid = SpmiFindApid(Addr);
  UINTN      Chan;
  EFI_STATUS Status;

  if (Apid == SPMI_NO_APID) {
    return EFI_NOT_FOUND;
  }

  Chan = SPMI_OBSRVR + 0x1000 * SPMI_EE + 0x8000 * Apid;
  MmioWrite32(Chan + SPMI_CMD, (SPMI_OP_EXT_READL << 27) | ((Addr & 0xFF) << 4));
  Status = SpmiWaitDone(Chan);
  if (!EFI_ERROR(Status)) {
    *Val = (UINT8)MmioRead32(Chan + SPMI_RDATA0);
  }
  return Status;
}

STATIC EFI_STATUS
SpmiWrite (
  IN UINT32  Addr,
  IN UINT8   Val
  )
{
  UINT16 Apid = SpmiFindApid(Addr);
  UINTN  Chan;

  if (Apid == SPMI_NO_APID) {
    return EFI_NOT_FOUND;
  }

  Chan = SPMI_CHNLS + 0x1000 * SPMI_EE + 0x8000 * Apid;
  MmioWrite32(Chan + SPMI_WDATA0, Val);
  MmioWrite32(Chan + SPMI_CMD, (SPMI_OP_EXT_WRITEL << 27) | ((Addr & 0xFF) << 4));
  return SpmiWaitDone(Chan);
}

/*
 * The PHY's analog side (VBUS comparators, D+/D- line receivers) runs from
 * LDO13. If it is off ULPI still works, but the PHY sees no VBUS and SE0.
 * Only switch it on ourselves if the apps EE owns the regulator; otherwise
 * it belongs to the RPM and must be voted for through it.
 */
STATIC VOID
EnsurePhySupply (
  IN CONST CHAR8  *Name,
  IN UINT32       Ldo
  )
{
  UINT16     Apid = SpmiFindApid(Ldo);
  UINT8      Owner;
  UINT8      St   = 0;
  UINT8      En   = 0;
  UINT8      Vset = 0;
  EFI_STATUS Status;

  if (Apid == SPMI_NO_APID) {
    DEBUG((EFI_D_ERROR, "UsbHost: %a not in SPMI channel map\n", Name));
    return;
  }

  Owner  = (UINT8)(MmioRead32(SPMI_APID_OWNER(Apid)) & 0x7);
  Status = SpmiRead(Ldo + LDO_STATUS1, &St);
  if (!EFI_ERROR(Status)) {
    Status = SpmiRead(Ldo + LDO_EN_CTL, &En);
  }
  if (!EFI_ERROR(Status)) {
    Status = SpmiRead(Ldo + LDO_VSET, &Vset);
  }
  DEBUG((EFI_D_ERROR, "UsbHost: %a apid %u owner EE%u: status %02x en_ctl %02x vset %02x (%r)\n",
         Name, Apid, Owner, St, En, Vset, Status));
  if (EFI_ERROR(Status) || (St & LDO_STATUS1_READY)) {
    return;
  }

  if (Owner != SPMI_EE) {
    DEBUG((EFI_D_ERROR, "UsbHost: %a is off and owned by EE%u (RPM), cannot switch it on directly\n",
          Name, Owner));
    return;
  }

  Status = SpmiWrite(Ldo + LDO_EN_CTL, En | LDO_EN_CTL_ENABLE);
  MicroSecondDelay(1000);
  SpmiRead(Ldo + LDO_STATUS1, &St);
  DEBUG((EFI_D_ERROR, "UsbHost: %a enable: %r, status now %02x\n", Name, Status, St));
}

STATIC VOID
BlockReset (
  IN UINTN  Bcr
  )
{
  MmioWrite32(Bcr, 1);
  MicroSecondDelay(1000);
  MmioWrite32(Bcr, 0);
  MicroSecondDelay(1000);
}

STATIC EFI_STATUS
I2cReadReg (
  IN  GPIO_I2C_BUS  *Bus,
  IN  UINT8         Addr,
  IN  UINT8         Reg,
  OUT UINT8         *Val
  )
{
  return GpioI2cTransfer(Bus, Addr, &Reg, 1, Val, 1);
}

STATIC EFI_STATUS
I2cUpdateReg (
  IN GPIO_I2C_BUS  *Bus,
  IN UINT8         Addr,
  IN UINT8         Reg,
  IN UINT8         Mask,
  IN UINT8         Val
  )
{
  EFI_STATUS Status;
  UINT8      Buf[2];

  Status = I2cReadReg(Bus, Addr, Reg, &Buf[1]);
  if (EFI_ERROR(Status)) {
    return Status;
  }

  Buf[0] = Reg;
  Buf[1] = (Buf[1] & ~Mask) | (Val & Mask);
  return GpioI2cTransfer(Bus, Addr, Buf, 2, NULL, 0);
}

STATIC EFI_STATUS
I2cWriteReg (
  IN GPIO_I2C_BUS  *Bus,
  IN UINT8         Addr,
  IN UINT8         Reg,
  IN UINT8         Val
  )
{
  UINT8 Buf[2];

  Buf[0] = Reg;
  Buf[1] = Val;
  return GpioI2cTransfer(Bus, Addr, Buf, 2, NULL, 0);
}

/*
 * In auto mode the MUIC does not connect the OTG adapter's D+/D- to the SoC.
 * Dump the interesting registers before and after switching the manual path.
 */
STATIC VOID
DumpMuic (
  IN GPIO_I2C_BUS  *Bus,
  IN UINT8         Addr,
  IN CONST CHAR8   *When
  )
{
  STATIC CONST UINT8 Regs[] = {
    MAX77849_MUIC_ID, MAX77849_MUIC_STATUS1, MAX77849_MUIC_ADC,
    MAX77849_MUIC_CTRL1, MAX77849_MUIC_CTRL2
  };
  UINT8 Val[ARRAY_SIZE(Regs)];
  UINTN Index;

  for (Index = 0; Index < ARRAY_SIZE(Regs); Index++) {
    if (EFI_ERROR(I2cReadReg(Bus, Addr, Regs[Index], &Val[Index]))) {
      Val[Index] = 0xEE;
    }
  }

  DEBUG((EFI_D_ERROR, "UsbHost: MUIC %a: id %02x status %02x adc %02x ctrl1 %02x ctrl2 %02x\n",
         When, Val[0], Val[1], Val[2], Val[3], Val[4]));
}

/*
 * Samsung's MUIC starts in auto mode, which leaves the OTG adapter's D+/D-
 * disconnected from the SoC.  The kernel selects the manual "USB to AP" path:
 * CTRL1 = BIN_1 on both switches, CTRL2 = charge pump on / low power off
 * (extcon-max77849.c max77849_muic_set_path(PATH_USB_AP)).
 */
STATIC VOID
RouteMuicToUsb (
  VOID
  )
{
  GPIO_I2C_BUS Bus;
  UINT8        Addr = FixedPcdGet8(PcdMuicI2cAddress);
  EFI_STATUS   Status;

  if (FixedPcdGet32(PcdMuicI2cSdaGpio) == GPIO_NONE) {
    return;
  }

  Bus.SdaGpio      = FixedPcdGet32(PcdMuicI2cSdaGpio);
  Bus.SclGpio      = FixedPcdGet32(PcdMuicI2cSclGpio);
  Bus.HalfPeriodUs = 10;
  GpioI2cInit(&Bus);

  DumpMuic(&Bus, Addr, "before");

  Status = I2cUpdateReg(&Bus, Addr, MAX77849_MUIC_CTRL1,
                        MAX77849_CTRL1_MASK, MAX77849_CTRL1_USB_AP);
  if (!EFI_ERROR(Status)) {
    Status = I2cUpdateReg(&Bus, Addr, MAX77849_MUIC_CTRL2,
                          MAX77849_CTRL2_MASK, MAX77849_CTRL2_ON);
  }
  MicroSecondDelay(10000);

  DumpMuic(&Bus, Addr, "after ");
  DEBUG((EFI_D_ERROR, "UsbHost: MUIC routed USB lines to SoC: %r\n", Status));
}

STATIC VOID
EnableOtgVbus (
  VOID
  )
{
  GPIO_I2C_BUS Bus;
  UINT8        ChgDtls;
  EFI_STATUS   Status;

  if (FixedPcdGet32(PcdOtgBoostI2cSdaGpio) == GPIO_NONE) {
    return;
  }

  Bus.SdaGpio      = FixedPcdGet32(PcdOtgBoostI2cSdaGpio);
  Bus.SclGpio      = FixedPcdGet32(PcdOtgBoostI2cSclGpio);
  Bus.HalfPeriodUs = 10;

  DEBUG((EFI_D_ERROR, "UsbHost: I2C pins before SetFunction SDA=%u SCL=%u\n",
         (UINT32)gGpioTlmm->Get(Bus.SdaGpio), (UINT32)gGpioTlmm->Get(Bus.SclGpio)));

  /* Switch pins to GPIO mode (function 0) before bit-banging */
  gGpioTlmm->SetFunction(Bus.SdaGpio, 0);
  gGpioTlmm->SetFunction(Bus.SclGpio, 0);
  DEBUG((EFI_D_ERROR, "UsbHost: I2C pins after SetFunction SDA=%u SCL=%u\n",
         (UINT32)gGpioTlmm->Get(Bus.SdaGpio), (UINT32)gGpioTlmm->Get(Bus.SclGpio)));

  gGpioTlmm->SetDriveStrength(Bus.SdaGpio, 2);
  gGpioTlmm->SetDriveStrength(Bus.SclGpio, 2);
  gGpioTlmm->SetPull(Bus.SdaGpio, GPIO_PULL_NONE);
  gGpioTlmm->SetPull(Bus.SclGpio, GPIO_PULL_NONE);
  gGpioTlmm->DirectionInput(Bus.SdaGpio);
  gGpioTlmm->DirectionInput(Bus.SclGpio);
  MicroSecondDelay(100);
  DEBUG((EFI_D_ERROR, "UsbHost: I2C pins SDA=%u SCL=%u (GPIO mode)\n",
         (UINT32)gGpioTlmm->Get(Bus.SdaGpio), (UINT32)gGpioTlmm->Get(Bus.SclGpio)));

  GpioI2cInit(&Bus);
  MicroSecondDelay(100);
  DEBUG((EFI_D_ERROR, "UsbHost: I2C pins after init SDA=%u SCL=%u\n",
         (UINT32)gGpioTlmm->Get(Bus.SdaGpio), (UINT32)gGpioTlmm->Get(Bus.SclGpio)));

  Status = I2cReadReg(&Bus, FixedPcdGet8(PcdOtgBoostI2cAddress),
                      MAX77849_CHG_DTLS_00, &ChgDtls);
  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR, "UsbHost: MAX77849 charger not responding: %r\n", Status));
  } else if (((ChgDtls & MAX77849_CHGIN_DTLS_MASK) >> 5) == MAX77849_CHGIN_DTLS_VALID) {
    /* Never fight a PC or charger that is already driving VBUS */
    DEBUG((EFI_D_ERROR, "UsbHost: external VBUS present, OTG boost stays off\n"));
    mExternalVbus = TRUE;
  } else {
    /* Unlock charger registers, allow 1.2A, select OTG boost, set 5.0V */
    Status = I2cWriteReg(&Bus, FixedPcdGet8(PcdOtgBoostI2cAddress),
                         MAX77849_CHG_CNFG_06, MAX77849_UNLOCK_CHARGER);
    if (!EFI_ERROR(Status)) {
      Status = I2cUpdateReg(&Bus, FixedPcdGet8(PcdOtgBoostI2cAddress),
                            MAX77849_CHG_CNFG_02, MAX77849_OTG_ILIM_MASK,
                            MAX77849_OTG_ILIM_MASK);
    }
    if (!EFI_ERROR(Status)) {
      Status = I2cUpdateReg(&Bus, FixedPcdGet8(PcdOtgBoostI2cAddress),
                            MAX77849_CHG_CNFG_00, MAX77849_CNFG00_MASK,
                            MAX77849_CNFG00_OTG_ON);
    }
    if (!EFI_ERROR(Status)) {
      Status = I2cWriteReg(&Bus, FixedPcdGet8(PcdOtgBoostI2cAddress),
                           MAX77849_CHG_CNFG_11, MAX77849_OTG_5V);
    }
    if (EFI_ERROR(Status)) {
      DEBUG((EFI_D_ERROR, "UsbHost: enabling OTG boost failed: %r\n", Status));
      return;
    }

    /* Give the boost time to come up and attached devices time to power on */
    MicroSecondDelay(100000);
    DEBUG((EFI_D_ERROR, "UsbHost: OTG boost on (5.0V, 1.2A limit)\n"));
  }

  RouteMuicToUsb();
}

STATIC VOID
DumpPort (
  IN CONST CHAR8  *When,
  IN BOOLEAN      Lpm
  )
{
  UINT32 PortSc = MmioRead32(mOp + OP_PORTSC);
  UINT32 Mode   = MmioRead32(mOp + (Lpm ? OP_USBMODE_LPM : OP_USBMODE_NOLPM));
  UINT32 OtgSc  = MmioRead32(mOp + (Lpm ? 0xC4 : 0x64));

  UINT8  IntSts = UlpiRead(ULPI_USB_INT_STS);

  DEBUG((EFI_D_ERROR, "UsbTest [%a]\n", When));
  DEBUG((EFI_D_ERROR, "  CMD %08x STS %08x CM %u | PORTSC %08x connect %u en %u pwr %u PTS %u\n",
         MmioRead32(mOp + OP_USBCMD), MmioRead32(mOp + 0x04), Mode & 3,
         PortSc, PortSc & 1, (PortSc >> 2) & 1, (PortSc >> 12) & 1, PortSc >> 30));
  DEBUG((EFI_D_ERROR, "  OTGSC %08x GENCFG2 %08x | ULPI func %02x ifc %02x otg %02x misc_a %02x\n",
         OtgSc, MmioRead32(USB_GENCONFIG_2), UlpiRead(ULPI_FUNC_CTRL), UlpiRead(ULPI_IFC_CTRL),
         UlpiRead(ULPI_OTG_CTRL), UlpiRead(ULPI_MISC_A)));
  DEBUG((EFI_D_ERROR, "  PHY sees: VBUS valid %u, session valid %u, ID grounded %u, LineState %u (0=SE0 none, 1/2=device)\n",
         (IntSts >> 1) & 1, (IntSts >> 2) & 1, (IntSts >> 4) & 1, UlpiRead(ULPI_DEBUG) & 3));
}

STATIC EFI_STATUS
ResetAndConfigure (
  IN BOOLEAN  Lpm
  );

/*
 * Diagnostic build only (PcdUsbHostSelfTest): show whether the PHY and the
 * port see the plugged in device with the controller halted and running,
 * then do the HCRESET Windows' usbehci does, redo the PHY init like Linux
 * does after every ChipIdea reset, and look again.
 */
STATIC VOID
TryRouting (
  IN CONST CHAR8  *Name,
  IN BOOLEAN      Lpm
  )
{
  UINT32 PortSc;
  UINTN  Ms;

  /* Re-assert port power and give the device a second to attach */
  MmioOr32(mOp + OP_PORTSC, PORTSC_PP);
  for (Ms = 0; Ms < 1000 && !(MmioRead32(mOp + OP_PORTSC) & BIT0); Ms++) {
    MicroSecondDelay(1000);
  }

  PortSc = MmioRead32(mOp + OP_PORTSC);
  DEBUG((EFI_D_ERROR, "Try %a: connect %u LS %u PORTSC %08x ULPI linestate %u func %02x\n",
         Name, PortSc & 1, (PortSc >> 10) & 3, PortSc, UlpiRead(ULPI_DEBUG) & 3,
         UlpiRead(ULPI_FUNC_CTRL)));
}

/*
 * Diagnostic build only (PcdUsbHostSelfTest): with the controller running
 * in host mode, try the MUIC routings that could connect the connector's
 * D+/D- to the PHY and report whether the port sees the device for each.
 */
STATIC VOID
ProbeDevice (
  IN CONST CHAR8  *Name,
  IN BOOLEAN      Lpm
  )
{
  UINT32 Sts;
  UINT32 PortSc;

  MmioAnd32(mOp + OP_USBCMD, ~USBCMD_RS);
  MicroSecondDelay(10000);
  MmioOr32(mOp + OP_USBCMD, USBCMD_RST);
  WaitClear(mOp + OP_USBCMD, USBCMD_RST, 100000);

  if (Lpm) {
    MmioAndThenOr32(mOp + OP_DEVLC, ~DEVLC_PTS_MASK, DEVLC_PTS_ULPI);
  } else {
    MmioAndThenOr32(mOp + OP_PORTSC, ~PORTSC_PTS_MASK, PORTSC_PTS_ULPI);
  }
  UlpiWrite(ULPI_MISC_A_SET, ULPI_MISC_A_VBUSVLDEXTSEL | ULPI_MISC_A_VBUSVLDEXT);
  MmioOr32(USB_GENCONFIG_2, GENCONFIG_2_SESS_VLD_CTRL_EN);
  MmioOr32(mOp + OP_USBCMD, USBCMD_SESS_VLD_CTRL);
  MmioWrite32(USB_SBUSCFG, 0);
  MmioWrite32(USB_AHB_MODE, 0x8);
  MmioAndThenOr32(mOp + (Lpm ? OP_USBMODE_LPM : OP_USBMODE_NOLPM), ~USBMODE_CM_MASK, 0x2);

  MmioWrite32(mOp + 0x04, 0xFFFFFFFF);
  MmioOr32(mOp + OP_USBCMD, USBCMD_RS | USBCMD_SESS_VLD_CTRL);
  MicroSecondDelay(6000000);

  Sts    = MmioRead32(mOp + 0x04);
  PortSc = MmioRead32(mOp + OP_PORTSC);
  DEBUG((EFI_D_ERROR, "Probe %a: bus reset %u PORTSC %08x en %u speed %u func %02x ls %u\n",
         Name, (Sts >> 6) & 1, PortSc, (PortSc >> 2) & 1, (PortSc >> 26) & 3,
         UlpiRead(ULPI_FUNC_CTRL), UlpiRead(ULPI_DEBUG) & 3));

  /* Drop off the bus so the PC sees a detach between probes */
  MmioAnd32(mOp + OP_USBCMD, ~USBCMD_RS);
  MicroSecondDelay(500000);
}

/*
 * Diagnostic build only, PC on the cable: starting from the state lk2nd's
 * working fastboot USB left behind, add our init differences one at a time
 * and see after which one the PC stops seeing the phone.
 */
STATIC VOID
DeviceProbeMatrix (
  VOID
  )
{
  struct clk *Iface = gClock->clk_get("usb_iface_clk");
  struct clk *Core  = gClock->clk_get("usb_core_clk");
  BOOLEAN    Lpm;

  /* Enable only, no rate change: keep whatever lk2nd programmed */
  gClock->clk_enable(Iface);
  gClock->clk_enable(Core);

  mOp = USB_CAP + (MmioRead32(USB_CAP) & 0xFF);
  Lpm = (MmioRead32(USB_HCCPARAMS) & HCCPARAMS_LEN) != 0;
  DEBUG((EFI_D_ERROR, "Probe: start PORTSC %08x USBMODE %08x sleep_cbcr %08x\n",
         MmioRead32(mOp + OP_PORTSC), MmioRead32(mOp + (Lpm ? OP_USBMODE_LPM : OP_USBMODE_NOLPM)),
         MmioRead32(GCC_USB2A_PHY_SLEEP_CBCR)));

  ProbeDevice("A lk2nd state, no GCC changes", Lpm);

  MmioOr32(GCC_USB2A_PHY_SLEEP_CBCR, BIT0);
  ProbeDevice("B + PHY sleep clock on", Lpm);

  BlockReset(GCC_USB_HS_BCR);
  ProbeDevice("C + USB_HS_BCR, clocks on", Lpm);

  /* lk2nd hsusb_clock_init(): clocks off, block reset, clocks on */
  gClock->clk_disable(Core);
  gClock->clk_disable(Iface);
  MicroSecondDelay(20000);
  MmioWrite32(GCC_USB_HS_BCR, 1);
  MicroSecondDelay(20000);
  MmioWrite32(GCC_USB_HS_BCR, 0);
  MicroSecondDelay(20000);
  gClock->clk_enable(Iface);
  gClock->clk_enable(Core);
  ProbeDevice("D lk2nd order: clk off, BCR, clk on", Lpm);

  DEBUG((EFI_D_ERROR, "UsbTest: done, holding 60s for a photo\n"));
  MicroSecondDelay(60 * 1000000);
}

STATIC VOID
SelfTest (
  IN BOOLEAN  Lpm
  )
{

  DumpPort("host mode, as set up", Lpm);
  MmioOr32(mOp + OP_USBCMD, USBCMD_RS | USBCMD_SESS_VLD_CTRL);

  TryRouting("1 as set up", Lpm);

  /*
   * FUNC_CTRL reads 0x45 in host mode: TermSelect=1 is the device-side
   * 1.5k D+ pull-up. A host needs TermSelect=0 (only the 15k pull-downs).
   */
  UlpiWrite(ULPI_FUNC_CTRL_CLR, ULPI_FUNC_TERMSELECT);
  TryRouting("2 TermSelect cleared", Lpm);

  UlpiWrite(ULPI_FUNC_CTRL, ULPI_FUNC_SUSPENDM | ULPI_FUNC_XCVR_FS);
  UlpiWrite(ULPI_OTG_CTRL_SET, ULPI_OTG_DP_PULLDOWN | ULPI_OTG_DM_PULLDOWN);
  TryRouting("3 FUNC=41 + pulldowns", Lpm);

  MmioAnd32(mOp + OP_PORTSC, ~PORTSC_PP);
  MicroSecondDelay(200000);
  UlpiWrite(ULPI_FUNC_CTRL, ULPI_FUNC_SUSPENDM | ULPI_FUNC_XCVR_FS);
  TryRouting("4 port power cycled", Lpm);

  /* Controller halted: does the link still own FUNC_CTRL? */
  MmioAnd32(mOp + OP_USBCMD, ~USBCMD_RS);
  MicroSecondDelay(10000);
  UlpiWrite(ULPI_FUNC_CTRL, ULPI_FUNC_SUSPENDM | ULPI_FUNC_XCVR_FS);
  TryRouting("5 halted, FUNC=41", Lpm);
  MmioOr32(mOp + OP_USBCMD, USBCMD_RS | USBCMD_SESS_VLD_CTRL);

  DumpPort("end", Lpm);
  MmioAnd32(mOp + OP_USBCMD, ~USBCMD_RS);

  DEBUG((EFI_D_ERROR, "UsbTest: done, holding 60s for a photo\n"));
  MicroSecondDelay(60 * 1000000);
}

/*
 * Controller reset, ULPI PHY init and host mode. ChipIdea also resets the
 * ULPI PHY on a controller reset, so this whole sequence belongs together
 * (Linux redoes it on every CI_HDRC_CONTROLLER_RESET_EVENT).
 */
STATIC EFI_STATUS
ResetAndConfigure (
  IN BOOLEAN  Lpm
  )
{
  EFI_STATUS Status;

  STEP("controller reset");
  MmioOr32(mOp + OP_USBCMD, USBCMD_RST);
  if (!WaitClear(mOp + OP_USBCMD, USBCMD_RST, 100000)) {
    DEBUG((EFI_D_ERROR, "UsbHost: controller reset timed out\n"));
    return EFI_DEVICE_ERROR;
  }

  STEP("ULPI PHY init");
  /* ULPI PHY interface */
  if (Lpm) {
    MmioAndThenOr32(mOp + OP_DEVLC, ~DEVLC_PTS_MASK, DEVLC_PTS_ULPI);
  } else {
    MmioAndThenOr32(mOp + OP_PORTSC, ~PORTSC_PTS_MASK, PORTSC_PTS_ULPI);
  }

  /*
   * On the J5 the PHY's VBUS pin is not wired to the connector (VBUS is
   * sensed by the MUIC/charger), so the PHY and controller never see a valid
   * session by themselves. lk2nd's working fastboot USB forces both in
   * software (target_usb_init: ULPI MISC_A, GENCONFIG_2 bit 7, USBCMD bit 25),
   * as does Linux ci_hdrc_msm when VBUS comes from an extcon. We supply VBUS
   * from the SM5703 boost, so claiming it is valid is the truth.
   */
  Status = UlpiWrite(ULPI_MISC_A_SET, ULPI_MISC_A_VBUSVLDEXTSEL | ULPI_MISC_A_VBUSVLDEXT);
  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR, "UsbHost: ULPI write failed: %r (PHY unpowered?)\n", Status));
    return Status;
  }
  MmioOr32(USB_GENCONFIG_2, GENCONFIG_2_SESS_VLD_CTRL_EN);
  MmioOr32(mOp + OP_USBCMD, USBCMD_SESS_VLD_CTRL);

  STEP("host mode");
  /* Same bus setup as lk2nd: AHB burst off, HPROT mode */
  MmioWrite32(USB_SBUSCFG, 0);
  MmioWrite32(USB_AHB_MODE, 0x8);

  /* Host instead of lk2nd's device mode, D+/D- pulldowns, port power */
  MmioAndThenOr32(mOp + (Lpm ? OP_USBMODE_LPM : OP_USBMODE_NOLPM), ~USBMODE_CM_MASK, USBMODE_CM_HOST);
  Status = UlpiWrite(ULPI_OTG_CTRL_SET, ULPI_OTG_DP_PULLDOWN | ULPI_OTG_DM_PULLDOWN);
  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR, "UsbHost: setting D+/D- pulldowns failed: %r\n", Status));
  }
  MmioOr32(mOp + OP_PORTSC, PORTSC_PP);

  DEBUG((EFI_D_ERROR, "UsbHost: PHY_CTRL %08x, ULPI tuning %02x %02x %02x %02x\n",
         MmioRead32(USB_HS_PHY_CTRL), UlpiRead(0x80), UlpiRead(0x81), UlpiRead(0x82), UlpiRead(0x83)));

  return EFI_SUCCESS;
}

STATIC EFI_EVENT mUsbStatusEvent;

/*
 * One-shot diagnostic: after EhciDxe and UsbBusDxe have had a chance to
 * run, log the controller state and whether a USB2 host controller
 * protocol appeared.  This makes a failed enumeration obvious in the
 * serial log without needing a debugger.
 */
STATIC VOID
EFIAPI
UsbStatusNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS  Status;
  VOID        *Hc;

  (VOID)Context;
  gBS->CloseEvent(Event);
  Status = gBS->LocateProtocol(&gEfiUsb2HcProtocolGuid, NULL, &Hc);
  DEBUG((EFI_D_ERROR,
         "UsbHost: 3s status USBMODE %08x USBCMD %08x USBSTS %08x PORTSC %08x USB2HC %r\n",
         MmioRead32(mOp + (mLpm ? OP_USBMODE_LPM : OP_USBMODE_NOLPM)),
         MmioRead32(mOp + OP_USBCMD),
         MmioRead32(mOp + 0x04),
         MmioRead32(mOp + OP_PORTSC),
         Status));
}

/*
 * Called by NonDiscoverablePciDeviceDxe when EhciDxe enables the emulated
 * PCI device.  Re-assert the ChipIdea-specific setup: EhciDxe only knows
 * the generic EHCI registers, so USBMODE and the ULPI transceiver select
 * have to stay in host mode for its reset/init sequence to work.
 */
STATIC EFI_STATUS
EFIAPI
EhciChipIdeaInit (
  IN NON_DISCOVERABLE_DEVICE  *This
  )
{
  (VOID)This;

  DEBUG((EFI_D_ERROR, "UsbHost: EHCI init callback, USBMODE %08x PORTSC %08x\n",
         MmioRead32(mOp + (mLpm ? OP_USBMODE_LPM : OP_USBMODE_NOLPM)),
         MmioRead32(mOp + OP_PORTSC)));

  MmioAndThenOr32(mOp + (mLpm ? OP_USBMODE_LPM : OP_USBMODE_NOLPM),
                  ~USBMODE_CM_MASK, USBMODE_CM_HOST);
  if (mLpm) {
    MmioAndThenOr32(mOp + OP_DEVLC, ~DEVLC_PTS_MASK, DEVLC_PTS_ULPI);
  } else {
    MmioAndThenOr32(mOp + OP_PORTSC, ~PORTSC_PTS_MASK, PORTSC_PTS_ULPI);
  }

  MmioOr32(mOp + OP_PORTSC, PORTSC_PP);
  UlpiWrite(ULPI_MISC_A_SET, ULPI_MISC_A_VBUSVLDEXTSEL | ULPI_MISC_A_VBUSVLDEXT);
  UlpiWrite(ULPI_OTG_CTRL_SET, ULPI_OTG_DP_PULLDOWN | ULPI_OTG_DM_PULLDOWN);
  MmioOr32(USB_GENCONFIG_2, GENCONFIG_2_SESS_VLD_CTRL_EN);
  MmioOr32(mOp + OP_USBCMD, USBCMD_SESS_VLD_CTRL);

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
UsbHostDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  BOOLEAN           Lpm;
  EFI_STATUS        Status;
  INTN              Ret;

  STEP("OTG power");
  EnableOtgVbus();

  STEP("PHY supplies");
  DEBUG((EFI_D_ERROR, "UsbHost: SPMI arbiter version %08x\n", MmioRead32(SPMI_CORE)));
  EnsurePhySupply("LDO7", PM8916_LDO7);
  EnsurePhySupply("LDO13", PM8916_LDO13);

  if (FixedPcdGetBool(PcdUsbHostSelfTest) && mExternalVbus) {
    DeviceProbeMatrix();
  }

  STEP("clocks");
  Ret = gClock->clk_get_set_enable("usb_iface_clk", 0, 1);
  if (Ret == 0) {
    Ret = gClock->clk_get_set_enable("usb_core_clk", 80000000, 1);
  }
  if (Ret != 0) {
    DEBUG((EFI_D_ERROR, "UsbHost: failed to enable clocks (%d)\n", Ret));
    return EFI_DEVICE_ERROR;
  }
  MmioOr32(GCC_USB2A_PHY_SLEEP_CBCR, BIT0);

  STEP("block resets");
  /* Start from a clean controller and PHY */
  BlockReset(GCC_USB_HS_BCR);

  mOp = USB_CAP + (MmioRead32(USB_CAP) & 0xFF);
  Lpm = (MmioRead32(USB_HCCPARAMS) & HCCPARAMS_LEN) != 0;
  mLpm = Lpm;

  Status = ResetAndConfigure(Lpm);
  if (EFI_ERROR(Status)) {
    return Status;
  }

  DEBUG((EFI_D_INFO, "UsbHost: ID 0x%08x, LPM %u, PORTSC 0x%08x, host mode ready\n",
         MmioRead32(USB_ID), Lpm, MmioRead32(mOp + OP_PORTSC)));

  if (FixedPcdGetBool(PcdUsbHostSelfTest)) {
    SelfTest(Lpm);
  }

  /*
   * Hand the controller to the generic EHCI stack.  NonDiscoverablePciDeviceDxe
   * turns this into a PciIo handle with the EHCI class code, EhciDxe drives it,
   * and UsbBusDxe/UsbKbDxe make an OTG keyboard usable from the Shell and the
   * boot menu.
   */
  Status = RegisterNonDiscoverableMmioDevice(
             NonDiscoverableDeviceTypeEhci,
             NonDiscoverableDeviceDmaTypeNonCoherent,
             EhciChipIdeaInit,
             NULL,
             1,
             (UINTN)USB_EHCI_BASE,
             (UINTN)USB_EHCI_SIZE
             );
  DEBUG((EFI_D_ERROR,
         "UsbHost: EHCI registered at 0x%08x (%u bytes): %r\n",
         (UINT32)USB_EHCI_BASE, (UINT32)USB_EHCI_SIZE, Status));

  if (!EFI_ERROR(Status)) {
    Status = gBS->CreateEvent(
                    EVT_TIMER | EVT_NOTIFY_SIGNAL,
                    TPL_CALLBACK,
                    UsbStatusNotify,
                    NULL,
                    &mUsbStatusEvent
                    );
    if (!EFI_ERROR(Status)) {
      gBS->SetTimer(mUsbStatusEvent, TimerRelative, 3 * 10000000ULL);
    }
  }

  return EFI_SUCCESS;
}
