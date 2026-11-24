#ifndef __PLATFORM_KEYPAD_LIB_H__
#define __PLATFORM_KEYPAD_LIB_H__

#include <Uefi.h>

/**
  Install a Simple Text Input (Ex) protocol backed by the platform buttons.

  This is called from GpioTlmmDxe, which is known to be dispatched early,
  so the boot manager always gets a working input device even when the
  separate GenericKeypadDeviceDxe/KeypadDxe pair is not loaded.
**/
EFI_STATUS
EFIAPI
PlatformKeypadInstall (
  VOID
  );

/**
  Protocol published on the keypad handle so PlatformBootManagerLib can start
  polling only after console/DXE initialization is complete.
**/
typedef EFI_STATUS (EFIAPI *PLATFORM_KEYPAD_START_POLL)(VOID);
typedef EFI_STATUS (EFIAPI *PLATFORM_KEYPAD_POLL_PMIC_KEYS)(VOID);

typedef struct {
  PLATFORM_KEYPAD_START_POLL       StartPoll;
  PLATFORM_KEYPAD_POLL_PMIC_KEYS   PollPmicKeys;
} PLATFORM_KEYPAD_POLL_PROTOCOL;

extern EFI_GUID gPlatformKeypadPollProtocolGuid;

#endif
