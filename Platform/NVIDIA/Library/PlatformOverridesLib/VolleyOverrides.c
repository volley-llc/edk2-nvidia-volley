/** @file
  DXE constructor that applies Volley-specific platform overrides.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/Eeprom.h>

#include "KeyboardSuppress.h"

STATIC
VOID SetVolleyDtbPath(VOID);

STATIC
VOID DeleteVolleyBootChainVariables(VOID);

STATIC
VOID
EFIAPI
OnCvmEepromAvailable(IN EFI_EVENT Event, IN VOID *Context);

STATIC EFI_EVENT mCvmEepromNotifyEvent = NULL;
STATIC VOID      *mCvmEepromNotifyRegistration = NULL;

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

  SetVolleyDtbPath();

  if (mCvmEepromNotifyEvent == NULL) {
    Status = gBS->CreateEvent(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, OnCvmEepromAvailable, NULL,
                              &mCvmEepromNotifyEvent);
    if (!EFI_ERROR(Status)) {
      Status = gBS->RegisterProtocolNotify(&gNVIDIACvmEepromProtocolGuid, mCvmEepromNotifyEvent,
                                           &mCvmEepromNotifyRegistration);
      if (EFI_ERROR(Status)) {
        DEBUG((DEBUG_WARN, "%a: Failed to register CVM EEPROM notification: %r\n", __FUNCTION__,
               Status));
      }
    } else {
      DEBUG((DEBUG_WARN, "%a: Failed to create CVM EEPROM notification: %r\n", __FUNCTION__,
             Status));
    }
  }

  return EFI_SUCCESS;
}

#define VOLLEY_DTB_OVERRIDE_VAR  L"VolleyDtbPath"
#define VOLLEY_DTB_PROFILE_VAR   L"VolleyDtbProfile"
#define VOLLEY_DTB_PREFIX        L"EFI\\volley\\dtb\\"
#define VOLLEY_DTB_AGX           L"tegra194-p2888-0001-p2822-0000.dtb"
#define VOLLEY_DTB_AGX_INDUSTRIAL L"tegra194-p2888-0008-p2822-0000.dtb"

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

STATIC
VOID
EFIAPI
OnCvmEepromAvailable(IN EFI_EVENT Event, IN VOID *Context)
{
  SetVolleyDtbPath();
}

STATIC
BOOLEAN IsIndustrialAgxProductId(CONST CHAR8 *ProductId)
{
  CONST TEGRA_EEPROM_PART_NUMBER *Pn;

  if (ProductId == NULL) {
    return FALSE;
  }

  // EEPROM part number is 699-<Class><Id>-<Sku>-..., e.g. 699-12888-0008-600.
  // The class digit varies (8 on devkit modules, 1 on production); ignore it
  // like NVIDIA's TegraBoardIdFromPartNumber does and match Id + Sku.
  Pn = &((CONST EEPROM_PART_NUMBER *)ProductId)->TegraEepromPartNumber;
  return (CompareMem(Pn->Id, "2888", 4) == 0) &&
         (CompareMem(Pn->Sku, "0008", 4) == 0);
}

STATIC
VOID SetVolleyDtbPath(VOID)
{
  EFI_STATUS                Status;
  EFI_HANDLE                *Handles = NULL;
  UINTN                     HandleCount = 0;
  TEGRA_EEPROM_BOARD_INFO   *Eeprom = NULL;
  BOOLEAN                   Industrial = FALSE;
  CHAR16                    DtbPath[128];
  UINT32                    Attributes = EFI_VARIABLE_BOOTSERVICE_ACCESS;
  CHAR8                     ProductId[TEGRA_PRODUCT_ID_LEN + 1];

  ZeroMem(ProductId, sizeof(ProductId));

  Status = gBS->LocateHandleBuffer(ByProtocol, &gNVIDIACvmEepromProtocolGuid, NULL, &HandleCount,
                                   &Handles);
  if (!EFI_ERROR(Status) && HandleCount > 0) {
    Status = gBS->HandleProtocol(Handles[0], &gNVIDIACvmEepromProtocolGuid, (VOID**)&Eeprom);
    if (!EFI_ERROR(Status) && Eeprom != NULL) {
      CopyMem(ProductId, Eeprom->ProductId,
              MIN(sizeof(ProductId) - 1, sizeof(Eeprom->ProductId)));
      if (IsIndustrialAgxProductId(ProductId)) {
        Industrial = TRUE;
      }
    }
  }

  if (Handles != NULL) {
    FreePool(Handles);
  }

  UnicodeSPrint(DtbPath, sizeof(DtbPath), VOLLEY_DTB_PREFIX L"%s",
                Industrial ? VOLLEY_DTB_AGX_INDUSTRIAL : VOLLEY_DTB_AGX);

  Status = gRT->SetVariable(VOLLEY_DTB_OVERRIDE_VAR, &gEfiGlobalVariableGuid, Attributes,
                            StrSize(DtbPath), DtbPath);
  if (EFI_ERROR(Status)) {
    DEBUG((DEBUG_WARN, "%a: Failed to set DTB path variable: %r\n", __FUNCTION__, Status));
  } else {
    DEBUG(
        (DEBUG_INFO, "%a: Selected DTB %s (ProductId=%a)\n", __FUNCTION__, DtbPath, ProductId));
  }

  Status = gRT->SetVariable(VOLLEY_DTB_PROFILE_VAR, &gEfiGlobalVariableGuid, Attributes,
                            sizeof(Industrial), &Industrial);
  if (EFI_ERROR(Status)) {
    DEBUG((DEBUG_WARN, "%a: Failed to set DTB profile variable: %r\n", __FUNCTION__, Status));
  }
}
