/* Included by J250fWpPrivate.c: private access to its validated userdata view.
 * Deliberately NOT a writable ext4 driver. SPDX-License-Identifier: BSD-2-Clause-Patent */
#define LOG_SECTORS 2048U
#include "j250f_log_format.h"
#define LOG_BYTES (LOG_SECTORS * 512U)
STATIC EFI_LBA mLogLbas[LOG_SECTORS];
STATIC BOOLEAN mLogMapped;
STATIC UINT32 mLogNext;
STATIC UINT8 mLogSuper[1024];
extern EFI_STATUS EFIAPI J250fLogBackendWrite(EFI_LBA Lba, CONST VOID *Buffer);
extern EFI_STATUS EFIAPI J250fLogInstall(UINT32 First);

STATIC VOID LogHeader(UINT8 *Out, UINT32 Index, BOOLEAN Used) {
  j250f_log_marker(Out, Index, Used, 0);
}

STATIC BOOLEAN LogValidUsed(CONST UINT8 *Data, UINT32 Index) {
  return j250f_log_valid_used(Data, Index) != 0;
}

/* ext4 gdt_csum (legacy crc16, not metadata_csum). */
STATIC UINT16 LogCrc16(UINT16 Crc, CONST UINT8 *Data, UINTN Length) {
  UINTN I;
  while (Length--) {
    Crc ^= *Data++;
    for (I = 0; I < 8; ++I)
      Crc = (UINT16)((Crc >> 1) ^ ((Crc & 1) ? 0xa001 : 0));
  }
  return Crc;
}

STATIC EFI_STATUS LogGroup(EXT4_VOLUME *V, UINT32 Group, UINT8 Desc[32]) {
  EFI_STATUS Status;
  UINT8 Number[4];
  UINT16 Crc;
  if (Group >= V->Groups) return EFI_VOLUME_CORRUPTED;
  Status = ReadDisk(V, V->TableOffset + (UINT64)Group * 32, 32, Desc);
  if (EFI_ERROR(Status)) return Status;
  if (Le32(V->Super + 100) & 0x10) {
    Number[0] = (UINT8)Group; Number[1] = (UINT8)(Group >> 8);
    Number[2] = (UINT8)(Group >> 16); Number[3] = (UINT8)(Group >> 24);
    Crc = LogCrc16(0xffff, V->Super + 104, 16);
    Crc = LogCrc16(Crc, Number, 4);
    Crc = LogCrc16(Crc, Desc, 30);
    if (Crc != Le16(Desc + 30)) return EFI_CRC_ERROR;
  }
  return EFI_SUCCESS;
}

/* No flex_bg/meta_bg/64bit: all allocation metadata must be in its own
   group. Conservatively reject ALL blocks at/before the inode table end. */
STATIC EFI_STATUS LogDataBlock(EXT4_VOLUME *V, UINT64 Block) {
  UINT8 Desc[32], Bitmap[EXT4_MAX_BLOCK];
  UINT32 Group, Bit;
  UINT64 Start, End, Map, InodeMap, Table, TableBlocks;
  EFI_STATUS Status;
  if (!Block || Block >= V->BlockCount) return EFI_VOLUME_CORRUPTED;
  Group = (UINT32)(Block / V->BlocksPerGroup);
  Start = (UINT64)Group * V->BlocksPerGroup;
  End = Start + V->BlocksPerGroup;
  if (End > V->BlockCount) End = V->BlockCount;
  Status = LogGroup(V, Group, Desc);
  if (EFI_ERROR(Status)) return Status;
  if (Le16(Desc + 18) & 2) return EFI_UNSUPPORTED; /* BLOCK_UNINIT */
  Map = Le32(Desc); InodeMap = Le32(Desc + 4); Table = Le32(Desc + 8);
  TableBlocks = ((UINT64)V->InodesPerGroup * V->InodeSize + V->BlockSize - 1) / V->BlockSize;
  if (Map < Start || Map >= End || InodeMap < Start || InodeMap >= End ||
      Table < Start || Table >= End || TableBlocks > End - Table ||
      Block <= Map || Block <= InodeMap || Block < Table + TableBlocks)
    return EFI_VOLUME_CORRUPTED;
  Status = ReadDisk(V, Map * V->BlockSize, V->BlockSize, Bitmap);
  if (EFI_ERROR(Status)) return Status;
  Bit = (UINT32)(Block - Start);
  if (!(Bitmap[Bit / 8] & (1U << (Bit % 8)))) return EFI_VOLUME_CORRUPTED;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS LogCheckInode(EXT4_VOLUME *V, UINT32 Number) {
  UINT8 Desc[32], Raw[128], Bitmap[EXT4_MAX_BLOCK];
  UINT32 Group = (Number - 1) / V->InodesPerGroup;
  UINT32 Index = (Number - 1) % V->InodesPerGroup;
  UINT64 Start = (UINT64)Group * V->BlocksPerGroup;
  UINT64 End = Start + V->BlocksPerGroup, Table, Map;
  UINT64 TableBlocks = ((UINT64)V->InodesPerGroup * V->InodeSize + V->BlockSize - 1) / V->BlockSize;
  EFI_STATUS Status = LogGroup(V, Group, Desc);
  if (EFI_ERROR(Status)) return Status;
  if (End > V->BlockCount) End = V->BlockCount;
  Map = Le32(Desc + 4); Table = Le32(Desc + 8);
  if ((Le16(Desc + 18) & 1) || Map < Start || Map >= End ||
      Table < Start || Table >= End || TableBlocks > End - Table)
    return EFI_UNSUPPORTED;
  Status = ReadDisk(V, Map * V->BlockSize, V->BlockSize, Bitmap);
  if (EFI_ERROR(Status)) return Status;
  if (!(Bitmap[Index / 8] & (1U << (Index % 8)))) return EFI_VOLUME_CORRUPTED;
  Status = ReadDisk(V, Table * V->BlockSize + (UINT64)Index * V->InodeSize, sizeof(Raw), Raw);
  if (EFI_ERROR(Status)) return Status;
  /* One link, no deleted inode, ACL, encryption, inline data, journal-data,
     immutable or append-only flags. Never touch inode size/timestamps. */
  if (Le16(Raw + 26) != 1 || Le32(Raw + 20) || Le32(Raw + 32) != 0x80000 ||
      Le32(Raw + 104) || Le32(Raw + 28) != LOG_BYTES / 512)
    return EFI_UNSUPPORTED;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS LogStage(EXT4_VOLUME *V, CONST EXT4_NODE *Folder) {
  EFI_STATUS Status;
  EXT4_NODE File;
  UINT32 Inode, Logical = 0, I, J, Sector = 0;
  UINT8 Data[512], Header[32];
  BOOLEAN ReadySeen = FALSE;
  mLogMapped = FALSE;
  mLogNext = LOG_SECTORS;
  /* This implementation is intentionally narrower than read-only ext4.
     Cleanly unmount recovery/Android: needs_recovery is a hard refusal. */
  if (V->BlockSize != 4096 || V->DescriptorSize != 32 ||
      Le32(V->Super + 20) != 0 || Le16(V->Super + 58) != 1 ||
      Le32(V->Super + 96) != 0x42 || (Le32(V->Super + 100) & ~0x13U) ||
      (Le32(V->Super + 92) & ~0x3cU) || Le32(V->Super + 232) ||
      V->InodesPerGroup > V->BlockSize * 8 || V->BlocksPerGroup > V->BlockSize * 8)
    return EFI_UNSUPPORTED;
  Status = Ext4Lookup(V, Folder, L"uefi.log", &Inode);
  if (EFI_ERROR(Status)) return Status;
  Status = Ext4LoadInode(V, Inode, &File);
  if (EFI_ERROR(Status)) return Status;
  if ((File.Mode & 0xf000) != 0x8000 || File.Size != LOG_BYTES ||
      File.Flags != 0x80000 || Le16(File.BlockMap) != 0xf30a ||
      Le16(File.BlockMap + 6) != 0 || !Le16(File.BlockMap + 2) ||
      Le16(File.BlockMap + 2) > 4 || Le16(File.BlockMap + 4) != 4)
    return EFI_UNSUPPORTED;
  Status = LogCheckInode(V, Inode);
  if (EFI_ERROR(Status)) return Status;
  for (I = 0; I < Le16(File.BlockMap + 2); ++I) {
    CONST UINT8 *Extent = File.BlockMap + 12 + I * 12;
    UINT32 Count = Le16(Extent + 4);
    UINT64 Physical = Le32(Extent + 8) | ((UINT64)Le16(Extent + 6) << 32);
    if (Le32(Extent) != Logical || !Count || Count > 32768 ||
        Count > LOG_BYTES / V->BlockSize - Logical || !Physical ||
        Physical >= V->BlockCount || Count > V->BlockCount - Physical)
      return EFI_VOLUME_CORRUPTED;
    for (J = 0; J < Count; ++J, ++Logical) {
      UINT32 K, Prior;
      Status = LogDataBlock(V, Physical + J);
      if (EFI_ERROR(Status)) return Status;
      for (K = 0; K < V->BlockSize / 512; ++K, ++Sector) {
        EFI_LBA Relative = (Physical + J) * (V->BlockSize / 512) + K;
        if (Relative > mBacking.Media.LastBlock ||
            mBacking.PhysicalStart > MAX_UINT64 - Relative)
          return EFI_VOLUME_CORRUPTED;
        mLogLbas[Sector] = mBacking.PhysicalStart + Relative;
        for (Prior = 0; Prior < Sector; ++Prior)
          if (mLogLbas[Prior] == mLogLbas[Sector]) return EFI_VOLUME_CORRUPTED;
        Status = ReadDisk(V, Relative * 512, sizeof(Data), Data);
        if (EFI_ERROR(Status)) return Status;
        LogHeader(Header, Sector, FALSE);
        if (!CompareMem(Data, Header, sizeof(Header))) {
          if (!ReadySeen) mLogNext = Sector;
          ReadySeen = TRUE;
          for (Prior = 32; Prior < 512; ++Prior)
            if (Data[Prior] != ' ') return EFI_VOLUME_CORRUPTED;
        } else {
          if (ReadySeen || !LogValidUsed(Data, Sector))
            return EFI_VOLUME_CORRUPTED;
        }
      }
    }
  }
  if (Sector != LOG_SECTORS || !ReadySeen) return EFI_VOLUME_FULL;
  CopyMem(mLogSuper, V->Super, sizeof(mLogSuper));
  mLogMapped = TRUE;
  return EFI_SUCCESS;
}

/* Sole physical write callsite: index -> previously verified allowlist.
   General EFI BlockIo and ext4 File.Write remain read-only. */
EFI_STATUS EFIAPI J250fLogWriteRecord(UINT32 Index, CONST VOID *Data) {
  EFI_STATUS Status;
  if (!mLogMapped || !mBacking.Concealed || Index != mLogNext ||
      Index >= LOG_SECTORS || !Data)
    return EFI_ACCESS_DENIED;
  if (!LogValidUsed(Data, Index)) return EFI_INVALID_PARAMETER;
  Status = J250fLogBackendWrite(mLogLbas[Index], Data);
  if (!EFI_ERROR(Status)) ++mLogNext;
  else if (Status != EFI_NOT_READY) mLogMapped = FALSE;
  return Status;
}

STATIC VOID LogStart(VOID) {
  UINT8 Super[1024];
  EFI_STATUS Status;
  if (!mLogMapped || !mBacking.Concealed) return;
  Status = ReadDisk(&mBacking.Ext4, 1024, sizeof(Super), Super);
  if (!EFI_ERROR(Status) && !CompareMem(Super, mLogSuper, sizeof(Super)))
    Status = J250fLogInstall(mLogNext);
  else if (!EFI_ERROR(Status)) Status = EFI_MEDIA_CHANGED;
  if (EFI_ERROR(Status)) {
    mLogMapped = FALSE;
    DEBUG((DEBUG_ERROR, "UEFI LOG disabled: %r\n", Status));
  }
}
