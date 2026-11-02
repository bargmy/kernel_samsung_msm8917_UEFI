/* SPDX-License-Identifier: BSD-2-Clause */
#include <Uefi.h>
EFI_STATUS EFIAPI UefiMain(EFI_HANDLE ImageHandle,EFI_SYSTEM_TABLE *SystemTable){
  (VOID)ImageHandle;
  if(SystemTable&&SystemTable->ConOut)
    SystemTable->ConOut->OutputString(SystemTable->ConOut,L"Uploaded ARM32 EFI application executed.\r\n");
  return EFI_SUCCESS;
}
