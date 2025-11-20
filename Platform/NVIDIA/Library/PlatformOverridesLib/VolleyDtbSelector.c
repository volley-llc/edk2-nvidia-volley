/** @file
  Select and load the correct DTB for the current board (p2888-0001 vs 0008)
  from the EFI partition and make it available to L4TLauncher via an EFI
  variable.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/DevicePathLib.h>
#include <Library/FileHandleLib.h>
#include <Library/PrintLib.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/Eeprom.h>

#define VOLLEY_DTB_VAR_NAME     L"VolleyDtbPath"
#define VOLLEY_DTB_PROFILE_VAR  L"VolleyDtbProfile"

STATIC
EFI_STATUS
GetBoardProductId(CHAR8* ProductId, UINTN ProductIdLen)
{
  EFI_STATUS   Status;
  EFI_HANDLE   *Handles = NULL;
  UINTN        HandleCount = 0;
  TEGRA_EEPROM_BOARD_INFO  *Eeprom = NULL;

  Status = gBS->LocateHandleBuffer(ByProtocol, &gNVIDIAEepromProtocolGuid, NULL, &HandleCount,
                                   &Handles);
  if (EFI_ERROR(Status) || HandleCount == 0) {
    return EFI_NOT_FOUND;
  }

  Status = gBS->HandleProtocol(Handles[0], &gNVIDIAEepromProtocolGuid, (VOID**)&Eeprom);
  if (!EFI_ERROR(Status) && (Eeprom != NULL)) {
    CopyMem(ProductId, Eeprom->ProductId, MIN(ProductIdLen, sizeof(Eeprom->ProductId)));
  }

  if (Handles != NULL) {
    FreePool(Handles);
  }

  return Status;
}

STATIC
EFI_STATUS
SetVolleyDtbPath(VOID)
{
  EFI_STATUS  Status;
  CHAR8       ProductId[TEGRA_PRODUCT_ID_LEN] = {0};
  BOOLEAN     Industrial = FALSE;
  CHAR16      DtbPath[128];
  UINT32      Attributes = EFI_VARIABLE_BOOTSERVICE_ACCESS;

  Status = GetBoardProductId(ProductId, sizeof(ProductId));
  if (!EFI_ERROR(Status)) {
    if (AsciiStrStr(ProductId, "0008") != NULL) {
      Industrial = TRUE;
    }
  } else {
    DEBUG((DEBUG_WARN, "%a: Unable to read product ID, defaulting to non-industrial\n",
           __FUNCTION__));
  }

  UnicodeSPrint(DtbPath, sizeof(DtbPath), L"EFI\\volley\\dtb\\%s",
                Industrial ? L"tegra194-p2888-0008-p2822-0000.dtb"
                           : L"tegra194-p2888-0001-p2822-0000.dtb");

  Status = gRT->SetVariable(VOLLEY_DTB_VAR_NAME, &gEfiGlobalVariableGuid, Attributes,
                            StrSize(DtbPath), DtbPath);
  if (EFI_ERROR(Status)) {
    DEBUG((DEBUG_WARN, "%a: Failed to set DTB path variable: %r\n", __FUNCTION__, Status));
  } else {
    DEBUG((DEBUG_INFO, "%a: Selected DTB: %s\n", __FUNCTION__, DtbPath));
  }

  Status = gRT->SetVariable(VOLLEY_DTB_PROFILE_VAR, &gEfiGlobalVariableGuid, Attributes,
                            sizeof(Industrial), &Industrial);
  if (EFI_ERROR(Status)) {
    DEBUG((DEBUG_WARN, "%a: Failed to set DTB profile variable: %r\n", __FUNCTION__, Status));
  }

  return Status;
}

EFI_STATUS
EFIAPI
VolleyDtbSelectorEntry(IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE* SystemTable)
{
  return SetVolleyDtbPath();
}
