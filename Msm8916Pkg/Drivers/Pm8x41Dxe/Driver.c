#include <PiDxe.h>

#include <Library/LKEnvLib.h>
#include <Library/DebugLib.h>
#include <Library/QcomPm8x41Lib.h>
#include <Library/UefiBootServicesTableLib.h>

EFI_STATUS
EFIAPI
Pm8x41DxeInitialize (
  IN EFI_HANDLE         ImageHandle,
  IN EFI_SYSTEM_TABLE   *SystemTable
  )
{
  EFI_HANDLE Handle = NULL;
  EFI_STATUS Status;

  DEBUG((DEBUG_ERROR, "Pm8x41DxeInitialize: entering\n"));

  Pm8x41ImplLibInitialize();

  Status = gBS->InstallMultipleProtocolInterfaces(
                  &Handle,
                  &gQcomPm8x41ProtocolGuid,      gPm8x41,
                  NULL
                  );
  ASSERT_EFI_ERROR(Status);

  return Status;
}
