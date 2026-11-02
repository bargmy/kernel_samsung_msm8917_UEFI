/** @file
  Samsung J250F (MSM8917/SDCC1) memory-mapped eMMC, Project Mu DXE.

  Publish the eMMC USER AREA via EFI_BLOCK_IO_PROTOCOL for UEFI disk/partition
  discovery. This is NOT an OS kernel storage driver. PIO, read-only by default.

  Platform must map SDCC1/GCC/TLMM regions as Device MMIO in its ARM memory
  map before DXE and install J250F_EMMC_POWER_PROTOCOL, which owns PM8917 RPM.

  SPDX-License-Identifier: GPL-2.0-only
**/
#include <Uefi.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/SerialPortLib.h>
#include <Library/PrintLib.h>
#include <Library/FrameBufferSerialPortLib.h>
#include "j250f_emmc.h"
#include "j250f_board.h"
#include "J250fPowerProtocol.h"

#define J250F_PRIVATE_SIG SIGNATURE_32('J','M','M','C')

typedef struct {
  VENDOR_DEVICE_PATH Vendor;
  EFI_DEVICE_PATH_PROTOCOL End;
} J250F_DEVICE_PATH;

typedef struct {
  UINT32 Signature;
  EFI_HANDLE Handle;
  EFI_BLOCK_IO_PROTOCOL BlockIo;
  EFI_BLOCK_IO_MEDIA Media;
  J250F_DEVICE_PATH Path;
  j250f_emmc Host;
  j250f_board Board;
  J250F_EMMC_POWER_PROTOCOL *Power;
  BOOLEAN InIo;
} J250F_DXE;

static J250F_DXE mDev;

static UINTN
HostAddress (j250f_region Region, uint32_t Offset)
{
  return (UINTN)(Region == J250F_REGION_HC ? J250F_HC_PHYS : J250F_CORE_PHYS) + Offset;
}

static uint8_t
HostRead8 (void *Context, j250f_region Region, uint32_t Offset)
{
  (void)Context;
  return MmioRead8 (HostAddress (Region, Offset));
}

static uint16_t
HostRead16 (void *Context, j250f_region Region, uint32_t Offset)
{
  (void)Context;
  return MmioRead16 (HostAddress (Region, Offset));
}

static uint32_t
HostRead32 (void *Context, j250f_region Region, uint32_t Offset)
{
  (void)Context;
  return MmioRead32 (HostAddress (Region, Offset));
}

static void
HostWrite8 (void *Context, j250f_region Region, uint32_t Offset, uint8_t Value)
{
  (void)Context;
  MmioWrite8 (HostAddress (Region, Offset), Value);
}

static void
HostWrite16 (void *Context, j250f_region Region, uint32_t Offset, uint16_t Value)
{
  (void)Context;
  MmioWrite16 (HostAddress (Region, Offset), Value);
}

static void
HostWrite32 (void *Context, j250f_region Region, uint32_t Offset, uint32_t Value)
{
  (void)Context;
  MmioWrite32 (HostAddress (Region, Offset), Value);
}

static uint32_t
BoardRead32 (void *Context, uint32_t Address)
{
  (void)Context;
  return MmioRead32 ((UINTN)Address);
}

static void
BoardWrite32 (void *Context, uint32_t Address, uint32_t Value)
{
  (void)Context;
  MmioWrite32 ((UINTN)Address, Value);
}

static void
IoBarrier (void *Context)
{
  (void)Context;
  MemoryFence ();
}

static void
IoDelay (void *Context, uint32_t Microseconds)
{
  (void)Context;
  MicroSecondDelay (Microseconds);
}

static int
GpllReady (void *Context)
{
  J250F_DXE *Dev = (J250F_DXE *)Context;
  EFI_STATUS Status;
  if (Dev->Power == NULL || Dev->Power->Gpll0Ready == NULL) {
    return -1;
  }
  Status = Dev->Power->Gpll0Ready (Dev->Power);
  return EFI_ERROR (Status) ? -1 : 0;
}

static int
SetClock (void *Context, uint32_t Hertz)
{
  J250F_DXE *Dev = (J250F_DXE *)Context;
  return j250f_gcc_sdcc1_clock (&Dev->Board, Hertz);
}

static int
SetPins (void *Context, int Active)
{
  J250F_DXE *Dev = (J250F_DXE *)Context;
  return j250f_tlmm_sdcc1_pins (&Dev->Board, Active);
}

static int
SetRails (void *Context, int Enable, uint32_t VddUv, uint32_t VqmmcUv)
{
  J250F_DXE *Dev = (J250F_DXE *)Context;
  EFI_STATUS Status;
  if (Dev->Power == NULL || Dev->Power->SetRails == NULL) {
    return -1;
  }
  Status = Dev->Power->SetRails (Dev->Power, Enable != 0, VddUv, VqmmcUv);
  return EFI_ERROR (Status) ? -1 : 0;
}

static const j250f_ops mHostOps = {
  HostRead8, HostRead16, HostRead32,
  HostWrite8, HostWrite16, HostWrite32,
  IoBarrier, IoDelay, SetClock, SetRails, SetPins
};

static EFI_STATUS
EmmcStatus (int Code)
{
  switch (Code) {
    case J250F_OK:           return EFI_SUCCESS;
    case J250F_E_ARGUMENT:   return EFI_INVALID_PARAMETER;
    case J250F_E_TIMEOUT:    return EFI_TIMEOUT;
    case J250F_E_NOT_READY:  return EFI_NOT_READY;
    case J250F_E_READ_ONLY:  return EFI_WRITE_PROTECTED;
    case J250F_E_RANGE:      return EFI_INVALID_PARAMETER;
    case J250F_E_UNSUPPORTED:return EFI_UNSUPPORTED;
    default:                 return EFI_DEVICE_ERROR;
  }
}

// Grab the non-reentrant controller for the complete read/reset operation,
// protecting ONLY the Busy flag at raised TPL, not the slow data transfer.
static BOOLEAN
AcquireIo (J250F_DXE *Dev)
{
  EFI_TPL OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  BOOLEAN Acquired = !Dev->InIo;
  if (Acquired) {
    Dev->InIo = TRUE;
  }
  gBS->RestoreTPL (OldTpl);
  return Acquired;
}

static void
ReleaseIo (J250F_DXE *Dev)
{
  EFI_TPL OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Dev->InIo = FALSE;
  gBS->RestoreTPL (OldTpl);
}

static EFI_STATUS
StartEmmc (J250F_DXE *Dev)
{
  int Result;
  EFI_STATUS Status;

  if (Dev->Power == NULL || Dev->Power->SetRails == NULL ||
      Dev->Power->Gpll0Ready == NULL) {
    return EFI_UNSUPPORTED;
  }

  // PMIC power cannot be inferred from registers in the supplied Linux tree.
  // This call must actually acquire/verify the required regulator votes.
  Status = Dev->Power->SetRails (Dev->Power, TRUE, J250F_VDD_UV, J250F_VQMMC_UV);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Result = j250f_tlmm_sdcc1_pins (&Dev->Board, 1);
  if (Result != 0) {
    return EFI_DEVICE_ERROR;
  }

  Result = j250f_init_host (&Dev->Host, &mHostOps, Dev);
  if (Result == J250F_OK) {
    Result = j250f_init_card (&Dev->Host);
  }
  if (Result != J250F_OK) {
    DEBUG ((DEBUG_ERROR, "J250F: SDCC1 init failed: %d, interrupt=0x%x, R1=0x%x\n",
            Result, Dev->Host.last_interrupt, Dev->Host.last_r1));
    Dev->Media.MediaPresent = FALSE;
    return EmmcStatus (Result);
  }
  // Never enable writes: this is the firmware discovery / read-only phase.
  j250f_allow_writes (&Dev->Host, 0);
  Dev->Media.MediaPresent = TRUE;
  Dev->Media.LastBlock = (EFI_LBA)Dev->Host.capacity_sectors - 1;
  DEBUG ((DEBUG_INFO, "J250F: eMMC user area: %u x 512 bytes, read only\n",
          Dev->Host.capacity_sectors));
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
EmmcReset (IN EFI_BLOCK_IO_PROTOCOL *This, IN BOOLEAN ExtendedVerification)
{
  J250F_DXE *Dev = &mDev;
  EFI_STATUS Status;
  (void)ExtendedVerification;
  if (This != &Dev->BlockIo) {
    return EFI_INVALID_PARAMETER;
  }
  if (!AcquireIo (Dev)) {
    return EFI_NOT_READY;
  }
  Dev->Media.MediaPresent = FALSE;
  Status = StartEmmc (Dev);
  Dev->Media.MediaId++;
  ReleaseIo (Dev);
  return Status;
}

static EFI_STATUS EFIAPI
EmmcRead (IN EFI_BLOCK_IO_PROTOCOL *This, IN UINT32 MediaId, IN EFI_LBA Lba,
          IN UINTN BufferSize, OUT VOID *Buffer)
{
  J250F_DXE *Dev = &mDev;
  EFI_STATUS Status;
  UINTN Blocks;
  int Result;

  if (This != &Dev->BlockIo) {
    return EFI_INVALID_PARAMETER;
  }
  if (MediaId != Dev->Media.MediaId) {
    return EFI_MEDIA_CHANGED;
  }
  if (!Dev->Media.MediaPresent) {
    return EFI_NO_MEDIA;
  }
  if (BufferSize == 0) {
    return EFI_SUCCESS;
  }
  if ((BufferSize % J250F_BLOCK_SIZE) != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }
  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  Blocks = BufferSize / J250F_BLOCK_SIZE;
  // Both LBA and count are validated BEFORE downcasting to uint32_t.
  if (Lba > Dev->Media.LastBlock ||
      (EFI_LBA)(Blocks - 1) > Dev->Media.LastBlock - Lba ||
      Blocks > MAX_UINT32) {
    return EFI_INVALID_PARAMETER;
  }
  if (!AcquireIo (Dev)) {
    return EFI_NOT_READY;
  }
  Result = j250f_read_blocks (&Dev->Host, (uint32_t)Lba,
                              (uint32_t)Blocks, Buffer);
  Status = EmmcStatus (Result);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "J250F: read LBA %Lu blocks %Lu failed: %d / irq=0x%x\n",
            Lba, (UINT64)Blocks, Result, Dev->Host.last_interrupt));
  }
  ReleaseIo (Dev);
  return Status;
}

static EFI_STATUS EFIAPI
EmmcWrite (IN EFI_BLOCK_IO_PROTOCOL *This, IN UINT32 MediaId, IN EFI_LBA Lba,
           IN UINTN BufferSize, IN VOID *Buffer)
{
  (void)Lba;
  (void)BufferSize;
  (void)Buffer;
  if (This != &mDev.BlockIo) {
    return EFI_INVALID_PARAMETER;
  }
  if (MediaId != mDev.Media.MediaId) {
    return EFI_MEDIA_CHANGED;
  }
  if (!mDev.Media.MediaPresent) {
    return EFI_NO_MEDIA;
  }
  return EFI_WRITE_PROTECTED;
}

static EFI_STATUS EFIAPI
EmmcFlush (IN EFI_BLOCK_IO_PROTOCOL *This)
{
  if (This != &mDev.BlockIo) {
    return EFI_INVALID_PARAMETER;
  }
  return mDev.Media.MediaPresent ? EFI_SUCCESS : EFI_NO_MEDIA;
}

EFI_STATUS EFIAPI
J250fEmmcDxeEntryPoint (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;
  J250F_DXE *Dev = &mDev;
  (void)ImageHandle;
  (void)SystemTable;
  ZeroMem (Dev, sizeof (*Dev));

  Status = gBS->LocateProtocol (&gJ250fEmmcPowerProtocolGuid,
                                NULL, (VOID **)&Dev->Power);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "J250F: no PM8917 power provider available: %r\n", Status));
    return Status;
  }

  Dev->Signature = J250F_PRIVATE_SIG;
  Dev->Board.ctx = Dev;
  Dev->Board.reg_read32 = BoardRead32;
  Dev->Board.reg_write32 = BoardWrite32;
  Dev->Board.barrier = IoBarrier;
  Dev->Board.delay_us = IoDelay;
  Dev->Board.gpll0_power_ready = GpllReady;

  Dev->Media.MediaId = 1;
  Dev->Media.RemovableMedia = FALSE;
  Dev->Media.MediaPresent = FALSE;
  Dev->Media.LogicalPartition = FALSE;
  Dev->Media.ReadOnly = TRUE;
  Dev->Media.WriteCaching = FALSE;
  Dev->Media.BlockSize = J250F_BLOCK_SIZE;
  Dev->Media.IoAlign = 1;  // PIO copies bytes, no DMA alignment requirement.
  Dev->Media.LogicalBlocksPerPhysicalBlock = 1;
  Dev->Media.OptimalTransferLengthGranularity = 1;

  Dev->BlockIo.Revision = EFI_BLOCK_IO_PROTOCOL_REVISION3;
  Dev->BlockIo.Media = &Dev->Media;
  Dev->BlockIo.Reset = EmmcReset;
  Dev->BlockIo.ReadBlocks = EmmcRead;
  Dev->BlockIo.WriteBlocks = EmmcWrite;
  Dev->BlockIo.FlushBlocks = EmmcFlush;

  Dev->Path.Vendor.Header.Type = HARDWARE_DEVICE_PATH;
  Dev->Path.Vendor.Header.SubType = HW_VENDOR_DP;
  Dev->Path.Vendor.Header.Length[0] = (UINT8)sizeof (VENDOR_DEVICE_PATH);
  Dev->Path.Vendor.Header.Length[1] = 0;
  CopyMem (&Dev->Path.Vendor.Guid, &gJ250fEmmcDevicePathGuid, sizeof (EFI_GUID));
  Dev->Path.End.Type = END_DEVICE_PATH_TYPE;
  Dev->Path.End.SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
  Dev->Path.End.Length[0] = (UINT8)sizeof (EFI_DEVICE_PATH_PROTOCOL);
  Dev->Path.End.Length[1] = 0;

  SerialPortInitialize ();
  #if defined(J250F_ADB_RAM_EXECUTION)
  FbConStartScreen ();
  #endif
  {
    CHAR8 Text[128];
    UINTN Len = AsciiSPrint (Text, sizeof (Text), "J250F: initializing eMMC at 400kHz...\r\n");
    SerialPortWrite ((UINT8 *)Text, Len);
  }
  Status = StartEmmc (Dev);
  {
    CHAR8 Text[384];
    UINTN Len, Second;
    #if defined(J250F_ADB_RAM_EXECUTION)
    FbConStartScreen ();
    #endif
    if (EFI_ERROR (Status)) {
      Len = AsciiSPrint (Text, sizeof (Text),
        "J250F eMMC initialization failed: %r\r\nCMD=%u IRQ=%08x R1=%08x\r\nSTATE_BEFORE_RESET=%08x\r\n",
        Status, (UINT32)Dev->Host.last_command, Dev->Host.last_interrupt, Dev->Host.last_r1,
        Dev->Host.last_present);
    } else {
      Len = AsciiSPrint (Text, sizeof (Text),
        "eMMC RECOGNIZED: %u sectors (%u MiB)\r\nCID=%08x %08x %08x %08x\r\n"
        "User area read-only, one-bit 400kHz.\r\n",
        Dev->Host.capacity_sectors, Dev->Host.capacity_sectors / 2048,
        Dev->Host.cid[0], Dev->Host.cid[1], Dev->Host.cid[2], Dev->Host.cid[3]);
    }
    SerialPortWrite ((UINT8 *)Text, Len);
    Len = AsciiSPrint (Text, sizeof (Text), "Holding result for 90 seconds.\r\n");
    SerialPortWrite ((UINT8 *)Text, Len);
    for (Second = 0; Second < 90; ++Second) gBS->Stall (1000000);
  }
  if (EFI_ERROR (Status)) {
    // Never install a pretend disk if CMD1/EXT_CSD/rail power failed.
    return Status;
  }

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Dev->Handle,
                  &gEfiBlockIoProtocolGuid, &Dev->BlockIo,
                  &gEfiDevicePathProtocolGuid, &Dev->Path,
                  NULL);
  return Status;
}
