/** @file
  DXE constructor that applies Volley-specific platform overrides.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include "KeyboardSuppress.h"

STATIC
VOID DeleteVolleyBootChainVariables(VOID);

EFI_STATUS
EFIAPI
VolleyOverridesEntry(IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE* SystemTable)
{
  EFI_STATUS Status;

  Status = DisableKeyboardInput();
  if (EFI_ERROR(Status)) {
    DEBUG((DEBUG_WARN, "%a: Failed to disable keyboard input: %r\n", __FUNCTION__, Status));
  } else {
    DEBUG((DEBUG_INFO, "%a: Keyboard input disabled\n", __FUNCTION__));
  }

  DeleteVolleyBootChainVariables();

  return EFI_SUCCESS;
}

STATIC
VOID DeleteVolleyVariable(IN CHAR16 *Name, IN EFI_GUID *Guid, IN UINT32 Attributes)
{
  EFI_STATUS Status;

  Status = gRT->SetVariable(Name, Guid, Attributes, 0, NULL);
  if (!EFI_ERROR(Status)) {
    DEBUG((DEBUG_INFO, "%a: Deleted %s\n", __FUNCTION__, Name));
  } else if (Status != EFI_NOT_FOUND) {
    DEBUG((DEBUG_WARN, "%a: Failed to delete %s: %r\n", __FUNCTION__, Name, Status));
  }
}

STATIC
VOID DeleteVolleyBootChainVariables(VOID)
{
  DeleteVolleyVariable(L"BootChainFwCurrent", &gNVIDIAPublicVariableGuid,
                       EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS);
  DeleteVolleyVariable(L"BootChainFwNext", &gNVIDIAPublicVariableGuid,
                       EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS |
                       EFI_VARIABLE_NON_VOLATILE);
  DeleteVolleyVariable(L"BootChainFwStatus", &gNVIDIAPublicVariableGuid,
                       EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS |
                       EFI_VARIABLE_NON_VOLATILE);
  DeleteVolleyVariable(L"AutoUpdateBrBct", &gNVIDIAPublicVariableGuid,
                       EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_NON_VOLATILE);
  DeleteVolleyVariable(L"BootChainFwPrevious", &gNVIDIATokenSpaceGuid,
                       EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_NON_VOLATILE);
  DeleteVolleyVariable(L"BootChainFwResetCount", &gNVIDIATokenSpaceGuid,
                       EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_NON_VOLATILE);
}
