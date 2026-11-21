[Defines]
  PLATFORM_NAME                  = Msm8916Pkg
  PLATFORM_GUID                  = 28f1a3bf-193a-47e3-a7b9-5a435eaab2ee
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010019
  OUTPUT_DIRECTORY               = Build/$(PLATFORM_NAME)
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = Msm8916Pkg/Devices/gt510wifi.fdf

!include Msm8916Pkg/Msm8916Pkg.dsc

[BuildOptions.common]
  GCC:*_*_AARCH64_CC_FLAGS =

[PcdsFixedAtBuild.common]
  # Simple FrameBuffer
  gMsm8916PkgTokenSpaceGuid.PcdMipiFrameBufferWidth|768
  gMsm8916PkgTokenSpaceGuid.PcdMipiFrameBufferHeight|1024
  gMsm8916PkgTokenSpaceGuid.PcdMipiFrameBufferAddress|0x8e000000
  gMsm8916PkgTokenSpaceGuid.PcdMipiFrameBufferPixelBpp|32
  gMsm8916PkgTokenSpaceGuid.PcdMipiFrameBufferVisibleWidth|768
  gMsm8916PkgTokenSpaceGuid.PcdMipiFrameBufferVisibleHeight|1024

  # SD card detect
  gQcomTokenSpaceGuid.PcdSdCardDetectGpio|38
  gQcomTokenSpaceGuid.PcdSdCardDetectActiveLow|TRUE

  # Touch (Atmel maXTouch)
  gQcomTokenSpaceGuid.PcdTouchI2cSdaGpio|18
  gQcomTokenSpaceGuid.PcdTouchI2cSclGpio|19
  gQcomTokenSpaceGuid.PcdTouchI2cAddress|0x4A
  gQcomTokenSpaceGuid.PcdTouchIntGpio|13
  gQcomTokenSpaceGuid.PcdTouchPowerEnableGpio|73
  gQcomTokenSpaceGuid.PcdTouchResetGpio|114
  gQcomTokenSpaceGuid.PcdTouchMaxX|1024
  gQcomTokenSpaceGuid.PcdTouchMaxY|768
  gQcomTokenSpaceGuid.PcdTouchInvertX|FALSE
  gQcomTokenSpaceGuid.PcdTouchInvertY|FALSE
  gQcomTokenSpaceGuid.PcdTouchSwapXY|FALSE

  # MAX77849 charger (OTG 5V boost) on bit-banged I2C: SDA GPIO 2, SCL GPIO 3
  gQcomTokenSpaceGuid.PcdOtgBoostI2cSdaGpio|2
  gQcomTokenSpaceGuid.PcdOtgBoostI2cSclGpio|3
  gQcomTokenSpaceGuid.PcdOtgBoostI2cAddress|0x66

  # MAX77849 MUIC on the same bus, address 0x25
  gQcomTokenSpaceGuid.PcdMuicI2cSdaGpio|2
  gQcomTokenSpaceGuid.PcdMuicI2cSclGpio|3
  gQcomTokenSpaceGuid.PcdMuicI2cAddress|0x25

  # SmBios Information
  gMsm8916PkgTokenSpaceGuid.PcdDeviceVendor|"Samsung"
  gMsm8916PkgTokenSpaceGuid.PcdDeviceProduct|"Galaxy Tab A 9.7 2015"
  gMsm8916PkgTokenSpaceGuid.PcdDeviceCodeName|"gt510wifi"
