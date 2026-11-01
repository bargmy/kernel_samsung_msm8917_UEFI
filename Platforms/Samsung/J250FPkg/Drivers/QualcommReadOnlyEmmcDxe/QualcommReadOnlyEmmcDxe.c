// SPDX-License-Identifier: BSD-2-Clause-Patent
// Experimental ARM32 MSM8917 SDCC1 read-only eMMC DXE driver.
// No media writes, erase/trim, CMD6 partition switches, RPMB, or DMA.
// No generic MMC PassThru is exposed. NOT VERIFIED ON HARDWARE.
//
// Hardware: J250F DTS; GCC RCG2: gcc-msm8917.c; host: sdhci-msm.c.
// Command sequencing is independently written following JEDEC/MMC SDHCI.
// GPL U-Boot code was used for hardware reference, not copied.

#include <Uefi.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define SDCC1_HOST            0x07824900U
#define SDCC1_CORE            0x07824000U
#define SDCC1_GCC_RCG_CMD     0x01842004U  // gcc@0x1800000 + SDCC1_APPS_SRC 0x42004

#define S_BLOCKSIZE           0x04U
#define S_BLOCKCOUNT          0x06U
#define S_ARGUMENT            0x08U
#define S_XFERMODE            0x0CU
#define S_COMMAND             0x0EU
#define S_RESPONSE            0x10U
#define S_BUFFER              0x20U
#define S_PRESENT             0x24U
#define S_HOST_CONTROL        0x28U
#define S_POWER               0x29U
#define S_CLOCK               0x2CU
#define S_INT_STATUS          0x30U
#define S_INT_ENABLE          0x34U
#define S_SIGNAL_ENABLE       0x38U
#define S_CAPABILITIES        0x40U
#define S_VERSION             0xFEU
#define C_MCI_VERSION         0x50U
#define C_HC_MODE             0x78U

#define INT_CMD_DONE          0x00000001U
#define INT_XFER_DONE         0x00000002U
#define INT_READ_READY        0x00000020U
#define INT_ERRORS            0xFFFF8000U
#define INT_REQUIRED          (INT_CMD_DONE | INT_XFER_DONE | INT_READ_READY | INT_ERRORS)
#define OCR_READY             0x80000000U
#define OCR_HCS               0x40000000U
#define OCR_VALID_VOLTAGES    0x00FF8000U
#define MAX_READ_BLOCKS       1024U
#define SDCC1_RCA             1U

STATIC EFI_BOOT_SERVICES *mBs;
STATIC EFI_BLOCK_IO_PROTOCOL mBlockIo;
STATIC EFI_BLOCK_IO_MEDIA mMedia;
STATIC EFI_HANDLE mDiskHandle = NULL;
STATIC UINT32 mSectorCount = 0;
STATIC BOOLEAN mReady = FALSE;

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH Vendor;
  EFI_DEVICE_PATH_PROTOCOL End;
} J250F_DISK_DEVICE_PATH;
#pragma pack()
STATIC J250F_DISK_DEVICE_PATH mPath = {
  { { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { sizeof (VENDOR_DEVICE_PATH), 0 } },
    { 0x522a82d2, 0xe6d7, 0x48ea, { 0x98, 0xeb, 0x42, 0x14, 0xda, 0xef, 0x5b, 0x20 } } },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC VOID Log (CONST CHAR8 *Message) {
  SerialPortWrite ((CONST UINT8 *)Message, AsciiStrLen (Message));
}
STATIC VOID DelayUs (UINTN Microseconds) {
  if (mBs != NULL) {
    mBs->Stall (Microseconds);
  }
}
STATIC VOID ShowResultPause (VOID) {
  UINTN Second;
  Log ("Read-only SDCC1 test: photograph results. Boot continues in 90s.\r\n");
  for (Second = 0; mBs != NULL && Second < 90; ++Second) {
    mBs->Stall (1000000);
  }
}

STATIC EFI_STATUS WaitIdle (BOOLEAN NeedDataIdle) {
  UINT32 Mask = NeedDataIdle ? 3U : 1U;
  UINTN Retry;
  for (Retry = 0; Retry < 2000; ++Retry) {
    if ((MmioRead32 (SDCC1_HOST + S_PRESENT) & Mask) == 0) return EFI_SUCCESS;
    DelayUs (1000);
  }
  return EFI_TIMEOUT;
}

// Hardware command allowlist. Only card identification, status,
// EXT_CSD read and single-block reads can ever reach the host.
// CMD6 (switch), CMD24/25 (writes), erase and boot partition ops
// cannot be expressed through this dispatcher.
STATIC EFI_STATUS Send (
  IN UINT32 Index, IN UINT32 Argument,
  IN BOOLEAN DataRead,
  OUT UINT32 *Response,
  OUT UINT8 *Data512 OPTIONAL
  )
{
  UINT16 Flags;
  UINT32 Status;
  UINTN Poll, Word;
  EFI_STATUS E;
  UINT32 Value;

  if (DataRead) {
    if ((Index != 8 && Index != 17) || Data512 == NULL) return EFI_ACCESS_DENIED;
  } else {
    if (Index != 0 && Index != 1 && Index != 2 &&
        Index != 3 && Index != 7 && Index != 9 && Index != 13)
      return EFI_ACCESS_DENIED;
  }
  E = WaitIdle (TRUE);
  if (EFI_ERROR (E)) return E;

  // SDHCI response types: NONE=0, R2=1, R1/R3=2, R1b=3.
  if (Index == 0) Flags = 0;
  else if (Index == 1) Flags = 2;
  else if (Index == 2 || Index == 9) Flags = 1 | 8;
  else if (Index == 7) Flags = 3 | 8 | 16;
  else Flags = 2 | 8 | 16;
  if (DataRead) Flags |= 32;

  MmioWrite32 (SDCC1_HOST + S_INT_STATUS, 0xFFFFFFFFU); // W1C
  if (DataRead) {
    MmioWrite16 (SDCC1_HOST + S_BLOCKSIZE, 512U);
    MmioWrite16 (SDCC1_HOST + S_BLOCKCOUNT, 1U);
    MmioWrite16 (SDCC1_HOST + S_XFERMODE, 0x0012U); // 1-block, PIO READ
  } else {
    MmioWrite16 (SDCC1_HOST + S_XFERMODE, 0);
  }
  MmioWrite32 (SDCC1_HOST + S_ARGUMENT, Argument);
  MmioWrite16 (SDCC1_HOST + S_COMMAND, (UINT16)((Index << 8) | Flags));

  for (Poll = 0; Poll < 2000; ++Poll) {
    Status = MmioRead32 (SDCC1_HOST + S_INT_STATUS);
    if ((Status & INT_ERRORS) != 0) return EFI_DEVICE_ERROR;
    if ((Status & INT_CMD_DONE) != 0) break;
    DelayUs (1000);
  }
  if (Poll == 2000) return EFI_TIMEOUT;
  if (Response != NULL) *Response = MmioRead32 (SDCC1_HOST + S_RESPONSE);
  MmioWrite32 (SDCC1_HOST + S_INT_STATUS, INT_CMD_DONE);

  if (DataRead) {
    for (Poll = 0; Poll < 2000; ++Poll) {
      Status = MmioRead32 (SDCC1_HOST + S_INT_STATUS);
      if ((Status & INT_ERRORS) != 0) return EFI_DEVICE_ERROR;
      if ((Status & INT_READ_READY) != 0) break;
      DelayUs (1000);
    }
    if (Poll == 2000) return EFI_TIMEOUT;
    for (Word = 0; Word < 128; ++Word) {
      Value = MmioRead32 (SDCC1_HOST + S_BUFFER);
      Data512[Word * 4 + 0] = (UINT8)(Value);
      Data512[Word * 4 + 1] = (UINT8)(Value >> 8);
      Data512[Word * 4 + 2] = (UINT8)(Value >> 16);
      Data512[Word * 4 + 3] = (UINT8)(Value >> 24);
    }
    MmioWrite32 (SDCC1_HOST + S_INT_STATUS, INT_READ_READY);
    for (Poll = 0; Poll < 2000; ++Poll) {
      Status = MmioRead32 (SDCC1_HOST + S_INT_STATUS);
      if ((Status & INT_ERRORS) != 0) return EFI_DEVICE_ERROR;
      if ((Status & INT_XFER_DONE) != 0) break;
      DelayUs (1000);
    }
    if (Poll == 2000) return EFI_TIMEOUT;
    MmioWrite32 (SDCC1_HOST + S_INT_STATUS, INT_XFER_DONE);
  }
  return EFI_SUCCESS;
}

// MSM8917 GCC SDCC1 application RCG2 400kHz, from gcc-msm8917.c:
// F(400000, P_XO, pre_div=12, m=1, n=4).
// Writes only the SDCC1 GCC root clock generator; no PMIC, voltage,
// bulk-reset, flash or non-SDCC1 clock registers.
STATIC EFI_STATUS SetQualcomm400kHzClock (VOID) {
  UINT32 Cfg, Cmd;
  UINT16 HostClock;
  UINTN Poll;
  if ((MmioRead16 (SDCC1_HOST + S_CLOCK) & 3U) != 3U) {
    Log ("STOP: internal SDHCI clock not stable.\r\n");
    return EFI_UNSUPPORTED;
  }
  // Stop card clock before changing the GCC parent configuration.
  HostClock = MmioRead16 (SDCC1_HOST + S_CLOCK);
  MmioWrite16 (SDCC1_HOST + S_CLOCK, (UINT16)(HostClock & ~4U));

  // GCC RCG2 M=1; N=~(4-1); D=~4, low 8 bits only.
  MmioWrite32 (SDCC1_GCC_RCG_CMD + 8U,
    (MmioRead32 (SDCC1_GCC_RCG_CMD + 8U) & ~0xFFU) | 1U);
  MmioWrite32 (SDCC1_GCC_RCG_CMD + 12U,
    (MmioRead32 (SDCC1_GCC_RCG_CMD + 12U) & ~0xFFU) | 0xFCU);
  MmioWrite32 (SDCC1_GCC_RCG_CMD + 16U,
    (MmioRead32 (SDCC1_GCC_RCG_CMD + 16U) & ~0xFFU) | 0xFBU);
  Cfg = MmioRead32 (SDCC1_GCC_RCG_CMD + 4U);
  Cfg &= ~(0x3FFFU); // clear CFG[12:13] MODE, [8:10] SRC, [0:4] HID
  Cfg |= 23U | (2U << 12); // XO source 0, HID=2*12-1, dual edge M/N
  MmioWrite32 (SDCC1_GCC_RCG_CMD + 4U, Cfg);
  Cmd = MmioRead32 (SDCC1_GCC_RCG_CMD);
  MmioWrite32 (SDCC1_GCC_RCG_CMD, Cmd | 1U);
  for (Poll = 0; Poll < 2000; ++Poll) {
    if ((MmioRead32 (SDCC1_GCC_RCG_CMD) & 1U) == 0) break;
    DelayUs (1000);
  }
  if (Poll == 2000) return EFI_TIMEOUT;

  // SDCC1 apps clock is now specified as 400kHz; select no host division.
  // Keep SDHCI internal oscillator on and enable card clock.
  MmioWrite16 (SDCC1_HOST + S_CLOCK, 1U);
  for (Poll = 0; Poll < 2000; ++Poll) {
    if ((MmioRead16 (SDCC1_HOST + S_CLOCK) & 2U) != 0) break;
    DelayUs (1000);
  }
  if (Poll == 2000) return EFI_TIMEOUT;
  MmioWrite16 (SDCC1_HOST + S_CLOCK, 7U);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS InitializeCard (VOID) {
  EFI_STATUS E;
  UINT16 Version;
  UINT32 CoreVersion, Ocr, OriginalOcr;
  UINTN I;
  UINT8 ExtCsd[512];
  CHAR8 Line[256];
  UINT8 HostCtrl;

  Version = MmioRead16 (SDCC1_HOST + S_VERSION);
  CoreVersion = MmioRead32 (SDCC1_CORE + C_MCI_VERSION);
  if (Version == 0 || Version == 0xFFFF ||
      ((CoreVersion >> 28) & 0xFU) != 1U ||
      (MmioRead8 (SDCC1_HOST + S_POWER) & 1U) == 0 ||
      (MmioRead32 (SDCC1_CORE + C_HC_MODE) & 1U) == 0) {
    Log ("STOP: SDCC1 host core not in expected Qualcomm v4 HC mode.\r\n");
    return EFI_UNSUPPORTED;
  }
  AsciiSPrint (Line, sizeof (Line), "HOST_VERSION=%04x CORE=%08x\r\n",
               (UINT32)Version, CoreVersion);
  Log (Line);

  // MMC card returns to 1-bit mode after CMD0; host must match.
  HostCtrl = MmioRead8 (SDCC1_HOST + S_HOST_CONTROL);
  HostCtrl &= (UINT8)~(0x22U); // no 4-/8-bit bus; do not touch voltage/PMIC
  MmioWrite8 (SDCC1_HOST + S_HOST_CONTROL, HostCtrl);
  MmioWrite32 (SDCC1_HOST + S_SIGNAL_ENABLE, 0);
  MmioWrite32 (SDCC1_HOST + S_INT_ENABLE, INT_REQUIRED);
  E = SetQualcomm400kHzClock ();
  if (EFI_ERROR (E)) { Log ("STOP: SDCC1 GCC 400k clock setup failed.\r\n"); return E; }
  E = Send (0, 0, FALSE, NULL, NULL);
  if (EFI_ERROR (E)) { Log ("STOP: CMD0 failed.\r\n"); return E; }
  DelayUs (2000);

  E = Send (1, 0, FALSE, &Ocr, NULL);
  if (EFI_ERROR (E) || (Ocr & OCR_VALID_VOLTAGES) == 0) {
    Log ("STOP: CMD1 voltage query failed.\r\n"); return EFI_DEVICE_ERROR;
  }
  OriginalOcr = Ocr;
  for (I = 0; I < 100; ++I) {
    E = Send (1, (OriginalOcr & OCR_VALID_VOLTAGES) | OCR_HCS, FALSE, &Ocr, NULL);
    if (EFI_ERROR (E)) return E;
    if ((Ocr & OCR_READY) != 0) break;
    DelayUs (10000);
  }
  if ((Ocr & (OCR_READY | OCR_HCS)) != (OCR_READY | OCR_HCS)) {
    Log ("STOP: no ready, high-capacity eMMC detected.\r\n");
    return EFI_UNSUPPORTED;
  }
  E = Send (2, 0, FALSE, NULL, NULL); // CID
  if (EFI_ERROR (E)) return E;
  E = Send (3, SDCC1_RCA << 16, FALSE, NULL, NULL);
  if (EFI_ERROR (E)) return E;
  E = Send (9, SDCC1_RCA << 16, FALSE, NULL, NULL);
  if (EFI_ERROR (E)) return E;
  E = Send (7, SDCC1_RCA << 16, FALSE, NULL, NULL);
  if (EFI_ERROR (E)) return E;
  E = Send (13, SDCC1_RCA << 16, FALSE, NULL, NULL);
  if (EFI_ERROR (E)) return E;
  E = Send (8, 0, TRUE, NULL, ExtCsd); // read-only EXT_CSD
  if (EFI_ERROR (E)) { Log ("STOP: cannot read EXT_CSD.\r\n"); return E; }
  mSectorCount = (UINT32)ExtCsd[212] | ((UINT32)ExtCsd[213] << 8) |
                 ((UINT32)ExtCsd[214] << 16) | ((UINT32)ExtCsd[215] << 24);
  if (mSectorCount < 0x200000U || mSectorCount > 0x10000000U) {
    Log ("STOP: implausible eMMC sector count.\r\n"); return EFI_DEVICE_ERROR;
  }
  AsciiSPrint (Line, sizeof (Line),
    "eMMC IDENTIFIED: OCR=%08x sectors=%u (%u MiB)\r\n",
    Ocr, mSectorCount, mSectorCount / 2048U);
  Log (Line);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS ReadOne (UINT32 Lba, OUT UINT8 *Out) {
  if (!mReady || Lba >= mSectorCount) return EFI_INVALID_PARAMETER;
  return Send (17, Lba, TRUE, NULL, Out);
}
STATIC EFI_STATUS EFIAPI Reset (EFI_BLOCK_IO_PROTOCOL *This, BOOLEAN ExtendedVerification) {
  // UEFI media-layer reset is a no-op. NEVER reset/repartition the card here.
  return mReady ? EFI_SUCCESS : EFI_NO_MEDIA;
}
STATIC EFI_STATUS EFIAPI Read (
  EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId,
  EFI_LBA Lba, UINTN BufferSize, VOID *Buffer
  )
{
  UINTN N, I;
  UINT8 *Out = (UINT8 *)Buffer;
  EFI_STATUS E;
  if (!mReady || !mMedia.MediaPresent) return EFI_NO_MEDIA;
  if (MediaId != mMedia.MediaId) return EFI_MEDIA_CHANGED;
  if (BufferSize == 0) return EFI_SUCCESS;
  if (Buffer == NULL || BufferSize % 512U != 0) return EFI_BAD_BUFFER_SIZE;
  N = BufferSize / 512U;
  if (N > MAX_READ_BLOCKS || Lba >= mSectorCount ||
      N > ((EFI_LBA)mSectorCount - Lba)) return EFI_INVALID_PARAMETER;
  for (I = 0; I < N; ++I) {
    E = ReadOne ((UINT32)Lba + (UINT32)I, Out + I * 512U);
    if (EFI_ERROR (E)) return E;
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Write (
  EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId,
  EFI_LBA Lba, UINTN BufferSize, VOID *Buffer
  )
{
  // This protocol is always read-only, independent of Media.ReadOnly.
  return EFI_WRITE_PROTECTED;
}
STATIC EFI_STATUS EFIAPI Flush (EFI_BLOCK_IO_PROTOCOL *This) {
  return EFI_SUCCESS; // no writes/cache
}

EFI_STATUS EFIAPI QualcommReadOnlyEmmcEntry (
  IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  EFI_STATUS E;
  UINT8 Mbr[512], Gpt[512];
  CHAR8 Line[220];

  mBs = SystemTable->BootServices;
  Log (
    "\r\n=== J250F READ-ONLY eMMC DXE v1 ===\r\n"
    "SDCC1 Qualcomm v4, low-speed PIO. NO sector/erase writes.\r\n"
    "Initializing host/card...\r\n"
    );
  E = InitializeCard ();
  if (EFI_ERROR (E)) goto Fail;
  mReady = TRUE;

  E = ReadOne (0, Mbr);
  if (EFI_ERROR (E)) { Log ("STOP: LBA0 read failed.\r\n"); goto Fail; }
  E = ReadOne (1, Gpt);
  if (EFI_ERROR (E)) { Log ("STOP: LBA1 read failed.\r\n"); goto Fail; }
  if (Mbr[510] != 0x55U || Mbr[511] != 0xAAU ||
      CompareMem (Gpt, "EFI PART", 8) != 0) {
    Log ("STOP: GPT signature mismatch; not exposing storage.\r\n");
    E = EFI_VOLUME_CORRUPTED;
    goto Fail;
  }

  ZeroMem (&mMedia, sizeof (mMedia));
  mMedia.MediaId = 1;
  mMedia.MediaPresent = TRUE;
  mMedia.RemovableMedia = FALSE;
  mMedia.LogicalPartition = FALSE;
  mMedia.ReadOnly = TRUE;
  mMedia.WriteCaching = FALSE;
  mMedia.BlockSize = 512;
  mMedia.IoAlign = 1;
  mMedia.LastBlock = mSectorCount - 1U;
  ZeroMem (&mBlockIo, sizeof (mBlockIo));
  mBlockIo.Revision = EFI_BLOCK_IO_PROTOCOL_REVISION;
  mBlockIo.Media = &mMedia;
  mBlockIo.Reset = Reset;
  mBlockIo.ReadBlocks = Read;
  mBlockIo.WriteBlocks = Write;
  mBlockIo.FlushBlocks = Flush;

  E = gBS->InstallMultipleProtocolInterfaces (
    &mDiskHandle,
    &gEfiBlockIoProtocolGuid, &mBlockIo,
    &gEfiDevicePathProtocolGuid, &mPath,
    NULL
    );
  if (EFI_ERROR (E)) goto Fail;
  AsciiSPrint (Line, sizeof (Line),
    "SUCCESS: GPT verified; read-only BlockIo published (%u MiB).\r\n",
    mSectorCount / 2048U);
  Log (Line);
  Log ("WRITE BLOCKS=EFI_WRITE_PROTECTED | MMC CMD24/25=IMPOSSIBLE\r\n");
  ShowResultPause ();
  return EFI_SUCCESS;

Fail:
  mReady = FALSE;
  AsciiSPrint (Line, sizeof (Line),
    "STOP: status=%08x; internal disk NOT exposed.\r\n", (UINT32)E);
  Log (Line);
  ShowResultPause ();
  return EFI_SUCCESS; // diagnostic failure should not block remaining DXE
}
