/** @file
  Implementation for PlatformBootManagerLib library class interfaces.

  SPDX-FileCopyrightText: Copyright (c) 2020-2025, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
  Copyright (C) 2015-2016, Red Hat, Inc.
  Copyright (c) 2014, ARM Ltd. All rights reserved.<BR>
  Copyright (c) 2004 - 2018, Intel Corporation. All rights reserved.<BR>
  Copyright (c) 2016, Linaro Ltd. All rights reserved.<BR>

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <IndustryStandard/Pci22.h>
#include <Library/BootLogoLib.h>
#include <Library/BaseLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/HobLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/PlatformBootOrderLib.h>
#include <Library/BaseCryptLib.h>
#include <Library/PlatformResourceLib.h>
#include <Library/PrintLib.h>
#include <Library/DtPlatformDtbLoaderLib.h>
#include <Library/NVIDIADebugLib.h>
#include <Library/TimerLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/GenericMemoryTest.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/IpmiTransportProtocol.h>
#include <Protocol/MemoryTestConfig.h>
#include <Protocol/PciIo.h>
#include <Protocol/PciRootBridgeIo.h>
#include <Protocol/PlatformBootManager.h>
#include <Guid/EventGroup.h>
#include <Guid/GlobalVariable.h>
#include <Guid/RtPropertiesTable.h>
#include <Guid/TtyTerm.h>
#include <Guid/SerialPortLibVendor.h>
#include <IndustryStandard/Ipmi.h>
#include <libfdt.h>
#include "PlatformBm.h"
#include <NVIDIAConfiguration.h>

#define DP_NODE_LEN(Type)  { (UINT8)sizeof (Type), (UINT8)(sizeof (Type) >> 8) }

STATIC PLATFORM_CONFIGURATION_DATA  CurrentPlatformConfigData;
/**
  Check if the handle satisfies a particular condition.

  @param[in] Handle      The handle to check.
  @param[in] ReportText  A caller-allocated string passed in for reporting
                         purposes. It must never be NULL.

  @retval TRUE   The condition is satisfied.
  @retval FALSE  Otherwise. This includes the case when the condition could not
                 be fully evaluated due to an error.
**/

typedef
BOOLEAN
(EFIAPI *FILTER_FUNCTION)(
  IN EFI_HANDLE   Handle,
  IN CONST CHAR16 *ReportText
  );

/**
  Process a handle.

  @param[in] Handle      The handle to process.
  @param[in] ReportText  A caller-allocated string passed in for reporting
                         purposes. It must never be NULL.
**/

typedef
VOID
(EFIAPI *CALLBACK_FUNCTION)(
  IN EFI_HANDLE   Handle,
  IN CONST CHAR16 *ReportText
  );

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
VOID
FilterAndProcess (
  IN EFI_GUID           *ProtocolGuid,
  IN FILTER_FUNCTION    Filter         OPTIONAL,
  IN CALLBACK_FUNCTION  Process
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles;
  UINTN       NoHandles;
  UINTN       Idx;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  ProtocolGuid,
                  NULL /* SearchKey */,
                  &NoHandles,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    //
    // This is not an error, just an informative condition.
    //
    DEBUG ((
      EFI_D_VERBOSE,
      "%a: %g: %r\n",
      __FUNCTION__,
      ProtocolGuid,
      Status
      ));
    return;
  }

  ASSERT (NoHandles > 0);
  for (Idx = 0; Idx < NoHandles; ++Idx) {
    CHAR16         *DevicePathText;
    STATIC CHAR16  Fallback[] = L"<device path unavailable>";

    //
    // The ConvertDevicePathToText() function handles NULL input transparently.
    //
    DevicePathText = ConvertDevicePathToText (
                       DevicePathFromHandle (Handles[Idx]),
                       FALSE, // DisplayOnly
                       FALSE  // AllowShortcuts
                       );
    if (DevicePathText == NULL) {
      DevicePathText = Fallback;
    }

    if ((Filter == NULL) || Filter (Handles[Idx], DevicePathText)) {
      Process (Handles[Idx], DevicePathText);
    }

    if (DevicePathText != Fallback) {
      FreePool (DevicePathText);
    }
  }

  gBS->FreePool (Handles);
}

/**
  Perform the memory test base on the memory test intensive level,
  and update the memory resource.

  @retval EFI_STATUS    Success test all the system memory and update
                        the memory resource

**/
EFI_STATUS
MemoryTest (
  VOID
  )
{
  EFI_STATUS                          Status;
  EFI_STATUS                          KeyStatus;
  BOOLEAN                             RequireSoftECCInit;
  EFI_GENERIC_MEMORY_TEST_PROTOCOL    *GenMemoryTest;
  UINT64                              TestedMemorySize;
  UINT64                              TotalMemorySize;
  BOOLEAN                             ErrorOut;
  BOOLEAN                             TestAbort;
  EFI_INPUT_KEY                       Key;
  EXTENDMEM_COVERAGE_LEVEL            Level;
  UINT64                              StartTime;
  UINT64                              EndTime;
  UINT64                              TimeTaken;
  NVIDIA_MEMORY_TEST_OPTIONS          *MemoryTestOptions;
  UINTN                               SizeOfBuffer;
  UINT8                               Iteration;
  NVIDIA_MEMORY_TEST_CONFIG_PROTOCOL  *TestConfig;
  CONST CHAR8                         *TestName;

  TestedMemorySize   = 0;
  TotalMemorySize    = 0;
  ErrorOut           = FALSE;
  TestAbort          = FALSE;
  RequireSoftECCInit = FALSE;
  ZeroMem (&Key, sizeof (EFI_INPUT_KEY));

  MemoryTestOptions = PcdGetPtr (PcdMemoryTest);
  NV_ASSERT_RETURN (MemoryTestOptions != NULL, return EFI_DEVICE_ERROR, "Failed to get memory test info\r\n");
  Level = MemoryTestOptions->TestLevel;

  Status = gBS->LocateProtocol (
                  &gEfiGenericMemTestProtocolGuid,
                  NULL,
                  (VOID **)&GenMemoryTest
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to find memory test protocol\r\n"));
    return EFI_SUCCESS;
  }

  Status = gBS->LocateProtocol (
                  &gNVIDIAMemoryTestConfig,
                  NULL,
                  (VOID **)&TestConfig
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to find gNVIDIAMemoryTestConfig protocol\r\n"));
    return EFI_SUCCESS;
  }

  if ((MemoryTestOptions->TestIterations < 0) ||
      (MemoryTestOptions->TestIterations > MAX_UINT8))
  {
    DEBUG ((DEBUG_ERROR, "TestIterations out of bounds\r\n"));
    return EFI_SUCCESS;
  }

  for (Iteration = 0; Iteration < MemoryTestOptions->TestIterations; Iteration++) {
    for (TestConfig->TestMode = MemoryTestWalking1Bit;
         TestConfig->TestMode < MemoryTestMaxTest;
         TestConfig->TestMode++)
    {
      switch (TestConfig->TestMode) {
        case MemoryTestWalking1Bit:
          if (!MemoryTestOptions->Walking1BitEnabled) {
            continue;
          }

          TestName = "Walking 1 bit";

          break;
        case MemoryTestAddressCheck:
          if (!MemoryTestOptions->AddressCheckEnabled) {
            continue;
          }

          TestName = "Address Check";

          break;
        case MemoryTestMovingInversions01:
          if (!MemoryTestOptions->MovingInversions01Enabled) {
            continue;
          }

          TestName = "Moving inversions, ones&zeros";

          break;
        case MemoryTestMovingInversions8Bit:
          if (!MemoryTestOptions->MovingInversions8BitEnabled) {
            continue;
          }

          TestName = "Moving inversions, 8 bit pattern";
          break;

        case MemoryTestMovingInversionsRandom:
          if (!MemoryTestOptions->MovingInversionsRandomEnabled) {
            continue;
          }

          TestName = "Moving inversions, random pattern";
          break;

        /*
                case MemoryTestBlockMode:
                  if (!MemoryTestOptions->BlockMoveEnabled) {
                    continue;
                  }

                  TestName = "Block move, 64 moves";
                  break;
        */
        case MemoryTestMovingInversions64Bit:
          if (!MemoryTestOptions->MovingInversions64BitEnabled) {
            continue;
          }

          TestName = "Moving inversions, 64 bit pattern";
          break;
        case MemoryTestRandomNumberSequence:
          if (!MemoryTestOptions->RandomNumberSequenceEnabled) {
            continue;
          }

          TestName = "Random number sequence";
          break;
        case MemoryTestModulo20Random:
          if (!MemoryTestOptions->Modulo20RandomEnabled) {
            continue;
          }

          TestName = "Modulo 20, random pattern";
          break;
        case MemoryTestBitFadeTest:
          if (!MemoryTestOptions->BitFadeEnabled) {
            continue;
          }

          TestName               = "Bit Fade";
          TestConfig->Parameter1 = MemoryTestOptions->BitFadePattern;
          TestConfig->Parameter2 = MemoryTestOptions->BitFadeWait;

          break;
        default:
          continue;
      }

      Print (L"[%03u] %a test starting\r\n", Iteration+1, TestName);
      Status = GenMemoryTest->MemoryTestInit (
                                GenMemoryTest,
                                Level,
                                &RequireSoftECCInit
                                );
      if (Status == EFI_NO_MEDIA) {
        //
        // The PEI codes also have the relevant memory test code to check the memory,
        // it can select to test some range of the memory or all of them. If PEI code
        // checks all the memory, this BDS memory test will has no not-test memory to
        // do the test, and then the status of EFI_NO_MEDIA will be returned by
        // "MemoryTestInit". So it does not need to test memory again, just return.
        //
        return EFI_SUCCESS;
      }

      if (MemoryTestOptions->NextBoot) {
        // Disable watchdog as memory tests can take a while.
        gBS->SetWatchdogTimer (0, 0, 0, NULL);
        StartTime = GetTimeInNanoSecond (GetPerformanceCounter ());
        Print (L"Perform memory test (ESC to skip).\r\n");

        do {
          Status = GenMemoryTest->PerformMemoryTest (
                                    GenMemoryTest,
                                    &TestedMemorySize,
                                    &TotalMemorySize,
                                    &ErrorOut,
                                    TestAbort
                                    );
          NV_ASSERT_RETURN (
            !(ErrorOut && (Status == EFI_DEVICE_ERROR)),
            return EFI_DEVICE_ERROR,
            "Memory Testing failed!\r\n"
            );

          Print (L"[%03u] Tested %8lld MB/%8lld MB\r", Iteration+1, TestedMemorySize / SIZE_1MB, TotalMemorySize / SIZE_1MB);

          if (!PcdGetBool (PcdConInConnectOnDemand)) {
            KeyStatus = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
            if (!EFI_ERROR (KeyStatus) && (Key.ScanCode == SCAN_ESC)) {
              if (!RequireSoftECCInit) {
                break;
              }

              TestAbort = TRUE;
            }
          }
        } while (Status != EFI_NOT_FOUND);

        EndTime   = GetTimeInNanoSecond (GetPerformanceCounter ());
        TimeTaken = EndTime - StartTime;
        Print (L"\r\n%llu bytes of system memory tested OK in %llu ms\r\n", TotalMemorySize, TimeTaken/1000000);
      }

      if (TestAbort) {
        break;
      }
    }

    if (TestAbort) {
      break;
    }
  }

  if (MemoryTestOptions->SingleBoot) {
    MemoryTestOptions->NextBoot = FALSE;
    SizeOfBuffer                = sizeof (NVIDIA_MEMORY_TEST_OPTIONS);
    PcdSetPtrS (PcdMemoryTest, &SizeOfBuffer, MemoryTestOptions);
  }

  Status = GenMemoryTest->Finished (GenMemoryTest);

  return EFI_SUCCESS;
}

/**
  This CALLBACK_FUNCTION attempts to connect a handle non-recursively, asking
  the matching driver to produce all first-level child handles.
**/
STATIC
VOID
EFIAPI
Connect (
  IN EFI_HANDLE    Handle,
  IN CONST CHAR16  *ReportText
  )
{
  EFI_STATUS  Status;

  Status = gBS->ConnectController (
                  Handle, // ControllerHandle
                  NULL,   // DriverImageHandle
                  NULL,   // RemainingDevicePath -- produce all children
                  FALSE   // Recursive
                  );
  DEBUG ((
    EFI_ERROR (Status) ? EFI_D_ERROR : EFI_D_VERBOSE,
    "%a: %s: %r\n",
    __FUNCTION__,
    ReportText,
    Status
    ));
}

STATIC
VOID
PlatformRegisterFvBootOption (
  CONST EFI_GUID                     *FileGuid,
  CHAR16                             *Description,
  UINT32                             Attributes,
  EFI_BOOT_MANAGER_LOAD_OPTION_TYPE  LoadOptionType
  )
{
  EFI_STATUS                         Status;
  INTN                               OptionIndex;
  EFI_BOOT_MANAGER_LOAD_OPTION       NewOption;
  EFI_BOOT_MANAGER_LOAD_OPTION       *BootOptions;
  UINTN                              BootOptionCount;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH  FileNode;
  EFI_LOADED_IMAGE_PROTOCOL          *LoadedImage;
  EFI_DEVICE_PATH_PROTOCOL           *DevicePath;

  Status = gBS->HandleProtocol (
                  gImageHandle,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **)&LoadedImage
                  );
  ASSERT_EFI_ERROR (Status);

  EfiInitializeFwVolDevicepathNode (&FileNode, FileGuid);
  DevicePath = DevicePathFromHandle (LoadedImage->DeviceHandle);
  ASSERT (DevicePath != NULL);
  DevicePath = AppendDevicePathNode (
                 DevicePath,
                 (EFI_DEVICE_PATH_PROTOCOL *)&FileNode
                 );
  ASSERT (DevicePath != NULL);

  Status = EfiBootManagerInitializeLoadOption (
             &NewOption,
             LoadOptionNumberUnassigned,
             LoadOptionType,
             Attributes,
             Description,
             DevicePath,
             NULL,
             0
             );
  ASSERT_EFI_ERROR (Status);
  FreePool (DevicePath);

  BootOptions = EfiBootManagerGetLoadOptions (
                  &BootOptionCount,
                  LoadOptionType
                  );

  OptionIndex = EfiBootManagerFindLoadOption (
                  &NewOption,
                  BootOptions,
                  BootOptionCount
                  );

  if (OptionIndex == -1) {
    Status = EfiBootManagerAddLoadOptionVariable (&NewOption, MAX_UINTN);
    ASSERT_EFI_ERROR (Status);
  }

  EfiBootManagerFreeLoadOption (&NewOption);
  EfiBootManagerFreeLoadOptions (BootOptions, BootOptionCount);
}

STATIC
VOID
GetPlatformOptions (
  VOID
  )
{
  EFI_STATUS                      Status;
  EFI_BOOT_MANAGER_LOAD_OPTION    *CurrentBootOptions;
  EFI_BOOT_MANAGER_LOAD_OPTION    *BootOptions;
  EFI_INPUT_KEY                   *BootKeys;
  PLATFORM_BOOT_MANAGER_PROTOCOL  *PlatformBootManager;
  UINTN                           CurrentBootOptionCount;
  UINTN                           Index;
  UINTN                           BootCount;

  Status = gBS->LocateProtocol (
                  &gPlatformBootManagerProtocolGuid,
                  NULL,
                  (VOID **)&PlatformBootManager
                  );
  if (EFI_ERROR (Status)) {
    return;
  }

  Status = PlatformBootManager->GetPlatformBootOptionsAndKeys (
                                  &BootCount,
                                  &BootOptions,
                                  &BootKeys
                                  );
  if (EFI_ERROR (Status)) {
    return;
  }

  //
  // Fetch the existent boot options. If there are none, CurrentBootCount
  // will be zeroed.
  //
  CurrentBootOptions = EfiBootManagerGetLoadOptions (
                         &CurrentBootOptionCount,
                         LoadOptionTypeBoot
                         );
  //
  // Process the platform boot options.
  //
  for (Index = 0; Index < BootCount; Index++) {
    INTN   Match;
    UINTN  BootOptionNumber;

    //
    // If there are any preexistent boot options, and the subject platform boot
    // option is already among them, then don't try to add it. Just get its
    // assigned boot option number so we can associate a hotkey with it. Note
    // that EfiBootManagerFindLoadOption() deals fine with (CurrentBootOptions
    // == NULL) if (CurrentBootCount == 0).
    //
    Match = EfiBootManagerFindLoadOption (
              &BootOptions[Index],
              CurrentBootOptions,
              CurrentBootOptionCount
              );
    if (Match >= 0) {
      BootOptionNumber = CurrentBootOptions[Match].OptionNumber;
    } else {
      //
      // Add the platform boot options as a new one, at the end of the boot
      // order. Note that if the platform provided this boot option with an
      // unassigned option number, then the below function call will assign a
      // number.
      //
      Status = EfiBootManagerAddLoadOptionVariable (
                 &BootOptions[Index],
                 MAX_UINTN
                 );
      if (EFI_ERROR (Status)) {
        DEBUG ((
          DEBUG_ERROR,
          "%a: failed to register \"%s\": %r\n",
          __FUNCTION__,
          BootOptions[Index].Description,
          Status
          ));
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

    Status = EfiBootManagerAddKeyOptionVariable (
               NULL,
               BootOptionNumber,
               0,
               &BootKeys[Index],
               NULL
               );
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "%a: failed to register hotkey for \"%s\": %r\n",
        __FUNCTION__,
        BootOptions[Index].Description,
        Status
        ));
    }
  }

  EfiBootManagerFreeLoadOptions (CurrentBootOptions, CurrentBootOptionCount);
  EfiBootManagerFreeLoadOptions (BootOptions, BootCount);
  FreePool (BootKeys);
}

/**
  Check if it's a Device Path pointing to BootManagerMenuApp.

  @param  DevicePath     Input device path.

  @retval TRUE   The device path is BootManagerMenuApp File Device Path.
  @retval FALSE  The device path is NOT BootManagerMenuApp File Device Path.
**/
BOOLEAN
IsBootManagerMenuAppFilePath (
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath
  )
{
  EFI_HANDLE  FvHandle;
  VOID        *NameGuid;
  EFI_STATUS  Status;

  Status = gBS->LocateDevicePath (&gEfiFirmwareVolume2ProtocolGuid, &DevicePath, &FvHandle);
  if (!EFI_ERROR (Status)) {
    NameGuid = EfiGetNameGuidFromFwVolDevicePathNode ((CONST MEDIA_FW_VOL_FILEPATH_DEVICE_PATH *)DevicePath);
    if (NameGuid != NULL) {
      return CompareGuid (NameGuid, PcdGetPtr (PcdBootMenuAppFile));
    }
  }

  return FALSE;
}

/**
  Register boot option for boot menu app and return its boot option instance.

  @param[out]  BootOption     Boot option of boot menu app.

  @retval EFI_SUCCESS             Boot option of boot menu app is registered.
  @retval EFI_NOT_FOUND           No boot menu app is found.
  @retval EFI_INVALID_PARAMETER   BootOption is NULL.
  @retval Others                  Error occurs.
**/
EFI_STATUS
BmRegisterBootMenuApp (
  OUT EFI_BOOT_MANAGER_LOAD_OPTION  *BootOption
  )
{
  EFI_STATUS                Status;
  CHAR16                    *Description;
  UINTN                     DescriptionLength;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  UINTN                     HandleCount;
  EFI_HANDLE                *Handles;
  UINTN                     Index;

  if (BootOption == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  HandleCount = 0;
  Handles     = NULL;
  DevicePath  = NULL;
  Description = NULL;

  //
  // Try to find BootMenu from LoadFile protocol
  //
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiLoadFileProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (!EFI_ERROR (Status)) {
    for (Index = 0; Index < HandleCount; Index++) {
      if (IsBootManagerMenuAppFilePath (DevicePathFromHandle (Handles[Index]))) {
        DevicePath  = DuplicateDevicePath (DevicePathFromHandle (Handles[Index]));
        Description = BmGetBootDescription (Handles[Index]);
        break;
      }
    }

    if (HandleCount != 0) {
      FreePool (Handles);
      Handles = NULL;
    }
  }

  //
  // Not found in LoadFile protocol. Search FV.
  //
  if (DevicePath == NULL) {
    Status = GetFileDevicePathFromAnyFv (
               PcdGetPtr (PcdBootMenuAppFile),
               EFI_SECTION_PE32,
               0,
               &DevicePath
               );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN, "%a: [Bds]Boot Menu App FFS section can not be found, skip its boot option registration\n", __FUNCTION__));
      return EFI_NOT_FOUND;
    }

    ASSERT (DevicePath != NULL);
    //
    // Get BootManagerMenu application's description from EFI User Interface Section.
    //
    Status = GetSectionFromAnyFv (
               PcdGetPtr (PcdBootMenuAppFile),
               EFI_SECTION_USER_INTERFACE,
               0,
               (VOID **)&Description,
               &DescriptionLength
               );
    if (EFI_ERROR (Status)) {
      if (Description != NULL) {
        FreePool (Description);
        Description = NULL;
      }
    }
  }

  //
  // Create new boot option
  //
  Status = EfiBootManagerInitializeLoadOption (
             BootOption,
             LoadOptionNumberUnassigned,
             LoadOptionTypeBoot,
             LOAD_OPTION_CATEGORY_APP | LOAD_OPTION_ACTIVE | LOAD_OPTION_HIDDEN,
             (Description != NULL) ? Description : L"Boot Manager Menu",
             DevicePath,
             NULL,
             0
             );
  ASSERT_EFI_ERROR (Status);

  //
  // Release resource
  //
  if (DevicePath != NULL) {
    FreePool (DevicePath);
  }

  if (Description != NULL) {
    FreePool (Description);
  }

  DEBUG_CODE (
    EFI_BOOT_MANAGER_LOAD_OPTION    *BootOptions;
    UINTN                           BootOptionCount;

    BootOptions = EfiBootManagerGetLoadOptions (&BootOptionCount, LoadOptionTypeBoot);
    ASSERT (EfiBootManagerFindLoadOption (BootOption, BootOptions, BootOptionCount) == -1);
    EfiBootManagerFreeLoadOptions (BootOptions, BootOptionCount);
    );

  return EfiBootManagerAddLoadOptionVariable (BootOption, (UINTN)-1);
}

/**
  Return the boot option number to the boot menu app. If not found it in the
  current boot option, create a new one.

  @param[out] BootOption  Pointer to boot menu app boot option.

  @retval EFI_SUCCESS   Boot option of boot menu app is found and returned.
  @retval Others        Error occurs.

**/
EFI_STATUS
EfiBootManagerGetBootMenuApp (
  OUT EFI_BOOT_MANAGER_LOAD_OPTION  *BootOption
  )
{
  EFI_STATUS                    Status;
  UINTN                         BootOptionCount;
  EFI_BOOT_MANAGER_LOAD_OPTION  *BootOptions;
  UINTN                         Index;

  BootOptions = EfiBootManagerGetLoadOptions (&BootOptionCount, LoadOptionTypeBoot);

  for (Index = 0; Index < BootOptionCount; Index++) {
    if (IsBootManagerMenuAppFilePath (BootOptions[Index].FilePath)) {
      Status = EfiBootManagerInitializeLoadOption (
                 BootOption,
                 BootOptions[Index].OptionNumber,
                 BootOptions[Index].OptionType,
                 BootOptions[Index].Attributes,
                 BootOptions[Index].Description,
                 BootOptions[Index].FilePath,
                 BootOptions[Index].OptionalData,
                 BootOptions[Index].OptionalDataSize
                 );
      ASSERT_EFI_ERROR (Status);
      break;
    }
  }

  EfiBootManagerFreeLoadOptions (BootOptions, BootOptionCount);

  //
  // Automatically create the Boot#### for Boot Menu App when not found.
  //
  if (Index >= BootOptionCount) {
    return BmRegisterBootMenuApp (BootOption);
  }

  return EFI_SUCCESS;
}

/**
  Register the platform boot options and its hotkeys.

  Supported hotkey:
    ENTER: continue boot
    ESC:   Boot manager menu
    F11:   Boot menu app

**/
VOID
PlatformRegisterOptionsAndKeys (
  VOID
  )
{
  EFI_STATUS                    Status;
  EFI_INPUT_KEY                 Enter;

  GetPlatformOptions ();

  //
  // Register ENTER as CONTINUE key
  //
  Enter.ScanCode    = SCAN_NULL;
  Enter.UnicodeChar = CHAR_CARRIAGE_RETURN;
  Status            = EfiBootManagerRegisterContinueKeyOption (0, &Enter, NULL);
  ASSERT_EFI_ERROR (Status);
}

/**
  System information is displayed at center of screen and hotkey
  information is displayed at upper left corner when GOP is
  available.

**/
VOID
DisplaySystemAndHotkeyInformation (
  VOID
  )
{
  if (PcdGetBool (PcdTegraPrintInternalBanner)) {
    Print (L"********** FOR NVIDIA INTERNAL USE ONLY **********\n");
  }

  Print (
    L"%s UEFI firmware (version %s built on %s)\n\r",
    (CHAR16 *)PcdGetPtr (PcdPlatformFamilyName),
    (CHAR16 *)PcdGetPtr (PcdFirmwareVersionString),
    (CHAR16 *)PcdGetPtr (PcdFirmwareDateTimeBuiltString)
    );
  return;

  EFI_STATUS                     Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL   *GraphicsOutput;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Black;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  White;
  CHAR16                         Buffer[100];
  UINTN                          CharCount;
  UINTN                          PosX;
  UINTN                          PosY;
  UINTN                          StartLineX = EFI_GLYPH_WIDTH+2;
  UINTN                          StartLineY = EFI_GLYPH_HEIGHT+1;
  UINTN                          LineDeltaY = EFI_GLYPH_HEIGHT+1;

  //
  // Display hotkey information at upper left corner.
  //
  Black.Blue = Black.Green = Black.Red = Black.Reserved = 0;
  White.Blue = White.Green = White.Red = White.Reserved = 0xFF;

  //
  // Show NVIDIA Internal Banner.
  //
  if (PcdGetBool (PcdTegraPrintInternalBanner)) {
    Print (L"********** FOR NVIDIA INTERNAL USE ONLY **********\n");
  }

  //
  // firmware version.
  //
  CharCount = UnicodeSPrint (
                Buffer,
                sizeof (Buffer),
                L"%s UEFI firmware (version %s built on %s)\n\r",
                (CHAR16 *)PcdGetPtr (PcdPlatformFamilyName),
                (CHAR16 *)PcdGetPtr (PcdFirmwareVersionString),
                (CHAR16 *)PcdGetPtr (PcdFirmwareDateTimeBuiltString)
                );

  //
  // Check and see if GOP is available.
  //
  Status = gBS->HandleProtocol (
                  gST->ConsoleOutHandle,
                  &gEfiGraphicsOutputProtocolGuid,
                  (VOID **)&GraphicsOutput
                  );
  if (!EFI_ERROR (Status)) {
    //
    // Find the center position on screen.
    //
    PosX = (GraphicsOutput->Mode->Info->HorizontalResolution -
            StrLen (Buffer) * EFI_GLYPH_WIDTH) / 2;
    PosY = 0;

    PrintXY (PosX, PosY, NULL, NULL, Buffer);
    PrintXY (StartLineX, StartLineY+LineDeltaY*0, &White, &Black, L"ESC   to enter Setup.                              ");
    PrintXY (StartLineX, StartLineY+LineDeltaY*1, &White, &Black, L"F11   to enter Boot Manager Menu.");
    PrintXY (StartLineX, StartLineY+LineDeltaY*2, &White, &Black, L"Enter to continue boot.");
  }

  //
  // Serial console only.
  //
  Print (Buffer);

  //
  // If Timeout is 0, next message comes in same line as previous message.
  // Add a newline to maintain ordering and readability of logs.
  //
  if (PcdGet16 (PcdPlatformBootTimeOut) == 0) {
    Print (L"\n\r");
  }

  Print (L"ESC   to enter Setup.\n");
  Print (L"F11   to enter Boot Manager Menu.\n");
  Print (L"Enter to continue boot.\n");
}

STATIC
BOOLEAN
IsSingleBootNeeded (
  VOID
  )
{
  VOID                          *Hob;
  TEGRA_PLATFORM_RESOURCE_INFO  *PlatformResourceInfo;

  Hob = GetFirstGuidHob (&gNVIDIAPlatformResourceDataGuid);
  if ((Hob != NULL) &&
      (GET_GUID_HOB_DATA_SIZE (Hob) == sizeof (TEGRA_PLATFORM_RESOURCE_INFO)))
  {
    PlatformResourceInfo = (TEGRA_PLATFORM_RESOURCE_INFO *)GET_GUID_HOB_DATA (Hob);
  } else {
    DEBUG ((DEBUG_ERROR, "%a: Failed to get PlatformResourceInfo\r\n", __FUNCTION__));
    ASSERT (FALSE);
  }

  if (PlatformResourceInfo->BootType == TegrablBootRcm) {
    return TRUE;
  }

  return FALSE;
}

STATIC
BOOLEAN
IsPlatformConfigurationNeeded (
  VOID
  )
{
  EFI_STATUS                   Status;
  BOOLEAN                      PlatformConfigurationNeeded;
  PLATFORM_CONFIGURATION_DATA  StoredPlatformConfigData;
  UINTN                        VariableSize;
  VOID                         *DTBBase;
  UINTN                        DTBSize;
  VOID                         *AcpiBase;
  UINTN                        CharCount;
  NVIDIA_KERNEL_COMMAND_LINE   AddlCmdLine;
  UINTN                        AddlCmdLen;
  NVIDIA_KERNEL_COMMAND_LINE   AddlCmdLineLast;
  UINTN                        AddlCmdLenLast;
  UINT32                       AddlCmdLineAttributes;

  //
  // If platform has been configured already, do not do it again
  //
  PlatformConfigurationNeeded = FALSE;
  gBS->SetMem (&CurrentPlatformConfigData, sizeof (CurrentPlatformConfigData), 0);

  //
  // Get Current DTB Hash
  //
  Status = DtPlatformLoadDtb (&DTBBase, &DTBSize);
  if (!EFI_ERROR (Status)) {
    Sha256HashAll ((VOID *)DTBBase, DTBSize, CurrentPlatformConfigData.DtbHash);
  }

  //
  // Get Current UEFI Version
  //
  CharCount = AsciiSPrint (
                CurrentPlatformConfigData.UEFIVersion,
                UEFI_VERSION_STRING_SIZE,
                "%s %s",
                (CHAR16 *)PcdGetPtr (PcdFirmwareVersionString),
                (CHAR16 *)PcdGetPtr (PcdFirmwareDateTimeBuiltString)
                );

  //
  // Get OS Hardware Description
  //
  CurrentPlatformConfigData.OsHardwareDescription = OS_USE_DT;
  Status                                          = EfiGetSystemConfigurationTable (&gEfiAcpiTableGuid, &AcpiBase);
  if (!EFI_ERROR (Status)) {
    CurrentPlatformConfigData.OsHardwareDescription = OS_USE_ACPI;
  }

  //
  // Get Stored Platform Configuration Data
  //
  VariableSize = sizeof (PLATFORM_CONFIGURATION_DATA);
  Status       = gRT->GetVariable (
                        PLATFORM_CONFIG_DATA_VARIABLE_NAME,
                        &gNVIDIATokenSpaceGuid,
                        NULL,
                        &VariableSize,
                        (VOID *)&StoredPlatformConfigData
                        );
  if (EFI_ERROR (Status) ||
      (VariableSize != sizeof (PLATFORM_CONFIGURATION_DATA)))
  {
    PlatformConfigurationNeeded = TRUE;
  } else {
    if (CompareMem (&StoredPlatformConfigData, &CurrentPlatformConfigData, sizeof (PLATFORM_CONFIGURATION_DATA)) != 0) {
      PlatformConfigurationNeeded = TRUE;
    }
  }

  if (PcdGet8 (PcdQuickBootEnabled) == 0) {
    PlatformConfigurationNeeded = TRUE;
  }

  if (!PlatformConfigurationNeeded) {
    AddlCmdLen = sizeof (AddlCmdLine);
    Status     = gRT->GetVariable (L"KernelCommandLine", &gNVIDIAPublicVariableGuid, &AddlCmdLineAttributes, &AddlCmdLen, &AddlCmdLine);
    if (EFI_ERROR (Status)) {
      AddlCmdLineAttributes = EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS;
      ZeroMem (&AddlCmdLine, sizeof (AddlCmdLine));
    }

    AddlCmdLenLast = sizeof (AddlCmdLineLast);
    Status         = gRT->GetVariable (L"KernelCommandLineLast", &gNVIDIATokenSpaceGuid, NULL, &AddlCmdLenLast, &AddlCmdLineLast);
    if (EFI_ERROR (Status)) {
      ZeroMem (&AddlCmdLenLast, sizeof (AddlCmdLineLast));
    }

    if (CompareMem (&AddlCmdLine, &AddlCmdLineLast, sizeof (AddlCmdLine)) != 0) {
      PlatformConfigurationNeeded = TRUE;
      AddlCmdLenLast              = sizeof (AddlCmdLineLast);
      Status                      = gRT->SetVariable (L"KernelCommandLineLast", &gNVIDIATokenSpaceGuid, AddlCmdLineAttributes, AddlCmdLenLast, &AddlCmdLineLast);
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "%a: Failed to update stored command line %r\r\n", __FUNCTION__, Status));
      }
    }
  }

  return PlatformConfigurationNeeded;
}

STATIC
VOID
PlatformConfigured (
  VOID
  )
{
  EFI_STATUS  Status;

  Status = gRT->SetVariable (
                  PLATFORM_CONFIG_DATA_VARIABLE_NAME,
                  &gNVIDIATokenSpaceGuid,
                  EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_NON_VOLATILE,
                  sizeof (PLATFORM_CONFIGURATION_DATA),
                  &CurrentPlatformConfigData
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Error setting Platform Config data: %r\r\n", __FUNCTION__, Status));
    // TODO: Evaluate what should be done in this case.
  }
}

STATIC
VOID
DeleteBootNextVariable (
  VOID
  )
{
  EFI_STATUS  Status;

  Status = gRT->SetVariable (
                  L"BootNext",
                  &gEfiGlobalVariableGuid,
                  EFI_VARIABLE_NON_VOLATILE |
                  EFI_VARIABLE_BOOTSERVICE_ACCESS |
                  EFI_VARIABLE_RUNTIME_ACCESS,
                  0,
                  NULL
                  );
  if (!EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "%a: deleted stale BootNext override\n", __FUNCTION__));
  } else if (Status != EFI_NOT_FOUND) {
    DEBUG ((DEBUG_WARN, "%a: failed to delete BootNext override: %r\n", __FUNCTION__, Status));
  }
}

/**
  Update ConOut, ErrOut, ConIn variables to contain all available devices.
  For initial boot, all consoles are registered. Afterwards, only GOP consoles
  are registered, as external display devices are dynamically attached.

  @param[in] InitialConsoleRegistration  TRUE:  register all  available ConOut/ErrOut consoles
                                         FALSE: register just NvDisplay ConOut/ErrOut consoles
  @param  none
  @retval none
**/
STATIC
VOID
PlatformRegisterConsoles (
  BOOLEAN  InitialConsoleRegistration
  )
{
  EFI_STATUS                    Status;
  EFI_HANDLE                    *Handles;
  UINTN                         NoHandles;
  UINTN                         Count;
  EFI_DEVICE_PATH_PROTOCOL      *Interface;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;

  ASSERT (FixedPcdGet8 (PcdDefaultTerminalType) == 4);

  // Headless: Register only serial console for output, skip display/USB
  // Serial console is identified by NOT having GraphicsOutputProtocol

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleTextOutProtocolGuid,
                  NULL,
                  &NoHandles,
                  &Handles
                  );
  if (!EFI_ERROR (Status)) {
    for (Count = 0; Count < NoHandles; Count++) {
      // Check if this handle has GraphicsOutputProtocol (display device)
      Status = gBS->HandleProtocol (
                      Handles[Count],
                      &gEfiGraphicsOutputProtocolGuid,
                      (VOID **)&Gop
                      );

      if (EFI_ERROR (Status)) {
        // No GOP = serial/UART console, register it for output
        Status = gBS->HandleProtocol (
                        Handles[Count],
                        &gEfiDevicePathProtocolGuid,
                        (VOID **)&Interface
                        );
        if (!EFI_ERROR (Status)) {
          // Register serial console for output only
          EfiBootManagerUpdateConsoleVariable (ConOut, Interface, NULL);
          EfiBootManagerUpdateConsoleVariable (ErrOut, Interface, NULL);
          DEBUG ((
            DEBUG_INFO,
            "%a: Registered serial console for output\n",
            __FUNCTION__
            ));
        }
      }
      // Skip handles with GOP (display devices) - headless mode
    }

    gBS->FreePool (Handles);
  }

  // Headless: Skip ConIn registration to prevent keyboard input
  // Keyboard input is already suppressed by VolleyOverrides
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
VOID
EFIAPI
PlatformBootManagerBeforeConsole (
  VOID
  )
{
  PcdSet16S (PcdPlatformBootTimeOut, 0);

  //
  // Signal EndOfDxe PI Event
  //
  EfiEventGroupSignal (&gEfiEndOfDxeEventGroupGuid);

  //
  // Dispatch deferred images after EndOfDxe event.
  //
  EfiBootManagerDispatchDeferredImages ();

  //
  // Locate the PCI root bridges and make the PCI bus driver connect each,
  // non-recursively. This will produce a number of child handles with PciIo on
  // them.
  //
  FilterAndProcess (&gEfiPciRootBridgeIoProtocolGuid, NULL, Connect);

  // Headless: skip connecting display devices and GOP output.

  //
  // Keep eMMC/L4T priority deterministic. SetBootOrder only rewrites BootOrder
  // when the stored order is wrong, so this has no steady-state variable churn.
  //
  SetBootOrder ();

  if (!IsSingleBootNeeded ()) {
    if (IsPlatformConfigurationNeeded ()) {
      // Headless: skip connecting all devices (avoid USB/display).

      //
      // Enumerate all possible boot options.
      //
      EfiBootManagerRefreshAllBootOption ();

      //
      // Register platform-specific boot options and keyboard shortcuts.
      //
      PlatformRegisterOptionsAndKeys ();

      //
      // Set Boot Order
      //
      SetBootOrder ();

      //
      // Set platform has been configured
      //
      PlatformConfigured ();
    }
  } else {
    //
    // Headless: skip connecting all devices (avoid USB/display).

    // Don't wait for timeout
    PcdSet16S (PcdPlatformBootTimeOut, 0);

    PlatformRegisterFvBootOption (
      &gNVIDIAL4TLauncherApplicationGuid,
      L"Boot Application",
      LOAD_OPTION_ACTIVE,
      LoadOptionTypeBoot
      );
  }

  //
  // Add the hardcoded short-form USB keyboard device path to ConIn.
  //
  DEBUG ((DEBUG_INFO, "%a: skipping USB keyboard registration\n", __FUNCTION__));

  //
  // Register all available consoles during intitial
  // boot, then set PCD to FALSE afterwards.
  //
  PlatformRegisterConsoles (PcdGetBool (PcdDoInitialConsoleRegistration));
  if (PcdGetBool (PcdDoInitialConsoleRegistration) == TRUE) {
    PcdSetBoolS (PcdDoInitialConsoleRegistration, FALSE);
  }
}

/**
  Do the platform specific action after the console is ready.
  Volley: Skip splash, diagnostics, and capsule processing for fast boot.
**/
VOID
EFIAPI
PlatformBootManagerAfterConsole (
  VOID
  )
{
  // Volley: headless/fast path; skip splash, memory tests, IPMI prints,
  // capsules, and boot-chain updates.
}


/**
  This function is called each second during the boot manager waits the
  timeout.

  @param TimeoutRemain  The remaining timeout.
**/
VOID
EFIAPI
PlatformBootManagerWaitCallback (
  UINT16  TimeoutRemain
  )
{
  return;
}

/**
  The function is called when no boot option could be launched,
  including platform recovery options and options pointing to applications
  built into firmware volumes.

  If this function returns, BDS attempts to enter an infinite loop.
**/
VOID
EFIAPI
PlatformBootManagerUnableToBoot (
  VOID
  )
{
  return;
}

/**
BDS Entry  - DXE phase complete, BDS Entered.
**/
VOID
EFIAPI
PlatformBootManagerBdsEntry (
  VOID
  )
{
  DeleteBootNextVariable ();
  return;
}

/**
 HardKeyBoot
**/
VOID
EFIAPI
PlatformBootManagerPriorityBoot (
  UINT16  **BootNext
  )
{
  if (BootNext != NULL) {
    *BootNext = NULL;
  }

  DeleteBootNextVariable ();
  return;
}

/**
 This is called from BDS right before going into front page
 when no bootable devices/options found
**/
VOID
EFIAPI
PlatformBootManagerProcessBootCompletion (
  IN EFI_BOOT_MANAGER_LOAD_OPTION  *BootOption
  )
{
  return;
}

/**
  OnDemandConInConnect
**/
VOID
EFIAPI
PlatformBootManagerOnDemandConInConnect (
  VOID
  )
{
  return;
}
