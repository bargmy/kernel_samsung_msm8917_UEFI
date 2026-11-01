// SPDX-License-Identifier: GPL-2.0-only
// Target-info discovery follows Samsung smem.c::smem_probe/init_target_info.
// Copyright (c) 2012-2017, The Linux Foundation. All rights reserved.
// GPLv2 only; see Original/COPYING-GPL2. No SMEM allocations are performed.
#include "SmemMap.h"
#include "RpmSmd.h"
#include <Library/DxeServicesTableLib.h>
#include <Library/IoLib.h>

STATIC EFI_STATUS DeviceMap (UINT64 Base, UINT64 Length)
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;
  EFI_STATUS S;
  UINT64 Start, End, Current, Stop;
  if (Length == 0 || Base >= 0x100000000ULL || Length > 0x100000000ULL - Base)
    return EFI_INVALID_PARAMETER;
  Start = Base & ~0xfffULL; End = (Base + Length + 4095) & ~0xfffULL;
  for (Current = Start; Current < End; Current = Stop) {
    S = gDS->GetMemorySpaceDescriptor (Current, &D);
    if (EFI_ERROR (S)) return S;
    if (D.BaseAddress > Current || D.Length == 0 || D.Length > MAX_UINT64 - D.BaseAddress)
      return EFI_COMPROMISED_DATA;
    Stop = D.BaseAddress + D.Length;
    if (Stop <= Current) return EFI_COMPROMISED_DATA;
    if (Stop > End) Stop = End;
    // Never remap allocatable RAM, firmware code, stacks, or another image's pool.
    if (D.GcdMemoryType == EfiGcdMemoryTypeSystemMemory || D.ImageHandle != NULL)
      return EFI_ACCESS_DENIED;
    if (D.GcdMemoryType == EfiGcdMemoryTypeNonExistent) {
      S = gDS->AddMemorySpace (EfiGcdMemoryTypeMemoryMappedIo, Current,
                               Stop - Current, EFI_MEMORY_UC);
      if (EFI_ERROR (S)) return S;
    } else if ((D.Capabilities & EFI_MEMORY_UC) == 0) {
      return EFI_UNSUPPORTED;
    }
    S = gDS->SetMemorySpaceAttributes (Current, Stop - Current, EFI_MEMORY_UC);
    if (EFI_ERROR (S)) return S;
  }
  return EFI_SUCCESS;
}
EFI_STATUS J250fMapRpmSmem (VOID)
{
  UINT64 Pointer, Base = 0x86300000ULL;
  UINT32 Size = 0x100000;
  EFI_STATUS S;
  // DTS smem_targ_info_reg points at the bootloader's SIII structure.
  Pointer = MmioRead32 (0x0193d000U);
  Pointer |= (UINT64)MmioRead32 (0x0193d004U) << 32;
  if (Pointer != 0 && Pointer < 0x100000000ULL) {
    S = DeviceMap (Pointer, 12);
    if (EFI_ERROR (S)) return S;
    if (MmioRead32 ((UINTN)Pointer) == 0x49494953U) {
      Size = MmioRead32 ((UINTN)Pointer + 4);
      Base = MmioRead32 ((UINTN)Pointer + 8);
      // Target kernel has CONFIG_PHYS_ADDR_T_64BIT=n: phys_base_addr is u32.
    }
  }
  // Reject corrupt target-info pointers/sizes before creating a DDR mapping.
  if (Base < 0x80000000ULL || Base >= 0xe0000000ULL ||
      Size < 0x4000 || Size > 0x1000000 || (Base & 4095) || (Size & 4095) ||
      Size > 0xe0000000ULL - Base) return EFI_COMPROMISED_DATA;
  S = DeviceMap (Base, Size);
  if (EFI_ERROR (S)) return S;
  return J250fRpmConfigureSmem ((UINTN)Base, Size);
}
