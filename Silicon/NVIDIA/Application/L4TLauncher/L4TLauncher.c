/** @file
  The main process for L4TLauncher application.

  SPDX-FileCopyrightText: Copyright (c) 2022-2025 NVIDIA CORPORATION & AFFILIATES. All rights
reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <PiPei.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiLib.h>
#include <Library/HobLib.h>
#include <Library/PcdLib.h>
#include <Library/ShellLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/HandleParsingLib.h>
#include <Library/PrintLib.h>
#include <Library/FileHandleLib.h>
#include <Library/DevicePathLib.h>
#include <Library/AndroidBootImgLib.h>

#include <Protocol/DevicePath.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/AndroidBootImg.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DiskIo.h>
#include <Protocol/LoadFile2.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/PciRootBridgeIo.h>

#include <Guid/LinuxEfiInitrdMedia.h>
#include <Protocol/Pkcs7Verify.h>

#include <Guid/LinuxEfiInitrdMedia.h>
#include <Guid/AuthenticatedVariableFormat.h>
#include <Guid/ImageAuthentication.h>

#include <UefiSecureBoot.h>
#include <Library/SecureBootVariableLib.h>

#include <NVIDIAConfiguration.h>
#include <libfdt.h>
#include <Library/PlatformResourceLib.h>
#include <Library/ResetSystemLib.h>
#include "L4TLauncher.h"
#include "L4TRootfsValidation.h"

// Kernel is at /boot/Image on eMMC APP partition
#ifndef VOLLEY_DIRECT_KERNEL_PATH
#define VOLLEY_DIRECT_KERNEL_PATH L"boot\\Image"
#endif

#define VOLLEY_DTB_OVERRIDE_VAR L"VolleyDtbPath"
#define VOLLEY_DTB_PROFILE_VAR L"VolleyDtbProfile"
#define VOLLEY_SYSTEM_NAME L"${VOLLEY_SYSTEM_NAME}"

#ifndef VOLLEY_DIRECT_INITRD_PATH
#define VOLLEY_DIRECT_INITRD_PATH NULL
#endif

#ifndef VOLLEY_DIRECT_BOOTARGS
#define VOLLEY_DIRECT_BOOTARGS L"usbcore.autosuspend=-1"
#endif

/**
  Causes the driver to load a specified file.

  @param  This       Protocol instance pointer.
  @param  FilePath   The device specific path of the file to load.
  @param  BootPolicy Should always be FALSE.
  @param  BufferSize On input the size of Buffer in bytes. On output with a return
                     code of EFI_SUCCESS, the amount of data transferred to
                     Buffer. On output with a return code of EFI_BUFFER_TOO_SMALL,
                     the size of Buffer required to retrieve the requested file.
                     On other errors this will not be changed.
  @param  Buffer     The memory buffer to transfer the file to. IF Buffer is NULL,
                     then no the size of the requested file is returned in
                     BufferSize.

  @retval EFI_SUCCESS           The file was loaded.
  @retval EFI_UNSUPPORTED       BootPolicy is TRUE.
  @retval EFI_INVALID_PARAMETER FilePath is not a valid device path, or
                                BufferSize is NULL.
  @retval EFI_NOT_FOUND         The file was not found
  @retval EFI_BUFFER_TOO_SMALL  The BufferSize is too small to read the current
                                directory entry. BufferSize has been updated with
                                the size needed to complete the request.


**/
STATIC
EFI_STATUS
EFIAPI
L4TImgLoadFile2(IN EFI_LOAD_FILE2_PROTOCOL* This, IN EFI_DEVICE_PATH_PROTOCOL* FilePath,
                IN BOOLEAN BootPolicy, IN OUT UINTN* BufferSize, IN VOID* Buffer OPTIONAL)

{
    // Verify if the valid parameters
    if ((This == NULL) || (BufferSize == NULL) || (FilePath == NULL) ||
        !IsDevicePathValid(FilePath, 0))
    {
        return EFI_INVALID_PARAMETER;
    }

    if (BootPolicy)
    {
        return EFI_UNSUPPORTED;
    }

    // Check if the given buffer size is big enough
    // EFI_BUFFER_TOO_SMALL to allow caller to allocate a bigger buffer
    if (mRamdiskSize == 0)
    {
        return EFI_NOT_FOUND;
    }

    if ((Buffer == NULL) || (*BufferSize < mRamdiskSize))
    {
        *BufferSize = mRamdiskSize;
        return EFI_BUFFER_TOO_SMALL;
    }

    // Copy InitRd
    CopyMem(Buffer, mRamdiskData, mRamdiskSize);
    *BufferSize = mRamdiskSize;

    return EFI_SUCCESS;
}

///
/// Load File Protocol instance
///
STATIC EFI_LOAD_FILE2_PROTOCOL mAndroidBootImgLoadFile2 = {L4TImgLoadFile2};

/**
  Find the index of the GPT on disk.

  @param[in]  DeviceHandle     The handle of partition.

  @retval Index of the partition.

**/
STATIC
UINT32
EFIAPI
LocatePartitionIndex(IN EFI_HANDLE DeviceHandle)
{
    EFI_DEVICE_PATH_PROTOCOL* DevicePath;
    HARDDRIVE_DEVICE_PATH* HardDrivePath;

    if (DeviceHandle == 0)
    {
        return 0;
    }

    DevicePath = DevicePathFromHandle(DeviceHandle);
    if (DevicePath == NULL)
    {
        ErrorPrint(L"%a: Unable to find device path\r\n", __FUNCTION__);
        return 0;
    }

    while (!IsDevicePathEndType(DevicePath))
    {
        if ((DevicePathType(DevicePath) == MEDIA_DEVICE_PATH) &&
            (DevicePathSubType(DevicePath) == MEDIA_HARDDRIVE_DP))
        {
            HardDrivePath = (HARDDRIVE_DEVICE_PATH*)DevicePath;
            return HardDrivePath->PartitionNumber;
        }

        DevicePath = NextDevicePathNode(DevicePath);
    }

    ErrorPrint(L"%a: Unable to locate harddrive device path node\r\n", __FUNCTION__);
    return 0;
}

/**
  Print the partition UUID for a device handle (for diagnostics)

  @param[in]  DeviceHandle     The handle of partition.

**/
STATIC
VOID
EFIAPI
PrintPartitionUuid(IN EFI_HANDLE DeviceHandle)
{
    EFI_DEVICE_PATH_PROTOCOL* DevicePath;
    HARDDRIVE_DEVICE_PATH* HardDrivePath;
    EFI_GUID* PartGuid;

    if (DeviceHandle == 0)
    {
        ErrorPrint(L"  Partition: (null handle)\r\n");
        return;
    }

    DevicePath = DevicePathFromHandle(DeviceHandle);
    if (DevicePath == NULL)
    {
        ErrorPrint(L"  Partition: (no device path)\r\n");
        return;
    }

    while (!IsDevicePathEndType(DevicePath))
    {
        if ((DevicePathType(DevicePath) == MEDIA_DEVICE_PATH) &&
            (DevicePathSubType(DevicePath) == MEDIA_HARDDRIVE_DP))
        {
            HardDrivePath = (HARDDRIVE_DEVICE_PATH*)DevicePath;
            PartGuid = (EFI_GUID*)&HardDrivePath->Signature;
            ErrorPrint(L"  Partition %d UUID: %g\r\n",
                       HardDrivePath->PartitionNumber, PartGuid);
            return;
        }

        DevicePath = NextDevicePathNode(DevicePath);
    }

    ErrorPrint(L"  Partition: (not a harddrive partition)\r\n");
}

/**
  Find the partition on the same disk as the loaded image

  Will fall back to the other bootchain if needed

  @param[in]  DeviceHandle     The handle of partition where this file lives on.
  @param[out] PartitionIndex   The partition index on the disk
  @param[out] PartitionHandle  The partition handle

  @retval EFI_SUCCESS    The operation completed successfully.
  @retval EFI_NOT_FOUND  The partition is not on the filesystem.

**/
STATIC
EFI_STATUS
EFIAPI
FindPartitionInfo(IN EFI_HANDLE DeviceHandle, IN CONST CHAR16* PartitionBasename,
                  IN UINT32 BootChain, OUT UINT32* PartitionIndex OPTIONAL,
                  OUT EFI_HANDLE* PartitionHandle OPTIONAL)
{
    EFI_STATUS Status;
    EFI_HANDLE* ParentHandles;
    UINTN ParentCount;
    UINTN ParentIndex;
    EFI_HANDLE* ChildHandles;
    UINTN ChildCount;
    UINTN ChildIndex;
    UINT32 FoundIndex = 0;
    EFI_PARTITION_INFO_PROTOCOL* PartitionInfo;
    EFI_HANDLE FoundHandle = 0;
    EFI_HANDLE FoundHandleGeneric = 0;
    EFI_HANDLE FoundHandleAlt = 0;
    CHAR16* SubString;
    UINTN PartitionBasenameLen;

    if (BootChain > 1)
    {
        return EFI_UNSUPPORTED;
    }

    if (PartitionBasename == NULL)
    {
        return EFI_INVALID_PARAMETER;
    }

    PartitionBasenameLen = StrnLenS(PartitionBasename, MAX_PARTITION_NAME_SIZE);

    Status = PARSE_HANDLE_DATABASE_PARENTS(DeviceHandle, &ParentCount, &ParentHandles);
    if (EFI_ERROR(Status))
    {
        ErrorPrint(L"%a: Failed to find parents - %r\r\n", __FUNCTION__, Status);
        return Status;
    }

    for (ParentIndex = 0; ParentIndex < ParentCount; ParentIndex++)
    {
        Status = ParseHandleDatabaseForChildControllers(ParentHandles[ParentIndex], &ChildCount,
                                                        &ChildHandles);
        if (EFI_ERROR(Status))
        {
            ErrorPrint(L"%a: Failed to find child controllers - %r\r\n", __FUNCTION__, Status);
            return Status;
        }

        for (ChildIndex = 0; ChildIndex < ChildCount; ChildIndex++)
        {
            Status = gBS->HandleProtocol(ChildHandles[ChildIndex], &gEfiPartitionInfoProtocolGuid,
                                         (VOID**)&PartitionInfo);
            if (EFI_ERROR(Status))
            {
                continue;
            }

            // Only GPT partitions are supported
            if (PartitionInfo->Type != PARTITION_TYPE_GPT)
            {
                continue;
            }

            // Look for A/B Names
            if (StrCmp(PartitionInfo->Info.Gpt.PartitionName, PartitionBasename) == 0)
            {
                ASSERT(FoundHandleGeneric == 0);
                FoundHandleGeneric = ChildHandles[ChildIndex];
            }
            else if ((PartitionBasenameLen + 2) == StrLen(PartitionInfo->Info.Gpt.PartitionName))
            {
                SubString = StrStr(PartitionInfo->Info.Gpt.PartitionName, PartitionBasename);
                if (SubString != NULL)
                {
                    // See if it is a prefix
                    if ((SubString == (PartitionInfo->Info.Gpt.PartitionName + 2)) &&
                        (PartitionInfo->Info.Gpt.PartitionName[1] == L'_'))
                    {
                        if ((PartitionInfo->Info.Gpt.PartitionName[0] == (L'A' + BootChain)) ||
                            (PartitionInfo->Info.Gpt.PartitionName[0] == (L'a' + BootChain)))
                        {
                            ASSERT(FoundHandle == 0);
                            FoundHandle = ChildHandles[ChildIndex];
                        }

                        if ((PartitionInfo->Info.Gpt.PartitionName[0] == (L'B' - BootChain)) ||
                            (PartitionInfo->Info.Gpt.PartitionName[0] == (L'b' - BootChain)))
                        {
                            ASSERT(FoundHandleAlt == 0);
                            FoundHandleAlt = ChildHandles[ChildIndex];
                        }

                        // See if it is a postfix
                    }
                    else if ((SubString == PartitionInfo->Info.Gpt.PartitionName) &&
                             (PartitionInfo->Info.Gpt.PartitionName[PartitionBasenameLen] == L'_'))
                    {
                        if ((PartitionInfo->Info.Gpt.PartitionName[PartitionBasenameLen + 1] ==
                             (L'a' + BootChain)) ||
                            (PartitionInfo->Info.Gpt.PartitionName[PartitionBasenameLen + 1] ==
                             (L'A' + BootChain)))
                        {
                            ASSERT(FoundHandle == 0);
                            FoundHandle = ChildHandles[ChildIndex];
                        }
                        else if ((PartitionInfo->Info.Gpt.PartitionName[PartitionBasenameLen + 1] ==
                                  (L'b' - BootChain)) ||
                                 (PartitionInfo->Info.Gpt.PartitionName[PartitionBasenameLen + 1] ==
                                  (L'B' - BootChain)))
                        {
                            ASSERT(FoundHandleAlt == 0);
                            FoundHandleAlt = ChildHandles[ChildIndex];
                        }
                    }
                }
            }
        }

        FreePool(ChildHandles);
    }

    FreePool(ParentHandles);

    if ((FoundHandle == 0) && (FoundHandleGeneric == 0) && (FoundHandleAlt == 0))
    {
        return EFI_NOT_FOUND;
    }
    else if (FoundHandle == 0)
    {
        if (FoundHandleGeneric != 0)
        {
            FoundHandle = FoundHandleGeneric;
        }
        else
        {
            FoundHandle = FoundHandleAlt;
            Print(L"Falling back to alternative boot path\r\n");
        }
    }

    FoundIndex = LocatePartitionIndex(FoundHandle);

    if (FoundIndex == 0)
    {
        ErrorPrint(L"%a: Failed to find both partitions index\r\n", __FUNCTION__);
        return EFI_DEVICE_ERROR;
    }

    if (PartitionIndex != NULL)
    {
        *PartitionIndex = FoundIndex;
    }

    if (PartitionHandle != NULL)
    {
        *PartitionHandle = FoundHandle;
    }

    return EFI_SUCCESS;
}

/**
  Update the grub boot configuration file

  @param[in]  DeviceHandle     The handle of partition where this file lives on.
  @param[in]  PartitionIndex   Partition number of the root file system
  @param[in]  BootImgPresent   BootImage is present on system
  @param[in]  RecoveryPresent  Recovery kernel partition is present on system

  @retval EFI_SUCCESS    The operation completed successfully.

**/
STATIC
EFI_STATUS
EFIAPI
UpdateBootCfgFile(IN EFI_HANDLE DeviceHandle, IN UINT32 PartitionIndex, IN BOOLEAN BootImgPresent,
                  IN BOOLEAN RecoveryPresent)
{
    EFI_STATUS Status;
    CHAR8 CorrectPartitionContent[MAX_BOOTCONFIG_CONTENT_SIZE];
    CHAR8 ReadPartitionContent[MAX_BOOTCONFIG_CONTENT_SIZE];
    CHAR16 CpuBootArgs[MAX_CBOOTARG_SIZE / sizeof(CHAR16)];
    UINTN CorrectSize;
    UINT64 FileSize;
    EFI_FILE_HANDLE FileHandle;
    EFI_DEVICE_PATH* FullDevicePath;
    ANDROID_BOOTIMG_PROTOCOL* AndroidBootProtocol;

    ZeroMem(CpuBootArgs, MAX_CBOOTARG_SIZE);
    Status = gBS->LocateProtocol(&gAndroidBootImgProtocolGuid, NULL, (VOID**)&AndroidBootProtocol);
    if (!EFI_ERROR(Status))
    {
        if (AndroidBootProtocol->AppendArgs != NULL)
        {
            Status = AndroidBootProtocol->AppendArgs(CpuBootArgs, MAX_CBOOTARG_SIZE);
            if (EFI_ERROR(Status))
            {
                ErrorPrint(L"%a: Failed to get platform addition arguments\r\n", __FUNCTION__);
                return Status;
            }
        }
    }

    CorrectSize = AsciiSPrint(CorrectPartitionContent, MAX_BOOTCONFIG_CONTENT_SIZE,
                              GRUB_BOOTCONFIG_CONTENT_FORMAT, CpuBootArgs, PartitionIndex,
                              BootImgPresent, RecoveryPresent);

    FullDevicePath = FileDevicePath(DeviceHandle, GRUB_BOOTCONFIG_FILE);
    if (FullDevicePath == NULL)
    {
        ErrorPrint(L"%a: Failed to create file device path\r\n", __FUNCTION__);
        return EFI_OUT_OF_RESOURCES;
    }

    Status =
        EfiOpenFileByDevicePath(&FullDevicePath, &FileHandle,
                                EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (EFI_ERROR(Status))
    {
        ErrorPrint(L"%a: Failed to open file: %r\r\n", __FUNCTION__, Status);
        return Status;
    }

    Status = FileHandleGetSize(FileHandle, &FileSize);
    if (EFI_ERROR(Status))
    {
        ErrorPrint(L"%a: Failed to get file size: %r\r\n", __FUNCTION__, Status);
        return Status;
    }

    if (FileSize == CorrectSize)
    {
        ASSERT(FileSize <= MAX_BOOTCONFIG_CONTENT_SIZE);
        Status = FileHandleRead(FileHandle, &FileSize, ReadPartitionContent);
        if (EFI_ERROR(Status))
        {
            ErrorPrint(L"%a: Failed to read current file content: %r\r\n", __FUNCTION__, Status);
            return Status;
        }

        if (CompareMem(CorrectPartitionContent, ReadPartitionContent, CorrectSize) == 0)
        {
            return EFI_SUCCESS;
        }
    }

    Status = FileHandleSetSize(FileHandle, 0);
    if (EFI_ERROR(Status))
    {
        ErrorPrint(L"%a: Failed to set file size to 0\r\n", __FUNCTION__);
        return Status;
    }

    Status = FileHandleWrite(FileHandle, &CorrectSize, CorrectPartitionContent);
    if (EFI_ERROR(Status))
    {
        ErrorPrint(L"%a: Failed to write file content\r\n", __FUNCTION__);
        return Status;
    }

    FileHandleClose(FileHandle);
    return EFI_SUCCESS;
}

/**
  Update the grub partition configuration files

  @param[in]  DeviceHandle     The handle of partition where this file lives on.
  @param[in]  BootChain        Numeric version of the chain

  @retval EFI_SUCCESS    The operation completed successfully.

**/
STATIC
EFI_STATUS
EFIAPI
UpdateBootConfig(IN EFI_HANDLE DeviceHandle, IN UINT32 BootChain)
{
    UINT32 PartitionIndex;
    EFI_STATUS Status;
    BOOLEAN BootImgPresent = FALSE;
    BOOLEAN RecoveryPresent = FALSE;

    Status = FindPartitionInfo(DeviceHandle, ROOTFS_BASE_NAME, BootChain, &PartitionIndex, NULL);
    if (EFI_ERROR(Status))
    {
        ErrorPrint(L"%a: Unable to find rootfs partition info\r\n", __FUNCTION__);
        return Status;
    }

    Status = FindPartitionInfo(DeviceHandle, BOOTIMG_BASE_NAME, BootChain, NULL, NULL);
    if (Status == EFI_SUCCESS)
    {
        BootImgPresent = TRUE;
    }
    else if (Status == EFI_NOT_FOUND)
    {
        BootImgPresent = FALSE;
    }
    else if (EFI_ERROR(Status))
    {
        ErrorPrint(L"%a: Unable to find bootimg partition info\r\n", __FUNCTION__);
        return Status;
    }

    Status = FindPartitionInfo(DeviceHandle, RECOVERY_BASE_NAME, BootChain, NULL, NULL);
    if (Status == EFI_SUCCESS)
    {
        RecoveryPresent = TRUE;
    }
    else if (Status == EFI_NOT_FOUND)
    {
        RecoveryPresent = FALSE;
    }
    else if (EFI_ERROR(Status))
    {
        ErrorPrint(L"%a: Unable to find recovery partition info\r\n", __FUNCTION__);
        return Status;
    }

    Status = UpdateBootCfgFile(DeviceHandle, PartitionIndex, BootImgPresent, RecoveryPresent);
    if (EFI_ERROR(Status))
    {
        ErrorPrint(L"Unable to update boot configuration file\r\n");
        return Status;
    }

    return Status;
}

/**
  Remove comments and leading trailing whitespace

  @param[in]  InputString

  @returns   Cleaned string

**/
/*
 *
  SetupCertList

  Function to read and setup the Ceriticate list according to what the PKCS
  Verification Lib expects.
  The PKCS Verification lib expects to walk a list of EFI_SIGNATURE_LIST entries
  and a NULL entry to mark the end of the list.
  To get this , first we get the stored list of certificates using variable
  services, then walk the list (each DB entry can vary in size) so before
  moving to the next EFI_SIGNATURE_LIST entry, we need to parse that header to
  determine the size of the entry.

  @param[in]  VariableName     The Variable Name under which the certificate DB
                               is stored.

  @retval SUCCESS : List of DB Entry pointers (with a terminating NULL entry)
          FAILURE:  NULL
 *
 */
STATIC
EFI_SIGNATURE_LIST** SetupCertList(IN CHAR16* VariableName)
{
    EFI_STATUS Status;
    EFI_SIGNATURE_LIST* CertDb = NULL;
    UINTN CertDbSize;
    EFI_SIGNATURE_LIST* CertList;
    UINTN CertListIndex;
    UINTN CertListCount;
    EFI_SIGNATURE_LIST** CertLists = NULL;

    Status =
        GetVariable2(VariableName, &gEfiImageSecurityDatabaseGuid, (VOID**)&CertDb, &CertDbSize);
    if (EFI_ERROR(Status))
    {
        if (Status != EFI_NOT_FOUND)
        {
            DEBUG((DEBUG_ERROR, "%a: Failed to retrieve certificate database '%s': %r\r\n",
                   __FUNCTION__, VariableName, Status));
        }

        // In case of an error, assume an empty certificate database.
        CertDb = NULL;
        CertDbSize = 0;
        Status = EFI_SUCCESS;
    }

    // Walk the list to determine how many signature lists are present.
    CertListCount = 0;
    CertList = CertDb;
    while (((UINT8*)CertList < (UINT8*)CertDb + CertDbSize) &&
           ((UINT8*)CertList + CertList->SignatureListSize <= (UINT8*)CertDb + CertDbSize))
    {
        CertListCount++;
        CertList = (EFI_SIGNATURE_LIST*)((UINT8*)CertList + CertList->SignatureListSize);
    }

    CertLists =
        (EFI_SIGNATURE_LIST**)AllocateZeroPool(sizeof(EFI_SIGNATURE_LIST*) * (CertListCount + 1));
    if (CertLists == NULL)
    {
        Status = EFI_OUT_OF_RESOURCES;
        goto Exit;
    }

    CertList = CertDb;
    for (CertListIndex = 0; CertListIndex < CertListCount; CertListIndex++)
    {
        CertLists[CertListIndex] = CertList;
        CertList = (EFI_SIGNATURE_LIST*)((UINT8*)CertList + CertList->SignatureListSize);
    }

    // Keep the last entry NULL (what the PKCS lib code expects)
    CertLists[CertListCount] = NULL;

Exit:
    if (EFI_ERROR(Status))
    {
        if (CertLists != NULL)
        {
            FreePool(CertLists);
        }

        if (CertDb != NULL)
        {
            FreePool(CertDb);
        }
    }

    return CertLists;
}

/**
  Verify a detached signature.

  @param[in]  SignData      Detached signature data
  @param[in]  SignDataSize  Size of the detached signature data
  @param[in]  InData        Data signed by the detached signature
  @param[in]  InDataSize    Length of the signed data

  @retval EFI_SUCCESS     Signature successfully verified
  @retval !(EFI_SUCCESS)  Could not verify signature
*/
STATIC
EFI_STATUS
VerifyDetachedSignature(IN VOID* CONST SignData, IN CONST UINTN SignDataSize, IN VOID* CONST InData,
                        IN CONST UINTN InDataSize)
{
    STATIC EFI_SIGNATURE_LIST** AllowedDb = NULL;
    STATIC EFI_SIGNATURE_LIST** RevokedDb = NULL;

    EFI_STATUS Status;
    EFI_SIGNATURE_LIST** TimeStampDb = NULL;
    EFI_PKCS7_VERIFY_PROTOCOL* Pkcs7VerifyProtocol;

    // Do these steps once, to locate and setup the DB/DBX certs.
    if (AllowedDb == NULL)
    {
        AllowedDb = SetupCertList(EFI_IMAGE_SECURITY_DATABASE);
    }

    if (RevokedDb == NULL)
    {
        RevokedDb = SetupCertList(EFI_IMAGE_SECURITY_DATABASE1);
    }

    Status = gBS->LocateProtocol(&gEfiPkcs7VerifyProtocolGuid, NULL, (VOID**)&Pkcs7VerifyProtocol);
    if (EFI_ERROR(Status))
    {
        DEBUG((DEBUG_ERROR, "%a: Failed to locate PKCS7 verification protocol: %r\r\n",
               __FUNCTION__, Status));
        return Status;
    }

    return Pkcs7VerifyProtocol->VerifyBuffer(Pkcs7VerifyProtocol, SignData, SignDataSize, InData,
                                             InDataSize, AllowedDb, RevokedDb, TimeStampDb,
                                             NULL, /* Content */
                                             NULL  /* ContentSize */
    );
}

/**
  Utility function to open a file named FileName, return its
  FileHandle and read the contents of the file into buffer Data.

  This function will open the file, allocate a data buffer based on
  the file size and returns file handle, the data buffer and the file
  size.

  Upon successful return, the caller is responsible for freeing all
  allocated resources (file handle and/or data buffer).

  @param[in]   PartitionHandle  Handle of the partition where this file lives on.
  @param[in]   FileName         Name of file to be processed
  @param[out]  FileHandle       File handle of the opened file.
  @param[out]  FileData         Buffer with file data.
  @param[out]  FileDataSize     Size of the opened file.

  @retval EFI_SUCCESS           The operation completed successfully.
  @retval EFI_OUT_OF_RESOURCES  Failed buffer allocation.
  @retval !(EFI_SUCCESS)        Error status from other APIs called.
*/
STATIC
EFI_STATUS
OpenAndReadUntrustedFileToBuffer(IN CONST EFI_HANDLE PartitionHandle,
                                 IN CONST CHAR16* CONST FileName,
                                 OUT EFI_FILE_HANDLE* CONST FileHandle OPTIONAL,
                                 OUT VOID** CONST FileData OPTIONAL,
                                 OUT UINT64* CONST FileDataSize OPTIONAL)
{
    EFI_STATUS Status = EFI_SUCCESS;
    EFI_DEVICE_PATH* DevicePath = NULL;
    EFI_DEVICE_PATH* NextDevicePath;
    EFI_FILE_HANDLE Handle = NULL;
    VOID* Data = NULL;
    UINT64 DataSize;

    if ((FileHandle != NULL) || (FileData != NULL) || (FileDataSize != NULL))
    {
        DevicePath = FileDevicePath(PartitionHandle, FileName);
        if (DevicePath == NULL)
        {
            ErrorPrint(L"%a: Failed to create file device path\r\n", __FUNCTION__);
            Status = EFI_OUT_OF_RESOURCES;
            goto Exit;
        }

        NextDevicePath = DevicePath;
        Status = EfiOpenFileByDevicePath(&NextDevicePath, &Handle, EFI_FILE_MODE_READ, 0);
        if (EFI_ERROR(Status))
        {
            ErrorPrint(L"%a: Failed to open %s: %r\r\n", __FUNCTION__, FileName, Status);
            goto Exit;
        }
    }

    if ((FileData != NULL) || (FileDataSize != NULL))
    {
        Status = FileHandleGetSize(Handle, &DataSize);
        if (EFI_ERROR(Status))
        {
            ErrorPrint(L"%a: Failed to get size of file %s: %r\r\n", __FUNCTION__, FileName,
                       Status);
            goto Exit;
        }
    }

    if (FileData != NULL)
    {
        Data = AllocatePool(DataSize);
        if (Data == NULL)
        {
            ErrorPrint(L"%a: Failed to allocate buffer for %s\r\n", __FUNCTION__, FileName);
            Status = EFI_OUT_OF_RESOURCES;
            goto Exit;
        }

        Status = FileHandleRead(Handle, &DataSize, Data);
        if (EFI_ERROR(Status))
        {
            ErrorPrint(L"%a: Failed to read %s\r\n", __FUNCTION__, FileName);
            goto Exit;
        }

        if (FileHandle != NULL)
        {
            // If both handle and data were requested, rewind the handle
            // back to the beginning of the file.
            Status = FileHandleSetPosition(Handle, 0);
            if (EFI_ERROR(Status))
            {
                ErrorPrint(L"%a: Failed to rewind %s\r\n", __FUNCTION__, FileName);
                goto Exit;
            }
        }
    }

    if (FileHandle != NULL)
    {
        *FileHandle = Handle;
        Handle = NULL;
    }

    if (FileData != NULL)
    {
        *FileData = Data;
        Data = NULL;
    }

    if (FileDataSize != NULL)
    {
        *FileDataSize = DataSize;
    }

Exit:
    if (Data != NULL)
    {
        FreePool(Data);
    }

    if (Handle != NULL)
    {
        FileHandleClose(Handle);
    }

    if (DevicePath != NULL)
    {
        FreePool(DevicePath);
    }

    return Status;
}

/**
  Utility function to open and decrypt the encrypted file named FileName to
  buffer.

  This function will open the encrypted file, allocate a block of shared
  memory with fixed size, read the encrypted contents to shared memory and
  call optee to decrypted the content block by block. Finally, get the clear
  image in the buffer FileData.

  Upon successful return, the caller is responsible for freeing all allocated
  resources (OpteeSession & Shared Memory).

  @param[in]   PartitionHandle  Handle of the partition where this file lives on.
  @param[in]   FileName         Name of file to be processed
  @param[out]  FileData         Buffer with file data.
  @param[out]  FileDataSize     Size of the opened file.

  @retval EFI_SUCCESS           The operation completed successfully.
  @retval EFI_OUT_OF_RESOURCES  Failed buffer allocation.
  @retval !(EFI_SUCCESS)        Error status from other APIs called.
*/
STATIC
EFI_STATUS
OpenAndDecryptFileToBuffer(IN CONST EFI_HANDLE PartitionHandle, IN CONST CHAR16* CONST FileName,
                           OUT VOID** CONST FileData, OUT UINT64* CONST FileDataSize)
{
    EFI_STATUS Status = EFI_SUCCESS;
    EFI_DEVICE_PATH* NextDevicePath;
    EFI_DEVICE_PATH* DevicePath = NULL;
    EFI_FILE_HANDLE Handle = NULL;
    UINT64 DataSize;

    if ((FileData == NULL) || (FileDataSize == NULL))
    {
        ErrorPrint(L"%a: FileData and FileDataSize can not be NULL\r\n", __FUNCTION__);
        Status = EFI_INVALID_PARAMETER;
        goto Exit;
    }

    DevicePath = FileDevicePath(PartitionHandle, FileName);
    if (DevicePath == NULL)
    {
        ErrorPrint(L"%a: Failed to create file device path\r\n", __FUNCTION__);
        Status = EFI_OUT_OF_RESOURCES;
        goto Exit;
    }

    NextDevicePath = DevicePath;
    Status = EfiOpenFileByDevicePath(&NextDevicePath, &Handle, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(Status))
    {
        ErrorPrint(L"%a: Failed to open %s: %r\r\n", __FUNCTION__, FileName, Status);
        goto Exit;
    }

    Status = FileHandleGetSize(Handle, &DataSize);
    if (EFI_ERROR(Status))
    {
        ErrorPrint(L"%a: Failed to get size of file %s: %r\r\n", __FUNCTION__, FileName, Status);
        goto Exit;
    }

    *FileData = AllocatePool(DataSize);
    if (*FileData == NULL)
    {
        ErrorPrint(L"%a: Failed to allocate buffer for %s\r\n", __FUNCTION__, FileName);
        Status = EFI_OUT_OF_RESOURCES;
        goto Exit;
    }

    Status = OpteeDecryptImage(&Handle, NULL, NULL, EncryptionInfo.ImageHeaderSize, DataSize,
                               FileData, FileDataSize);
    if (EFI_ERROR(Status))
    {
        ErrorPrint(L"%a: OpteeDecryptImage failed for %s\r\n", __FUNCTION__, FileName);
        goto Exit;
    }

Exit:

    if (Handle != NULL)
    {
        FileHandleClose(Handle);
    }

    if (DevicePath != NULL)
    {
        FreePool(DevicePath);
    }

    return Status;
}

/**
  Utility function to open and read file to buffer.

  If UEFI Secure Boot is not enabled. This function do exactly what
  OpenAndReadUntrustedFileToBuffer does.

  If UEFI Secure Boot is enabled, and image encryption is enabled. This
  function first call OpenAndDecryptFileToBuffer to read and decrypt
  encrypted file to buffer, then check detached signature of the file.

  If UEFI Secure Boot is enabled, but image encryption is not enabled.
  This function first call OpenAndReadUntrustedFileToBuffer to read file
  to buffer, then check detached signature of the file.

  @param[in]   PartitionHandle  Handle of the partition where this file lives on.
  @param[in]   FileName         Name of file to be processed
  @param[out]  FileHandle       File handle of the opened file. Not support for
  the encrypted File.
  @param[out]  FileData         Buffer with file data.
  @param[out]  FileDataSize     Size of the opened file.

  @retval EFI_SUCCESS     The operation completed successfully.
  @retval !(EFI_SUCCESS)  Error status from other APIs called.
*/
STATIC
EFI_STATUS
OpenAndReadFileToBuffer(IN CONST EFI_HANDLE PartitionHandle, IN CONST CHAR16* CONST FileName,
                        OUT EFI_FILE_HANDLE* CONST FileHandle OPTIONAL,
                        OUT VOID** CONST FileData OPTIONAL, OUT UINT64* CONST FileDataSize OPTIONAL)
{
    EFI_STATUS Status;
    EFI_FILE_HANDLE Handle = NULL;
    VOID* Data = NULL;
    UINT64 DataSize;
    CHAR16* SigFileName = NULL;
    UINTN SigFileNameSize;
    VOID* SigData = NULL;
    UINT64 SigSize;

    if (!IsSecureBootEnabled())
    {
        DEBUG((DEBUG_INFO, "%a: Secure Boot is disabled\r\n", __FUNCTION__));

        return OpenAndReadUntrustedFileToBuffer(PartitionHandle, FileName, FileHandle, FileData,
                                                FileDataSize);
    }

    // Encryption of extlinux.conf is not supported
    if (EncryptionInfo.ImageEncrypted && StrCmp(FileName, EXTLINUX_CONF_PATH))
    {
        Status = OpenAndDecryptFileToBuffer(PartitionHandle, FileName, &Data, &DataSize);
    }
    else
    {
        Status = OpenAndReadUntrustedFileToBuffer(
            PartitionHandle, FileName, FileHandle != NULL ? &Handle : NULL, &Data, &DataSize);
    }

    if (EFI_ERROR(Status))
    {
        goto Exit;
    }

    // The detached signature file should be <filename>.sig
    SigFileNameSize = StrSize(FileName) + StrSize(DETACHED_SIG_FILE_EXTENSION) - sizeof(CHAR16);
    SigFileName = AllocatePool(SigFileNameSize);
    if (SigFileName == NULL)
    {
        DEBUG((DEBUG_ERROR, "%a: cannot allocate buffer for signature file name (%u bytes)\r\n",
               __FUNCTION__, SigFileNameSize));
        Status = EFI_OUT_OF_RESOURCES;
        goto Exit;
    }

    UnicodeSPrint(SigFileName, SigFileNameSize, L"%s%s", FileName, DETACHED_SIG_FILE_EXTENSION);

    Status =
        OpenAndReadUntrustedFileToBuffer(PartitionHandle, SigFileName, NULL, &SigData, &SigSize);
    if (EFI_ERROR(Status))
    {
        goto Exit;
    }

    Status = VerifyDetachedSignature(SigData, SigSize, Data, DataSize);
    if (EFI_ERROR(Status))
    {
        ErrorPrint(L"%a: %s failed signature verification: %r\r\n", __FUNCTION__, FileName, Status);
        goto Exit;
    }

    DEBUG((DEBUG_INFO, "%a: %s signature verification successful\r\n", __FUNCTION__, FileName));

    if (FileHandle != NULL)
    {
        *FileHandle = Handle;
        Handle = NULL;
    }

    if (FileData != NULL)
    {
        *FileData = Data;
        Data = NULL;
    }

    if (FileDataSize != NULL)
    {
        *FileDataSize = DataSize;
    }

Exit:
    if (SigData != NULL)
    {
        FreePool(SigData);
    }

    if (SigFileName != NULL)
    {
        FreePool(SigFileName);
    }

    if (Data != NULL)
    {
        FreePool(Data);
    }

    if (Handle != NULL)
    {
        FileHandleClose(Handle);
    }

    return Status;
}

STATIC
CHAR16* EFIAPI GetVolleyDtbPath(VOID)
{
    EFI_STATUS Status;
    CHAR16* DtbPath = NULL;
    UINTN Size = 0;

    Status = gRT->GetVariable(VOLLEY_DTB_OVERRIDE_VAR, &gEfiGlobalVariableGuid, NULL, &Size, NULL);
    if (Status == EFI_BUFFER_TOO_SMALL && Size > sizeof(CHAR16))
    {
        DtbPath = AllocatePool(Size);
        if (DtbPath != NULL)
        {
            Status = gRT->GetVariable(VOLLEY_DTB_OVERRIDE_VAR, &gEfiGlobalVariableGuid, NULL, &Size,
                                      DtbPath);
            if (EFI_ERROR(Status))
            {
                FreePool(DtbPath);
                DtbPath = NULL;
            }
        }
    }

    return DtbPath;
}

STATIC
BOOLEAN
VolleyIsIndustrial(VOID)
{
    EFI_STATUS Status;
    UINT8 Profile = 0;
    UINTN Size = sizeof(Profile);

    Status =
        gRT->GetVariable(VOLLEY_DTB_PROFILE_VAR, &gEfiGlobalVariableGuid, NULL, &Size, &Profile);
    if (EFI_ERROR(Status) || Size != sizeof(Profile))
    {
        return FALSE;
    }

    return (Profile != 0);
}

STATIC
EFI_STATUS
AllocateBootOptionString(CHAR16** Target, CONST CHAR16* Source)
{
    if (Target == NULL)
    {
        return EFI_INVALID_PARAMETER;
    }

    if (Source == NULL)
    {
        *Target = NULL;
        return EFI_SUCCESS;
    }

    *Target = AllocateCopyPool(StrSize(Source), Source);
    if (*Target == NULL)
    {
        return EFI_OUT_OF_RESOURCES;
    }

    return EFI_SUCCESS;
}

#if 1  // NVMe slot selection enabled
//
// ============================================================================
// Volley Boot Mode Detection Functions
// ============================================================================
//

#define VOLLEY_HASH_READ_CHUNK_SIZE (1024 * 1024)
#define VOLLEY_SHA256_DIGEST_SIZE   32
#define VOLLEY_SHA256_HEX_LEN       64

STATIC
EFI_STATUS
TrimAsciiSpan(
    IN OUT CONST CHAR8 **Start,
    IN OUT CONST CHAR8 **End
);

typedef struct {
    UINT8  Data[64];
    UINT32 State[8];
    UINT64 BitLen;
    UINTN  DataLen;
} VOLLEY_SHA256_CTX;

STATIC CONST UINT32 mSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROTR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTR32((x), 2) ^ ROTR32((x), 13) ^ ROTR32((x), 22))
#define EP1(x) (ROTR32((x), 6) ^ ROTR32((x), 11) ^ ROTR32((x), 25))
#define SIG0(x) (ROTR32((x), 7) ^ ROTR32((x), 18) ^ ((x) >> 3))
#define SIG1(x) (ROTR32((x), 17) ^ ROTR32((x), 19) ^ ((x) >> 10))

STATIC
VOID
VolleySha256Transform(
    IN OUT VOLLEY_SHA256_CTX *Ctx,
    IN CONST UINT8           Data[64]
)
{
    UINT32 A;
    UINT32 B;
    UINT32 C;
    UINT32 D;
    UINT32 E;
    UINT32 F;
    UINT32 G;
    UINT32 H;
    UINT32 T1;
    UINT32 T2;
    UINT32 W[64];
    UINTN  I;

    for (I = 0; I < 16; I++) {
        W[I] = ((UINT32)Data[I * 4] << 24) |
               ((UINT32)Data[I * 4 + 1] << 16) |
               ((UINT32)Data[I * 4 + 2] << 8) |
               ((UINT32)Data[I * 4 + 3]);
    }

    for (I = 16; I < 64; I++) {
        W[I] = SIG1(W[I - 2]) + W[I - 7] + SIG0(W[I - 15]) + W[I - 16];
    }

    A = Ctx->State[0];
    B = Ctx->State[1];
    C = Ctx->State[2];
    D = Ctx->State[3];
    E = Ctx->State[4];
    F = Ctx->State[5];
    G = Ctx->State[6];
    H = Ctx->State[7];

    for (I = 0; I < 64; I++) {
        T1 = H + EP1(E) + CH(E, F, G) + mSha256K[I] + W[I];
        T2 = EP0(A) + MAJ(A, B, C);
        H = G;
        G = F;
        F = E;
        E = D + T1;
        D = C;
        C = B;
        B = A;
        A = T1 + T2;
    }

    Ctx->State[0] += A;
    Ctx->State[1] += B;
    Ctx->State[2] += C;
    Ctx->State[3] += D;
    Ctx->State[4] += E;
    Ctx->State[5] += F;
    Ctx->State[6] += G;
    Ctx->State[7] += H;
}

STATIC
VOID
VolleySha256Init(
    IN OUT VOLLEY_SHA256_CTX *Ctx
)
{
    Ctx->DataLen = 0;
    Ctx->BitLen = 0;
    Ctx->State[0] = 0x6a09e667;
    Ctx->State[1] = 0xbb67ae85;
    Ctx->State[2] = 0x3c6ef372;
    Ctx->State[3] = 0xa54ff53a;
    Ctx->State[4] = 0x510e527f;
    Ctx->State[5] = 0x9b05688c;
    Ctx->State[6] = 0x1f83d9ab;
    Ctx->State[7] = 0x5be0cd19;
}

STATIC
VOID
VolleySha256Update(
    IN OUT VOLLEY_SHA256_CTX *Ctx,
    IN CONST UINT8           *Data,
    IN UINTN                 Len
)
{
    UINTN I;

    for (I = 0; I < Len; I++) {
        Ctx->Data[Ctx->DataLen++] = Data[I];
        if (Ctx->DataLen == sizeof(Ctx->Data)) {
            VolleySha256Transform(Ctx, Ctx->Data);
            Ctx->BitLen += 512;
            Ctx->DataLen = 0;
        }
    }
}

STATIC
VOID
VolleySha256Final(
    IN OUT VOLLEY_SHA256_CTX *Ctx,
    OUT UINT8                *Digest
)
{
    UINTN I;
    UINT64 BitLen;

    BitLen = Ctx->BitLen + ((UINT64)Ctx->DataLen * 8);

    Ctx->Data[Ctx->DataLen++] = 0x80;
    if (Ctx->DataLen > 56) {
        SetMem(Ctx->Data + Ctx->DataLen, 64 - Ctx->DataLen, 0);
        VolleySha256Transform(Ctx, Ctx->Data);
        Ctx->DataLen = 0;
    }

    SetMem(Ctx->Data + Ctx->DataLen, 56 - Ctx->DataLen, 0);
    Ctx->Data[56] = (UINT8)(BitLen >> 56);
    Ctx->Data[57] = (UINT8)(BitLen >> 48);
    Ctx->Data[58] = (UINT8)(BitLen >> 40);
    Ctx->Data[59] = (UINT8)(BitLen >> 32);
    Ctx->Data[60] = (UINT8)(BitLen >> 24);
    Ctx->Data[61] = (UINT8)(BitLen >> 16);
    Ctx->Data[62] = (UINT8)(BitLen >> 8);
    Ctx->Data[63] = (UINT8)(BitLen);
    VolleySha256Transform(Ctx, Ctx->Data);

    for (I = 0; I < 8; I++) {
        Digest[I * 4] = (UINT8)(Ctx->State[I] >> 24);
        Digest[I * 4 + 1] = (UINT8)(Ctx->State[I] >> 16);
        Digest[I * 4 + 2] = (UINT8)(Ctx->State[I] >> 8);
        Digest[I * 4 + 3] = (UINT8)(Ctx->State[I]);
    }
}

STATIC
INTN
HexCharToNibble(
    IN CHAR8 C
)
{
    if (C >= '0' && C <= '9') {
        return C - '0';
    }
    if (C >= 'a' && C <= 'f') {
        return 10 + (C - 'a');
    }
    if (C >= 'A' && C <= 'F') {
        return 10 + (C - 'A');
    }
    return -1;
}

STATIC
EFI_STATUS
ParseSha256DigestSpan(
    IN  CONST CHAR8 *Start,
    IN  CONST CHAR8 *End,
    OUT UINT8       *Digest
)
{
    UINTN Index;
    INTN  Hi;
    INTN  Lo;

    if (Start == NULL || End == NULL || Digest == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    if ((End <= Start) || ((End - Start) != VOLLEY_SHA256_HEX_LEN)) {
        return EFI_INVALID_PARAMETER;
    }

    for (Index = 0; Index < VOLLEY_SHA256_DIGEST_SIZE; Index++) {
        Hi = HexCharToNibble(Start[Index * 2]);
        Lo = HexCharToNibble(Start[Index * 2 + 1]);
        if (Hi < 0 || Lo < 0) {
            return EFI_INVALID_PARAMETER;
        }
        Digest[Index] = (UINT8)((Hi << 4) | Lo);
    }

    return EFI_SUCCESS;
}

STATIC
EFI_STATUS
ParseSlotCheckEntry(
    IN  CONST CHAR8         *Value,
    IN  VOLLEY_SLOT_META    *SlotMeta
)
{
    CONST CHAR8 *ValueStart;
    CONST CHAR8 *ValueEnd;
    CONST CHAR8 *AlgStart;
    CONST CHAR8 *AlgEnd;
    CONST CHAR8 *DigestStart;
    CONST CHAR8 *DigestEnd;
    CONST CHAR8 *PathStart;
    CONST CHAR8 *PathEnd;
    CONST CHAR8 *Comma;
    CONST CHAR8 *Comma2;
    CHAR8        Alg[16];
    CHAR8        PathAscii[VOLLEY_MAX_CHECK_PATH_CHARS];
    UINTN        AlgLen;
    UINTN        PathLen;
    EFI_STATUS   Status;
    RETURN_STATUS StrStatus;
    VOLLEY_SLOT_META *Meta;
    UINT8        Digest[VOLLEY_SHA256_DIGEST_SIZE];

    if (Value == NULL || SlotMeta == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    Meta = SlotMeta;
    if (Meta->CheckCount >= VOLLEY_MAX_CHECKS) {
        return EFI_BUFFER_TOO_SMALL;
    }

    ValueStart = Value;
    ValueEnd = Value + AsciiStrLen(Value);
    TrimAsciiSpan(&ValueStart, &ValueEnd);
    if (ValueEnd <= ValueStart) {
        return EFI_INVALID_PARAMETER;
    }

    AlgStart = ValueStart;
    Comma = AlgStart;
    while (Comma < ValueEnd && *Comma != ',') {
        Comma++;
    }
    if (Comma >= ValueEnd) {
        return EFI_INVALID_PARAMETER;
    }

    AlgEnd = Comma;
    TrimAsciiSpan(&AlgStart, &AlgEnd);
    AlgLen = (UINTN)(AlgEnd - AlgStart);
    if (AlgLen == 0 || AlgLen >= sizeof(Alg)) {
        return EFI_INVALID_PARAMETER;
    }
    CopyMem(Alg, AlgStart, AlgLen);
    Alg[AlgLen] = '\0';

    if (AsciiStriCmp(Alg, "sha256") != 0) {
        return EFI_UNSUPPORTED;
    }

    DigestStart = Comma + 1;
    Comma2 = DigestStart;
    while (Comma2 < ValueEnd && *Comma2 != ',') {
        Comma2++;
    }
    if (Comma2 >= ValueEnd) {
        return EFI_INVALID_PARAMETER;
    }

    DigestEnd = Comma2;
    TrimAsciiSpan(&DigestStart, &DigestEnd);
    Status = ParseSha256DigestSpan(DigestStart, DigestEnd, Digest);
    if (EFI_ERROR(Status)) {
        return Status;
    }

    PathStart = Comma2 + 1;
    PathEnd = ValueEnd;
    TrimAsciiSpan(&PathStart, &PathEnd);
    PathLen = (UINTN)(PathEnd - PathStart);
    if (PathLen == 0 || PathLen >= sizeof(PathAscii)) {
        return EFI_INVALID_PARAMETER;
    }

    CopyMem(PathAscii, PathStart, PathLen);
    PathAscii[PathLen] = '\0';

    Meta->Checks[Meta->CheckCount].Valid = TRUE;
    CopyMem(Meta->Checks[Meta->CheckCount].Sha256, Digest, sizeof(Digest));
    StrStatus = AsciiStrToUnicodeStrS(PathAscii, Meta->Checks[Meta->CheckCount].Path,
                                      VOLLEY_MAX_CHECK_PATH_CHARS);
    if (RETURN_ERROR(StrStatus)) {
        Meta->Checks[Meta->CheckCount].Valid = FALSE;
        return EFI_INVALID_PARAMETER;
    }

    Meta->CheckCount++;
    return EFI_SUCCESS;
}

/**
  Parse a key=value configuration file and invoke callback for each pair.

  Handles:
  - Lines starting with '#' (comments, ignored)
  - Empty lines (ignored)
  - Both \r\n (Windows) and \n (Unix) line endings
  - No spaces around '=' sign expected

  @param[in]  Buffer      ASCII file content
  @param[in]  BufferSize  Size of buffer in bytes
  @param[in]  Callback    Function to call for each key=value pair
  @param[in]  Context     User context passed to callback

  @retval EFI_SUCCESS     File parsed successfully
**/
STATIC
EFI_STATUS
TrimAsciiSpan(
    IN OUT CONST CHAR8 **Start,
    IN OUT CONST CHAR8 **End
)
{
    const CHAR8 *S;
    const CHAR8 *E;

    if (Start == NULL || End == NULL || *Start == NULL || *End == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    S = *Start;
    E = *End;

    while (S < E && (*S == ' ' || *S == '\t')) {
        S++;
    }

    while (E > S && (E[-1] == ' ' || E[-1] == '\t')) {
        E--;
    }

    *Start = S;
    *End = E;
    return EFI_SUCCESS;
}

STATIC
EFI_STATUS
ParseKeyValueFile(
    IN  CONST CHAR8     *Buffer,
    IN  UINTN           BufferSize,
    IN  EFI_STATUS      (*Callback)(CONST CHAR8 *Key, CONST CHAR8 *Value, VOID *Context),
    IN  VOID            *Context
)
{
    CONST CHAR8 *LineStart;
    CONST CHAR8 *LineEnd;
    CONST CHAR8 *BufferEnd;
    CONST CHAR8 *Equals;
    CONST CHAR8 *KeyStart;
    CONST CHAR8 *KeyEnd;
    CONST CHAR8 *ValueStart;
    CONST CHAR8 *ValueEnd;
    CONST CHAR8 *Comment;
    CHAR8       Key[64];
    CHAR8       Value[256];
    UINTN       KeyLen;
    UINTN       ValueLen;
    EFI_STATUS  Status;

    if (Buffer == NULL || BufferSize == 0 || Callback == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    BufferEnd = Buffer + BufferSize;
    LineStart = Buffer;

    while (LineStart < BufferEnd) {
        // Find end of line
        LineEnd = LineStart;
        while (LineEnd < BufferEnd && *LineEnd != '\n' && *LineEnd != '\r') {
            LineEnd++;
        }

        // Skip empty lines and comments
        KeyStart = LineStart;
        KeyEnd = LineEnd;
        TrimAsciiSpan(&KeyStart, &KeyEnd);
        if (KeyEnd > KeyStart && *KeyStart != '#') {
            // Find '=' separator
            Equals = KeyStart;
            while (Equals < KeyEnd && *Equals != '=') {
                Equals++;
            }

            if (Equals < KeyEnd && Equals > KeyStart) {
                // Extract key
                KeyStart = KeyStart;
                KeyEnd = Equals;
                TrimAsciiSpan(&KeyStart, &KeyEnd);
                KeyLen = (UINTN)(KeyEnd - KeyStart);
                if (KeyLen >= sizeof(Key)) {
                    KeyLen = sizeof(Key) - 1;
                }
                CopyMem(Key, KeyStart, KeyLen);
                Key[KeyLen] = '\0';

                // Extract value
                ValueStart = Equals + 1;
                ValueEnd = LineEnd;
                TrimAsciiSpan(&ValueStart, &ValueEnd);

                // Strip inline comments in value
                Comment = ValueStart;
                while (Comment < ValueEnd && *Comment != '#') {
                    Comment++;
                }
                if (Comment < ValueEnd) {
                    ValueEnd = Comment;
                    TrimAsciiSpan(&ValueStart, &ValueEnd);
                }

                ValueLen = (UINTN)(ValueEnd - ValueStart);
                if (ValueLen >= sizeof(Value)) {
                    ValueLen = sizeof(Value) - 1;
                }
                CopyMem(Value, ValueStart, ValueLen);
                Value[ValueLen] = '\0';

                // Call callback
                Status = Callback(Key, Value, Context);
                if (EFI_ERROR(Status)) {
                    return Status;
                }
            }
        }

        // Move to next line
        LineStart = LineEnd;
        if (LineStart < BufferEnd && *LineStart == '\r') {
            LineStart++;
        }
        if (LineStart < BufferEnd && *LineStart == '\n') {
            LineStart++;
        }
    }

    return EFI_SUCCESS;
}

/**
  Callback for parsing boot_config.txt - extracts expected_nvme_uuid
**/
STATIC
EFI_STATUS
BootConfigCallback(
    IN  CONST CHAR8     *Key,
    IN  CONST CHAR8     *Value,
    IN  VOID            *Context
)
{
    VOLLEY_BOOT_CONFIG  *Config = (VOLLEY_BOOT_CONFIG *)Context;
    RETURN_STATUS       ReturnStatus;
    CHAR16              UuidStr[64];

    if (AsciiStrCmp(Key, "expected_nvme_uuid") == 0) {
        // Convert ASCII to Unicode for GUID parsing
        AsciiStrToUnicodeStrS(Value, UuidStr, sizeof(UuidStr) / sizeof(CHAR16));

        // Parse the GUID string
        ReturnStatus = StrToGuid(UuidStr, &Config->ExpectedNvmeUuid);
        if (!RETURN_ERROR(ReturnStatus)) {
            Config->Valid = TRUE;
            DEBUG((DEBUG_INFO, "Volley: Parsed expected_nvme_uuid: %g\n", &Config->ExpectedNvmeUuid));
        } else {
            DEBUG((DEBUG_ERROR, "Volley: Failed to parse UUID: %a\n", Value));
        }
    }

    return EFI_SUCCESS;
}

/**
  Read and parse /boot_config.txt from eMMC ESP partition.

  @param[in]  EspHandle   Handle to ESP partition
  @param[out] Config      Parsed configuration

  @retval EFI_SUCCESS     Configuration read and parsed successfully
  @retval EFI_NOT_FOUND   File not found
  @retval Other           Read or parse error
**/
STATIC
EFI_STATUS
ReadVolleyBootConfig(
    IN  EFI_HANDLE          EspHandle,
    OUT VOLLEY_BOOT_CONFIG  *Config
)
{
    EFI_STATUS  Status;
    VOID        *FileData = NULL;
    UINT64      FileSize = 0;

    if (Config == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    ZeroMem(Config, sizeof(VOLLEY_BOOT_CONFIG));
    Config->Valid = FALSE;

    // Read boot_config.txt from ESP
    Status = OpenAndReadUntrustedFileToBuffer(
        EspHandle,
        VOLLEY_BOOT_CONFIG_PATH,
        NULL,
        &FileData,
        &FileSize
    );

    if (EFI_ERROR(Status)) {
        DEBUG((DEBUG_INFO, "Volley: boot_config.txt not found or unreadable: %r\n", Status));
        return Status;
    }

    if (FileData == NULL || FileSize == 0) {
        DEBUG((DEBUG_INFO, "Volley: boot_config.txt is empty\n"));
        return EFI_NOT_FOUND;
    }

    // Parse the file
    Status = ParseKeyValueFile((CHAR8 *)FileData, (UINTN)FileSize, BootConfigCallback, Config);

    FreePool(FileData);

    if (!Config->Valid) {
        DEBUG((DEBUG_INFO, "Volley: boot_config.txt missing expected_nvme_uuid\n"));
        return EFI_NOT_FOUND;
    }

    return EFI_SUCCESS;
}

/**
  Connect all PCI root bridges to enumerate PCI devices including NVMe.

  This is necessary before attempting to find NVMe devices, as they may
  not be visible until the PCI bus driver connects the root bridges.
**/
STATIC
VOID
ConnectPciRootBridges(
    VOID
)
{
    EFI_STATUS  Status;
    EFI_HANDLE  *Handles = NULL;
    UINTN       NumHandles;
    UINTN       Index;

    // Locate all PCI root bridge handles
    Status = gBS->LocateHandleBuffer(
        ByProtocol,
        &gEfiPciRootBridgeIoProtocolGuid,
        NULL,
        &NumHandles,
        &Handles
    );

    if (EFI_ERROR(Status)) {
        ErrorPrint(L"Volley: No PCI root bridges found: %r\r\n", Status);
        return;
    }

    ErrorPrint(L"Volley: Connecting %u PCI root bridge(s)...\r\n", NumHandles);

    // Connect each root bridge to enumerate PCI devices
    for (Index = 0; Index < NumHandles; Index++) {
        Status = gBS->ConnectController(
            Handles[Index],
            NULL,   // DriverImageHandle - use all available drivers
            NULL,   // RemainingDevicePath - produce all children
            TRUE    // Recursive - connect all child controllers
        );
        if (EFI_ERROR(Status)) {
            DEBUG((DEBUG_WARN, "Volley: Failed to connect PCI root bridge %u: %r\n", Index, Status));
        }
    }

    FreePool(Handles);
    ErrorPrint(L"Volley: PCI enumeration complete\r\n");
}

/**
  Find NVMe disk device by enumerating Block I/O protocol handles.

  @param[out] BlockIo       Block I/O protocol for NVMe disk
  @param[out] DeviceHandle  Handle for NVMe device

  @retval EFI_SUCCESS       NVMe device found
  @retval EFI_NOT_FOUND     No NVMe device present
**/
STATIC
EFI_STATUS
FindNvmeDevice(
    OUT EFI_BLOCK_IO_PROTOCOL   **BlockIo,
    OUT EFI_HANDLE              *DeviceHandle
)
{
    EFI_STATUS                  Status;
    UINTN                       NumHandles;
    EFI_HANDLE                  *HandleBuffer = NULL;
    UINTN                       Index;
    EFI_BLOCK_IO_PROTOCOL       *TempBlockIo;
    EFI_DEVICE_PATH_PROTOCOL    *DevicePath;
    EFI_DEVICE_PATH_PROTOCOL    *Node;

    if (BlockIo == NULL || DeviceHandle == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    *BlockIo = NULL;
    *DeviceHandle = NULL;

    // Get all Block I/O handles
    Status = gBS->LocateHandleBuffer(
        ByProtocol,
        &gEfiBlockIoProtocolGuid,
        NULL,
        &NumHandles,
        &HandleBuffer
    );

    if (EFI_ERROR(Status)) {
        DEBUG((DEBUG_INFO, "Volley: No Block I/O devices found\n"));
        return Status;
    }

    DEBUG((DEBUG_INFO, "Volley: Found %u Block I/O handles\n", NumHandles));

    for (Index = 0; Index < NumHandles; Index++) {
        // Get Block I/O protocol
        Status = gBS->HandleProtocol(
            HandleBuffer[Index],
            &gEfiBlockIoProtocolGuid,
            (VOID **)&TempBlockIo
        );
        if (EFI_ERROR(Status)) {
            continue;
        }

        // Skip partitions - we want the whole disk
        if (TempBlockIo->Media->LogicalPartition) {
            continue;
        }

        // Get device path
        Status = gBS->HandleProtocol(
            HandleBuffer[Index],
            &gEfiDevicePathProtocolGuid,
            (VOID **)&DevicePath
        );
        if (EFI_ERROR(Status)) {
            continue;
        }

        // Walk device path looking for NVMe node
        Node = DevicePath;
        while (!IsDevicePathEnd(Node)) {
            if (DevicePathType(Node) == MESSAGING_DEVICE_PATH &&
                DevicePathSubType(Node) == MSG_NVME_NAMESPACE_DP) {
                // Found NVMe device
                DEBUG((DEBUG_INFO, "Volley: Found NVMe device at handle index %u\n", Index));
                *BlockIo = TempBlockIo;
                *DeviceHandle = HandleBuffer[Index];
                FreePool(HandleBuffer);
                return EFI_SUCCESS;
            }
            Node = NextDevicePathNode(Node);
        }
    }

    FreePool(HandleBuffer);
    DEBUG((DEBUG_INFO, "Volley: No NVMe device found\n"));
    return EFI_NOT_FOUND;
}

/**
  Read GPT header from device and extract DiskGUID.

  @param[in]  BlockIo     Block I/O protocol for device
  @param[out] DiskGuid    The GPT Disk GUID

  @retval EFI_SUCCESS         GUID read successfully
  @retval EFI_VOLUME_CORRUPTED Invalid GPT header
**/
STATIC
EFI_STATUS
ReadNvmeDiskGuid(
    IN  EFI_BLOCK_IO_PROTOCOL   *BlockIo,
    OUT EFI_GUID                *DiskGuid
)
{
    EFI_STATUS                  Status;
    VOID                        *Buffer = NULL;
    UINT32                      BlockSize;
    EFI_PARTITION_TABLE_HEADER  *GptHeader;

    if (BlockIo == NULL || DiskGuid == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    BlockSize = BlockIo->Media->BlockSize;

    // Allocate buffer for GPT header (LBA 1)
    Buffer = AllocatePool(BlockSize);
    if (Buffer == NULL) {
        return EFI_OUT_OF_RESOURCES;
    }

    // Read LBA 1 (primary GPT header)
    Status = BlockIo->ReadBlocks(
        BlockIo,
        BlockIo->Media->MediaId,
        1,  // GPT header is at LBA 1
        BlockSize,
        Buffer
    );

    if (EFI_ERROR(Status)) {
        DEBUG((DEBUG_ERROR, "Volley: Failed to read GPT header: %r\n", Status));
        FreePool(Buffer);
        return Status;
    }

    GptHeader = (EFI_PARTITION_TABLE_HEADER *)Buffer;

    // Validate GPT signature
    if (GptHeader->Header.Signature != EFI_PTAB_HEADER_ID) {
        DEBUG((DEBUG_ERROR, "Volley: Invalid GPT signature\n"));
        FreePool(Buffer);
        return EFI_VOLUME_CORRUPTED;
    }

    // Extract DiskGUID
    CopyGuid(DiskGuid, &GptHeader->DiskGUID);
    DEBUG((DEBUG_INFO, "Volley: NVMe Disk GUID: %g\n", DiskGuid));

    FreePool(Buffer);
    return EFI_SUCCESS;
}

/**
  Find NVMe partition handle by partition number.

  @param[in]  NvmeDeviceHandle  Handle to parent NVMe device
  @param[in]  PartitionNumber   1 for p1, 2 for p2
  @param[out] PartitionHandle   Handle for the partition

  @retval EFI_SUCCESS       Partition found
  @retval EFI_NOT_FOUND     Partition not found
**/
STATIC
EFI_STATUS
FindNvmePartitionByNumber(
    IN  EFI_HANDLE      NvmeDeviceHandle,
    IN  UINT32          PartitionNumber,
    OUT EFI_HANDLE      *PartitionHandle
)
{
    EFI_STATUS                  Status;
    UINTN                       NumHandles;
    EFI_HANDLE                  *HandleBuffer = NULL;
    UINTN                       Index;
    EFI_PARTITION_INFO_PROTOCOL *PartitionInfo;
    EFI_DEVICE_PATH_PROTOCOL    *ParentPath;
    EFI_DEVICE_PATH_PROTOCOL    *ChildPath;
    EFI_DEVICE_PATH_PROTOCOL    *Node;
    HARDDRIVE_DEVICE_PATH       *HdPath;
    UINTN                       ParentPathSize;

    if (PartitionHandle == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    *PartitionHandle = NULL;

    // Get parent device path
    Status = gBS->HandleProtocol(
        NvmeDeviceHandle,
        &gEfiDevicePathProtocolGuid,
        (VOID **)&ParentPath
    );
    if (EFI_ERROR(Status)) {
        return Status;
    }
    ParentPathSize = GetDevicePathSize(ParentPath) - sizeof(EFI_DEVICE_PATH_PROTOCOL);

    // Get all partition info handles
    Status = gBS->LocateHandleBuffer(
        ByProtocol,
        &gEfiPartitionInfoProtocolGuid,
        NULL,
        &NumHandles,
        &HandleBuffer
    );

    if (EFI_ERROR(Status)) {
        return Status;
    }

    for (Index = 0; Index < NumHandles; Index++) {
        // Get partition info
        Status = gBS->HandleProtocol(
            HandleBuffer[Index],
            &gEfiPartitionInfoProtocolGuid,
            (VOID **)&PartitionInfo
        );
        if (EFI_ERROR(Status)) {
            continue;
        }

        // Check if it's a GPT partition
        if (PartitionInfo->Type != PARTITION_TYPE_GPT) {
            continue;
        }

        // Get child device path
        Status = gBS->HandleProtocol(
            HandleBuffer[Index],
            &gEfiDevicePathProtocolGuid,
            (VOID **)&ChildPath
        );
        if (EFI_ERROR(Status)) {
            continue;
        }

        // Check if child is under parent (device path starts with parent path)
        if (CompareMem(ChildPath, ParentPath, ParentPathSize) != 0) {
            continue;
        }

        // Check partition number using the hard drive device path
        Node = ChildPath;
        while (!IsDevicePathEnd(Node)) {
            if (DevicePathType(Node) == MEDIA_DEVICE_PATH &&
                DevicePathSubType(Node) == MEDIA_HARDDRIVE_DP) {
                HdPath = (HARDDRIVE_DEVICE_PATH *)Node;
                if (HdPath->PartitionNumber == PartitionNumber) {
                    DEBUG((DEBUG_INFO, "Volley: Found NVMe partition %u\n", PartitionNumber));
                    *PartitionHandle = HandleBuffer[Index];
                    FreePool(HandleBuffer);
                    return EFI_SUCCESS;
                }
            }
            Node = NextDevicePathNode(Node);
        }
    }

    FreePool(HandleBuffer);
    DEBUG((DEBUG_INFO, "Volley: NVMe partition %u not found\n", PartitionNumber));
    return EFI_NOT_FOUND;
}

/**
  Callback for parsing slot_meta.txt - extracts update_counter and valid flag
**/
STATIC
EFI_STATUS
SlotMetaCallback(
    IN  CONST CHAR8     *Key,
    IN  CONST CHAR8     *Value,
    IN  VOID            *Context
)
{
    VOLLEY_SLOT_META *SlotMeta = (VOLLEY_SLOT_META *)Context;
    EFI_STATUS        Status;

    if (AsciiStrCmp(Key, "update_counter") == 0) {
        SlotMeta->UpdateCounter = (UINT32)AsciiStrDecimalToUintn(Value);
        DEBUG((DEBUG_INFO, "Volley: Parsed update_counter=%u\n", SlotMeta->UpdateCounter));
    } else if (AsciiStrCmp(Key, "valid") == 0) {
        if (AsciiStrCmp(Value, "1") == 0) {
            SlotMeta->Valid = TRUE;
            DEBUG((DEBUG_INFO, "Volley: Slot is valid\n"));
        }
    } else if (AsciiStrCmp(Key, "check") == 0) {
        Status = ParseSlotCheckEntry(Value, SlotMeta);
        if (EFI_ERROR(Status)) {
            SlotMeta->ParseError = TRUE;
            ErrorPrint(L"Volley: Invalid check entry in slot_meta.txt\r\n");
        }
    }

    return EFI_SUCCESS;
}

/**
  Read and parse /slot_meta.txt from NVMe partition.

  @param[in]  PartitionHandle   Handle to FAT32 partition
  @param[out] SlotMeta          Parsed slot metadata

  @retval EFI_SUCCESS       Metadata read and parsed successfully
  @retval EFI_NOT_FOUND     File not found or slot invalid
**/
STATIC
EFI_STATUS
ReadSlotMetadata(
    IN  EFI_HANDLE          PartitionHandle,
    OUT VOLLEY_SLOT_META    *SlotMeta
)
{
    EFI_STATUS  Status;
    VOID        *FileData = NULL;
    UINT64      FileSize = 0;

    if (SlotMeta == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    ZeroMem(SlotMeta, sizeof(VOLLEY_SLOT_META));
    SlotMeta->ParseError = FALSE;
    SlotMeta->Valid = FALSE;
    SlotMeta->UpdateCounter = 0;
    SlotMeta->CheckCount = 0;

    // Read slot_meta.txt from partition
    Status = OpenAndReadUntrustedFileToBuffer(
        PartitionHandle,
        VOLLEY_SLOT_META_PATH,
        NULL,
        &FileData,
        &FileSize
    );

    if (EFI_ERROR(Status)) {
        DEBUG((DEBUG_INFO, "Volley: slot_meta.txt not found: %r\n", Status));
        return Status;
    }

    if (FileData == NULL || FileSize == 0) {
        DEBUG((DEBUG_INFO, "Volley: slot_meta.txt is empty\n"));
        return EFI_NOT_FOUND;
    }

    // Parse the file
    Status = ParseKeyValueFile((CHAR8 *)FileData, (UINTN)FileSize, SlotMetaCallback, SlotMeta);

    FreePool(FileData);

    if (EFI_ERROR(Status)) {
        return Status;
    }

    if (SlotMeta->ParseError) {
        SlotMeta->Valid = FALSE;
    }

    if (!SlotMeta->Valid) {
        DEBUG((DEBUG_INFO, "Volley: Slot marked as invalid (valid != 1)\n"));
    }

    return EFI_SUCCESS;
}

STATIC
EFI_STATUS
HashFileSha256(
    IN  EFI_HANDLE      PartitionHandle,
    IN  CONST CHAR16    *FileName,
    OUT UINT8           *Digest
)
{
    EFI_STATUS      Status = EFI_SUCCESS;
    EFI_DEVICE_PATH *DevicePath = NULL;
    EFI_DEVICE_PATH *NextDevicePath;
    EFI_FILE_HANDLE Handle = NULL;
    VOLLEY_SHA256_CTX Ctx;
    UINT8            *Buffer = NULL;
    UINTN            ReadSize;

    if (FileName == NULL || Digest == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    DevicePath = FileDevicePath(PartitionHandle, FileName);
    if (DevicePath == NULL) {
        Status = EFI_OUT_OF_RESOURCES;
        goto Exit;
    }

    NextDevicePath = DevicePath;
    Status = EfiOpenFileByDevicePath(&NextDevicePath, &Handle, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(Status)) {
        goto Exit;
    }

    VolleySha256Init(&Ctx);

    Buffer = AllocatePool(VOLLEY_HASH_READ_CHUNK_SIZE);
    if (Buffer == NULL) {
        Status = EFI_OUT_OF_RESOURCES;
        goto Exit;
    }

    do {
        ReadSize = VOLLEY_HASH_READ_CHUNK_SIZE;
        Status = FileHandleRead(Handle, &ReadSize, Buffer);
        if (EFI_ERROR(Status)) {
            goto Exit;
        }
        if (ReadSize == 0) {
            break;
        }
        VolleySha256Update(&Ctx, Buffer, ReadSize);
    } while (ReadSize > 0);

    VolleySha256Final(&Ctx, Digest);

Exit:
    if (Buffer != NULL) {
        FreePool(Buffer);
    }
    if (Handle != NULL) {
        FileHandleClose(Handle);
    }
    if (DevicePath != NULL) {
        FreePool(DevicePath);
    }

    return Status;
}

STATIC
EFI_STATUS
ValidateSlotChecks(
    IN  EFI_HANDLE          PartitionHandle,
    IN  VOLLEY_SLOT_META    *SlotMeta
)
{
    EFI_STATUS  Status;
    UINTN       Index;
    UINT8       Digest[VOLLEY_SHA256_DIGEST_SIZE];

    if (SlotMeta == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    if (!SlotMeta->Valid || SlotMeta->CheckCount == 0) {
        return EFI_SUCCESS;
    }

    for (Index = 0; Index < SlotMeta->CheckCount; Index++) {
        if (!SlotMeta->Checks[Index].Valid) {
            SlotMeta->Valid = FALSE;
            return EFI_INVALID_PARAMETER;
        }

        Status = HashFileSha256(PartitionHandle, SlotMeta->Checks[Index].Path, Digest);
        if (EFI_ERROR(Status)) {
            ErrorPrint(L"Volley: Hash read failed for %s: %r\r\n",
                       SlotMeta->Checks[Index].Path, Status);
            SlotMeta->Valid = FALSE;
            return Status;
        }

        if (CompareMem(Digest, SlotMeta->Checks[Index].Sha256, sizeof(Digest)) != 0) {
            ErrorPrint(L"Volley: Hash mismatch for %s\r\n", SlotMeta->Checks[Index].Path);
            SlotMeta->Valid = FALSE;
            return EFI_COMPROMISED_DATA;
        }

        ErrorPrint(L"Volley: Hash OK for %s\r\n", SlotMeta->Checks[Index].Path);
    }

    return EFI_SUCCESS;
}

/**
  Validate that a partition is bootable for Volley.

  Requires a mountable filesystem and the kernel image present.

  @param[in]  PartitionHandle  Handle to the partition
  @param[in]  KernelPath       Path to kernel image (device-relative)

  @retval EFI_SUCCESS          Bootable
  @retval Others               Not bootable
**/
STATIC
EFI_STATUS
ValidateVolleyBootPartition(
    IN  EFI_HANDLE      PartitionHandle,
    IN  CONST CHAR16    *KernelPath
)
{
    EFI_STATUS       Status;
    EFI_FILE_HANDLE  KernelHandle = NULL;
    VOID             *Fs = NULL;

    if (KernelPath == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    Status = gBS->HandleProtocol(PartitionHandle, &gEfiSimpleFileSystemProtocolGuid, &Fs);
    if (EFI_ERROR(Status)) {
        return Status;
    }

    Status = OpenAndReadUntrustedFileToBuffer(PartitionHandle, KernelPath, &KernelHandle, NULL, NULL);
    if (!EFI_ERROR(Status) && KernelHandle != NULL) {
        FileHandleClose(KernelHandle);
    }

    return Status;
}

/**
  Select the boot slot after validation.

  @param[in]  SlotA         Metadata for slot A
  @param[in]  SlotB         Metadata for slot B
  @param[out] SelectedMode  Selected boot mode

  @retval EFI_SUCCESS       Slot selected successfully
  @retval EFI_NOT_FOUND     Both slots are invalid
**/
STATIC
EFI_STATUS
SelectBestSlot(
    IN  CONST VOLLEY_SLOT_META  *SlotA,
    IN  CONST VOLLEY_SLOT_META  *SlotB,
    OUT VOLLEY_BOOT_MODE        *SelectedMode
)
{
    if ((SlotA == NULL) || (SlotB == NULL) || (SelectedMode == NULL)) {
        return EFI_INVALID_PARAMETER;
    }

    if (SlotA->Valid && SlotB->Valid) {
        if (SlotA->UpdateCounter >= SlotB->UpdateCounter) {
            DEBUG((DEBUG_INFO, "Volley: Selecting Slot A (counter %u >= %u)\n",
                   SlotA->UpdateCounter, SlotB->UpdateCounter));
            *SelectedMode = VOLLEY_MODE_SLOT_A;
        } else {
            DEBUG((DEBUG_INFO, "Volley: Selecting Slot B (counter %u > %u)\n",
                   SlotB->UpdateCounter, SlotA->UpdateCounter));
            *SelectedMode = VOLLEY_MODE_SLOT_B;
        }
        return EFI_SUCCESS;
    }

    if (SlotA->Valid) {
        DEBUG((DEBUG_INFO, "Volley: Only Slot A is valid\n"));
        *SelectedMode = VOLLEY_MODE_SLOT_A;
        return EFI_SUCCESS;
    }

    if (SlotB->Valid) {
        DEBUG((DEBUG_INFO, "Volley: Only Slot B is valid\n"));
        *SelectedMode = VOLLEY_MODE_SLOT_B;
        return EFI_SUCCESS;
    }

    DEBUG((DEBUG_INFO, "Volley: Both slots invalid, will use install mode\n"));
    return EFI_NOT_FOUND;
}

/**
  Main orchestrator - determine Volley boot mode.

  Determines boot mode and which partition to load kernel from.

  Algorithm:
  1. Read boot_config.txt from eMMC APP partition
  2. Connect PCI root bridges to enumerate NVMe devices
  3. Find NVMe device
  4. Read NVMe GPT DiskGUID
  5. Compare UUIDs - if mismatch, install mode
  6. If match, read slot metadata and select best slot
  7. Verify selected slot's partition GUID matches expected PARTUUID

  @param[in]  EspHandle     Handle to eMMC ESP partition (where L4TLauncher lives)
  @param[in]  EmmcAppHandle Handle to eMMC APP partition (for install mode kernel)
  @param[out] BootMode      Determined boot mode
  @param[out] RootFsHandle  Handle to partition to load kernel from

  @retval EFI_SUCCESS       Boot mode determined successfully
**/
STATIC
EFI_STATUS
VolleyDetermineBootMode(
    IN  EFI_HANDLE          EspHandle,
    IN  EFI_HANDLE          EmmcAppHandle,
    OUT VOLLEY_BOOT_MODE    *BootMode,
    OUT EFI_HANDLE          *RootFsHandle
)
{
    EFI_STATUS                  Status;
    VOLLEY_BOOT_CONFIG          BootConfig;
    EFI_BLOCK_IO_PROTOCOL       *NvmeBlockIo = NULL;
    EFI_HANDLE                  NvmeDeviceHandle = NULL;
    EFI_GUID                    NvmeDiskGuid;
    EFI_HANDLE                  SlotAHandle = NULL;
    EFI_HANDLE                  SlotBHandle = NULL;
    VOLLEY_SLOT_META            SlotAMeta;
    VOLLEY_SLOT_META            SlotBMeta;
    EFI_PARTITION_INFO_PROTOCOL *PartInfo = NULL;
    EFI_GUID                    ExpectedGuid;
    EFI_HANDLE                  SelectedHandle = NULL;

    if (BootMode == NULL || RootFsHandle == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    ErrorPrint(L"Volley: Determining boot mode...\r\n");

    // Step 1: Read boot_config.txt from eMMC APP partition
    Status = ReadVolleyBootConfig(EmmcAppHandle, &BootConfig);
    if (EFI_ERROR(Status) || !BootConfig.Valid) {
        ErrorPrint(L"Volley: boot_config.txt missing or invalid\r\n");
        ErrorPrint(L"Volley: Install mode (PARTUUID=" VOLLEY_EMMC_APP_PARTUUID L")\r\n");
        *BootMode = VOLLEY_MODE_INSTALL;
        *RootFsHandle = EmmcAppHandle;
        return EFI_SUCCESS;
    }

    // Step 2: Connect PCI root bridges to enumerate NVMe devices
    ConnectPciRootBridges();

    // Step 3: Find NVMe device
    Status = FindNvmeDevice(&NvmeBlockIo, &NvmeDeviceHandle);
    if (EFI_ERROR(Status)) {
        ErrorPrint(L"Volley: No NVMe device found\r\n");
        ErrorPrint(L"Volley: Install mode (PARTUUID=" VOLLEY_EMMC_APP_PARTUUID L")\r\n");
        *BootMode = VOLLEY_MODE_INSTALL;
        *RootFsHandle = EmmcAppHandle;
        return EFI_SUCCESS;
    }

    // Step 4: Read NVMe GPT DiskGUID
    Status = ReadNvmeDiskGuid(NvmeBlockIo, &NvmeDiskGuid);
    if (EFI_ERROR(Status)) {
        ErrorPrint(L"Volley: Failed to read NVMe GPT header\r\n");
        ErrorPrint(L"Volley: Install mode (PARTUUID=" VOLLEY_EMMC_APP_PARTUUID L")\r\n");
        *BootMode = VOLLEY_MODE_INSTALL;
        *RootFsHandle = EmmcAppHandle;
        return EFI_SUCCESS;
    }

    // Step 5: Compare UUIDs
    if (!CompareGuid(&NvmeDiskGuid, &BootConfig.ExpectedNvmeUuid)) {
        ErrorPrint(L"Volley: NVMe UUID mismatch\r\n");
        ErrorPrint(L"  Expected: %g\r\n", &BootConfig.ExpectedNvmeUuid);
        ErrorPrint(L"  Actual:   %g\r\n", &NvmeDiskGuid);
        ErrorPrint(L"Volley: Install mode (PARTUUID=" VOLLEY_EMMC_APP_PARTUUID L")\r\n");
        *BootMode = VOLLEY_MODE_INSTALL;
        *RootFsHandle = EmmcAppHandle;
        return EFI_SUCCESS;
    }

    ErrorPrint(L"Volley: NVMe UUID matches (%g)\r\n", &NvmeDiskGuid);

    // Step 6: Find partition handles for slots A and B
    // Require a mountable filesystem and kernel image before reading slot_meta.txt
    Status = FindNvmePartitionByNumber(NvmeDeviceHandle, 1, &SlotAHandle);
    if (EFI_ERROR(Status)) {
        ErrorPrint(L"Volley: Slot A (p1) partition not found\r\n");
        ZeroMem(&SlotAMeta, sizeof(SlotAMeta));
    } else {
        Status = ValidateVolleyBootPartition(SlotAHandle, VOLLEY_DIRECT_KERNEL_PATH);
        if (EFI_ERROR(Status)) {
            ErrorPrint(L"Volley: Slot A not bootable (missing filesystem or kernel)\r\n");
            ZeroMem(&SlotAMeta, sizeof(SlotAMeta));
        } else {
            ReadSlotMetadata(SlotAHandle, &SlotAMeta);
            ValidateSlotChecks(SlotAHandle, &SlotAMeta);
            ErrorPrint(L"Volley: Slot A: valid=%d update_counter=%u\r\n",
                       SlotAMeta.Valid, SlotAMeta.UpdateCounter);
        }
    }

    Status = FindNvmePartitionByNumber(NvmeDeviceHandle, 2, &SlotBHandle);
    if (EFI_ERROR(Status)) {
        ErrorPrint(L"Volley: Slot B (p2) partition not found\r\n");
        ZeroMem(&SlotBMeta, sizeof(SlotBMeta));
    } else {
        Status = ValidateVolleyBootPartition(SlotBHandle, VOLLEY_DIRECT_KERNEL_PATH);
        if (EFI_ERROR(Status)) {
            ErrorPrint(L"Volley: Slot B not bootable (missing filesystem or kernel)\r\n");
            ZeroMem(&SlotBMeta, sizeof(SlotBMeta));
        } else {
            ReadSlotMetadata(SlotBHandle, &SlotBMeta);
            ValidateSlotChecks(SlotBHandle, &SlotBMeta);
            ErrorPrint(L"Volley: Slot B: valid=%d update_counter=%u\r\n",
                       SlotBMeta.Valid, SlotBMeta.UpdateCounter);
        }
    }

    // Step 7: Select best slot
    Status = SelectBestSlot(&SlotAMeta, &SlotBMeta, BootMode);
    if (EFI_ERROR(Status)) {
        ErrorPrint(L"Volley: Both slots invalid\r\n");
        ErrorPrint(L"Volley: Install mode (PARTUUID=" VOLLEY_EMMC_APP_PARTUUID L")\r\n");
        *BootMode = VOLLEY_MODE_INSTALL;
        *RootFsHandle = EmmcAppHandle;
        return EFI_SUCCESS;
    }

    // Step 8: Verify partition GUID matches expected PARTUUID
    if (*BootMode == VOLLEY_MODE_SLOT_A) {
        SelectedHandle = SlotAHandle;
        StrToGuid(VOLLEY_NVME_SLOT_A_PARTUUID, &ExpectedGuid);
    } else {
        SelectedHandle = SlotBHandle;
        StrToGuid(VOLLEY_NVME_SLOT_B_PARTUUID, &ExpectedGuid);
    }

    // Get actual partition GUID from GPT entry
    Status = gBS->HandleProtocol(SelectedHandle, &gEfiPartitionInfoProtocolGuid, (VOID**)&PartInfo);
    if (!EFI_ERROR(Status) && PartInfo->Type == PARTITION_TYPE_GPT) {
        if (!CompareGuid(&PartInfo->Info.Gpt.UniquePartitionGUID, &ExpectedGuid)) {
            ErrorPrint(L"Volley: PARTUUID mismatch!\r\n");
            ErrorPrint(L"  Expected: %g\r\n", &ExpectedGuid);
            ErrorPrint(L"  Actual:   %g\r\n", &PartInfo->Info.Gpt.UniquePartitionGUID);
            ErrorPrint(L"Volley: Install mode (PARTUUID=" VOLLEY_EMMC_APP_PARTUUID L")\r\n");
            *BootMode = VOLLEY_MODE_INSTALL;
            *RootFsHandle = EmmcAppHandle;
            return EFI_SUCCESS;
        }
    }

    // PARTUUID verified - set RootFsHandle to selected slot
    *RootFsHandle = SelectedHandle;

    // Log selected slot
    if (*BootMode == VOLLEY_MODE_SLOT_A) {
        ErrorPrint(L"Volley: Selected Slot A (PARTUUID=" VOLLEY_NVME_SLOT_A_PARTUUID L")\r\n");
    } else {
        ErrorPrint(L"Volley: Selected Slot B (PARTUUID=" VOLLEY_NVME_SLOT_B_PARTUUID L")\r\n");
    }

    return EFI_SUCCESS;
}

//
// ============================================================================
// End of Volley Boot Mode Detection Functions
// ============================================================================
//
#endif  // NVMe slot selection enabled

STATIC
EFI_STATUS
BuildVolleyBootConfigForMode(
    IN  VOLLEY_BOOT_MODE        Mode,
    OUT EXTLINUX_BOOT_CONFIG    *BootConfig
)
{
    EFI_STATUS Status;
    EXTLINUX_BOOT_OPTION* Option;

    if (BootConfig == NULL)
    {
        return EFI_INVALID_PARAMETER;
    }

    BootConfig->DefaultBootEntry = 0;
    BootConfig->NumberOfBootOptions = 1;
    BootConfig->Timeout = 0;

    Option = &BootConfig->BootOptions[0];

    Status = AllocateBootOptionString(&Option->Label, L"volley-default");
    if (EFI_ERROR(Status))
    {
        goto Error;
    }

    Status = AllocateBootOptionString(&Option->MenuLabel, L"Volley Direct Boot");
    if (EFI_ERROR(Status))
    {
        goto Error;
    }

    Status = AllocateBootOptionString(&Option->LinuxPath, VOLLEY_DIRECT_KERNEL_PATH);
    if (EFI_ERROR(Status))
    {
        goto Error;
    }

    Option->DtbPath = GetVolleyDtbPath();
    if (Option->DtbPath == NULL)
    {
        Status = AllocateBootOptionString(&Option->DtbPath,
                                          L"EFI\\volley\\dtb\\tegra194-p2888-0001-p2822-0000.dtb");
    }
    if (EFI_ERROR(Status))
    {
        goto Error;
    }

    Status = AllocateBootOptionString(&Option->InitrdPath, VOLLEY_DIRECT_INITRD_PATH);
    if (EFI_ERROR(Status))
    {
        goto Error;
    }

    // Set boot args based on mode - this is the key cmdline for Linux init
    // VOLLEY_BASE_CMDLINE provides earlycon/console/noinitrd (Jetson 5.10 has no CMDLINE_EXTEND)
    ErrorPrint(L"Volley: BuildVolleyBootConfigForMode: mode=%d\r\n", Mode);
    ErrorPrint(L"Volley: USB autosuspend disabled (usbcore.autosuspend=-1)\r\n");
    switch (Mode) {
        case VOLLEY_MODE_INSTALL:
            ErrorPrint(L"Volley: Setting boot args for INSTALL mode\r\n");
            Status = AllocateBootOptionString(&Option->BootArgs,
                VOLLEY_BASE_CMDLINE L"root=PARTUUID=" VOLLEY_EMMC_APP_PARTUUID L" volley.mode=install");
            break;
        case VOLLEY_MODE_SLOT_A:
            ErrorPrint(L"Volley: Setting boot args for SLOT_A mode\r\n");
            Status = AllocateBootOptionString(&Option->BootArgs,
                VOLLEY_BASE_CMDLINE L"root=PARTUUID=" VOLLEY_NVME_SLOT_A_PARTUUID);
            break;
        case VOLLEY_MODE_SLOT_B:
            ErrorPrint(L"Volley: Setting boot args for SLOT_B mode\r\n");
            Status = AllocateBootOptionString(&Option->BootArgs,
                VOLLEY_BASE_CMDLINE L"root=PARTUUID=" VOLLEY_NVME_SLOT_B_PARTUUID);
            break;
        default:
            ErrorPrint(L"Volley: Unknown mode %d, defaulting to INSTALL\r\n", Mode);
            Status = AllocateBootOptionString(&Option->BootArgs,
                VOLLEY_BASE_CMDLINE L"root=PARTUUID=" VOLLEY_EMMC_APP_PARTUUID L" volley.mode=install");
            break;
    }
    if (EFI_ERROR(Status))
    {
        goto Error;
    }

    ErrorPrint(L"Volley: Boot config built:\r\n");
    ErrorPrint(L"  Kernel: %s\r\n", Option->LinuxPath);
    ErrorPrint(L"  DTB:    %s\r\n", Option->DtbPath);
    ErrorPrint(L"  Args:   %s\r\n", Option->BootArgs);

    return EFI_SUCCESS;

Error:
    if (Option->Label != NULL)
    {
        FreePool(Option->Label);
        Option->Label = NULL;
    }

    if (Option->MenuLabel != NULL)
    {
        FreePool(Option->MenuLabel);
        Option->MenuLabel = NULL;
    }

    if (Option->LinuxPath != NULL)
    {
        FreePool(Option->LinuxPath);
        Option->LinuxPath = NULL;
    }

    if (Option->DtbPath != NULL)
    {
        FreePool(Option->DtbPath);
        Option->DtbPath = NULL;
    }

    if (Option->InitrdPath != NULL)
    {
        FreePool(Option->InitrdPath);
        Option->InitrdPath = NULL;
    }

    if (Option->BootArgs != NULL)
    {
        FreePool(Option->BootArgs);
        Option->BootArgs = NULL;
    }

    BootConfig->NumberOfBootOptions = 0;
    BootConfig->DefaultBootEntry = 0;

    return Status;
}

/**
  Process the extlinux.conf file

  @param[in]  DeviceHandle     The handle of partition where
this file lives on.
  @param[in]  BootChain        Numeric version of the chain
  @param[out] ExtLinuxConfig   Pointer to an extlinux config object
  @param[out] RootFsHandle     Pointer to the handle of the device tree

  @retval EFI_SUCCESS    The operation completed successfully.

**/
STATIC
EFI_STATUS
EFIAPI
ProcessExtLinuxConfig(IN EFI_HANDLE DeviceHandle, IN UINT32 BootChain,
                      OUT EXTLINUX_BOOT_CONFIG* BootConfig, OUT EFI_HANDLE* RootFsHandle)
{
    EFI_STATUS Status;
    UINTN Index;
    VOLLEY_BOOT_MODE BootMode;
    EFI_HANDLE EmmcAppHandle = NULL;

    ZeroMem(BootConfig, sizeof(EXTLINUX_BOOT_CONFIG));

    if (RootFsHandle == NULL)
    {
        return EFI_INVALID_PARAMETER;
    }

    // DeviceHandle is the eMMC ESP partition (where L4TLauncher.efi lives)
    ErrorPrint(L"Volley: ProcessExtLinuxConfig starting\r\n");
    ErrorPrint(L"Volley: ESP device handle: 0x%p\r\n", DeviceHandle);
    PrintPartitionUuid(DeviceHandle);

    // Find eMMC APP partition - kernel is at /boot/Image on this partition for install mode
    ErrorPrint(L"Volley: Searching for eMMC APP partition...\r\n");
    Status = FindPartitionInfo(DeviceHandle, L"APP", 0, NULL, &EmmcAppHandle);
    if (EFI_ERROR(Status) || EmmcAppHandle == NULL) {
        ErrorPrint(L"Volley: FAILED to find eMMC APP partition: %r\r\n", Status);
        ErrorPrint(L"Volley: Kernel cannot be loaded without APP partition\r\n");
        return EFI_NOT_FOUND;
    }
    ErrorPrint(L"Volley: Found eMMC APP partition: 0x%p\r\n", EmmcAppHandle);
    PrintPartitionUuid(EmmcAppHandle);

    // VolleyDetermineBootMode will:
    //   - Read boot_config.txt from EspHandle (1st arg)
    //   - Fall back to EmmcAppHandle (2nd arg) if install mode
    //   - Set RootFsHandle to NVMe slot if normal boot
    ErrorPrint(L"Volley: Calling VolleyDetermineBootMode...\r\n");
    Status = VolleyDetermineBootMode(DeviceHandle, EmmcAppHandle, &BootMode, RootFsHandle);
    if (EFI_ERROR(Status)) {
        ErrorPrint(L"Volley: Boot mode detection failed: %r\r\n", Status);
        ErrorPrint(L"Volley: Falling back to install mode\r\n");
        BootMode = VOLLEY_MODE_INSTALL;
        *RootFsHandle = EmmcAppHandle;
    }

    ErrorPrint(L"Volley: Boot mode = %d (0=INSTALL, 1=SLOT_A, 2=SLOT_B)\r\n", BootMode);
    ErrorPrint(L"Volley: RootFsHandle = 0x%p\r\n", *RootFsHandle);
    PrintPartitionUuid(*RootFsHandle);

    // Build boot configuration based on determined mode
    Status = BuildVolleyBootConfigForMode(BootMode, BootConfig);
    if (EFI_ERROR(Status))
    {
        return Status;
    }

    for (Index = 0; Index < BootConfig->NumberOfBootOptions; Index++)
    {
        if (BootConfig->BootOptions[Index].DtbPath != NULL)
        {
            PathCleanUpDirectories(BootConfig->BootOptions[Index].DtbPath);
        }

        if (BootConfig->BootOptions[Index].InitrdPath != NULL)
        {
            PathCleanUpDirectories(BootConfig->BootOptions[Index].InitrdPath);
        }

        if (BootConfig->BootOptions[Index].LinuxPath != NULL)
        {
            PathCleanUpDirectories(BootConfig->BootOptions[Index].LinuxPath);
        }
    }

    return EFI_SUCCESS;
}

/**
  Wait for user input boot option

  @param[out] ExtLinuxConfig   Pointer to an extlinux.conf file

  @retval Selected boot option.

**/
STATIC
UINT32
EFIAPI
ExtLinuxBootMenu(IN EXTLINUX_BOOT_CONFIG* BootConfig)
{
    //
    // Volley builds suppress all keyboard input, so present no delay/hotkey UI.
    //
    return BootConfig->DefaultBootEntry;
}

/**
  Boots an android style partition located with Partition base name and bootchain

  @param[in]  ImageHandle       Handle of this application
  @param[in]  DeviceHandle      The handle of partition where this file lives on.
  @param[in]  BootOption        Boot options to load

  @retval EFI_SUCCESS    The operation completed successfully.

**/
STATIC
EFI_STATUS
EFIAPI
ExtLinuxBoot(IN EFI_HANDLE ImageHandle, IN EFI_HANDLE DeviceHandle,
             IN EXTLINUX_BOOT_OPTION* BootOption)
{
    EFI_STATUS Status;
    CHAR16* NewArgs = NULL;
    UINTN ArgSize;
    ANDROID_BOOTIMG_PROTOCOL* AndroidBootProtocol;
    EFI_HANDLE RamDiskLoadFileHandle = NULL;
    UINTN FdtSize;
    UINTN KernelSize;
    VOID* KernelBase = NULL;
    VOID* AcpiBase = NULL;
    VOID* OldFdtBase = NULL;
    VOID* NewFdtBase = NULL;
    VOID* ExpandedFdtBase = NULL;
    BOOLEAN FdtUpdated = FALSE;
    EFI_DEVICE_PATH_PROTOCOL* KernelDevicePath = NULL;
    EFI_HANDLE KernelHandle = NULL;
    EFI_LOADED_IMAGE_PROTOCOL* ImageInfo;
    EFI_LOADED_IMAGE_PROTOCOL* LoadedImage = NULL;
    EFI_HANDLE EspDeviceHandle = NULL;

    // Get ESP device handle (where L4TLauncher.efi was loaded from)
    Status = gBS->HandleProtocol(ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID**)&LoadedImage);
    if (!EFI_ERROR(Status) && LoadedImage != NULL)
    {
        EspDeviceHandle = LoadedImage->DeviceHandle;
        ErrorPrint(L"%a: ESP device handle acquired for DTB loading\r\n", __FUNCTION__);
    }
    else
    {
        ErrorPrint(L"%a: Failed to get ESP device handle, will use rootfs device\r\n",
                   __FUNCTION__);
        EspDeviceHandle = DeviceHandle;  // Fallback to rootfs device
    }

    // Process Args
    ArgSize = StrSize(BootOption->BootArgs) + MAX_CBOOTARG_SIZE;
    NewArgs = AllocateCopyPool(ArgSize, BootOption->BootArgs);
    if (NewArgs == NULL)
    {
        Status = EFI_OUT_OF_RESOURCES;
        goto Exit;
    }

    Status = gBS->LocateProtocol(&gAndroidBootImgProtocolGuid, NULL, (VOID**)&AndroidBootProtocol);
    if (!EFI_ERROR(Status))
    {
        if (AndroidBootProtocol->AppendArgs != NULL)
        {
            Status = AndroidBootProtocol->AppendArgs(NewArgs, ArgSize);
            if (EFI_ERROR(Status))
            {
                ErrorPrint(L"%a: Failed to get platform addition arguments\r\n", __FUNCTION__);
                goto Exit;
            }
        }
    }

    // Expose LoadFile2 for initrd
    if (BootOption->InitrdPath != NULL)
    {
        Status = OpenAndReadFileToBuffer(DeviceHandle, BootOption->InitrdPath, NULL, &mRamdiskData,
                                         &mRamdiskSize);
        if (EFI_ERROR(Status))
        {
            ErrorPrint(L"%a:sds Failed to Authenticate %s (%r)\r\n", __FUNCTION__,
                       BootOption->InitrdPath, Status);
            return Status;
        }

        Status = gBS->InstallMultipleProtocolInterfaces(
            &RamDiskLoadFileHandle, &gEfiLoadFile2ProtocolGuid, &mAndroidBootImgLoadFile2,
            &gEfiDevicePathProtocolGuid, &mRamdiskDevicePath, NULL);
        if (EFI_ERROR(Status))
        {
            goto Exit;
        }
    }

    // Reload fdt if needed
    // Volley: Always prefer explicit DTB from ESP over ACPI or configuration table
    Status = EfiGetSystemConfigurationTable(&gEfiAcpiTableGuid, &AcpiBase);
    if (BootOption->DtbPath != NULL)
    {
        Status = EfiGetSystemConfigurationTable(&gFdtTableGuid, &OldFdtBase);
        if (EFI_ERROR(Status))
        {
            OldFdtBase = NULL;
        }

        // Try loading DTB from ESP device (where BOOTAA64.efi lives)
        ErrorPrint(L"%a: Attempting to load DTB from ESP: %s\r\n", __FUNCTION__,
                   BootOption->DtbPath);
        Status = OpenAndReadFileToBuffer(EspDeviceHandle, BootOption->DtbPath, NULL, &NewFdtBase,
                                         &FdtSize);
        if (EFI_ERROR(Status))
        {
            // Fallback: try rootfs device
            ErrorPrint(L"%a: ESP load failed (%r), trying rootfs device\r\n", __FUNCTION__, Status);
            Status =
                OpenAndReadFileToBuffer(DeviceHandle, BootOption->DtbPath, NULL, &NewFdtBase, &FdtSize);
        }

        if (EFI_ERROR(Status))
        {
            ErrorPrint(L"%a:sds Failed to Authenticate %s (%r)\r\n", __FUNCTION__,
                       BootOption->DtbPath, Status);
            goto Exit;
        }
        else
        {
            ErrorPrint(L"%a: Successfully loaded DTB (%lu bytes)\r\n", __FUNCTION__, FdtSize);
        }

        ExpandedFdtBase = AllocatePages(EFI_SIZE_TO_PAGES(2 * fdt_totalsize(NewFdtBase)));
        if (fdt_open_into(NewFdtBase, ExpandedFdtBase, 2 * fdt_totalsize(NewFdtBase)) != 0)
        {
            Status = EFI_NOT_FOUND;
            goto Exit;
        }

        Status = gBS->InstallConfigurationTable(&gFdtTableGuid, ExpandedFdtBase);
        if (EFI_ERROR(Status))
        {
            ErrorPrint(L"%a: Failed to install fdt\r\n", __FUNCTION__);
            goto Exit;
        }

        FdtUpdated = TRUE;
    }

    // Load and start the kernel
    if (BootOption->LinuxPath != NULL)
    {
        ErrorPrint(L"%a: Loading kernel from path: %s\r\n", __FUNCTION__, BootOption->LinuxPath);
        ErrorPrint(L"%a: Device handle for kernel: 0x%p\r\n", __FUNCTION__, DeviceHandle);
        PrintPartitionUuid(DeviceHandle);

        if (EncryptionInfo.ImageEncrypted)
        {
            ErrorPrint(L"%a: Kernel is encrypted, decrypting...\r\n", __FUNCTION__);
            Status = OpenAndDecryptFileToBuffer(DeviceHandle, BootOption->LinuxPath, &KernelBase,
                                                &KernelSize);
            if (EFI_ERROR(Status))
            {
                ErrorPrint(L"%a: Unable to decrypt image: %s %r\r\n", __FUNCTION__,
                           BootOption->LinuxPath, Status);
                goto Exit;
            }

            ErrorPrint(L"%a: Decrypted kernel, size=%u, loading image...\r\n", __FUNCTION__, KernelSize);
            Status = gBS->LoadImage(TRUE, ImageHandle, NULL, KernelBase, KernelSize, &KernelHandle);
            if (EFI_ERROR(Status))
            {
                ErrorPrint(L"%a: Unable to load image: %s %r\r\n", __FUNCTION__,
                           BootOption->LinuxPath, Status);
                PrintPartitionUuid(DeviceHandle);
                goto Exit;
            }
        }
        else
        {
            ErrorPrint(L"%a: Kernel is not encrypted, loading directly...\r\n", __FUNCTION__);
            KernelDevicePath = FileDevicePath(DeviceHandle, BootOption->LinuxPath);
            if (KernelDevicePath == NULL)
            {
                ErrorPrint(L"%a: Failed to create device path for kernel\r\n", __FUNCTION__);
                Status = EFI_OUT_OF_RESOURCES;
                goto Exit;
            }

            ErrorPrint(L"%a: Calling LoadImage for kernel...\r\n", __FUNCTION__);
            Status = gBS->LoadImage(FALSE, ImageHandle, KernelDevicePath, NULL, 0, &KernelHandle);
            if (EFI_ERROR(Status))
            {
                ErrorPrint(L"%a: FAILED to load kernel: %s %r\r\n", __FUNCTION__,
                           BootOption->LinuxPath, Status);
                ErrorPrint(L"%a: Searched on partition:\r\n", __FUNCTION__);
                PrintPartitionUuid(DeviceHandle);
                goto Exit;
            }
        }

        ErrorPrint(L"%a: Kernel loaded successfully!\r\n", __FUNCTION__);

        if (NewArgs != NULL)
        {
            // Set kernel arguments
            Status =
                gBS->HandleProtocol(KernelHandle, &gEfiLoadedImageProtocolGuid, (VOID**)&ImageInfo);
            if (EFI_ERROR(Status))
            {
                ErrorPrint(L"%a: Failed to get loaded image protocol: %r\r\n", __FUNCTION__, Status);
                goto Exit;
            }

            ImageInfo->LoadOptions = NewArgs;
            ImageInfo->LoadOptionsSize = StrLen(NewArgs) * sizeof(CHAR16);
            ErrorPrint(L"%a: Kernel cmdline: %s\r\n", __FUNCTION__, NewArgs);
        }

        // Before calling the image, enable the Watchdog Timer for  the 5 Minute period
        gBS->SetWatchdogTimer(5 * 60, 0x10000, 0, NULL);

        ErrorPrint(L"%a: Starting kernel image...\r\n", __FUNCTION__);
        Status = gBS->StartImage(KernelHandle, NULL, NULL);

        // Clear the Watchdog Timer if the image returns
        gBS->SetWatchdogTimer(0, 0x10000, 0, NULL);

        if (EFI_ERROR(Status))
        {
            ErrorPrint(L"%a: Unable to start image: %r\r\n", __FUNCTION__, Status);
        }
    }

Exit:
    // Unload fdt
    if (FdtUpdated)
    {
        gBS->InstallConfigurationTable(&gFdtTableGuid, OldFdtBase);
    }

    // Close handles
    if (RamDiskLoadFileHandle != NULL)
    {
        gBS->UninstallMultipleProtocolInterfaces(
            RamDiskLoadFileHandle, &gEfiLoadFile2ProtocolGuid, &mAndroidBootImgLoadFile2,
            &gEfiDevicePathProtocolGuid, &mRamdiskDevicePath, NULL);
    }

    // Free Memory
    if (KernelDevicePath != NULL)
    {
        FreePool(KernelDevicePath);
        KernelDevicePath = NULL;
    }

    if (ExpandedFdtBase != NULL)
    {
        FreePages(ExpandedFdtBase, EFI_SIZE_TO_PAGES(2 * fdt_totalsize(NewFdtBase)));
        ExpandedFdtBase = NULL;
    }

    if (KernelBase != NULL)
    {
        FreePool(KernelBase);
        KernelBase = NULL;
    }

    KernelSize = 0;

    if (NewFdtBase != NULL)
    {
        FreePool(NewFdtBase);
        NewFdtBase = NULL;
    }

    FdtSize = 0;

    if (mRamdiskData != NULL)
    {
        FreePool(mRamdiskData);
        mRamdiskData = NULL;
    }

    mRamdiskSize = 0;

    if (NewArgs != NULL)
    {
        FreePool(NewArgs);
        NewArgs = NULL;
    }

    return Status;
}

/**
  Initialize deterministic boot parameters.

  @param[in]  LoadedImage The
LoadedImage protocol for this execution
  @param[out] BootParams      The current boot parameters

  @retval EFI_SUCCESS    The operation completed successfully.

**/
STATIC
EFI_STATUS
EFIAPI
ProcessBootParams(IN EFI_LOADED_IMAGE_PROTOCOL* LoadedImage, OUT L4T_BOOT_PARAMS* BootParams)
{
    if ((LoadedImage == NULL) || (BootParams == NULL))
    {
        return EFI_INVALID_PARAMETER;
    }

    ZeroMem(BootParams, sizeof(*BootParams));
    BootParams->BootMode = NVIDIA_L4T_BOOTMODE_DIRECT;
    BootParams->BootChain = 0;

    DEBUG((DEBUG_INFO, "%a: using deterministic direct boot path on boot chain A\r\n",
           __FUNCTION__));

    return EFI_SUCCESS;
}

// DISABLED: Android boot path removed for deterministic boot
// These functions are unused when Android boot fallback is disabled
#if 0
/**
  Reads an android style kernel partition located with Partition base
  name and bootchain.

  This function allocates memory for Image with AllocatePool; the
  caller is responsible for passing Image to FreePool after use.

  @param[in]  DeviceHandle      The handle of device where the partition lives on.
  @param[in]  PartitionBasename The base name of the partion where the image to boot is located.
  @param[in]  BootParams        Boot params for L4T.
  @param[out] Image             Pointer to the kernel image.
  @param[out] ImageSize         Size of the kernel image.

  @retval EFI_SUCCESS    The operation completed successfully.
  @retval !=EFI_SUCCESS  Errors occurred.

**/
STATIC
EFI_STATUS
ReadAndroidStyleKernelPartition(IN CONST EFI_HANDLE DeviceHandle,
                                IN CONST CHAR16* CONST PartitionBasename,
                                IN CONST L4T_BOOT_PARAMS* CONST BootParams, OUT VOID** CONST Image,
                                OUT UINTN* CONST ImageSize)
{
    EFI_STATUS Status;
    EFI_HANDLE PartitionHandle;
    EFI_BLOCK_IO_PROTOCOL* BlockIo;
    EFI_DISK_IO_PROTOCOL* DiskIo;
    ANDROID_BOOTIMG_HEADER ImageHeader;
    VOID* ImageBuffer = NULL;
    UINTN ImageBufferSize;
    UINTN EncryptedImageBufferSize;
    UINTN DecryptedImageBufferSize;
    UINTN SignatureOffset;
    UINT8 Signature[SIZE_2KB];
    UINTN SignatureSize = sizeof(Signature);
    UINT8 BCH[MAX_BOOT_COMPONENT_HEADER_SIZE];

    Status = FindPartitionInfo(DeviceHandle, PartitionBasename, BootParams->BootChain, NULL,
                               &PartitionHandle);
    if (EFI_ERROR(Status))
    {
        ErrorPrint(L"%a: Unable to located partition\r\n", __FUNCTION__);
        goto Exit;
    }

    Status = gBS->HandleProtocol(PartitionHandle, &gEfiBlockIoProtocolGuid, (VOID**)&BlockIo);
    if (EFI_ERROR(Status))
    {
        ErrorPrint(L"%a: Unable to locate block io protocol on partition\r\n", __FUNCTION__);
        goto Exit;
    }

    Status = gBS->HandleProtocol(PartitionHandle, &gEfiDiskIoProtocolGuid, (VOID**)&DiskIo);
    if (EFI_ERROR(Status))
    {
        ErrorPrint(L"%a: Unable to locate disk io protocol on partition\r\n", __FUNCTION__);
        goto Exit;
    }

    if (EncryptionInfo.ImageEncrypted)
    {
        Status = DiskIo->ReadDisk(DiskIo, BlockIo->Media->MediaId, 0,
                                  EncryptionInfo.ImageHeaderSize, BCH);
        if (EFI_ERROR(Status))
        {
            ErrorPrint(L"Failed to read disk\r\n");
            goto Exit;
        }

        DecryptedImageBufferSize = *(UINT32*)(BCH + EncryptionInfo.ImageLengthOffset);
        EncryptedImageBufferSize = DecryptedImageBufferSize + EncryptionInfo.ImageHeaderSize;

        ImageBuffer = AllocatePool(DecryptedImageBufferSize);
        if (ImageBuffer == NULL)
        {
            ErrorPrint(L"Failed to allocate buffer for decrypted image\r\n");
            Status = EFI_OUT_OF_RESOURCES;
            goto Exit;
        }

        Status =
            OpteeDecryptImage(NULL, DiskIo, BlockIo, EncryptionInfo.ImageHeaderSize,
                              EncryptedImageBufferSize, &ImageBuffer, &DecryptedImageBufferSize);
        if (EFI_ERROR(Status))
        {
            ErrorPrint(L"%a: OpteeDecryptImage failed \r\n", __FUNCTION__);
            goto Exit;
        }

        memcpy(&ImageHeader, ImageBuffer, sizeof(ANDROID_BOOTIMG_HEADER));
        Status = AndroidBootImgGetImgSize(&ImageHeader, &ImageBufferSize);
        if (EFI_ERROR(Status))
        {
            ErrorPrint(L"Header not seen\r\n");
            goto Exit;
        }

        SignatureOffset = ALIGN_VALUE(ImageBufferSize, SignatureSize);
        SignatureSize = DecryptedImageBufferSize - SignatureOffset;
        memcpy(Signature, ImageBuffer + SignatureOffset, SignatureSize);
    }
    else
    {
        Status = DiskIo->ReadDisk(DiskIo, BlockIo->Media->MediaId, 0,
                                  sizeof(ANDROID_BOOTIMG_HEADER), &ImageHeader);
        if (EFI_ERROR(Status))
        {
            ErrorPrint(L"Failed to read disk\r\n");
            goto Exit;
        }

        Status = AndroidBootImgGetImgSize(&ImageHeader, &ImageBufferSize);
        if (EFI_ERROR(Status))
        {
            ErrorPrint(L"Android image header not seen\r\n");
            goto Exit;
        }

        ImageBuffer = AllocatePool(ImageBufferSize);
        if (ImageBuffer == NULL)
        {
            ErrorPrint(L"Failed to allocate buffer for Image\r\n");
            Status = EFI_OUT_OF_RESOURCES;
            goto Exit;
        }

        Status = DiskIo->ReadDisk(DiskIo, BlockIo->Media->MediaId, 0, ImageBufferSize, ImageBuffer);
        if (EFI_ERROR(Status))
        {
            ErrorPrint(L"Failed to read disk\r\n");
            goto Exit;
        }

        if (IsSecureBootEnabled())
        {
            SignatureOffset = ALIGN_VALUE(ImageBufferSize, SignatureSize);
            Status = DiskIo->ReadDisk(DiskIo, BlockIo->Media->MediaId, SignatureOffset,
                                      SignatureSize, Signature);
            if (EFI_ERROR(Status))
            {
                ErrorPrint(L"Failed to read kernel image signature\r\n");
                goto Exit;
            }
        }
    }

    if (IsSecureBootEnabled())
    {
        Status = VerifyDetachedSignature(Signature, SignatureSize, ImageBuffer, ImageBufferSize);
        if (EFI_ERROR(Status))
        {
            ErrorPrint(L"Failed to verify kernel image signature\r\n");
            goto Exit;
        }
    }

    *Image = ImageBuffer;
    ImageBuffer = NULL;
    *ImageSize = ImageBufferSize;

Exit:
    if (ImageBuffer != NULL)
    {
        FreePool(ImageBuffer);
    }

    return Status;
}

/**
  Reads an android style kernel dtb partition located with Partition base
  name and bootchain.

  This function allocates memory for Dtb with AllocatePool; the
  caller is responsible for passing Dtb to FreePool after use.

  @param[in]  DeviceHandle      The handle of device where this partition lives on.
  @param[in]  PartitionBasename The base name of the partion where the image to boot is located.
  @param[in]  BootParams        Boot params for L4T.
  @param[out] Dtb               Pointer to the allocated dtb buffer.
  @param[out] DtbSize           Size of the dtb buffer.

  @retval EFI_SUCCESS    The operation completed successfully.
  @retval !=EFI_SUCCESS  Errors occurred.

**/
STATIC
EFI_STATUS
ReadAndroidStyleDtbPartition(IN CONST EFI_HANDLE DeviceHandle,
                             IN CONST CHAR16* CONST PartitionBasename,
                             IN CONST L4T_BOOT_PARAMS* CONST BootParams, OUT VOID** CONST Dtb,
                             OUT UINTN* CONST DtbSize)
{
    EFI_STATUS Status;
    EFI_HANDLE PartitionHandle;
    EFI_BLOCK_IO_PROTOCOL* BlockIo;
    EFI_DISK_IO_PROTOCOL* DiskIo;
    VOID* DtbBuffer;
    UINT64 DtbBufferSize;
    UINT64 EncryptedDtbBufferSize;
    UINTN Size;
    UINTN SignatureOffset;
    UINTN SignatureSize = SIZE_2KB;
    UINT8 BCH[MAX_BOOT_COMPONENT_HEADER_SIZE];

    Status = FindPartitionInfo(DeviceHandle, PartitionBasename, BootParams->BootChain, NULL,
                               &PartitionHandle);
    if (EFI_ERROR(Status))
    {
        ErrorPrint(L"%a: Unable to located partition\r\n", __FUNCTION__);
        goto Exit;
    }

    Status = gBS->HandleProtocol(PartitionHandle, &gEfiBlockIoProtocolGuid, (VOID**)&BlockIo);
    if (EFI_ERROR(Status))
    {
        ErrorPrint(L"%a: Unable to locate block io protocol on partition\r\n", __FUNCTION__);
        goto Exit;
    }

    Status = gBS->HandleProtocol(PartitionHandle, &gEfiDiskIoProtocolGuid, (VOID**)&DiskIo);
    if (EFI_ERROR(Status))
    {
        ErrorPrint(L"%a: Unable to locate disk io protocol on partition\r\n", __FUNCTION__);
        goto Exit;
    }

    if (EncryptionInfo.ImageEncrypted)
    {
        Status = DiskIo->ReadDisk(DiskIo, BlockIo->Media->MediaId, 0,
                                  EncryptionInfo.ImageHeaderSize, &BCH);
        if (EFI_ERROR(Status))
        {
            ErrorPrint(L"Failed to read disk\r\n");
            goto Exit;
        }

        DtbBufferSize = *(UINT32*)(BCH + EncryptionInfo.ImageLengthOffset);
        EncryptedDtbBufferSize = DtbBufferSize + EncryptionInfo.ImageHeaderSize;

        DtbBuffer = AllocatePool(DtbBufferSize);
        if (DtbBuffer == NULL)
        {
            ErrorPrint(L"Failed to allocate buffer for dtb\r\n");
            Status = EFI_OUT_OF_RESOURCES;
            goto Exit;
        }

        Status = OpteeDecryptImage(NULL, DiskIo, BlockIo, EncryptionInfo.ImageHeaderSize,
                                   EncryptedDtbBufferSize, &DtbBuffer, &DtbBufferSize);
        if (EFI_ERROR(Status))
        {
            ErrorPrint(L"%a: OpteeDecryptImage failed \r\n", __FUNCTION__);
            goto Exit;
        }

        if (fdt_check_header((UINT8*)DtbBuffer) != 0)
        {
            ErrorPrint(L"DTB on partition was corrupted, attempt use to UEFI DTB\r\n");
            goto Exit;
        }

        Size = fdt_totalsize((UINT8*)DtbBuffer);
        SignatureOffset = ALIGN_VALUE(Size, SignatureSize);
        SignatureSize = DtbBufferSize - SignatureOffset;
    }
    else
    {
        DtbBufferSize = MultU64x32(BlockIo->Media->LastBlock + 1, BlockIo->Media->BlockSize);

        DtbBuffer = AllocatePool(DtbBufferSize);
        if (DtbBuffer == NULL)
        {
            ErrorPrint(L"Failed to allocate buffer for dtb\r\n");
            Status = EFI_OUT_OF_RESOURCES;
            goto Exit;
        }

        Status = DiskIo->ReadDisk(DiskIo, BlockIo->Media->MediaId, 0, DtbBufferSize, DtbBuffer);
        if (EFI_ERROR(Status))
        {
            ErrorPrint(L"Failed to read disk\r\n");
            goto Exit;
        }

        if (fdt_check_header((UINT8*)DtbBuffer) != 0)
        {
            ErrorPrint(L"DTB on partition was corrupted, try using UEFI DTB\r\n");
            goto Exit;
        }

        Size = fdt_totalsize((UINT8*)DtbBuffer);
        if (IsSecureBootEnabled())
        {
            SignatureOffset = ALIGN_VALUE(Size, SignatureSize);
            if (SignatureOffset + SignatureSize > DtbBufferSize)
            {
                ErrorPrint(L"DTB signature missing\r\n");
                Status = EFI_SECURITY_VIOLATION;
                goto Exit;
            }
        }
    }

    if (IsSecureBootEnabled())
    {
        Status = VerifyDetachedSignature((UINT8*)DtbBuffer + SignatureOffset, SignatureSize,
                                         (UINT8*)DtbBuffer, Size);
        if (EFI_ERROR(Status))
        {
            ErrorPrint(L"DTB signature invalid\r\n");
            goto Exit;
        }
    }

    *Dtb = DtbBuffer;
    DtbBuffer = NULL;
    *DtbSize = Size;

Exit:
    if (DtbBuffer != NULL)
    {
        FreePool(DtbBuffer);
    }

    return Status;
}

/**
  Boots an android style partition located with Partition base name and bootchain

  @param[in]  DeviceHandle      The handle of partition where this file lives on.
  @param[in]  PartitionBasename The base name of the partion where the image to boot is located.
  @param[in]  BootChain         Numeric version of the chain


  @retval EFI_SUCCESS    The operation completed successfully.

**/
STATIC
EFI_STATUS
EFIAPI
BootAndroidStylePartition(IN EFI_HANDLE DeviceHandle, IN CONST CHAR16* BootImgPartitionBasename,
                          IN CONST CHAR16* BootImgDtbPartitionBasename,
                          IN L4T_BOOT_PARAMS* BootParams)
{
    EFI_STATUS Status;
    EFI_STATUS Status1;
    VOID* Image = NULL;
    UINTN ImageSize;
    VOID* AcpiBase;
    VOID* Dtb = NULL;
    UINTN DtbSize;
    VOID* OldDtb;
    VOID* NewDtb = NULL;
    UINTN NewDtbPages;
    BOOLEAN NewDtbInstalled = FALSE;

    Status = ReadAndroidStyleKernelPartition(DeviceHandle, BootImgPartitionBasename, BootParams,
                                             &Image, &ImageSize);
    if (EFI_ERROR(Status))
    {
        goto Exit;
    }

    do
    {
        Status = EfiGetSystemConfigurationTable(&gEfiAcpiTableGuid, &AcpiBase);
        if (!EFI_ERROR(Status))
        {
            break;
        }

        Status = ReadAndroidStyleDtbPartition(DeviceHandle, BootImgDtbPartitionBasename, BootParams,
                                              &Dtb, &DtbSize);
        if (EFI_ERROR(Status))
        {
            break;
        }

        NewDtbPages = EFI_SIZE_TO_PAGES(2 * DtbSize);
        NewDtb = AllocatePages(NewDtbPages);
        if (NewDtb == NULL)
        {
            DEBUG((DEBUG_WARN, "%a: failed to allocate pages for expanded kernel DTB\r\n",
                   __FUNCTION__));
            break;
        }

        if (fdt_open_into((UINT8*)Dtb, NewDtb, EFI_PAGES_TO_SIZE(NewDtbPages)) != 0)
        {
            DEBUG((DEBUG_WARN, "%a: failed to relocate kernel DTB\r\n", __FUNCTION__));
            break;
        }

        DEBUG((DEBUG_ERROR, "%a: Installing Kernel DTB\r\n", __FUNCTION__));
        Status = EfiGetSystemConfigurationTable(&gFdtTableGuid, &OldDtb);
        if (EFI_ERROR(Status))
        {
            ErrorPrint(L"No existing DTB\r\n");
            goto Exit;
        }

        Status = gBS->InstallConfigurationTable(&gFdtTableGuid, NewDtb);
        if (EFI_ERROR(Status))
        {
            ErrorPrint(L"DTB Installation Failed\r\n");
            goto Exit;
        }

        NewDtbInstalled = TRUE;
    } while (FALSE);

    DEBUG((DEBUG_ERROR, "%a: Cmdline: \n", __FUNCTION__));

    DEBUG((DEBUG_ERROR, "%a", ((ANDROID_BOOTIMG_HEADER*)Image)->KernelArgs));

    Status = AndroidBootImgBoot(Image, ImageSize);
    if (EFI_ERROR(Status))
    {
        ErrorPrint(L"Failed to boot image: %r\r\n", Status);
    }

Exit:
    if (NewDtbInstalled)
    {
        Status1 = gBS->InstallConfigurationTable(&gFdtTableGuid, OldDtb);
        if (EFI_ERROR(Status1))
        {
            ErrorPrint(L"%a: Failed to re-install UEFI DTB: %r\r\n", __FUNCTION__, Status);
        }

        if (!EFI_ERROR(Status))
        {
            Status = Status1;
        }
    }

    if (NewDtb != NULL)
    {
        FreePages(NewDtb, NewDtbPages);
    }

    if (Dtb != NULL)
    {
        FreePool(Dtb);
    }

    if (Image != NULL)
    {
        FreePool(Image);
    }

    return Status;
}
#endif  // Android boot-from-partition path disabled

/**
  Boots an android style image already loaded in memory

  @param[in]  ImageBase  Address of android style image in memory.
  @param[in]  ImageSize  Size of android style image in memory.


  @retval EFI_SUCCESS    The operation completed successfully.

**/
STATIC
EFI_STATUS
EFIAPI
BootAndroidStyleImage(IN EFI_PHYSICAL_ADDRESS ImageBase, IN UINT64 ImageSize)
{
    EFI_STATUS Status;
    ANDROID_BOOTIMG_HEADER ImageHeader;
    UINTN ImageBufferSize;
    UINTN SignatureOffset;
    UINT8 Signature[SIZE_2KB];
    UINTN SignatureSize = sizeof(Signature);

    memcpy(&ImageHeader, (VOID*)ImageBase, sizeof(ANDROID_BOOTIMG_HEADER));
    Status = AndroidBootImgGetImgSize(&ImageHeader, &ImageBufferSize);
    if (EFI_ERROR(Status))
    {
        ErrorPrint(L"Android image header not seen\r\n");
        return Status;
    }

    if (ImageSize < ImageBufferSize)
    {
        return Status;
    }

    if (IsSecureBootEnabled())
    {
        SignatureOffset = ALIGN_VALUE(ImageBufferSize, SignatureSize);
        memcpy(Signature, (VOID*)(ImageBase + SignatureOffset), SignatureSize);
        Status =
            VerifyDetachedSignature(Signature, SignatureSize, (VOID*)ImageBase, ImageBufferSize);
        if (EFI_ERROR(Status))
        {
            ErrorPrint(L"Failed to verify kernel image signature\r\n");
            goto Exit;
        }
    }

    DEBUG((DEBUG_ERROR, "%a: Cmdline: \n", __FUNCTION__));

    DEBUG((DEBUG_ERROR, "%a", ImageHeader.KernelArgs));

    Status = AndroidBootImgBoot((VOID*)ImageBase, ImageBufferSize);
    if (EFI_ERROR(Status))
    {
        ErrorPrint(L"Failed to boot image: %r\r\n", Status);
    }

Exit:

    return Status;
}

/**
  This is the declaration of an EFI image entry point. This entry point is
  the same for UEFI Applications, UEFI OS Loaders, and UEFI Drivers, including
  both device drivers and bus drivers.

  The entry point for StackCheck application that should casue an abort due to stack overwrite.

  @param[in] ImageHandle    The image handle of this application.
  @param[in] SystemTable    The pointer to the EFI System Table.

  @retval EFI_SUCCESS    The operation completed successfully.

**/
EFI_STATUS
EFIAPI
L4TLauncher(IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE* SystemTable)
{
    EFI_DEVICE_PATH* FullDevicePath;
    EFI_LOADED_IMAGE_PROTOCOL* LoadedImage;
    EFI_STATUS Status;
    EFI_HANDLE LoadedImageHandle = 0;
    EFI_HANDLE RootFsDeviceHandle = 0;
    L4T_BOOT_PARAMS BootParams;
    EXTLINUX_BOOT_CONFIG ExtLinuxConfig;
    UINTN ExtLinuxBootOption;
    UINTN Index;
    VOID* Hob;
    TEGRA_PLATFORM_RESOURCE_INFO* PlatformResourceInfo;

    Print(L"VOLLEY modified L4TLauncher ${VOLLEY_BUILD_INFO}\r\n");
    Print(L"VOLLEY features: KeyboardIgnored;DirectBootFixedDTB;DtbAutoSelect;eMMCPrimary;SHA256Checks\r\n");
    Print(L"VOLLEY target: %s (%s)\r\n", VOLLEY_SYSTEM_NAME,
          VolleyIsIndustrial() ? L"industrial" : L"non-industrial");
    Status = gBS->HandleProtocol(ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID**)&LoadedImage);
    if (EFI_ERROR(Status))
    {
        ErrorPrint(L"%a: Unable to locate loaded image: %r\r\n", __FUNCTION__, Status);
        return Status;
    }

    Status = ProcessBootParams(LoadedImage, &BootParams);
    if (EFI_ERROR(Status))
    {
        ErrorPrint(L"%a: Unable to process boot parameters: %r\r\n", __FUNCTION__, Status);
        return Status;
    }

    //
    // Volley deployment always boots via the Direct (ExtLinux) path with fixed inputs.
    //
    BootParams.BootMode = NVIDIA_L4T_BOOTMODE_DIRECT;

    if (IsSecureBootEnabled())
    {
        Status = GetImageEncryptionInfo(&EncryptionInfo);
        if (EFI_ERROR(Status))
        {
            ErrorPrint(L"%a: Unable to get image status: %r\r\n", __FUNCTION__, Status);
        }
    }

    Hob = GetFirstGuidHob(&gNVIDIAPlatformResourceDataGuid);
    if ((Hob != NULL) && (GET_GUID_HOB_DATA_SIZE(Hob) == sizeof(TEGRA_PLATFORM_RESOURCE_INFO)))
    {
        PlatformResourceInfo = (TEGRA_PLATFORM_RESOURCE_INFO*)GET_GUID_HOB_DATA(Hob);
    }
    else
    {
        ErrorPrint(L"%a: Failed to get PlatformResourceInfo\r\n", __FUNCTION__);
        return EFI_NOT_FOUND;
    }

    if (PlatformResourceInfo->BootType == TegrablBootRcm)
    {
        ErrorPrint(L"%a: Attempting RCM Boot\r\n", __FUNCTION__);
        Status = BootAndroidStyleImage(PcdGet64(PcdRcmKernelBase), PcdGet64(PcdRcmKernelSize));
        if (EFI_ERROR(Status))
        {
            ErrorPrint(L"Failed to boot image: %r\r\n", Status);
        }
    }
    else
    {
        if (BootParams.BootMode == NVIDIA_L4T_BOOTMODE_GRUB)
        {
            ErrorPrint(L"%a: Attempting GRUB Boot\r\n", __FUNCTION__);
            do
            {
                FullDevicePath = FileDevicePath(LoadedImage->DeviceHandle, GRUB_PATH);
                if (FullDevicePath == NULL)
                {
                    ErrorPrint(L"%a: Failed to create full device path\r\n", __FUNCTION__);
                    BootParams.BootMode = NVIDIA_L4T_BOOTMODE_DIRECT;
                    break;
                }

                Status =
                    gBS->LoadImage(FALSE, ImageHandle, FullDevicePath, NULL, 0, &LoadedImageHandle);
                if (EFI_ERROR(Status))
                {
                    if (Status != EFI_NOT_FOUND)
                    {
                        ErrorPrint(L"%a: Unable to load image: %r\r\n", __FUNCTION__, Status);
                    }

                    BootParams.BootMode = NVIDIA_L4T_BOOTMODE_DIRECT;
                    break;
                }

                Status = UpdateBootConfig(LoadedImage->DeviceHandle, BootParams.BootChain);
                if (EFI_ERROR(Status))
                {
                    ErrorPrint(L"%a: Unable to update partition files\r\n", __FUNCTION__);
                    BootParams.BootMode = NVIDIA_L4T_BOOTMODE_DIRECT;
                    break;
                }

                // Before calling the image, enable the Watchdog Timer for  the 5 Minute period
                gBS->SetWatchdogTimer(5 * 60, 0x10000, 0, NULL);

                Status = gBS->StartImage(LoadedImageHandle, NULL, NULL);

                // Clear the Watchdog Timer if the image returns
                gBS->SetWatchdogTimer(0, 0x10000, 0, NULL);

                if (EFI_ERROR(Status))
                {
                    ErrorPrint(L"%a: Unable to start image: %r\r\n", __FUNCTION__, Status);
                    break;
                }
            } while (FALSE);
        }

        if (BootParams.BootMode == NVIDIA_L4T_BOOTMODE_DIRECT)
        {
            ErrorPrint(L"%a: Attempting Direct Boot\r\n", __FUNCTION__);
            ErrorPrint(L"%a: LoadedImage->DeviceHandle = 0x%p\r\n", __FUNCTION__, LoadedImage->DeviceHandle);
            do
            {
                ErrorPrint(L"%a: Calling ProcessExtLinuxConfig...\r\n", __FUNCTION__);
                Status = ProcessExtLinuxConfig(LoadedImage->DeviceHandle, BootParams.BootChain,
                                               &ExtLinuxConfig, &RootFsDeviceHandle);
                if (EFI_ERROR(Status))
                {
                    ErrorPrint(L"%a: ProcessExtLinuxConfig FAILED: %r\r\n", __FUNCTION__, Status);
                    ErrorPrint(L"%a: FATAL: No fallback boot path available\r\n", __FUNCTION__);
                    // Do NOT fall back to Android boot - it loads unwanted initrd
                    break;
                }

                ErrorPrint(L"%a: ProcessExtLinuxConfig succeeded\r\n", __FUNCTION__);
                ErrorPrint(L"%a: RootFsDeviceHandle = 0x%p\r\n", __FUNCTION__, RootFsDeviceHandle);
                PrintPartitionUuid(RootFsDeviceHandle);

                ExtLinuxBootOption = ExtLinuxBootMenu(&ExtLinuxConfig);

                ErrorPrint(L"%a: Calling ExtLinuxBoot with boot option %u...\r\n", __FUNCTION__, ExtLinuxBootOption);
                Status = ExtLinuxBoot(ImageHandle, RootFsDeviceHandle,
                                      &ExtLinuxConfig.BootOptions[ExtLinuxBootOption]);
                if (EFI_ERROR(Status))
                {
                    ErrorPrint(L"%a: ExtLinuxBoot FAILED: %r\r\n", __FUNCTION__, Status);
                    ErrorPrint(L"%a: FATAL: No fallback boot path available\r\n", __FUNCTION__);
                    // Do NOT fall back to Android boot - it loads unwanted initrd
                    break;
                }
            } while (FALSE);

            for (Index = 0; Index < ExtLinuxConfig.NumberOfBootOptions; Index++)
            {
                if (ExtLinuxConfig.BootOptions[Index].BootArgs != NULL)
                {
                    FreePool(ExtLinuxConfig.BootOptions[Index].BootArgs);
                    ExtLinuxConfig.BootOptions[Index].BootArgs = NULL;
                }

                if (ExtLinuxConfig.BootOptions[Index].DtbPath != NULL)
                {
                    FreePool(ExtLinuxConfig.BootOptions[Index].DtbPath);
                    ExtLinuxConfig.BootOptions[Index].DtbPath = NULL;
                }

                if (ExtLinuxConfig.BootOptions[Index].InitrdPath != NULL)
                {
                    FreePool(ExtLinuxConfig.BootOptions[Index].InitrdPath);
                    ExtLinuxConfig.BootOptions[Index].InitrdPath = NULL;
                }

                if (ExtLinuxConfig.BootOptions[Index].Label != NULL)
                {
                    FreePool(ExtLinuxConfig.BootOptions[Index].Label);
                    ExtLinuxConfig.BootOptions[Index].Label = NULL;
                }

                if (ExtLinuxConfig.BootOptions[Index].LinuxPath != NULL)
                {
                    FreePool(ExtLinuxConfig.BootOptions[Index].LinuxPath);
                    ExtLinuxConfig.BootOptions[Index].LinuxPath = NULL;
                }

                if (ExtLinuxConfig.BootOptions[Index].MenuLabel != NULL)
                {
                    FreePool(ExtLinuxConfig.BootOptions[Index].MenuLabel);
                    ExtLinuxConfig.BootOptions[Index].MenuLabel = NULL;
                }
            }

            if (ExtLinuxConfig.MenuTitle != NULL)
            {
                FreePool(ExtLinuxConfig.MenuTitle);
                ExtLinuxConfig.MenuTitle = NULL;
            }
        }

        // DISABLED: Android boot path removed for deterministic boot
        // Kernel must be at /boot/Image on the selected device (eMMC or NVMe slot)
        // The kernel has embedded initramfs - no external ramdisk needed
        if (BootParams.BootMode == NVIDIA_L4T_BOOTMODE_BOOTIMG)
        {
            ErrorPrint(L"%a: Android boot path disabled for deterministic boot\r\n", __FUNCTION__);
            ErrorPrint(L"%a: Kernel must be at /boot/Image on selected device\r\n", __FUNCTION__);
            Status = EFI_UNSUPPORTED;
        }
        else if (BootParams.BootMode == NVIDIA_L4T_BOOTMODE_RECOVERY)
        {
            ErrorPrint(L"%a: Recovery boot attempt blocked\r\n", __FUNCTION__);
            Status = EFI_UNSUPPORTED;
        }
    }

    return Status;
}
