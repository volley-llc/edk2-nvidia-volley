/** @file
  Interface for disabling keyboard input in UEFI.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef KEYBOARD_SUPPRESS_H_
#define KEYBOARD_SUPPRESS_H_

#include <Uefi.h>

EFI_STATUS
DisableKeyboardInput(VOID);

#endif // KEYBOARD_SUPPRESS_H_
