/** @file

  Copyright (c) 2022-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef __L4T_LAUNCHER_H_
#define __L4T_LAUNCHER_H_
#include "L4TOpteeDecrypt.h"

#define GRUB_PATH                       L"EFI\\BOOT\\grubaa64.efi"
#define GRUB_BOOTCONFIG_FILE            L"EFI\\BOOT\\boot.cfg"
#define MAX_BOOTCONFIG_CONTENT_SIZE     512
#define MAX_CBOOTARG_SIZE               256
#define GRUB_BOOTCONFIG_CONTENT_FORMAT  "set cbootargs=\"%s\"\r\nset root_partition_number=%d\r\nset bootimg_present=%d\r\nset recovery_present=%d\r\n"
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

#define EXTLINUX_CBOOT_ARG  L"${cbootargs}"

#define MAX_EXTLINUX_OPTIONS  10

//
// Volley Boot Mode - determines which device/slot to boot from
//
typedef enum {
    VOLLEY_MODE_INSTALL,    // Boot from eMMC for installation (UUID mismatch or no NVMe)
    VOLLEY_MODE_SLOT_A,     // Boot from NVMe slot A (p1)
    VOLLEY_MODE_SLOT_B,     // Boot from NVMe slot B (p2)
} VOLLEY_BOOT_MODE;

//
// Parsed boot_config.txt from eMMC ESP
//
typedef struct {
    BOOLEAN     Valid;              // TRUE if config was successfully parsed
    EFI_GUID    ExpectedNvmeUuid;   // expected_nvme_uuid value (GPT DiskGUID)
} VOLLEY_BOOT_CONFIG;

//
// Parsed slot_meta.txt from NVMe partition
//
typedef struct {
    BOOLEAN     Valid;          // TRUE if valid=1 in file
    UINT32      UpdateCounter;  // update_counter value for slot selection
} VOLLEY_SLOT_META;

//
// Volley boot configuration file paths and partition UUIDs
//
#define VOLLEY_BOOT_CONFIG_PATH     L"boot_config.txt"
#define VOLLEY_SLOT_META_PATH       L"slot_meta.txt"

// Fixed PARTUUIDs for boot cmdline
#define VOLLEY_EMMC_APP_PARTUUID    L"840a8a6a-e0c7-4d16-9007-fea99d2c4bc9"
#define VOLLEY_NVME_SLOT_A_PARTUUID L"e1fce829-2b17-4b1c-b635-1093618dd0f4"
#define VOLLEY_NVME_SLOT_B_PARTUUID L"b77bb181-a13f-4dca-ae3c-38c564326356"

typedef struct {
  CHAR16    *Label;
  CHAR16    *MenuLabel;
  CHAR16    *LinuxPath;
  CHAR16    *DtbPath;
  CHAR16    *InitrdPath;
  CHAR16    *BootArgs;
} EXTLINUX_BOOT_OPTION;

typedef struct {
  UINT32                  DefaultBootEntry;
  CHAR16                  *MenuTitle;
  EXTLINUX_BOOT_OPTION    BootOptions[MAX_EXTLINUX_OPTIONS];
  UINT32                  NumberOfBootOptions;
  UINT32                  Timeout;
} EXTLINUX_BOOT_CONFIG;

STATIC ImageEncryptionInfo  EncryptionInfo;

STATIC VOID   *mRamdiskData = NULL;
STATIC UINTN  mRamdiskSize  = 0;

typedef struct {
  VENDOR_DEVICE_PATH          VendorMediaNode;
  EFI_DEVICE_PATH_PROTOCOL    EndNode;
} RAMDISK_DEVICE_PATH;

STATIC CONST RAMDISK_DEVICE_PATH  mRamdiskDevicePath =
{
  {
    {
      MEDIA_DEVICE_PATH,
      MEDIA_VENDOR_DP,
      { sizeof (VENDOR_DEVICE_PATH),       0 }
    },
    LINUX_EFI_INITRD_MEDIA_GUID
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 }
  }
};

#endif /* __L4T_LAUNCHER_H_ */
