/** @file
  J250F SDCC1 firmware backend for LK-style MMC/SDHCI BlockIo bridge.

  Uses the self-contained polled SDHCI controller audited against Qualcomm LK
  and the supplied Samsung Linux kernel, NOT an imported copy of LK. This file
  replaces the previous EFI_UNSUPPORTED backend with working transfer calls.
  The PM8917 provider remains mandatory: absent real regulator control or a
  verified pre-powered board adapter no eMMC can be safely published.
  SPDX-License-Identifier: GPL-2.0-only
**/
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/SerialPortLib.h>
#include <Library/PrintLib.h>
#include <Library/FrameBufferSerialPortLib.h>
#include "J250fLkPlatform.h"
#include "j250f_emmc.h"
#include "j250f_board.h"
#include "J250fPowerProtocol.h"

STATIC j250f_emmc mHost;
STATIC j250f_board mBoard;
STATIC J250F_EMMC_POWER_PROTOCOL *mPower;

STATIC uint32_t RegRead32 (void *Ctx, uint32_t Phys) {
  (VOID)Ctx;
  return MmioRead32((UINTN)Phys);
}
STATIC void RegWrite32 (void *Ctx, uint32_t Phys, uint32_t V) {
  (VOID)Ctx;
  MmioWrite32((UINTN)Phys,V);
}
STATIC UINTN HostAddress(j250f_region Region, uint32_t Offset) {
  return (UINTN)(Region==J250F_REGION_HC ? J250F_HC_PHYS : J250F_CORE_PHYS) + Offset;
}
STATIC uint8_t R8(void *Ctx,j250f_region Region,uint32_t Offset) {
  (VOID)Ctx;return MmioRead8(HostAddress(Region,Offset));
}
STATIC uint16_t R16(void *Ctx,j250f_region Region,uint32_t Offset) {
  (VOID)Ctx;return MmioRead16(HostAddress(Region,Offset));
}
STATIC uint32_t R32(void *Ctx,j250f_region Region,uint32_t Offset) {
  (VOID)Ctx;return MmioRead32(HostAddress(Region,Offset));
}
STATIC void W8(void *Ctx,j250f_region Region,uint32_t Offset,uint8_t V) {
  (VOID)Ctx;MmioWrite8(HostAddress(Region,Offset),V);
}
STATIC void W16(void *Ctx,j250f_region Region,uint32_t Offset,uint16_t V) {
  (VOID)Ctx;MmioWrite16(HostAddress(Region,Offset),V);
}
STATIC void W32(void *Ctx,j250f_region Region,uint32_t Offset,uint32_t V) {
  (VOID)Ctx;MmioWrite32(HostAddress(Region,Offset),V);
}
STATIC void Barrier(void *Ctx) { (VOID)Ctx; MemoryFence(); }
STATIC void Delay(void *Ctx,uint32_t Us) { (VOID)Ctx; MicroSecondDelay(Us); }
STATIC int Rails(void *Ctx,int On,uint32_t Vdd,uint32_t Vio) {
  (VOID)Ctx;
#ifdef J250F_ALLOW_LK_POWER_HANDOFF
  if (mPower==NULL) {
    // EXPERIMENTAL: LK/ABL has already powered the always-on eMMC rails.
    // Never attempt to turn off rails that firmware has not taken ownership of.
    // Confirm this invariant on the target before enabling the build flag.
    (VOID)Vdd;(VOID)Vio;
    return On ? 0 : -1;
  }
#endif
  if (mPower==NULL || mPower->SetRails==NULL) return -1;
  return EFI_ERROR(mPower->SetRails(mPower,On!=0,Vdd,Vio))?-1:0;
}
STATIC int GpllReady(void *Ctx) {
  (VOID)Ctx;
#ifdef J250F_ALLOW_LK_POWER_HANDOFF
  if (mPower==NULL) {
    // At least verify hardware reports GPLL0 active; still not a PMIC check.
    return (MmioRead32((UINTN)J250F_GCC_PHYS+0x21000u) & (1u<<30)) ? 0 : -1;
  }
#endif
  if (mPower==NULL || mPower->Gpll0Ready==NULL) return -1;
  return EFI_ERROR(mPower->Gpll0Ready(mPower))?-1:0;
}
STATIC int Clk(void *Ctx,uint32_t Hz) {
  (VOID)Ctx;return j250f_gcc_sdcc1_clock(&mBoard,Hz);
}
STATIC int Pins(void *Ctx,int Active) {
  (VOID)Ctx;return j250f_tlmm_sdcc1_pins(&mBoard,Active);
}
STATIC const j250f_ops mOps={ R8,R16,R32,W8,W16,W32,Barrier,Delay,Clk,Rails,Pins };

STATIC EFI_STATUS EFIAPI ReadMmc (VOID *Device, EFI_LBA Lba,
                                  UINTN Count, VOID *Buffer) {
  UINT8 *Dst=(UINT8*)Buffer;
  if (Device!=&mHost || !mHost.card_ready) return EFI_NOT_READY;
  if ((UINT64)Lba > UINT32_MAX || Count>UINT32_MAX) return EFI_INVALID_PARAMETER;
  if (Lba > mHost.capacity_sectors || Count>(UINTN)(mHost.capacity_sectors-(UINT32)Lba))
    return EFI_INVALID_PARAMETER;
  while (Count) {
    UINT32 Chunk=(Count>128u)?128u:(UINT32)Count;
    int Result=j250f_read_blocks(&mHost,(UINT32)Lba,Chunk,Dst);
    if (Result!=J250F_OK) {
      DEBUG((DEBUG_ERROR,"J250F eMMC: read LBA=%Lu count=%u status=%d IRQ=0x%x R1=0x%x\n",
             (UINT64)Lba,Chunk,Result,mHost.last_interrupt,mHost.last_r1));
      return Result==J250F_E_TIMEOUT?EFI_TIMEOUT:EFI_DEVICE_ERROR;
    }
    Count-=Chunk;Lba+=Chunk;Dst+=(UINTN)Chunk*J250F_BLOCK_SIZE;
  }
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI J250fLkBoardOpen(OUT J250F_LK_DISK *Disk) {
  EFI_STATUS Status;
  UINT8 Sector0[J250F_BLOCK_SIZE];
  int Result;
  if (Disk==NULL) return EFI_INVALID_PARAMETER;
  ZeroMem(Disk,sizeof(*Disk));
  Status=gBS->LocateProtocol(&gJ250fEmmcPowerProtocolGuid,NULL,(VOID**)&mPower);
  if (EFI_ERROR(Status)) {
#ifndef J250F_ALLOW_LK_POWER_HANDOFF
    DEBUG((DEBUG_ERROR,"J250F eMMC: PM8917 power provider missing (%r)\n",Status));
    return Status;
#else
    DEBUG((DEBUG_WARN,"J250F eMMC: EXPERIMENTAL LK pre-powered rails mode; no PMIC verification\n"));
    mPower=NULL;
#endif
  }
  if (mPower!=NULL) {
    if (mPower->SetRails==NULL || mPower->Gpll0Ready==NULL) return EFI_UNSUPPORTED;
    Status=mPower->SetRails(mPower,TRUE,J250F_VDD_UV,J250F_VQMMC_UV);
    if (EFI_ERROR(Status)) return Status;
  }
  // The opt-in LK handoff mode only works if original boot firmware kept
  // eMMC VDD=2.9V/VQMMC=1.8V powered; the subsequent card response proves it.
  ZeroMem(&mBoard,sizeof(mBoard));
  mBoard.reg_read32=RegRead32;
  mBoard.reg_write32=RegWrite32;
  mBoard.barrier=Barrier;
  mBoard.delay_us=Delay;
  mBoard.gpll0_power_ready=GpllReady;
  Result=j250f_tlmm_sdcc1_pins(&mBoard,1);
  if (Result) {DEBUG((DEBUG_ERROR,"J250F eMMC: SDC1 TLMM init failed\n"));return EFI_DEVICE_ERROR;}
  Result=j250f_init_host(&mHost,&mOps,NULL);
  if (Result==J250F_OK) Result=j250f_init_card(&mHost);
  if (Result!=J250F_OK) {
    CHAR8 Text[384];
    UINTN Len;
    Len=AsciiSPrint(Text,sizeof(Text),
      "Card init result=%d CMD=%u IRQ=%08x R1=%08x\r\nSTATE_BEFORE_RESET=%08x\r\nVENDOR=%08x DLL=%08x CLK=%04x\r\n",
      Result,(UINT32)mHost.last_command,mHost.last_interrupt,mHost.last_r1,mHost.last_present,
      mHost.last_vendor,mHost.last_dll,(UINT32)mHost.last_clock);
    SerialPortWrite((UINT8*)Text,Len);
    DEBUG((DEBUG_ERROR,"J250F eMMC: CMD init failed=%d IRQ=0x%x R1=0x%x\n",
      Result,mHost.last_interrupt,mHost.last_r1));
    return Result==J250F_E_TIMEOUT?EFI_TIMEOUT:EFI_DEVICE_ERROR;
  }
  j250f_allow_writes(&mHost,0);
  if (!mHost.capacity_sectors) return EFI_DEVICE_ERROR;
  {
    CHAR8 Text[256];
    UINTN Len=AsciiSPrint(Text,sizeof(Text),
      "eMMC IDENTIFIED: %u sectors (%u MiB)\r\nCID=%08x %08x %08x %08x\r\nReading sector 0...\r\n",
      mHost.capacity_sectors,mHost.capacity_sectors/2048,
      mHost.cid[0],mHost.cid[1],mHost.cid[2],mHost.cid[3]);
    SerialPortWrite((UINT8*)Text,Len);
  }
  // Prove a physical read before exposing a UEFI disk/partition map.
  Status=ReadMmc(&mHost,0,1,Sector0);
  if (EFI_ERROR(Status)) {
    DEBUG((DEBUG_ERROR,"J250F eMMC: LBA0 read failed: %r\n",Status));
    return Status;
  }
  DEBUG((DEBUG_INFO,"J250F eMMC: identified %u x 512; sector0=%02x %02x GPT/MBR signature=%02x%02x\n",
    mHost.capacity_sectors, Sector0[0],Sector0[1],Sector0[511],Sector0[510]));
  Disk->Device=&mHost;
  Disk->NumberOfBlocks=mHost.capacity_sectors;
  Disk->BlockSize=J250F_BLOCK_SIZE;
  Disk->IoAlignment=1; // polled programmed-I/O, no DMA alignment restriction
  Disk->ReadBlocks=ReadMmc;
  return EFI_SUCCESS;
}
