/** @file
  Normal FDF GOP DXE entry point. Reuse the same implementation that the
  eMMC/ADB driver can invoke if GOP dispatch did not publish a protocol.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Uefi.h>
#include <J250fGopSupportLib.h>

EFI_STATUS
EFIAPI
J250fGopBootEntry (
  IN EFI_HANDLE ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  return J250fGopEntry (ImageHandle, SystemTable);
}
