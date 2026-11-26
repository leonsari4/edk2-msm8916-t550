/** @file
  Implementation for PlatformBootManagerLib library class interfaces.

  Copyright (C) 2015-2016, Red Hat, Inc.
  Copyright (c) 2014 - 2019, ARM Ltd. All rights reserved.<BR>
  Copyright (c) 2004 - 2018, Intel Corporation. All rights reserved.<BR>
  Copyright (c) 2016, Linaro Ltd. All rights reserved.<BR>

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Guid/ConsoleInDevice.h>
#include <Guid/EventGroup.h>
#include <Guid/SerialPortLibVendor.h>
#include <Guid/TtyTerm.h>
#include <IndustryStandard/Pci22.h>
#include <Library/BootLogoLib.h>
#include <Library/CapsuleLib.h>
#include <Library/DevicePathLib.h>
#include <Library/HobLib.h>
#include <Library/PcdLib.h>
#include <Library/PlatformKeypadLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/EsrtManagement.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/PciIo.h>
#include <Protocol/PciRootBridgeIo.h>
#include <Protocol/PlatformBootManager.h>

#include "PlatformBm.h"

#define DP_NODE_LEN(Type)                                                      \
  {                                                                            \
    (UINT8)sizeof(Type), (UINT8)(sizeof(Type) >> 8)                            \
  }

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH         SerialDxe;
  UART_DEVICE_PATH           Uart;
  VENDOR_DEFINED_DEVICE_PATH TermType;
  EFI_DEVICE_PATH_PROTOCOL   End;
} PLATFORM_SERIAL_CONSOLE;
#pragma pack()

STATIC PLATFORM_SERIAL_CONSOLE mSerialConsole = {
    //
    // VENDOR_DEVICE_PATH SerialDxe
    //
    {{HARDWARE_DEVICE_PATH, HW_VENDOR_DP, DP_NODE_LEN(VENDOR_DEVICE_PATH)},
     EDKII_SERIAL_PORT_LIB_VENDOR_GUID},

    //
    // UART_DEVICE_PATH Uart
    //
    {
        {MESSAGING_DEVICE_PATH, MSG_UART_DP, DP_NODE_LEN(UART_DEVICE_PATH)},
        0,                                     // Reserved
        FixedPcdGet64(PcdUartDefaultBaudRate), // BaudRate
        FixedPcdGet8(PcdUartDefaultDataBits),  // DataBits
        FixedPcdGet8(PcdUartDefaultParity),    // Parity
        FixedPcdGet8(PcdUartDefaultStopBits)   // StopBits
    },

    //
    // VENDOR_DEFINED_DEVICE_PATH TermType
    //
    {
        {MESSAGING_DEVICE_PATH, MSG_VENDOR_DP,
         DP_NODE_LEN(VENDOR_DEFINED_DEVICE_PATH)}
        //
        // Guid to be filled in dynamically
        //
    },

    //
    // EFI_DEVICE_PATH_PROTOCOL End
    //
    {END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE,
     DP_NODE_LEN(EFI_DEVICE_PATH_PROTOCOL)}};

#pragma pack(1)
typedef struct {
  USB_CLASS_DEVICE_PATH    Keyboard;
  EFI_DEVICE_PATH_PROTOCOL End;
} PLATFORM_USB_KEYBOARD;
#pragma pack()

STATIC PLATFORM_USB_KEYBOARD mUsbKeyboard = {
    //
    // USB_CLASS_DEVICE_PATH Keyboard
    //
    {
        {MESSAGING_DEVICE_PATH, MSG_USB_CLASS_DP,
         DP_NODE_LEN(USB_CLASS_DEVICE_PATH)},
        0xFFFF, // VendorId: any
        0xFFFF, // ProductId: any
        3,      // DeviceClass: HID
        1,      // DeviceSubClass: boot
        1       // DeviceProtocol: keyboard
    },

    //
    // EFI_DEVICE_PATH_PROTOCOL End
    //
    {END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE,
     DP_NODE_LEN(EFI_DEVICE_PATH_PROTOCOL)}};

/**
  Check if the handle satisfies a particular condition.

  @param[in] Handle      The handle to check.
  @param[in] ReportText  A caller-allocated string passed in for reporting
                         purposes. It must never be NULL.

  @retval TRUE   The condition is satisfied.
  @retval FALSE  Otherwise. This includes the case when the condition could not
                 be fully evaluated due to an error.
**/
typedef BOOLEAN(EFIAPI *FILTER_FUNCTION)(
    IN EFI_HANDLE Handle, IN CONST CHAR16 *ReportText);

/**
  Process a handle.

  @param[in] Handle      The handle to process.
  @param[in] ReportText  A caller-allocated string passed in for reporting
                         purposes. It must never be NULL.
**/
typedef VOID(EFIAPI *CALLBACK_FUNCTION)(
    IN EFI_HANDLE Handle, IN CONST CHAR16 *ReportText);

/**
  Locate all handles that carry the specified protocol, filter them with a
  callback function, and pass each handle that passes the filter to another
  callback.

  @param[in] ProtocolGuid  The protocol to look for.

  @param[in] Filter        The filter function to pass each handle to. If this
                           parameter is NULL, then all handles are processed.

  @param[in] Process       The callback function to pass each handle to that
                           clears the filter.
**/
STATIC
VOID FilterAndProcess(
    IN EFI_GUID *ProtocolGuid, IN FILTER_FUNCTION Filter OPTIONAL,
    IN CALLBACK_FUNCTION Process)
{
  EFI_STATUS  Status;
  EFI_HANDLE *Handles;
  UINTN       NoHandles;
  UINTN       Idx;

  Status = gBS->LocateHandleBuffer(
      ByProtocol, ProtocolGuid, NULL /* SearchKey */, &NoHandles, &Handles);
  if (EFI_ERROR(Status)) {
    //
    // This is not an error, just an informative condition.
    //
    DEBUG((EFI_D_VERBOSE, "%a: %g: %r\n", __FUNCTION__, ProtocolGuid, Status));
    return;
  }

  ASSERT(NoHandles > 0);
  for (Idx = 0; Idx < NoHandles; ++Idx) {
    CHAR16 *      DevicePathText;
    STATIC CHAR16 Fallback[] = L"<device path unavailable>";

    //
    // The ConvertDevicePathToText() function handles NULL input transparently.
    //
    DevicePathText = ConvertDevicePathToText(
        DevicePathFromHandle(Handles[Idx]),
        FALSE, // DisplayOnly
        FALSE  // AllowShortcuts
    );
    if (DevicePathText == NULL) {
      DevicePathText = Fallback;
    }

    if (Filter == NULL || Filter(Handles[Idx], DevicePathText)) {
      Process(Handles[Idx], DevicePathText);
    }

    if (DevicePathText != Fallback) {
      FreePool(DevicePathText);
    }
  }
  gBS->FreePool(Handles);
}

/**
  This FILTER_FUNCTION checks if a handle corresponds to a PCI display device.
**/
STATIC
BOOLEAN
EFIAPI
IsPciDisplay(IN EFI_HANDLE Handle, IN CONST CHAR16 *ReportText)
{
  EFI_STATUS           Status;
  EFI_PCI_IO_PROTOCOL *PciIo;
  PCI_TYPE00           Pci;

  Status = gBS->HandleProtocol(Handle, &gEfiPciIoProtocolGuid, (VOID **)&PciIo);
  if (EFI_ERROR(Status)) {
    //
    // This is not an error worth reporting.
    //
    return FALSE;
  }

  Status = PciIo->Pci.Read(
      PciIo, EfiPciIoWidthUint32, 0 /* Offset */, sizeof Pci / sizeof(UINT32),
      &Pci);
  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR, "%a: %s: %r\n", __FUNCTION__, ReportText, Status));
    return FALSE;
  }

  return IS_PCI_DISPLAY(&Pci);
}

/**
  This CALLBACK_FUNCTION attempts to connect a handle non-recursively, asking
  the matching driver to produce all first-level child handles.
**/
STATIC
VOID EFIAPI Connect(IN EFI_HANDLE Handle, IN CONST CHAR16 *ReportText)
{
  EFI_STATUS Status;

  Status = gBS->ConnectController(
      Handle, // ControllerHandle
      NULL,   // DriverImageHandle
      NULL,   // RemainingDevicePath -- produce all children
      FALSE   // Recursive
  );
  DEBUG(
      (EFI_ERROR(Status) ? EFI_D_ERROR : EFI_D_VERBOSE, "%a: %s: %r\n",
       __FUNCTION__, ReportText, Status));
}

/**
  This CALLBACK_FUNCTION retrieves the EFI_DEVICE_PATH_PROTOCOL from the
  handle, and adds it to ConOut and ErrOut.
**/
STATIC
VOID EFIAPI AddOutput(IN EFI_HANDLE Handle, IN CONST CHAR16 *ReportText)
{
  EFI_STATUS                Status;
  EFI_DEVICE_PATH_PROTOCOL *DevicePath;

  DevicePath = DevicePathFromHandle(Handle);
  if (DevicePath == NULL) {
    DEBUG(
        (EFI_D_ERROR, "%a: %s: handle %p: device path not found\n",
         __FUNCTION__, ReportText, Handle));
    return;
  }

  Status = EfiBootManagerUpdateConsoleVariable(ConOut, DevicePath, NULL);
  if (EFI_ERROR(Status)) {
    DEBUG(
        (EFI_D_ERROR, "%a: %s: adding to ConOut: %r\n", __FUNCTION__,
         ReportText, Status));
    return;
  }

  Status = EfiBootManagerUpdateConsoleVariable(ErrOut, DevicePath, NULL);
  if (EFI_ERROR(Status)) {
    DEBUG(
        (EFI_D_ERROR, "%a: %s: adding to ErrOut: %r\n", __FUNCTION__,
         ReportText, Status));
    return;
  }

  DEBUG(
      (EFI_D_VERBOSE, "%a: %s: added to ConOut and ErrOut\n", __FUNCTION__,
       ReportText));
}

STATIC
UINT16
PlatformRegisterFvBootOption(
    CONST EFI_GUID *FileGuid, CHAR16 *Description, UINT32 Attributes)
{
  EFI_STATUS                        Status;
  INTN                              OptionIndex;
  EFI_BOOT_MANAGER_LOAD_OPTION      NewOption;
  EFI_BOOT_MANAGER_LOAD_OPTION *    BootOptions;
  UINTN                             BootOptionCount;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH FileNode;
  EFI_LOADED_IMAGE_PROTOCOL *       LoadedImage;
  EFI_DEVICE_PATH_PROTOCOL *        DevicePath;
  UINT16                            OptionNumber;

  Status = gBS->HandleProtocol(
      gImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&LoadedImage);
  ASSERT_EFI_ERROR(Status);

  EfiInitializeFwVolDevicepathNode(&FileNode, FileGuid);
  DevicePath = DevicePathFromHandle(LoadedImage->DeviceHandle);
  ASSERT(DevicePath != NULL);
  DevicePath =
      AppendDevicePathNode(DevicePath, (EFI_DEVICE_PATH_PROTOCOL *)&FileNode);
  ASSERT(DevicePath != NULL);

  Status = EfiBootManagerInitializeLoadOption(
      &NewOption, LoadOptionNumberUnassigned, LoadOptionTypeBoot, Attributes,
      Description, DevicePath, NULL, 0);
  ASSERT_EFI_ERROR(Status);
  FreePool(DevicePath);

  BootOptions =
      EfiBootManagerGetLoadOptions(&BootOptionCount, LoadOptionTypeBoot);

  OptionIndex =
      EfiBootManagerFindLoadOption(&NewOption, BootOptions, BootOptionCount);

  if (OptionIndex == -1) {
    Status = EfiBootManagerAddLoadOptionVariable(&NewOption, MAX_UINTN);
    ASSERT_EFI_ERROR(Status);
  }
  OptionNumber = NewOption.OptionNumber;
  EfiBootManagerFreeLoadOption(&NewOption);
  EfiBootManagerFreeLoadOptions(BootOptions, BootOptionCount);
  return OptionNumber;
}

//
// GUID used by UefiBootManagerLib to mark boot options it created itself
// (BmBoot.c: mBmAutoCreateBootOptionGuid).
//
STATIC CONST EFI_GUID mBmAutoCreateBootOptionGuid = {
  0x8108ac4e,
  0x9f11,
  0x4d59,
  { 0x85, 0x0e, 0xe2, 0x1a, 0x52, 0x2c, 0x59, 0xb2 }
};

//
// Vendor device path GUID published by SdhciMMCHSDxe. The slot number is
// stored in Guid.Data4[7] (1 = eMMC, 2 = SD card).
//
STATIC CONST EFI_GUID mMmchsDevicePathGuid = {
  0x891f2b63,
  0x4ed3,
  0x4305,
  { 0xa6, 0x60, 0x01, 0x6d, 0x02, 0x81, 0x05, 0x41 }
};

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH       Vendor;
  EFI_DEVICE_PATH_PROTOCOL End;
} MMCHS_BOOT_DEVICE_PATH;
#pragma pack()

STATIC CONST MMCHS_BOOT_DEVICE_PATH mMmchsBootDevicePathTemplate = {
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      {
        (UINT8)(sizeof(VENDOR_DEVICE_PATH)),
        (UINT8)((sizeof(VENDOR_DEVICE_PATH)) >> 8)
      }
    },
    mMmchsDevicePathGuid
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    {sizeof(EFI_DEVICE_PATH_PROTOCOL), 0}
  }
};

//
// MdeModulePkg's platform boot manager protocol.  EmbeddedPkg publishes a
// different protocol under the same header guard, so declare the MdeModulePkg
// layout locally.  BDS calls RefreshAllBootOptions on every refresh (including
// the one UiApp performs when it opens the boot menu); returning an empty list
// keeps the auto-enumerated "UEFI Misc Device" entries out of NVRAM.
//
typedef
EFI_STATUS
(EFIAPI *PLATFORM_REFRESH_ALL_BOOT_OPTIONS)(
  IN  CONST EFI_BOOT_MANAGER_LOAD_OPTION  *BootOptions,
  IN  CONST UINTN                        BootOptionsCount,
  OUT       EFI_BOOT_MANAGER_LOAD_OPTION **UpdatedBootOptions,
  OUT       UINTN                        *UpdatedBootOptionsCount
  );

typedef struct {
  UINT64                             Revision;
  PLATFORM_REFRESH_ALL_BOOT_OPTIONS  RefreshAllBootOptions;
} MDE_PLATFORM_BOOT_MANAGER_PROTOCOL;

STATIC EFI_STATUS
EFIAPI
PlatformRefreshAllBootOptions (
  IN  CONST EFI_BOOT_MANAGER_LOAD_OPTION  *BootOptions,
  IN  CONST UINTN                        BootOptionsCount,
  OUT       EFI_BOOT_MANAGER_LOAD_OPTION **UpdatedBootOptions,
  OUT       UINTN                        *UpdatedBootOptionsCount
  )
{
  UINTN Index;

  DEBUG((DEBUG_ERROR, "PlatformBm: refresh callback (%u auto options)\n",
         (UINTN)BootOptionsCount));
  for (Index = 0; Index < BootOptionsCount; Index++) {
    DEBUG((DEBUG_ERROR, "PlatformBm:  drop auto '%s'\n",
           BootOptions[Index].Description));
  }

  *UpdatedBootOptions      = NULL;
  *UpdatedBootOptionsCount = 0;
  return EFI_SUCCESS;
}

STATIC MDE_PLATFORM_BOOT_MANAGER_PROTOCOL mPlatformBootManagerProtocol = {
  0x00000001,
  PlatformRefreshAllBootOptions
};

STATIC EFI_HANDLE mPlatformBootManagerHandle = NULL;

STATIC
VOID
PlatformLogBootOptions (
  IN CONST CHAR8 *Where
  )
{
  EFI_BOOT_MANAGER_LOAD_OPTION *BootOptions;
  UINTN                         BootOptionCount;
  UINTN                         Index;

  BootOptions =
      EfiBootManagerGetLoadOptions(&BootOptionCount, LoadOptionTypeBoot);
  DEBUG((DEBUG_ERROR, "PlatformBm: boot options (%a): %u\n", Where,
         (UINTN)BootOptionCount));
  if (BootOptions == NULL) {
    return;
  }
  for (Index = 0; Index < BootOptionCount; Index++) {
    CHAR16 *Text =
        ConvertDevicePathToText(BootOptions[Index].FilePath, FALSE, FALSE);
    DEBUG((DEBUG_ERROR, "PlatformBm:  %04x '%s' %s\n",
           (UINTN)BootOptions[Index].OptionNumber,
           BootOptions[Index].Description,
           (Text != NULL) ? Text : L"<null>"));
    if (Text != NULL) {
      FreePool(Text);
    }
  }
  EfiBootManagerFreeLoadOptions(BootOptions, BootOptionCount);
}

STATIC
VOID
PlatformDeleteBootOptionByDescription (
  IN CONST CHAR16 *Description
  )
{
  EFI_BOOT_MANAGER_LOAD_OPTION *BootOptions;
  UINTN                         BootOptionCount;
  UINTN                         Index;

  BootOptions =
      EfiBootManagerGetLoadOptions(&BootOptionCount, LoadOptionTypeBoot);
  if (BootOptions == NULL) {
    return;
  }
  for (Index = 0; Index < BootOptionCount; Index++) {
    if (StrCmp(BootOptions[Index].Description, Description) == 0) {
      DEBUG((DEBUG_ERROR, "PlatformBm: delete stale option %04x '%s'\n",
             (UINTN)BootOptions[Index].OptionNumber,
             BootOptions[Index].Description));
      EfiBootManagerDeleteLoadOptionVariable(BootOptions[Index].OptionNumber,
                                             LoadOptionTypeBoot);
    }
  }
  EfiBootManagerFreeLoadOptions(BootOptions, BootOptionCount);
}

/**
  Remove every boot option that BDS auto-enumerated. Those show up in the
  menu as "UEFI Misc Device" / "UEFI Non-Block Boot Device N" and are not
  useful on this device.
**/
STATIC
VOID
PlatformRemoveAutoBootOptions (
  VOID
  )
{
  EFI_BOOT_MANAGER_LOAD_OPTION *BootOptions;
  UINTN                         BootOptionCount;
  UINTN                         Index;

  BootOptions =
      EfiBootManagerGetLoadOptions(&BootOptionCount, LoadOptionTypeBoot);
  if (BootOptions == NULL) {
    return;
  }
  for (Index = 0; Index < BootOptionCount; Index++) {
    if ((BootOptions[Index].OptionalDataSize == sizeof(EFI_GUID)) &&
        (BootOptions[Index].OptionalData != NULL) &&
        CompareGuid((EFI_GUID *)BootOptions[Index].OptionalData,
                    &mBmAutoCreateBootOptionGuid)) {
      DEBUG((DEBUG_ERROR, "PlatformBm: delete auto option %04x '%s'\n",
             (UINTN)BootOptions[Index].OptionNumber,
             BootOptions[Index].Description));
      EfiBootManagerDeleteLoadOptionVariable(BootOptions[Index].OptionNumber,
                                             LoadOptionTypeBoot);
    }
  }
  EfiBootManagerFreeLoadOptions(BootOptions, BootOptionCount);
}

STATIC
BOOLEAN
PlatformIsMmchsNode (
  IN VENDOR_DEVICE_PATH *Node
  )
{
  //
  // SdhciMMCHSDxe stores the slot number in Guid.Data4[7], so the full GUID
  // differs for eMMC and SD.  Compare every byte except that slot byte.
  //
  return (BOOLEAN)(CompareMem(&Node->Guid, &mMmchsDevicePathGuid,
                              sizeof(EFI_GUID) - 1) == 0);
}

STATIC
INTN
PlatformGetMmcSlot (
  IN EFI_DEVICE_PATH_PROTOCOL *DevicePath
  )
{
  EFI_DEVICE_PATH_PROTOCOL *Node;

  for (Node = DevicePath; (Node != NULL) && !IsDevicePathEnd(Node);
       Node = NextDevicePathNode(Node)) {
    if ((DevicePathType(Node) == HARDWARE_DEVICE_PATH) &&
        (DevicePathSubType(Node) == HW_VENDOR_DP) &&
        PlatformIsMmchsNode((VENDOR_DEVICE_PATH *)Node)) {
      return (INTN)((VENDOR_DEVICE_PATH *)Node)->Guid.Data4[7];
    }
  }
  return -1;
}

/**
  Register exactly one "eMMC" and one "SD Card" boot option, pointing at the
  whole MMC device. BDS then performs removable-media boot (appending
  \EFI\BOOT\BOOTAA64.EFI) for every FAT partition on the device.
**/
STATIC
VOID
PlatformRegisterMmcBootOptions (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles;
  UINTN       HandleCount;
  UINTN       Index;
  EFI_HANDLE  SlotHandle[3];
  CHAR16      *SlotName[3];
  UINTN       Slot;

  SlotHandle[0] = NULL;
  SlotHandle[1] = NULL;
  SlotHandle[2] = NULL;
  SlotName[0] = NULL;
  SlotName[1] = L"eMMC";
  SlotName[2] = L"SD Card";

  Handles = NULL;
  HandleCount = 0;
  Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiBlockIoProtocolGuid, NULL,
                                   &HandleCount, &Handles);
  if (EFI_ERROR(Status) || (Handles == NULL)) {
    DEBUG((DEBUG_ERROR, "PlatformBm: locate BlockIo failed: %r\n", Status));
    return;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
    EFI_BLOCK_IO_PROTOCOL     *BlockIo;
    INTN                      MmcSlot;

    DevicePath = DevicePathFromHandle(Handles[Index]);
    if (DevicePath == NULL) {
      continue;
    }
    MmcSlot = PlatformGetMmcSlot(DevicePath);
    if ((MmcSlot != 1) && (MmcSlot != 2)) {
      continue;
    }
    Status = gBS->HandleProtocol(Handles[Index], &gEfiBlockIoProtocolGuid,
                                 (VOID **)&BlockIo);
    if (EFI_ERROR(Status) || (BlockIo == NULL) || (BlockIo->Media == NULL)) {
      continue;
    }
    DEBUG((DEBUG_ERROR,
           "PlatformBm: mmc handle %p slot %u logical %u present %u last %lu\n",
           Handles[Index], (UINTN)MmcSlot,
           (UINTN)BlockIo->Media->LogicalPartition,
           (UINTN)BlockIo->Media->MediaPresent,
           (UINT64)BlockIo->Media->LastBlock));
    if (BlockIo->Media->LogicalPartition) {
      continue;
    }
    if (SlotHandle[MmcSlot] == NULL) {
      SlotHandle[MmcSlot] = Handles[Index];
    }
  }
  FreePool(Handles);

  for (Slot = 1; Slot <= 2; Slot++) {
    MMCHS_BOOT_DEVICE_PATH      DevicePath;
    EFI_BOOT_MANAGER_LOAD_OPTION NewOption;

    //
    // Build the vendor device path ourselves instead of depending on a
    // BlockIo handle being present.  This keeps the "SD Card" entry visible
    // even when no card is inserted; BDS will find the matching handle and
    // its FAT partitions once a card is present.
    //
    DevicePath = mMmchsBootDevicePathTemplate;
    DevicePath.Vendor.Guid.Data4[7] = (UINT8)Slot;

    PlatformDeleteBootOptionByDescription(SlotName[Slot]);
    Status = EfiBootManagerInitializeLoadOption(
        &NewOption, LoadOptionNumberUnassigned, LoadOptionTypeBoot,
        LOAD_OPTION_ACTIVE, SlotName[Slot],
        (EFI_DEVICE_PATH_PROTOCOL *)&DevicePath, NULL, 0);
    if (EFI_ERROR(Status)) {
      DEBUG((DEBUG_ERROR, "PlatformBm: init '%s' option failed: %r\n",
             SlotName[Slot], Status));
      continue;
    }
    Status = EfiBootManagerAddLoadOptionVariable(&NewOption, MAX_UINTN);
    DEBUG((DEBUG_ERROR,
           "PlatformBm: add '%s' option %04x (slot %u, handle %p): %r\n",
           SlotName[Slot], (UINTN)NewOption.OptionNumber, (UINTN)Slot,
           SlotHandle[Slot], Status));
    EfiBootManagerFreeLoadOption(&NewOption);
  }
}

STATIC
VOID GetPlatformOptions(VOID)
{
  EFI_STATUS                      Status;
  EFI_BOOT_MANAGER_LOAD_OPTION *  CurrentBootOptions;
  EFI_BOOT_MANAGER_LOAD_OPTION *  BootOptions;
  EFI_INPUT_KEY *                 BootKeys;
  PLATFORM_BOOT_MANAGER_PROTOCOL *PlatformBootManager;
  UINTN                           CurrentBootOptionCount;
  UINTN                           Index;
  UINTN                           BootCount;

  Status = gBS->LocateProtocol(
      &gPlatformBootManagerProtocolGuid, NULL, (VOID **)&PlatformBootManager);
  if (EFI_ERROR(Status)) {
    return;
  }
  Status = PlatformBootManager->GetPlatformBootOptionsAndKeys(
      &BootCount, &BootOptions, &BootKeys);
  if (EFI_ERROR(Status)) {
    return;
  }
  //
  // Fetch the existent boot options. If there are none, CurrentBootCount
  // will be zeroed.
  //
  CurrentBootOptions =
      EfiBootManagerGetLoadOptions(&CurrentBootOptionCount, LoadOptionTypeBoot);
  //
  // Process the platform boot options.
  //
  for (Index = 0; Index < BootCount; Index++) {
    INTN  Match;
    UINTN BootOptionNumber;

    //
    // If there are any preexistent boot options, and the subject platform boot
    // option is already among them, then don't try to add it. Just get its
    // assigned boot option number so we can associate a hotkey with it. Note
    // that EfiBootManagerFindLoadOption() deals fine with (CurrentBootOptions
    // == NULL) if (CurrentBootCount == 0).
    //
    Match = EfiBootManagerFindLoadOption(
        &BootOptions[Index], CurrentBootOptions, CurrentBootOptionCount);
    if (Match >= 0) {
      BootOptionNumber = CurrentBootOptions[Match].OptionNumber;
    }
    else {
      //
      // Add the platform boot options as a new one, at the end of the boot
      // order. Note that if the platform provided this boot option with an
      // unassigned option number, then the below function call will assign a
      // number.
      //
      Status =
          EfiBootManagerAddLoadOptionVariable(&BootOptions[Index], MAX_UINTN);
      if (EFI_ERROR(Status)) {
        DEBUG(
            (DEBUG_ERROR, "%a: failed to register \"%s\": %r\n", __FUNCTION__,
             BootOptions[Index].Description, Status));
        continue;
      }
      BootOptionNumber = BootOptions[Index].OptionNumber;
    }

    //
    // Register a hotkey with the boot option, if requested.
    //
    if (BootKeys[Index].UnicodeChar == L'\0') {
      continue;
    }

    Status = EfiBootManagerAddKeyOptionVariable(
        NULL, BootOptionNumber, 0, &BootKeys[Index], NULL);
    if (EFI_ERROR(Status)) {
      DEBUG(
          (DEBUG_ERROR, "%a: failed to register hotkey for \"%s\": %r\n",
           __FUNCTION__, BootOptions[Index].Description, Status));
    }
  }
  EfiBootManagerFreeLoadOptions(CurrentBootOptions, CurrentBootOptionCount);
  EfiBootManagerFreeLoadOptions(BootOptions, BootCount);
  FreePool(BootKeys);
}

STATIC
VOID PlatformRegisterOptionsAndKeys(VOID)
{
  EFI_STATUS                   Status;
  EFI_INPUT_KEY                Enter;
  EFI_INPUT_KEY                UP;
  EFI_INPUT_KEY                Esc;
  EFI_BOOT_MANAGER_LOAD_OPTION BootOption;

  GetPlatformOptions();

  //
  // Register ENTER as CONTINUE key
  //
  Enter.ScanCode    = SCAN_NULL;
  Enter.UnicodeChar = CHAR_CARRIAGE_RETURN;
  Status            = EfiBootManagerRegisterContinueKeyOption(0, &Enter, NULL);
  ASSERT_EFI_ERROR(Status);

  //
  // Map UP, DOWN, ESC and ENTER to Boot Manager Menu
  //
  UP.ScanCode     = SCAN_UP;
  UP.UnicodeChar  = CHAR_NULL;
  Esc.ScanCode    = SCAN_ESC;
  Esc.UnicodeChar = CHAR_NULL;
  {
    EFI_INPUT_KEY Down;
    Down.ScanCode    = SCAN_DOWN;
    Down.UnicodeChar = CHAR_NULL;

    Status = EfiBootManagerGetBootManagerMenu(&BootOption);
    ASSERT_EFI_ERROR(Status);

    Status = EfiBootManagerAddKeyOptionVariable(
        NULL, (UINT16)BootOption.OptionNumber, 0, &UP, NULL);
    ASSERT(Status == EFI_SUCCESS || Status == EFI_ALREADY_STARTED);
    Status = EfiBootManagerAddKeyOptionVariable(
        NULL, (UINT16)BootOption.OptionNumber, 0, &Down, NULL);
    ASSERT(Status == EFI_SUCCESS || Status == EFI_ALREADY_STARTED);
    Status = EfiBootManagerAddKeyOptionVariable(
        NULL, (UINT16)BootOption.OptionNumber, 0, &Esc, NULL);
    ASSERT(Status == EFI_SUCCESS || Status == EFI_ALREADY_STARTED);
    Status = EfiBootManagerAddKeyOptionVariable(
        NULL, (UINT16)BootOption.OptionNumber, 0, &Enter, NULL);
    ASSERT(Status == EFI_SUCCESS || Status == EFI_ALREADY_STARTED);
  }
}

//
// BDS Platform Functions
//
/**
  Do the platform init, can be customized by OEM/IBV
  Possible things that can be done in PlatformBootManagerBeforeConsole:
  > Update console variable: 1. include hot-plug devices;
  >                          2. Clear ConIn and add SOL for AMT
  > Register new Driver#### or Boot####
  > Register new Key####: e.g.: F12
  > Signal ReadyToLock event
  > Authentication action: 1. connect Auth devices;
  >                        2. Identify auto logon user.
**/
VOID EFIAPI PlatformBootManagerBeforeConsole(VOID)
{
  DEBUG((DEBUG_ERROR, "PlatformBm: BeforeConsole enter\n"));

  //
  // Signal EndOfDxe PI Event
  //
  EfiEventGroupSignal(&gEfiEndOfDxeEventGroupGuid);

  //
  // Dispatch deferred images after EndOfDxe event.
  //
  EfiBootManagerDispatchDeferredImages();

  //
  // Locate the PCI root bridges and make the PCI bus driver connect each,
  // non-recursively. This will produce a number of child handles with PciIo on
  // them.
  //
  FilterAndProcess(&gEfiPciRootBridgeIoProtocolGuid, NULL, Connect);

  //
  // Find all display class PCI devices (using the handles from the previous
  // step), and connect them non-recursively. This should produce a number of
  // child handles with GOPs on them.
  //
  FilterAndProcess(&gEfiPciIoProtocolGuid, IsPciDisplay, Connect);

  //
  // Now add the device path of all handles with GOP on them to ConOut and
  // ErrOut.
  //
  FilterAndProcess(&gEfiGraphicsOutputProtocolGuid, NULL, AddOutput);

  //
  // Add the hardcoded short-form USB keyboard device path to ConIn.
  //
  EfiBootManagerUpdateConsoleVariable(
      ConIn, (EFI_DEVICE_PATH_PROTOCOL *)&mUsbKeyboard, NULL);

  //
  // Add the internal keypad (and any other text input) device paths to ConIn.
  // Never treat a protocol GUID as a device path, and never index the handle
  // buffer without checking its count.
  //
  {
    EFI_HANDLE *Handles = NULL;
    UINTN       NoHandles = 0;
    UINTN       Idx;
    EFI_STATUS  Status;

    Status = gBS->LocateHandleBuffer(
        ByProtocol, &gEFIDroidKeypadDeviceProtocolGuid, NULL, &NoHandles,
        &Handles);
    DEBUG((DEBUG_ERROR, "PlatformBm: keypad handles=%u status=%r\n", NoHandles, Status));
    if (!EFI_ERROR(Status) && NoHandles > 0 && Handles != NULL) {
      for (Idx = 0; Idx < NoHandles; Idx++) {
        EFI_DEVICE_PATH_PROTOCOL *DevicePath = DevicePathFromHandle(Handles[Idx]);
        if (DevicePath != NULL) {
          EfiBootManagerUpdateConsoleVariable(ConIn, DevicePath, NULL);
        }
      }
      gBS->FreePool(Handles);
    }

    Handles   = NULL;
    NoHandles = 0;
    Status = gBS->LocateHandleBuffer(
        ByProtocol, &gEfiSimpleTextInputExProtocolGuid, NULL, &NoHandles,
        &Handles);
    if (!EFI_ERROR(Status) && NoHandles > 0 && Handles != NULL) {
      for (Idx = 0; Idx < NoHandles; Idx++) {
        EFI_DEVICE_PATH_PROTOCOL *DevicePath = DevicePathFromHandle(Handles[Idx]);
        if (DevicePath != NULL) {
          EfiBootManagerUpdateConsoleVariable(ConIn, DevicePath, NULL);
        }
      }
      gBS->FreePool(Handles);
    }
  }

  //
  // Add the hardcoded serial console device path to ConIn, ConOut, ErrOut.
  //
  ASSERT(FixedPcdGet8(PcdDefaultTerminalType) == 4);
  CopyGuid(&mSerialConsole.TermType.Guid, &gEfiTtyTermGuid);

  EfiBootManagerUpdateConsoleVariable(
      ConIn, (EFI_DEVICE_PATH_PROTOCOL *)&mSerialConsole, NULL);
  EfiBootManagerUpdateConsoleVariable(
      ConOut, (EFI_DEVICE_PATH_PROTOCOL *)&mSerialConsole, NULL);
  EfiBootManagerUpdateConsoleVariable(
      ErrOut, (EFI_DEVICE_PATH_PROTOCOL *)&mSerialConsole, NULL);

  //
  // Register platform-specific boot options and keyboard shortcuts.
  //
  PlatformRegisterOptionsAndKeys();
  DEBUG((DEBUG_ERROR, "PlatformBm: BeforeConsole done\n"));
}

STATIC
VOID HandleCapsules(VOID)
{
  ESRT_MANAGEMENT_PROTOCOL *EsrtManagement;
  EFI_PEI_HOB_POINTERS      HobPointer;
  EFI_CAPSULE_HEADER *      CapsuleHeader;
  BOOLEAN                   NeedReset;
  EFI_STATUS                Status;

  DEBUG((DEBUG_INFO, "%a: processing capsules ...\n", __FUNCTION__));

  Status = gBS->LocateProtocol(
      &gEsrtManagementProtocolGuid, NULL, (VOID **)&EsrtManagement);
  if (!EFI_ERROR(Status)) {
    EsrtManagement->SyncEsrtFmp();
  }

  //
  // Find all capsule images from hob
  //
  HobPointer.Raw = GetHobList();
  NeedReset      = FALSE;
  while ((HobPointer.Raw =
              GetNextHob(EFI_HOB_TYPE_UEFI_CAPSULE, HobPointer.Raw)) != NULL) {
    CapsuleHeader = (VOID *)(UINTN)HobPointer.Capsule->BaseAddress;

    Status = ProcessCapsuleImage(CapsuleHeader);
    if (EFI_ERROR(Status)) {
      DEBUG(
          (DEBUG_ERROR, "%a: failed to process capsule %p - %r\n", __FUNCTION__,
           CapsuleHeader, Status));
      return;
    }

    NeedReset      = TRUE;
    HobPointer.Raw = GET_NEXT_HOB(HobPointer);
  }

  if (NeedReset) {
    DEBUG(
        (DEBUG_WARN, "%a: capsule update successful, resetting ...\n",
         __FUNCTION__));

    gRT->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
    CpuDeadLoop();
  }
}

#define VERSION_STRING_PREFIX L"Tianocore/EDK2 firmware version "

/**
  Do the platform specific action after the console is ready
  Possible things that can be done in PlatformBootManagerAfterConsole:
  > Console post action:
    > Dynamically switch output mode from 100x31 to 80x25 for certain scenario
    > Signal console ready platform customized event
  > Run diagnostics like memory testing
  > Connect certain devices
  > Dispatch additional option roms
  > Special boot: e.g.: USB boot, enter UI
**/
VOID EFIAPI PlatformBootManagerAfterConsole(VOID)
{
  EFI_STATUS                    Status;

  DEBUG((DEBUG_ERROR, "PlatformBm: AfterConsole enter\n"));
  UINTN                         FirmwareVerLength;
  UINTN                         PosX;
  UINTN                         PosY;

  FirmwareVerLength = StrLen(PcdGetPtr(PcdFirmwareVersionString));

  //
  // Show the splash screen.
  //
  //
  // Keep the text console (do not switch to the possibly-black boot logo) so the
  // boot menu stays readable on this device.
  //
  if (FirmwareVerLength > 0) {
    Print(VERSION_STRING_PREFIX L"%s\n", PcdGetPtr(PcdFirmwareVersionString));
  }
  Print(L"Home/Power=Enter (hold=Esc), Vol+=Down (hold=Up), Vol-=Down\n");

  //
  // Connect the rest of the devices.
  //
  EfiBootManagerConnectAll();
  DEBUG((DEBUG_ERROR, "PlatformBm: ConnectAll done\n"));

  //
  // On ARM, there is currently no reason to use the phased capsule
  // update approach where some capsules are dispatched before EndOfDxe
  // and some are dispatched after. So just handle all capsules here,
  // when the console is up and we can actually give the user some
  // feedback about what is going on.
  //
  HandleCapsules();

  //
  // Install the refresh filter before enumerating anything.  UiApp calls
  // EfiBootManagerRefreshAllBootOption() again when it opens the boot menu,
  // so filtering only here would let the auto entries come back.
  //
  if (mPlatformBootManagerHandle == NULL) {
    Status = gBS->InstallProtocolInterface(
        &mPlatformBootManagerHandle,
        &gEdkiiPlatformBootManagerProtocolGuid,
        EFI_NATIVE_INTERFACE,
        &mPlatformBootManagerProtocol);
    DEBUG((DEBUG_ERROR, "PlatformBm: install refresh filter: %r\n", Status));
  }

  //
  // Enumerate all possible boot options, then replace the auto-generated
  // ones with the entries this device should offer.
  //
  EfiBootManagerRefreshAllBootOption();
  PlatformLogBootOptions("after refresh");
  PlatformRemoveAutoBootOptions();
  PlatformRegisterMmcBootOptions();

  //
  // Register UEFI Shell last so the menu order is eMMC, SD Card, Shell.
  //
  PlatformDeleteBootOptionByDescription(L"UEFI Shell");
  PlatformRegisterFvBootOption(
      &gUefiShellFileGuid, L"UEFI Shell", LOAD_OPTION_ACTIVE);
  PlatformLogBootOptions("final");

  //
  // Only now start polling the side buttons. Doing this during DXE or
  // EfiBootManagerConnectAll() hangs MMCHS/console initialization.
  //
  {
    PLATFORM_KEYPAD_POLL_PROTOCOL *KeypadPoll = NULL;
    DEBUG((DEBUG_ERROR, "PlatformBm: AfterConsole done, starting keypad poll\n"));
    if (!EFI_ERROR(gBS->LocateProtocol(&gPlatformKeypadPollProtocolGuid, NULL,
                                       (VOID **)&KeypadPoll)) &&
        KeypadPoll != NULL && KeypadPoll->StartPoll != NULL) {
      KeypadPoll->StartPoll();
    }

    // The keypad is the only real keyboard, and the HII front page reads
    // gST->ConIn directly. Point the system console input at it explicitly
    // instead of depending on ConSplitter and ConPlatform variable plumbing.
    {
      STATIC CONST EFI_GUID KeypadDpGuid = {
        0x5d1c2c7c, 0x8e64, 0x4c1b,
        { 0x9a, 0x8f, 0x2c, 0x5a, 0x1b, 0x3f, 0x6d, 0x40 }
      };
      EFI_HANDLE                      *Handles = NULL;
      UINTN                            NoHandles = 0;
      UINTN                            Idx;
      EFI_HANDLE                       KeypadHandle = NULL;
      EFI_DEVICE_PATH_PROTOCOL        *KeypadDp = NULL;
      EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *KeypadTextIn = NULL;

      if (!EFI_ERROR(gBS->LocateHandleBuffer(
              ByProtocol, &gEfiSimpleTextInputExProtocolGuid, NULL,
              &NoHandles, &Handles)) &&
          Handles != NULL) {
        for (Idx = 0; Idx < NoHandles; Idx++) {
          EFI_DEVICE_PATH_PROTOCOL *DevicePath = DevicePathFromHandle(Handles[Idx]);

          if ((DevicePath != NULL) &&
              (DevicePathType(DevicePath) == HARDWARE_DEVICE_PATH) &&
              (DevicePathSubType(DevicePath) == HW_VENDOR_DP) &&
              CompareGuid(&((VENDOR_DEVICE_PATH *)DevicePath)->Guid,
                          &KeypadDpGuid)) {
            KeypadHandle = Handles[Idx];
            KeypadDp     = DevicePath;
            break;
          }
        }
        gBS->FreePool(Handles);
      }

      DEBUG((DEBUG_ERROR, "PlatformBm: keypad handle=%p dp=%p\n",
             KeypadHandle, KeypadDp));

      if ((KeypadHandle != NULL) && (KeypadDp != NULL) &&
          !EFI_ERROR(gBS->HandleProtocol(
                         KeypadHandle, &gEfiSimpleTextInProtocolGuid,
                         (VOID **)&KeypadTextIn))) {
        EFI_STATUS Status;

        Status = EfiBootManagerUpdateConsoleVariable(ConIn, KeypadDp, NULL);
        DEBUG((DEBUG_ERROR, "PlatformBm: add keypad to ConIn variable: %r\n",
               Status));

        Status = gBS->ConnectController(KeypadHandle, NULL, NULL, TRUE);
        DEBUG((DEBUG_ERROR, "PlatformBm: keypad ConnectController: %r\n",
               Status));

        {
          EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *Info = NULL;
          UINTN                                Count = 0;
          UINTN                                InfoIdx;

          Status = gBS->OpenProtocolInformation(
                           KeypadHandle, &gEfiConsoleInDeviceGuid,
                           &Info, &Count);
          DEBUG((DEBUG_ERROR,
                 "PlatformBm: keypad ConsoleIn opens status=%r count=%u\n",
                 Status, Count));
          if (!EFI_ERROR(Status) && (Info != NULL)) {
            for (InfoIdx = 0; InfoIdx < Count; InfoIdx++) {
              DEBUG((DEBUG_ERROR,
                     "PlatformBm:  open agent=%p controller=%p attrs=0x%x\n",
                     Info[InfoIdx].AgentHandle,
                     Info[InfoIdx].ControllerHandle,
                     Info[InfoIdx].Attributes));
            }
            FreePool(Info);
          }
        }

        // Clear anything queued while booting, then make the keypad the
        // system console input. This bypasses ConSplitter entirely.
        KeypadTextIn->Reset(KeypadTextIn, FALSE);
        DEBUG((DEBUG_ERROR,
               "PlatformBm: ConIn old handle=%p protocol=%p -> keypad handle=%p protocol=%p\n",
               gST->ConsoleInHandle, gST->ConIn, KeypadHandle, KeypadTextIn));
        gST->ConsoleInHandle = KeypadHandle;
        gST->ConIn           = KeypadTextIn;
      }
    }
    (void)KeypadPoll;
  }
}

/**
  This function is called each second during the boot manager waits the
  timeout.

  @param TimeoutRemain  The remaining timeout.
**/
VOID EFIAPI PlatformBootManagerWaitCallback(UINT16 TimeoutRemain)
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL_UNION Black;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL_UNION White;
  UINT16                              Timeout;
  EFI_STATUS                          Status;

  Timeout = PcdGet16(PcdPlatformBootTimeOut);
  DEBUG((DEBUG_ERROR, "PlatformBm: wait callback remain=%u timeout=%u\n",
         TimeoutRemain, Timeout));

  {
    PLATFORM_KEYPAD_POLL_PROTOCOL *KeypadPoll = NULL;

    if (!EFI_ERROR(gBS->LocateProtocol(&gPlatformKeypadPollProtocolGuid, NULL,
                                       (VOID **)&KeypadPoll)) &&
        (KeypadPoll != NULL) && (KeypadPoll->PollPmicKeys != NULL)) {
      KeypadPoll->PollPmicKeys();
    }
  }

  if (Timeout != 0 && TimeoutRemain <= 0) {
    // Do not clear the screen / re-enable the (possibly black) logo; keep the
    // boot menu text visible.
    Print(L"\n");
    return;
  }

  Black.Raw = 0x00000000;
  White.Raw = 0x00FFFFFF;

  Status = BootLogoUpdateProgress(
      White.Pixel, Black.Pixel,
      L"Press any side button for Boot Options",
      White.Pixel, (Timeout - TimeoutRemain) * 100 / Timeout, 0);
  if (EFI_ERROR(Status)) {
    Print(L".");
  }
}

/**
  The function is called when no boot option could be launched,
  including platform recovery options and options pointing to applications
  built into firmware volumes.

  If this function returns, BDS attempts to enter an infinite loop.
**/
VOID EFIAPI PlatformBootManagerUnableToBoot(VOID) { return; }
