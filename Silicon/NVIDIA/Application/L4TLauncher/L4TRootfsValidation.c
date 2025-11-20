/** @file

  Rootfs Validation Library

  Copyright (c) 2022-2023, NVIDIA CORPORATION & AFFILIATES. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Library/PlatformResourceLib.h>
#include <Library/ResetSystemLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <NVIDIAConfiguration.h>
#include "L4TRootfsValidation.h"

#pragma GCC diagnostic ignored "-Wunused-function"

L4T_RF_AB_PARAM mRootfsInfo = {0};

RF_AB_VARIABLE mRFAbVariable[RF_VARIABLE_INDEX_MAX] = {
    [RF_STATUS_A] = {L"RootfsStatusSlotA",
                     EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS |
                         EFI_VARIABLE_NON_VOLATILE,
                     sizeof(UINT32), &gNVIDIAPublicVariableGuid},
    [RF_STATUS_B] = {L"RootfsStatusSlotB",
                     EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS |
                         EFI_VARIABLE_NON_VOLATILE,
                     sizeof(UINT32), &gNVIDIAPublicVariableGuid},
    [RF_REDUNDANCY] = {L"RootfsRedundancyLevel",
                       EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS |
                           EFI_VARIABLE_NON_VOLATILE,
                       sizeof(UINT32), &gNVIDIAPublicVariableGuid},
    [RF_RETRY_MAX] = {L"RootfsRetryCountMax",
                      EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS |
                          EFI_VARIABLE_NON_VOLATILE,
                      sizeof(UINT32), &gNVIDIAPublicVariableGuid},
    [RF_FW_NEXT] = {L"BootChainFwNext",
                    EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS |
                        EFI_VARIABLE_NON_VOLATILE,
                    sizeof(UINT32), &gNVIDIAPublicVariableGuid},
    [RF_BC_STATUS] = {L"BootChainFwStatus",
                      EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS |
                          EFI_VARIABLE_NON_VOLATILE,
                      sizeof(UINT32), &gNVIDIAPublicVariableGuid},
};

/**
  Get rootfs A/B related variable according to input index

  @param[in]  VariableIndex       Rootfs A/B related variable index
  @param[out] Value               Value of the variable

  @retval EFI_SUCCESS            The operation completed successfully
  @retval EFI_INVALID_PARAMETER  Input parameter invalid

**/
STATIC
EFI_STATUS
EFIAPI
RFGetVariable(IN RF_VARIABLE_INDEX VariableIndex, OUT UINT32* Value)
{
    RF_AB_VARIABLE* Variable;
    UINTN Size;
    EFI_STATUS Status;

    if (VariableIndex >= RF_VARIABLE_INDEX_MAX)
    {
        return EFI_INVALID_PARAMETER;
    }

    Variable = &mRFAbVariable[VariableIndex];

    *Value = 0;
    Size = Variable->Bytes;
    Status = gRT->GetVariable(Variable->Name, Variable->Guid, NULL, &Size, Value);
    if (EFI_ERROR(Status))
    {
        // The BootChainFwNext and BootChainFwStatus does not exist by default
        if ((Status == EFI_NOT_FOUND) &&
            ((VariableIndex == RF_FW_NEXT) || (VariableIndex == RF_BC_STATUS)))
        {
            DEBUG((DEBUG_INFO, "%a: Info: %s is not found\n", __FUNCTION__, Variable->Name));
            Status = EFI_SUCCESS;
        }
        else
        {
            DEBUG(
                (DEBUG_ERROR, "%a: Error getting %s: %r\n", __FUNCTION__, Variable->Name, Status));
        }
    }

    return Status;
}

/**
  Set rootfs A/B related variable according to input index

  @param[in] VariableIndex       Rootfs A/B related variable index
  @param[in] Value               Value of the variable

  @retval EFI_SUCCESS            The operation completed successfully
  @retval EFI_INVALID_PARAMETER  Input parameter invalid

**/
STATIC
EFI_STATUS
EFIAPI
RFSetVariable(IN RF_VARIABLE_INDEX VariableIndex, IN UINT32 Value)
{
    RF_AB_VARIABLE* Variable;
    EFI_STATUS Status;

    if (VariableIndex >= RF_VARIABLE_INDEX_MAX)
    {
        return EFI_INVALID_PARAMETER;
    }

    Variable = &mRFAbVariable[VariableIndex];
    Status = gRT->SetVariable(Variable->Name, Variable->Guid, Variable->Attributes, Variable->Bytes,
                              &Value);
    if (EFI_ERROR(Status))
    {
        DEBUG((DEBUG_ERROR, "%a: Error setting %s to %u: %r\n", __FUNCTION__, Variable->Name, Value,
               Status));
    }

    return Status;
}

/**
  Delete rootfs A/B related variable according to input index

  @param[in]  VariableIndex       Rootfs A/B related variable index

  @retval EFI_SUCCESS            The operation completed successfully
  @retval EFI_INVALID_PARAMETER  Input parameter invalid

**/
STATIC
EFI_STATUS
EFIAPI
RFDeleteVariable(IN RF_VARIABLE_INDEX VariableIndex)
{
    RF_AB_VARIABLE* Variable;
    EFI_STATUS Status;

    if (VariableIndex >= RF_VARIABLE_INDEX_MAX)
    {
        return EFI_INVALID_PARAMETER;
    }

    Variable = &mRFAbVariable[VariableIndex];
    DEBUG((DEBUG_INFO, "%a: Deleting %s\n", __FUNCTION__, Variable->Name));

    Status = gRT->SetVariable(Variable->Name, Variable->Guid, Variable->Attributes, 0, NULL);
    if (EFI_ERROR(Status) && (Status != EFI_NOT_FOUND))
    {
        DEBUG((DEBUG_ERROR, "%a: Error deleting %s: %r\n", __FUNCTION__, Variable->Name, Status));
    }

    return Status;
}

/**
  Initialize rootfs status register

  @param[in]  RootfsSlot          Rootfs slot
  @param[out] RegisterValueRf     Value of rootfs status register

  @retval EFI_SUCCESS    The operation completed successfully.

**/
STATIC
EFI_STATUS
EFIAPI
InitializeRootfsStatusReg(IN UINT32 RootfsSlot, OUT UINT32* RegisterValueRf)
{
    EFI_STATUS Status;
    UINT32 RegisterValue;
    UINT32 RetryCount;
    UINT32 MaxRetryCount;
    UINT32 RootfsStatus;

    Status = GetRootfsStatusReg(&RegisterValue);
    if (EFI_ERROR(Status))
    {
        DEBUG((DEBUG_ERROR, "%a: Failed to get rootfs status register\n", __FUNCTION__));
        return Status;
    }

    if (SR_RF_MAGIC_GET(RegisterValue) == SR_RF_MAGIC)
    {
        // Rootfs Status Reg has been properly set in previous boot
        goto Exit;
    }

    // This is first boot. Initialize SR_RF
    RegisterValue = 0;

    // Set magic
    RegisterValue = SR_RF_MAGIC_SET(RegisterValue);

    RegisterValue = SR_RF_CURRENT_SLOT_SET(RootfsSlot, RegisterValue);

    // Set retry count for rootfs A
    MaxRetryCount = mRootfsInfo.RootfsVar[RF_RETRY_MAX].Value;
    RootfsStatus = mRootfsInfo.RootfsVar[RF_STATUS_A].Value;
    if (RootfsStatus == NVIDIA_OS_STATUS_UNBOOTABLE)
    {
        RetryCount = 0;
    }
    else
    {
        RetryCount = MaxRetryCount;
    }

    RegisterValue = SR_RF_RETRY_COUNT_A_SET(RetryCount, RegisterValue);

    // Set retry count for rootfs B
    RootfsStatus = mRootfsInfo.RootfsVar[RF_STATUS_B].Value;
    if (RootfsStatus == NVIDIA_OS_STATUS_UNBOOTABLE)
    {
        RetryCount = 0;
    }
    else
    {
        RetryCount = MaxRetryCount;
    }

    RegisterValue = SR_RF_RETRY_COUNT_B_SET(RetryCount, RegisterValue);

    // Write Rootfs Status register
    Status = SetRootfsStatusReg(RegisterValue);
    if (EFI_ERROR(Status))
    {
        DEBUG(
            (DEBUG_ERROR, "%a: Failed to set Rootfs status register: %r\n", __FUNCTION__, Status));
    }

Exit:
    *RegisterValueRf = RegisterValue;
    return Status;
}

/**
  Set rootfs status value to mRootfsInfo and set the update flag

  @param[in]  RootfsSlot      Current rootfs slot
  @param[in]  RootfsStatus    Value of rootfs status

  @retval EFI_SUCCESS            The operation completed successfully.
  @retval EFI_INVALID_PARAMETER  Input parameter invalid

**/
STATIC
EFI_STATUS
EFIAPI
SetStatusTomRootfsInfo(IN UINT32 RootfsSlot, IN UINT32 RootfsStatus)
{
    if ((RootfsSlot > ROOTFS_SLOT_B))
    {
        return EFI_INVALID_PARAMETER;
    }

    if (RootfsSlot == ROOTFS_SLOT_A)
    {
        mRootfsInfo.RootfsVar[RF_STATUS_A].Value = RootfsStatus;
        mRootfsInfo.RootfsVar[RF_STATUS_A].UpdateFlag = 1;
    }
    else
    {
        mRootfsInfo.RootfsVar[RF_STATUS_B].Value = RootfsStatus;
        mRootfsInfo.RootfsVar[RF_STATUS_B].UpdateFlag = 1;
    }

    return EFI_SUCCESS;
}

/**
  Get rootfs retry count from mRootfsInfo

  @param[in]  RootfsSlot          Current rootfs slot
  @param[out] RootfsRetryCount    Rertry count of current RootfsSlot

  @retval EFI_SUCCESS            The operation completed successfully.
  @retval EFI_INVALID_PARAMETER  Input parameter invalid

**/
STATIC
EFI_STATUS
EFIAPI
GetRetryCountFrommRootfsInfo(IN UINT32 RootfsSlot, OUT UINT32* RootfsRetryCount)
{
    if ((RootfsSlot > ROOTFS_SLOT_B) || (RootfsRetryCount == NULL))
    {
        return EFI_INVALID_PARAMETER;
    }

    if (RootfsSlot == ROOTFS_SLOT_A)
    {
        *RootfsRetryCount = mRootfsInfo.RetryCountSlotA;
    }
    else
    {
        *RootfsRetryCount = mRootfsInfo.RetryCountSlotB;
    }

    return EFI_SUCCESS;
}

/**
  Set rootfs retry count value to mRootfsInfo

  @param[in]  RootfsSlot          Current rootfs slot
  @param[in]  RootfsRetryCount    Rootfs retry count of current RootfsSlot
  @param[out] RootfsInfo          The value of RootfsInfo variable

  @retval EFI_SUCCESS            The operation completed successfully.
  @retval EFI_INVALID_PARAMETER  Input parameter invalid

**/
STATIC
EFI_STATUS
EFIAPI
SetRetryCountTomRootfsInfo(IN UINT32 RootfsSlot, IN UINT32 RootfsRetryCount)
{
    if ((RootfsSlot > ROOTFS_SLOT_B))
    {
        return EFI_INVALID_PARAMETER;
    }

    if (RootfsSlot == ROOTFS_SLOT_A)
    {
        mRootfsInfo.RetryCountSlotA = RootfsRetryCount;
    }
    else
    {
        mRootfsInfo.RetryCountSlotB = RootfsRetryCount;
    }

    return EFI_SUCCESS;
}

/**
  Sync the Rootfs status register and mRootfsInfo variable according to
  the specified direction

  @param[in]     Direction          The sync up direction
  @param[in/out] RegisterValue      The pointer of Rootfs Status register

  @retval EFI_SUCCESS    The operation completed successfully.

**/
STATIC
EFI_STATUS
EFIAPI
SyncSrRfAndmRootfsInfo(IN UINT32 Direction, OUT UINT32* RegisterValue)
{
    EFI_STATUS Status = EFI_SUCCESS;

    switch (Direction)
    {
    case FROM_REG_TO_VAR:
        // Copy CurrentSlot, RetryCountA/B from Scratch Register to mRootfsInfo
        mRootfsInfo.CurrentSlot = SR_RF_CURRENT_SLOT_GET(*RegisterValue);

        mRootfsInfo.RetryCountSlotA = SR_RF_RETRY_COUNT_A_GET(*RegisterValue);
        mRootfsInfo.RetryCountSlotB = SR_RF_RETRY_COUNT_B_GET(*RegisterValue);
        break;
    case FROM_VAR_TO_REG:
        // Copy CurrentSlot, RetryCountA/B from mRootfsInfo to Scratch Register
        *RegisterValue = SR_RF_CURRENT_SLOT_SET(mRootfsInfo.CurrentSlot, *RegisterValue);

        *RegisterValue = SR_RF_RETRY_COUNT_A_SET(mRootfsInfo.RetryCountSlotA, *RegisterValue);
        *RegisterValue = SR_RF_RETRY_COUNT_B_SET(mRootfsInfo.RetryCountSlotB, *RegisterValue);
        break;
    default:
        break;
    }

    return Status;
}

/**
  Check if there is valid rootfs or not

  @retval TRUE     There is valid rootfs
  @retval FALSE    There is no valid rootfs

**/
BOOLEAN
EFIAPI
IsValidRootfs(VOID)
{
    BOOLEAN Status = TRUE;

    if ((mRootfsInfo.RootfsVar[RF_REDUNDANCY].Value == NVIDIA_OS_REDUNDANCY_BOOT_ONLY) &&
        (mRootfsInfo.RootfsVar[RF_STATUS_A].Value == NVIDIA_OS_STATUS_UNBOOTABLE))
    {
        Status = FALSE;
    }

    if ((mRootfsInfo.RootfsVar[RF_REDUNDANCY].Value == NVIDIA_OS_REDUNDANCY_BOOT_ROOTFS) &&
        (mRootfsInfo.RootfsVar[RF_STATUS_A].Value == NVIDIA_OS_STATUS_UNBOOTABLE) &&
        (mRootfsInfo.RootfsVar[RF_STATUS_B].Value == NVIDIA_OS_STATUS_UNBOOTABLE))
    {
        Status = FALSE;
    }

    return Status;
}

/**
  Check mRootfsInfo.RootfsVar, update the variable if UpdateFlag is set

  @retval EFI_SUCCESS    The operation completed successfully.

**/
STATIC
EFI_STATUS
EFIAPI
CheckAndUpdateVariable(VOID)
{
    EFI_STATUS Status = EFI_SUCCESS;
    UINT32 Index;

    // Check mRootfsInfo.RootfsVar[], update the variable if UpdateFlag is set
    for (Index = 0; Index < RF_VARIABLE_INDEX_MAX; Index++)
    {
        if (mRootfsInfo.RootfsVar[Index].UpdateFlag)
        {
            Status = RFSetVariable(Index, mRootfsInfo.RootfsVar[Index].Value);
            if (EFI_ERROR(Status))
            {
                DEBUG((DEBUG_ERROR, "%a: Failed to write: %a\n", __FUNCTION__,
                       mRFAbVariable[Index].Name));
                return Status;
            }
        }
    }

    return Status;
}

/**
  Check input rootfs slot is bootable or not:
  If the retry count of rootfs slot is not 0, rootfs slot is bootable.
  If the retry count of rootfs slot is 0, rootfs slot is unbootable.

  @param[in] RootfsSlot      The rootfs slot number

  @retval TRUE     The input rootfs slot is bootable
  @retval FALSE    The input rootfs slot is unbootable

**/
STATIC
BOOLEAN
EFIAPI
IsRootfsSlotBootable(IN UINT32 RootfsSlot)
{
    EFI_STATUS Status;
    UINT32 RetryCount;
    BOOLEAN Bootable = FALSE;

    Status = GetRetryCountFrommRootfsInfo(RootfsSlot, &RetryCount);
    if (EFI_ERROR(Status))
    {
        DEBUG((DEBUG_ERROR,
               "%a: Failed to Get Rootfs retry count of slot %d from mRootfsInfo: %r\n",
               __FUNCTION__, RootfsSlot, Status));
        goto Exit;
    }

    // The rootfs slot is bootable.
    if (RetryCount != 0)
    {
        Bootable = TRUE;
    }
    else
    {
        // The rootfs slot is unbootable
        Bootable = FALSE;
    }

Exit:
    return Bootable;
}

/**
  Decrease the RetryCount of input rootfs slot and save to mRootfsInfo

  @param[in] RootfsSlot      The rootfs slot number

  @retval EFI_SUCCESS            The operation completed successfully
  @retval EFI_INVALID_PARAMETER  RetryCount of input rootfs slot is invalid

**/
STATIC
EFI_STATUS
EFIAPI
DecreaseRootfsRetryCount(IN UINT32 RootfsSlot)
{
    EFI_STATUS Status;
    UINT32 RetryCount;

    Status = GetRetryCountFrommRootfsInfo(RootfsSlot, &RetryCount);
    if (EFI_ERROR(Status))
    {
        DEBUG((DEBUG_ERROR,
               "%a: Failed to Get Rootfs retry count of slot %d from mRootfsInfo: %r\n",
               __FUNCTION__, RootfsSlot, Status));
        goto Exit;
    }

    // The rootfs slot is bootable.
    if (RetryCount != 0)
    {
        RetryCount--;
        Status = SetRetryCountTomRootfsInfo(RootfsSlot, RetryCount);
        if (EFI_ERROR(Status))
        {
            DEBUG((DEBUG_ERROR, "%a: Failed to set retry count of slot %d to mRootfsInfo: %r\n",
                   __FUNCTION__, RootfsSlot, Status));
            goto Exit;
        }
    }
    else
    {
        Status = EFI_INVALID_PARAMETER;
    }

Exit:
    return Status;
}

/**
  Validate rootfs A/B status and update BootMode and BootChain accordingly, basic flow:
  If there is no rootfs B,
     (1) boot to rootfs A if retry count of rootfs A is not 0;
     (2) boot to recovery if rtry count of rootfs A is 0.
  If there is rootfs B,
     (1) boot to current rootfs slot if the retry count of current slot is not 0;
     (2) switch to non-current rootfs slot if the retry count of current slot is 0
         and non-current rootfs is bootable
     (3) boot to recovery if both rootfs slots are invalid.

  @param[out] BootParams      The current rootfs boot parameters

  @retval EFI_SUCCESS    The operation completed successfully.

**/
EFI_STATUS
EFIAPI
ValidateRootfsStatus(OUT L4T_BOOT_PARAMS* BootParams)
{
    // Sanity check in case BootChain got corrupted
    if (BootParams->BootChain > 1)
    {
        BootParams->BootChain = 0; // Default to A
    }

    if (BootParams->BootMode == NVIDIA_L4T_BOOTMODE_RECOVERY)
    {
        DEBUG(
            (DEBUG_WARN, "%a: Recovery boot suppressed during rootfs validation\n", __FUNCTION__));
        BootParams->BootMode = NVIDIA_L4T_BOOTMODE_GRUB;
    }

    return EFI_SUCCESS;
}
