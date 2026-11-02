/** @file
  Shared J250F GOP publisher used by the early DXE and ADB recovery path.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#ifndef J250F_GOP_SUPPORT_LIB_H_
#define J250F_GOP_SUPPORT_LIB_H_

#include <Uefi.h>

/* Safe to call again when GOP already exists. No eMMC access. */
EFI_STATUS
EFIAPI
J250fGopEntry (
  IN EFI_HANDLE ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  );

#endif
