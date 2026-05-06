/** @file
  Helper functions to disable keyboard input at the SimpleTextIn level.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextInEx.h>

#include "KeyboardSuppress.h"

STATIC
EFI_STATUS
EFIAPI
DisabledSimpleTextInReset(IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL* This, IN BOOLEAN ExtendedVerification)
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
DisabledSimpleTextInReadKeyStroke(IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL* This, OUT EFI_INPUT_KEY* Key)
{
  if (Key != NULL) {
    Key->ScanCode   = 0;
    Key->UnicodeChar = 0;
  }

  return EFI_NOT_READY;
}

STATIC
EFI_STATUS
EFIAPI
DisabledSimpleTextInExReset(IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL* This,
                            IN BOOLEAN ExtendedVerification)
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
DisabledSimpleTextInExReadKeyStroke(IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL* This,
                                    OUT EFI_KEY_DATA* KeyData)
{
  if (KeyData != NULL) {
    ZeroMem(KeyData, sizeof(EFI_KEY_DATA));
  }

  return EFI_NOT_READY;
}

STATIC
EFI_STATUS
EFIAPI
DisabledSimpleTextInExSetState(IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL* This,
                               IN EFI_KEY_TOGGLE_STATE* KeyToggleState)
{
  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
EFIAPI
DisabledSimpleTextInExRegisterKeyNotify(IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL* This,
                                        IN EFI_KEY_DATA* KeyData,
                                        IN EFI_KEY_NOTIFY_FUNCTION KeyNotificationFunction,
                                        OUT VOID** NotifyHandle)
{
  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
EFIAPI
DisabledSimpleTextInExUnregisterKeyNotify(IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL* This,
                                          IN VOID* NotificationHandle)
{
  return EFI_UNSUPPORTED;
}

STATIC EFI_SIMPLE_TEXT_INPUT_PROTOCOL mDisabledSimpleTextIn = {
  DisabledSimpleTextInReset, DisabledSimpleTextInReadKeyStroke, NULL};

STATIC EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL mDisabledSimpleTextInEx = {
  DisabledSimpleTextInExReset,
  DisabledSimpleTextInExReadKeyStroke,
  NULL,
  DisabledSimpleTextInExSetState,
  DisabledSimpleTextInExRegisterKeyNotify,
  DisabledSimpleTextInExUnregisterKeyNotify};

STATIC
VOID
EFIAPI
NopNotify(IN EFI_EVENT Event, IN VOID* Context)
{
  (VOID)Event;
  (VOID)Context;
}

STATIC
VOID
EFIAPI
OnTextInInstalled(IN EFI_EVENT Event, IN VOID* Context)
{
  (VOID)Event;
  (VOID)Context;

  DisableKeyboardInput();
}

STATIC
EFI_STATUS
InitializeStubEvents(VOID)
{
  EFI_STATUS Status;
  static EFI_EVENT mSimpleTextInNotifyEvent = NULL;
  static EFI_EVENT mSimpleTextInExNotifyEvent = NULL;
  static VOID* mSimpleTextInNotifyReg = NULL;
  static VOID* mSimpleTextInExNotifyReg = NULL;

  if (mDisabledSimpleTextIn.WaitForKey == NULL) {
    Status = gBS->CreateEvent(EVT_NOTIFY_WAIT, TPL_CALLBACK, NopNotify, NULL,
                              &mDisabledSimpleTextIn.WaitForKey);
    if (EFI_ERROR(Status)) {
      return Status;
    }
  }

  if (mDisabledSimpleTextInEx.WaitForKeyEx == NULL) {
    Status = gBS->CreateEvent(EVT_NOTIFY_WAIT, TPL_CALLBACK, NopNotify, NULL,
                              &mDisabledSimpleTextInEx.WaitForKeyEx);
    if (EFI_ERROR(Status)) {
      return Status;
    }
  }

  // Register protocol notifications so any late-bound consoles are immediately stubbed.
  if (mSimpleTextInNotifyEvent == NULL) {
    Status = gBS->CreateEvent(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, OnTextInInstalled, NULL,
                              &mSimpleTextInNotifyEvent);
    if (!EFI_ERROR(Status)) {
      gBS->RegisterProtocolNotify(&gEfiSimpleTextInProtocolGuid, mSimpleTextInNotifyEvent,
                                  &mSimpleTextInNotifyReg);
    }
  }

  if (mSimpleTextInExNotifyEvent == NULL) {
    Status = gBS->CreateEvent(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, OnTextInInstalled, NULL,
                              &mSimpleTextInExNotifyEvent);
    if (!EFI_ERROR(Status)) {
      gBS->RegisterProtocolNotify(&gEfiSimpleTextInputExProtocolGuid,
                                  mSimpleTextInExNotifyEvent, &mSimpleTextInExNotifyReg);
    }
  }

  return EFI_SUCCESS;
}

STATIC
VOID UpdateSystemTableInputInterfaces(VOID)
{
  if (gST == NULL) {
    return;
  }

  gST->ConIn = &mDisabledSimpleTextIn;
}

STATIC
VOID ReplaceSimpleTextInOnHandle(IN EFI_HANDLE Handle)
{
  EFI_STATUS Status;
  EFI_SIMPLE_TEXT_INPUT_PROTOCOL* Original;

  Status = gBS->HandleProtocol(Handle, &gEfiSimpleTextInProtocolGuid, (VOID**)&Original);
  if (EFI_ERROR(Status)) {
    return;
  }

  if (Original == &mDisabledSimpleTextIn) {
    return;
  }

  Status = gBS->ReinstallProtocolInterface(Handle, &gEfiSimpleTextInProtocolGuid, Original,
                                           &mDisabledSimpleTextIn);
  if (EFI_ERROR(Status)) {
    DEBUG((DEBUG_WARN, "%a: Failed to replace SimpleTextIn on %p: %r\n", __FUNCTION__, Handle,
           Status));
    return;
  }

  if (gST != NULL && Handle == gST->ConsoleInHandle) {
    gST->ConIn = &mDisabledSimpleTextIn;
  }
}

STATIC
VOID ReplaceSimpleTextInExOnHandle(IN EFI_HANDLE Handle)
{
  EFI_STATUS Status;
  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL* Original;

  Status = gBS->HandleProtocol(Handle, &gEfiSimpleTextInputExProtocolGuid, (VOID**)&Original);
  if (EFI_ERROR(Status)) {
    return;
  }

  if (Original == &mDisabledSimpleTextInEx) {
    return;
  }

  Status = gBS->ReinstallProtocolInterface(Handle, &gEfiSimpleTextInputExProtocolGuid, Original,
                                           &mDisabledSimpleTextInEx);
  if (EFI_ERROR(Status)) {
    DEBUG((DEBUG_WARN, "%a: Failed to replace SimpleTextInEx on %p: %r\n", __FUNCTION__, Handle,
           Status));
    return;
  }

  // No ConInEx member in gST; reinstall is sufficient.
}

EFI_STATUS
DisableKeyboardInput(VOID)
{
  EFI_STATUS Status;
  EFI_HANDLE* Handles;
  UINTN HandleCount;

  Handles     = NULL;
  HandleCount = 0;

  Status = InitializeStubEvents();
  if (EFI_ERROR(Status)) {
    DEBUG((DEBUG_ERROR, "%a: Failed to initialize stub events: %r\n", __FUNCTION__, Status));
    return Status;
  }

  UpdateSystemTableInputInterfaces();

  Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiSimpleTextInProtocolGuid, NULL, &HandleCount,
                                   &Handles);
  if (!EFI_ERROR(Status)) {
    for (UINTN Index = 0; Index < HandleCount; Index++) {
      ReplaceSimpleTextInOnHandle(Handles[Index]);
    }

    if (Handles != NULL) {
      FreePool(Handles);
      Handles = NULL;
    }
  } else if (Status != EFI_NOT_FOUND) {
    DEBUG((DEBUG_WARN, "%a: Failed to find SimpleTextIn handles: %r\n", __FUNCTION__, Status));
  }

  HandleCount = 0;
  Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiSimpleTextInputExProtocolGuid, NULL,
                                   &HandleCount, &Handles);
  if (!EFI_ERROR(Status)) {
    for (UINTN Index = 0; Index < HandleCount; Index++) {
      ReplaceSimpleTextInExOnHandle(Handles[Index]);
    }

    if (Handles != NULL) {
      FreePool(Handles);
      Handles = NULL;
    }
  } else if (Status != EFI_NOT_FOUND) {
    DEBUG(
        (DEBUG_WARN, "%a: Failed to find SimpleTextInEx handles: %r\n", __FUNCTION__, Status));
  }

  if (Handles != NULL) {
    FreePool(Handles);
  }

  return EFI_SUCCESS;
}
