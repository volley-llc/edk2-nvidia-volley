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

  SetVolleyDtbPath();

  return EFI_SUCCESS;
}

#define VOLLEY_DTB_OVERRIDE_VAR  L"VolleyDtbPath"
#define VOLLEY_DTB_PROFILE_VAR   L"VolleyDtbProfile"
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

  Status = gBS->LocateHandleBuffer(ByProtocol, &gNVIDIAEepromProtocolGuid, NULL, &HandleCount,
                                   &Handles);
  if (!EFI_ERROR(Status) && HandleCount > 0) {
    Status = gBS->HandleProtocol(Handles[0], &gNVIDIAEepromProtocolGuid, (VOID**)&Eeprom);
    if (!EFI_ERROR(Status) && Eeprom != NULL) {
      CopyMem(ProductId, Eeprom->ProductId,
              MIN(sizeof(ProductId) - 1, sizeof(Eeprom->ProductId)));
      if (AsciiStrStr(ProductId, "0008") != NULL) {
        Industrial = TRUE;
      }
    }
  }

  if (Handles != NULL) {
    FreePool(Handles);
  }

  UnicodeSPrint(DtbPath, sizeof(DtbPath), L"EFI\\volley\\dtb\\%s",
                Industrial ? L"tegra194-p2888-0008-p2822-0000.dtb"
                           : L"tegra194-p2888-0001-p2822-0000.dtb");

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
