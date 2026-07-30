/** @file

  SPDX-FileCopyrightText: Copyright (c) 2022-2025, NVIDIA CORPORATION & AFFILIATES. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef __L4T_LAUNCHER_H_
#define __L4T_LAUNCHER_H_
#include "L4TOpteeDecrypt.h"

#include <Protocol/L4TLauncherSupportProtocol.h>

#define GRUB_PATH                       L"EFI\\BOOT\\grubaa64.efi"
#define GRUB_BOOTCONFIG_FILE            L"EFI\\BOOT\\boot.cfg"
#define MAX_BOOTCONFIG_CONTENT_SIZE     512
#define MAX_CBOOTARG_SIZE               256
#define GRUB_BOOTCONFIG_CONTENT_FORMAT  "set cbootargs=\"%s\"\r\nset root_partition_number=%u\r\nset bootimg_present=%u\r\nset recovery_present=%u\r\n"
#define DETACHED_SIG_FILE_EXTENSION     L".sig"

#define EXTLINUX_CONF_PATH  L"boot\\extlinux\\extlinux.conf"

#define BOOTMODE_DIRECT_STRING    L"bootmode=direct"
#define BOOTMODE_GRUB_STRING      L"bootmode=grub"
#define BOOTMODE_BOOTIMG_STRING   L"bootmode=bootimg"
#define BOOTMODE_RECOVERY_STRING  L"bootmode=recovery"

#define BOOTCHAIN_OVERRIDE_STRING  L"bootchain="

#define MAX_PARTITION_NAME_SIZE  36       // From the UEFI spec for GPT partitions

#define BOOT_FW_VARIABLE_NAME  L"BootChainFwCurrent"
#define BOOT_OS_VARIABLE_NAME  L"BootChainOsCurrent"

#define ROOTFS_BASE_NAME        L"APP"
#define BOOTIMG_BASE_NAME       L"kernel"
#define BOOTIMG_DTB_BASE_NAME   L"kernel-dtb"
#define RECOVERY_BASE_NAME      L"recovery"
#define RECOVERY_DTB_BASE_NAME  L"recovery-dtb"

#define EXTLINUX_KEY_TIMEOUT     L"TIMEOUT"
#define EXTLINUX_KEY_DEFAULT     L"DEFAULT"
#define EXTLINUX_KEY_MENU_TITLE  L"MENU TITLE"
#define EXTLINUX_KEY_LABEL       L"LABEL"
#define EXTLINUX_KEY_MENU_LABEL  L"MENU LABEL"
#define EXTLINUX_KEY_LINUX       L"LINUX"
#define EXTLINUX_KEY_INITRD      L"INITRD"
#define EXTLINUX_KEY_FDT         L"FDT"
#define EXTLINUX_KEY_APPEND      L"APPEND"
#define EXTLINUX_KEY_OVERLAYS    L"OVERLAYS"

#define EXTLINUX_CBOOT_ARG  L"${cbootargs}"

#define MAX_EXTLINUX_OPTIONS  10

#define SIG_FILE_SIZE_2KB  SIZE_2KB
#define SIG_FILE_SIZE_4KB  SIZE_4KB

//
// Volley Boot Mode - determines which slot/partition to boot from.
// ornx (Orin NX / DSBOARD-ORNX): everything lives on the single NVMe disk.
// INSTALL boots the persistent installer partition (GPT name "APP");
// SLOT_A/B boot the VOLLEY_A / VOLLEY_B slot partitions.
//
typedef enum {
    VOLLEY_MODE_INSTALL,    // Boot from APP installer partition (slot invalid/missing)
    VOLLEY_MODE_SLOT_A,     // Boot from slot partition VOLLEY_A
    VOLLEY_MODE_SLOT_B,     // Boot from slot partition VOLLEY_B
} VOLLEY_BOOT_MODE;

// Slot verification limits
#define VOLLEY_MAX_CHECKS           16
#define VOLLEY_MAX_CHECK_PATH_CHARS 256

//
// Parsed slot_meta.txt from NVMe partition
//
typedef struct {
    BOOLEAN     ParseError;     // TRUE if slot metadata parsing failed
    BOOLEAN     Valid;          // TRUE if valid=1 in file
    UINT32      UpdateCounter;  // update_counter value for slot selection
    UINTN       CheckCount;     // number of hash checks
    struct {
        BOOLEAN Valid;
        UINT8   Sha256[32];
        CHAR16  Path[VOLLEY_MAX_CHECK_PATH_CHARS];
    } Checks[VOLLEY_MAX_CHECKS];
} VOLLEY_SLOT_META;

//
// Volley slot metadata path and partition UUIDs
//
#define VOLLEY_SLOT_META_PATH       L"slot_meta.txt"

// Fixed PARTUUIDs for boot cmdline — must match unique_guid values in
// flash_l4t_t234_nvme_volley.xml (VOLLEY_A id16, VOLLEY_B id17, APP id1).
// Slot A/B GUIDs carried over from Xavier; APP (installer) GUID is ornx-new.
#define VOLLEY_INSTALLER_APP_PARTUUID L"73dc0914-303f-41bc-9b91-d5416f8e1f9c"
#define VOLLEY_NVME_SLOT_A_PARTUUID   L"e1fce829-2b17-4b1c-b635-1093618dd0f4"
#define VOLLEY_NVME_SLOT_B_PARTUUID   L"b77bb181-a13f-4dca-ae3c-38c564326356"

// GPT partition names on the single ornx NVMe disk
#define VOLLEY_SLOT_A_PART_NAME    L"VOLLEY_A"
#define VOLLEY_SLOT_B_PART_NAME    L"VOLLEY_B"
#define VOLLEY_INSTALLER_PART_NAME L"APP"

// Base kernel command line.
// usbcore.autosuspend=-1 disables USB autosuspend to prevent ZED camera disconnects.
// t234 TCU earlycon uses the same combined-UART mailbox address as t194.
#define VOLLEY_BASE_CMDLINE         L"earlycon=tegra_comb_uart,mmio32,0x0c168000 console=ttyTCU0,115200n8 noinitrd usbcore.autosuspend=-1 "

typedef struct {
  CHAR16    *Label;
  CHAR16    *MenuLabel;
  CHAR16    *LinuxPath;
  CHAR16    *DtbPath;
  CHAR16    *InitrdPath;
  CHAR16    *BootArgs;
  CHAR16    *Overlays;
} EXTLINUX_BOOT_OPTION;

typedef struct {
  UINT32                  DefaultBootEntry;
  CHAR16                  *MenuTitle;
  EXTLINUX_BOOT_OPTION    BootOptions[MAX_EXTLINUX_OPTIONS];
  UINT32                  NumberOfBootOptions;
  UINT32                  Timeout;
} EXTLINUX_BOOT_CONFIG;

extern L4T_LAUNCHER_SUPPORT_PROTOCOL  *gL4TSupportProtocol;

#endif /* __L4T_LAUNCHER_H_ */
