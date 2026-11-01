/** @file
  Platform contract for a Qualcomm LK MMC backend used by Project Mu.
  SPDX-License-Identifier: BSD-2-Clause
**/
#ifndef J250F_LK_PLATFORM_H
#define J250F_LK_PLATFORM_H
#include <Uefi.h>

typedef EFI_STATUS (EFIAPI *J250F_LK_READ) (
  IN VOID *Device,
  IN EFI_LBA StartBlock,
  IN UINTN NumberOfBlocks,
  OUT VOID *Buffer
  );

typedef struct {
  VOID          *Device;
  EFI_LBA        NumberOfBlocks; // eMMC user area, sectors
  UINT32         BlockSize;      // Normally 512
  UINT32         IoAlignment;    // DMA/cache maintenance requirement, power of 2
  J250F_LK_READ  ReadBlocks;
} J250F_LK_DISK;

// Must set board clocks/rails/pins, ensure MMIO, IRQ or polled PWRCTL
// handling and call Qualcomm LK mmc_init() before returning success.
// NEVER return success unless the eMMC card is identified and readable.
EFI_STATUS EFIAPI J250fLkBoardOpen (OUT J250F_LK_DISK *Disk);

#endif
