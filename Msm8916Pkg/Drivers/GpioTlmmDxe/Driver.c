#include <PiDxe.h>

#include <Library/LKEnvLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/QcomGpioTlmmLib.h>

EFI_STATUS
EFIAPI
GpioTlmmDxeInitialize (
  IN EFI_HANDLE         ImageHandle,
  IN EFI_SYSTEM_TABLE   *SystemTable
  )
{
  EFI_HANDLE Handle = NULL;
  EFI_STATUS Status;

  DEBUG((DEBUG_ERROR, "GpioTlmmDxeInitialize: entering\n"));
  GpioTlmmImplLibInitialize();

  Status = gBS->InstallMultipleProtocolInterfaces(
                  &Handle,
                  &gQcomGpioTlmmProtocolGuid,      gGpioTlmm,
                  NULL
                  );
  ASSERT_EFI_ERROR(Status);

  return Status;
}
