// SPDX-License-Identifier: BSD-2-Clause-Patent
// J250F SDCC1: single-bit, low-speed PIO eMMC user-area read-only driver.
// Register semantics reference Samsung sdhci-msm.c and SDHCI definitions.
// No Linux code is linked. No DMA, CMD6, erase, or write commands supported.
#include <Uefi.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/FrameBufferSerialPortLib.h>
#include <Library/J250fSdccPlatformLib.h>

#define HC 0x07824900U
#define CORE 0x07824000U
#define ERR 0xffff8000U
#define R1_ERRORS 0xfdffa080U
STATIC EFI_BLOCK_IO_MEDIA mMedia;
STATIC EFI_BLOCK_IO_PROTOCOL mIo;
STATIC EFI_HANDLE mHandle;
STATIC BOOLEAN mReady;
STATIC BOOLEAN mBusy;
STATIC UINT32 mLastStatus;
STATIC UINT32 mFailureState;
STATIC CONST CHAR8 *mStage = "entry";
STATIC UINT8 mLastCommand;
STATIC UINT32 mCidRaw[4];

// Finish already-powered BUS_ON / IO_HIGH handshakes inherited from aboot.
// Do not acknowledge requests that would require PMIC or pin changes.
// Startup-only cleanup from Samsung probe: these are stale status bits,
// not requests for new regulator transitions. Runtime handling remains strict.
STATIC EFI_STATUS ClearStartupPower (VOID)
{
  UINT32 Pending = MmioRead32 (CORE + 0xdc) & 15U;
  UINT32 Ack = MmioRead32 (CORE + 0xe8);
  UINTN I;
  for (I = 0; I < 10 && Pending != 0; ++I) {
    MmioWrite32 (CORE + 0xe4, Pending);
    if ((MmioRead32 (CORE + 0xdc) & Pending) == 0) break;
    gBS->Stall (10);
  }
  if (I == 10) return EFI_TIMEOUT;
  if ((Pending & 3U) != 0) Ack |= 1U;
  if ((Pending & 12U) != 0) Ack |= 4U;
  MmioWrite32 (CORE + 0xe8, Ack);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS ServicePower (VOID)
{
  UINT32 Pending = MmioRead32 (CORE + 0xdc) & 15U;
  UINT32 Ack = 0;
  UINTN I;
  if (Pending == 0) return EFI_SUCCESS;
  if ((Pending & 5U) != 0 || (MmioRead8 (HC + 0x29) & 1) == 0)
    return EFI_UNSUPPORTED;
  if ((Pending & 2U) != 0) Ack |= 1U;
  if ((Pending & 8U) != 0) Ack |= 4U;
  // Samsung probe clears pending status before acknowledging it.
  for (I = 0; I < 10; ++I) {
    MmioWrite32 (CORE + 0xe4, Pending);
    if ((MmioRead32 (CORE + 0xdc) & Pending) == 0) break;
    gBS->Stall (10);
  }
  if (I == 10) return EFI_TIMEOUT;
  MmioWrite32 (CORE + 0xe8, MmioRead32 (CORE + 0xe8) | Ack);
  return EFI_SUCCESS;
}
STATIC struct {
  MEMMAP_DEVICE_PATH Controller;
  EFI_DEVICE_PATH_PROTOCOL End;
} mPath = {
  {{HARDWARE_DEVICE_PATH, HW_MEMMAP_DP, {sizeof (MEMMAP_DEVICE_PATH), 0}},
   EfiMemoryMappedIO, HC, HC + 0x4ff},
  {END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE,
   {sizeof (EFI_DEVICE_PATH_PROTOCOL), 0}}
};

STATIC EFI_STATUS WaitState (UINT32 Mask, UINT32 Value)
{
  UINTN I;
  for (I = 0; I < 100000; ++I) {
    if ((MmioRead32 (HC + 0x24) & Mask) == Value) return EFI_SUCCESS;
    gBS->Stall (10);
  }
  return EFI_TIMEOUT;
}

STATIC EFI_STATUS ResetLines (UINT8 Bits)
{
  UINTN I;
  MmioWrite8 (HC + 0x2f, Bits);
  for (I = 0; I < 10000; ++I) {
    if ((MmioRead8 (HC + 0x2f) & Bits) == 0) return EFI_SUCCESS;
    gBS->Stall (10);
  }
  return EFI_TIMEOUT;
}

STATIC EFI_STATUS WaitStatus (UINT32 Bits)
{
  UINTN I;
  for (I = 0; I < 100000; ++I) {
    EFI_STATUS PowerStatus = ServicePower ();
    if (EFI_ERROR (PowerStatus)) return PowerStatus;
    mLastStatus = MmioRead32 (HC + 0x30);
    if ((mLastStatus & ERR) != 0) return EFI_DEVICE_ERROR;
    if ((mLastStatus & Bits) == Bits) return EFI_SUCCESS;
    gBS->Stall (10);
  }
  return EFI_TIMEOUT;
}

// Central command allowlist. Only initialization, status and user-area reads.
STATIC EFI_STATUS Command (UINT8 Index, UINT32 Arg, UINT32 *Response, UINT8 *Data)
{
  EFI_STATUS S;
  UINT16 Flags;
  UINTN I;
  UINT32 Word;
  switch (Index) {
    case 0: Flags = 0; break;
    case 1: Flags = 2; break;
    case 2: case 9: Flags = 9; break;
    case 3: case 13: case 16: Flags = 0x1a; break;
    case 7: Flags = 0x1b; break;
    case 8: case 17: Flags = 0x3a; break;
    default: return EFI_ACCESS_DENIED;
  }
  if (((Index == 8 || Index == 17) && Data == NULL) ||
      (Data != NULL && Index != 8 && Index != 17)) return EFI_INVALID_PARAMETER;
  mLastCommand = Index;
  mStage = "command idle";
  S = WaitState (3, 0);
  if (EFI_ERROR (S)) goto Fail;
  MmioWrite32 (HC + 0x30, 0xffffffffU);
  // Clear transfer mode even for no-data commands; do not inherit aboot DMA.
  MmioWrite16 (HC + 0x0c, Data != NULL ? 0x12 : 0);
  if (Data != NULL) {
    MmioWrite16 (HC + 0x04, 512);
    MmioWrite16 (HC + 0x06, 1);
  }
  MmioWrite32 (HC + 0x08, Arg);
  MmioWrite16 (HC + 0x0e, ((UINT16)Index << 8) | Flags);
  mStage = "command completion";
  S = WaitStatus (1);
  if (EFI_ERROR (S)) goto Fail;
  Word = MmioRead32 (HC + 0x10);
  if (Response != NULL) *Response = Word;
  if (Index == 2) {
    for (I = 0; I < 4; ++I) mCidRaw[I] = MmioRead32 (HC + 0x10 + I * 4);
  }
  MmioWrite32 (HC + 0x30, 1);
  if (Index != 0 && Index != 1 && Index != 2 && Index != 9 &&
      (Word & R1_ERRORS) != 0) { S = EFI_DEVICE_ERROR; goto Fail; }
  if (Data != NULL) {
    mStage = "PIO buffer ready";
    S = WaitStatus (0x20);
    if (EFI_ERROR (S)) goto Fail;
    for (I = 0; I < 512; I += 4) {
      Word = MmioRead32 (HC + 0x20);
      CopyMem (Data + I, &Word, 4);
    }
    MmioWrite32 (HC + 0x30, 0x20);
    mStage = "PIO transfer complete";
    S = WaitStatus (2);
    if (EFI_ERROR (S)) goto Fail;
    MmioWrite32 (HC + 0x30, 2);
  } else if (Index == 7) {
    S = WaitState (2, 0);
    if (EFI_ERROR (S)) goto Fail;
  }
  return EFI_SUCCESS;
Fail:
  mLastStatus = MmioRead32 (HC + 0x30);
  mFailureState = MmioRead32 (HC + 0x24);
  DEBUG ((DEBUG_ERROR, "eMMC CMD%u failed %r status=%08x state=%08x\n",
          Index, S, mLastStatus, MmioRead32 (HC + 0x24)));
  // No automatic card reinitialization after an I/O failure.
  mReady = FALSE;
  ResetLines (6);
  return S;
}

STATIC EFI_STATUS InitializeCard (VOID)
{
  EFI_STATUS S;
  UINT32 Mode, Vendor, Ocr, Count;
  UINT16 Clock;
  UINT8 Ext[512];
  UINTN I;
  mReady = FALSE;
  mLastStatus = mFailureState = 0;
  mLastCommand = 0xff;
  mStage = "inherited power/version";
  // Require the bootloader's powered peripheral; PMIC/GCC enablement is
  // platform-specific and is not guessed here.
  if ((MmioRead8 (HC + 0x29) & 1) == 0 ||
      (MmioRead16 (HC + 0xfe) & 0xff) < 2 ||
      MmioRead16 (HC + 0xfe) == 0xffff) return EFI_UNSUPPORTED;
  // Stop card clock before replacing inherited GCC source and pad settings.
  MmioWrite16 (HC + 0x2c, 0);
  mStage = "J250F GCC 400kHz source";
  S = J250fSdccSetIdentificationClock ();
  if (EFI_ERROR (S)) return S;
  mStage = "J250F SDCC1 active pads";
  S = J250fSdccSetActivePads ();
  if (EFI_ERROR (S)) return S;
  Mode = MmioRead32 (CORE + 0x78);
  MmioWrite32 (CORE + 0x78, Mode | 0x2001);
  // Bit13 is a variant-specific clock-reset workaround, not enable status.
  if ((MmioRead32 (CORE + 0x78) & 1) == 0) return EFI_DEVICE_ERROR;
  mStage = "startup Qualcomm power status cleanup";
  S = ClearStartupPower ();
  if (EFI_ERROR (S)) return S;
  mStage = "CMD/DAT line reset";
  S = ResetLines (6);
  if (EFI_ERROR (S)) return S;
  MmioWrite16 (HC + 0x2c, 0);
  Vendor = MmioRead32 (HC + 0x10c);
  Vendor = (Vendor & ~((3U << 8) | (1U << 18) | (7U << 19) | 2U)) | (2U << 8);
  MmioWrite32 (HC + 0x10c, Vendor);
  MmioWrite16 (HC + 0x3e, MmioRead16 (HC + 0x3e) & (UINT16)~0x80c7U);
  MmioWrite8 (HC + 0x28, MmioRead8 (HC + 0x28) & (UINT8)~0x26U);
  // GCC now supplies an explicit 400kHz; SDHCI divider zero means /1.
  Clock = 1;
  mStage = "internal clock stable";
  MmioWrite16 (HC + 0x2c, Clock);
  for (I = 0; I < 10000; ++I) {
    if ((MmioRead16 (HC + 0x2c) & 2) != 0) break;
    gBS->Stall (10);
  }
  if (I == 10000) return EFI_TIMEOUT;
  MmioWrite16 (HC + 0x2c, Clock | 4);
  MmioWrite8 (HC + 0x2e, 0x0e);
  // Poll status only: suppress host IRQ signals, enable command/data status.
  MmioWrite32 (HC + 0x38, 0);
  MmioWrite32 (HC + 0x34, 0x03ff8033U);
  gBS->Stall (2000);
  S = Command (0, 0, NULL, NULL);
  if (EFI_ERROR (S)) return S;
  gBS->Stall (2000);
  S = Command (1, 0, &Ocr, NULL);
  if (EFI_ERROR (S) || (Ocr & 0x00ff8000U) == 0) return EFI_DEVICE_ERROR;
  Ocr = (Ocr & 0x00ff8000U) | 0x40000000U;
  for (I = 0; I < 1000; ++I) {
    S = Command (1, Ocr, &Count, NULL);
    if (EFI_ERROR (S)) return S;
    if ((Count & 0x80000000U) != 0) break;
    gBS->Stall (1000);
  }
  if (I == 1000) return EFI_TIMEOUT;
  if ((Count & 0x40000000U) == 0) return EFI_UNSUPPORTED;
  S = Command (2, 0, NULL, NULL);
  if (EFI_ERROR (S)) return S;
  S = Command (3, 1U << 16, NULL, NULL);
  if (EFI_ERROR (S)) return S;
  S = Command (9, 1U << 16, NULL, NULL);
  if (EFI_ERROR (S)) return S;
  S = Command (7, 1U << 16, NULL, NULL);
  if (EFI_ERROR (S)) return S;
  S = Command (8, 0, NULL, Ext);
  if (EFI_ERROR (S)) return S;
  // Never expose a hardware boot partition or RPMB selected by prior firmware.
  if ((Ext[179] & 7) != 0) return EFI_UNSUPPORTED;
  Count = (UINT32)Ext[212] | ((UINT32)Ext[213] << 8) |
          ((UINT32)Ext[214] << 16) | ((UINT32)Ext[215] << 24);
  if (Count == 0) return EFI_UNSUPPORTED;
  ZeroMem (&mMedia, sizeof (mMedia));
  mMedia.MediaId = 1;
  mMedia.MediaPresent = TRUE;
  mMedia.ReadOnly = TRUE;
  mMedia.BlockSize = 512;
  mMedia.LastBlock = Count - 1;
  mMedia.IoAlign = 1;
  mReady = TRUE;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI Reset (EFI_BLOCK_IO_PROTOCOL *This, BOOLEAN Extended)
{
  if (This != &mIo) return EFI_INVALID_PARAMETER;
  // Recovery requires an explicit reboot, rather than silently resetting card.
  return mReady ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}
STATIC EFI_STATUS EFIAPI Read (EFI_BLOCK_IO_PROTOCOL *This, UINT32 Id,
                              EFI_LBA Lba, UINTN Size, VOID *Buffer)
{
  EFI_STATUS S = EFI_SUCCESS;
  UINTN I;
  EFI_TPL OldTpl;
  if (This != &mIo) return EFI_INVALID_PARAMETER;
  if (Id != mMedia.MediaId) return EFI_MEDIA_CHANGED;
  if (!mReady) return EFI_DEVICE_ERROR;
  if (Size == 0) return EFI_SUCCESS;
  if (Buffer == NULL) return EFI_INVALID_PARAMETER;
  if (Size % 512 != 0) return EFI_BAD_BUFFER_SIZE;
  if (Lba > mMedia.LastBlock || Size / 512 - 1 > mMedia.LastBlock - Lba)
    return EFI_INVALID_PARAMETER;
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (mBusy) { gBS->RestoreTPL (OldTpl); return EFI_NOT_READY; }
  mBusy = TRUE;
  for (I = 0; I < Size; I += 512) {
    S = Command (17, (UINT32)Lba++, NULL, (UINT8 *)Buffer + I);
    if (EFI_ERROR (S)) break;
  }
  mBusy = FALSE;
  gBS->RestoreTPL (OldTpl);
  return S;
}
STATIC EFI_STATUS EFIAPI Write (EFI_BLOCK_IO_PROTOCOL *This, UINT32 Id,
                               EFI_LBA Lba, UINTN Size, VOID *Buffer)
{
  return EFI_WRITE_PROTECTED;
}
STATIC EFI_STATUS EFIAPI Flush (EFI_BLOCK_IO_PROTOCOL *This)
{
  return This == &mIo ? EFI_SUCCESS : EFI_INVALID_PARAMETER;
}
EFI_STATUS EFIAPI EmmcEntry (EFI_HANDLE Image, EFI_SYSTEM_TABLE *System)
{
  EFI_STATUS S;
  CHAR8 Text[512];
  UINTN Length, Second;
  FbConStartScreen ();
  Length = AsciiSPrint (Text, sizeof (Text), "\r\nJ250F: initializing eMMC...\r\n");
  SerialPortWrite ((UINT8 *)Text, Length);
  S = InitializeCard ();
  if (EFI_ERROR (S)) {
    Length = AsciiSPrint (Text, sizeof (Text),
      "eMMC initialization failed: %r\r\n"
      "STAGE=%a CMD=%u\r\n"
      "FAIL_INT_STATUS=%08x FAIL_PRESENT_STATE=%08x\r\n"
      "NOW_INT_STATUS=%08x NOW_PRESENT_STATE=%08x\r\n"
      "No Block IO installed. Holding result for 90 seconds.\r\n",
      S, mStage, (UINT32)mLastCommand, mLastStatus, mFailureState,
      MmioRead32 (HC + 0x30), MmioRead32 (HC + 0x24));
  } else {
    mIo.Revision = EFI_BLOCK_IO_PROTOCOL_REVISION;
    mIo.Media = &mMedia;
    mIo.Reset = Reset;
    mIo.ReadBlocks = Read;
    mIo.WriteBlocks = Write;
    mIo.FlushBlocks = Flush;
    S = gBS->InstallMultipleProtocolInterfaces (&mHandle,
      &gEfiBlockIoProtocolGuid, &mIo, &gEfiDevicePathProtocolGuid, &mPath, NULL);
    Length = AsciiSPrint (Text, sizeof (Text),
      "eMMC RECOGNIZED: %Lu sectors, %Lu MiB.\r\n"
      "CID_RAW=%08x %08x %08x %08x\r\n"
      "Block IO installation: %r\r\n"
      "Holding result for 90 seconds.\r\n", mMedia.LastBlock + 1,
      (mMedia.LastBlock + 1) / 2048, mCidRaw[3], mCidRaw[2], mCidRaw[1], mCidRaw[0], S);
  }
  FbConStartScreen ();
  SerialPortWrite ((UINT8 *)Text, Length);
  // Hold only after initialization/registration, never inside a command timeout.
  for (Second = 0; Second < 90; ++Second) gBS->Stall (1000000);
  return S;
}
