/** @file
  Strictly read-only ext4 volume and inode reader.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include "Ext4Read.h"
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define INODE_DIR  0x4000
#define INODE_FILE 0x8000
#define EXT4_EXTENTS 0x00080000U
#define EXT4_INLINE  0x10000000U
#define EXT4_ENCRYPT 0x00000800U
/* INCOMPAT: FILETYPE, RECOVER, EXTENTS, 64BIT, MMP, FLEX_BG,
   EA_INODE and LARGEDIR. Unknown layout-changing features are refused. */
#define EXT4_ALLOWED_INCOMPAT 0x000027C6U
#define EXT4_RO_BIGALLOC 0x00000200U

STATIC UINT16 Le16 (CONST UINT8 *P) {
  return (UINT16)(P[0] | ((UINT16)P[1] << 8));
}
STATIC UINT32 Le32 (CONST UINT8 *P) {
  return (UINT32)P[0] | ((UINT32)P[1] << 8) |
         ((UINT32)P[2] << 16) | ((UINT32)P[3] << 24);
}
STATIC UINT64 Le64 (CONST UINT8 *P) {
  return (UINT64)Le32(P) | ((UINT64)Le32(P + 4) << 32);
}
STATIC UINTN MinN (UINTN A, UINTN B) { return A < B ? A : B; }

/* All access is within a partition's BlockIo LastBlock. ReadBlocks is the
   ONLY disk operation in this entire driver. Scratch honors IoAlign. */
STATIC EFI_STATUS ReadDisk (EXT4_VOLUME *V, UINT64 Offset,
                            UINTN Length, VOID *Output) {
  UINT8 *Out = (UINT8 *)Output;
  UINT32 Sector = V->SectorSize;
  if (!V->Disk || !V->Disk->Media || !V->Scratch || !Sector ||
      (Length && !Output)) return EFI_INVALID_PARAMETER;
  while (Length) {
    EFI_STATUS Status;
    UINT64 Lba = Offset / Sector;
    UINTN Inside = (UINTN)(Offset % Sector);
    UINTN Count = MinN(EXT4_MAX_BLOCK / Sector,
                       (Inside + Length + Sector - 1) / Sector);
    UINTN Bytes;
    if (!Count || Lba > V->Disk->Media->LastBlock ||
        Count - 1 > V->Disk->Media->LastBlock - Lba)
      return EFI_VOLUME_CORRUPTED;
    Bytes = MinN(Length, Count * Sector - Inside);
    Status = V->Disk->ReadBlocks(V->Disk, V->Disk->Media->MediaId,
                                 Lba, Count * Sector, V->Scratch);
    if (EFI_ERROR(Status)) return Status;
    CopyMem(Out, V->Scratch + Inside, Bytes);
    Offset += Bytes;
    Out += Bytes;
    Length -= Bytes;
  }
  return EFI_SUCCESS;
}

EFI_STATUS Ext4Probe (EXT4_VOLUME *V) {
  EFI_STATUS Status;
  UINT32 LogBlock, LogGroup, FeatureRo;
  UINT64 Blocks, Inodes, Groups;
  UINTN Alignment;
  UINT8 *S;
  if (!V || !V->Disk || !V->Disk->Media ||
      !V->Disk->Media->MediaPresent ||
      !V->Disk->Media->LogicalPartition ||
      !V->Disk->Media->ReadOnly || !V->Disk->ReadBlocks)
    return EFI_UNSUPPORTED;
  V->SectorSize = V->Disk->Media->BlockSize;
  Alignment = V->Disk->Media->IoAlign;
  if (!Alignment) Alignment = 1;
  if (V->SectorSize < 512 || V->SectorSize > EXT4_MAX_BLOCK ||
      (EXT4_MAX_BLOCK % V->SectorSize) ||
      (Alignment & (Alignment - 1)) || Alignment > 65536)
    return EFI_UNSUPPORTED;
  Status = gBS->AllocatePool(EfiBootServicesData, EXT4_MAX_BLOCK + Alignment,
                             &V->ScratchAllocation);
  if (EFI_ERROR(Status)) return Status;
  V->Scratch = (UINT8 *)(((UINTN)V->ScratchAllocation + Alignment - 1)
                          & ~(Alignment - 1));
  Status = ReadDisk(V, 1024, sizeof(V->Super), V->Super);
  if (EFI_ERROR(Status)) return Status;
  S = V->Super;
  if (Le16(S + 56) != 0xef53) return EFI_UNSUPPORTED;
  if (Le32(S + 76) < 1) return EFI_UNSUPPORTED; /* dynamic inodes */
  V->Incompat = Le32(S + 96);
  FeatureRo = Le32(S + 100);
  if ((V->Incompat & ~EXT4_ALLOWED_INCOMPAT) ||
      (FeatureRo & EXT4_RO_BIGALLOC))
    return EFI_UNSUPPORTED;
  LogBlock = Le32(S + 24);
  LogGroup = Le32(S + 28);
  if (LogBlock > 2 || LogGroup != LogBlock) return EFI_UNSUPPORTED;
  V->BlockSize = 1024U << LogBlock;
  V->InodesPerGroup = Le32(S + 40);
  V->BlocksPerGroup = Le32(S + 32);
  V->InodeSize = Le16(S + 88);
  V->DescriptorSize = (V->Incompat & 0x80) ? Le16(S + 254) : 32;
  Blocks = Le32(S + 4);
  V->FreeBlocks = Le32(S + 12);
  if (V->Incompat & 0x80) {
    Blocks |= (UINT64)Le32(S + 336) << 32;
    V->FreeBlocks |= (UINT64)Le32(S + 344) << 32;
  }
  Inodes = Le32(S);
  if (!Blocks || !Inodes || !V->InodesPerGroup || !V->BlocksPerGroup ||
      V->InodeSize < 128 || V->InodeSize > V->BlockSize ||
      (V->InodeSize & 3) || V->DescriptorSize < 32 ||
      V->DescriptorSize > 64 || (V->DescriptorSize & 7) ||
      (V->Incompat & 0x80 && V->DescriptorSize < 64))
    return EFI_UNSUPPORTED;
  if (Blocks > (V->Disk->Media->LastBlock + 1) /
               (V->BlockSize / V->SectorSize))
    return EFI_VOLUME_CORRUPTED;
  Groups = (Inodes + V->InodesPerGroup - 1) / V->InodesPerGroup;
  if (!Groups || Groups > 0xffffffffU ||
      (Blocks + V->BlocksPerGroup - 1) / V->BlocksPerGroup > Groups + 1)
    return EFI_UNSUPPORTED;
  V->Groups = (UINT32)Groups;
  V->BlockCount = Blocks;
  V->InodeCount = Inodes;
  V->TableOffset = V->BlockSize == 1024 ? 2048 : V->BlockSize;
  if (V->TableOffset / V->BlockSize >= V->BlockCount ||
      (UINT64)V->Groups * V->DescriptorSize >
       (V->BlockCount * V->BlockSize - V->TableOffset))
    return EFI_VOLUME_CORRUPTED;
  V->Signature = EXT4_SIGNATURE;
  return EFI_SUCCESS;
}

EFI_STATUS Ext4LoadInode (EXT4_VOLUME *V, UINT32 Number, EXT4_NODE *Node) {
  UINT32 Group, Index;
  UINT64 Table;
  UINT8 Desc[64], Raw[128];
  EFI_STATUS Status;
  if (!V || V->Signature != EXT4_SIGNATURE || !Node ||
      Number == 0 || Number > V->InodeCount) return EFI_INVALID_PARAMETER;
  Group = (Number - 1) / V->InodesPerGroup;
  Index = (Number - 1) % V->InodesPerGroup;
  if (Group >= V->Groups) return EFI_VOLUME_CORRUPTED;
  Status = ReadDisk(V, V->TableOffset + (UINT64)Group * V->DescriptorSize,
                    V->DescriptorSize, Desc);
  if (EFI_ERROR(Status)) return Status;
  Table = Le32(Desc + 8);
  if (V->Incompat & 0x80) Table |= (UINT64)Le32(Desc + 40) << 32;
  if (!Table || Table >= V->BlockCount ||
      ((UINT64)Index * V->InodeSize + sizeof(Raw)) >
       (V->BlockCount - Table) * V->BlockSize)
    return EFI_VOLUME_CORRUPTED;
  Status = ReadDisk(V, Table * V->BlockSize +
                    (UINT64)Index * V->InodeSize, sizeof(Raw), Raw);
  if (EFI_ERROR(Status)) return Status;
  ZeroMem(Node, sizeof(*Node));
  Node->Number = Number;
  Node->Mode = Le16(Raw);
  Node->Flags = Le32(Raw + 32);
  Node->Size = Le32(Raw + 4) | ((UINT64)Le32(Raw + 108) << 32);
  CopyMem(Node->BlockMap, Raw + 40, sizeof(Node->BlockMap));
  if ((Node->Mode & 0xf000) != INODE_FILE &&
      (Node->Mode & 0xf000) != INODE_DIR) return EFI_UNSUPPORTED;
  if (Node->Flags & (EXT4_INLINE | EXT4_ENCRYPT)) return EFI_UNSUPPORTED;
  if (Node->Size > (UINT64)V->BlockCount * V->BlockSize)
    return EFI_VOLUME_CORRUPTED;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS ExtentMap (EXT4_VOLUME *V, CONST UINT8 *Tree,
                             UINTN TreeSize, UINT32 Depth, UINT32 Logical,
                             UINT64 *Physical, BOOLEAN *Hole) {
  UINTN I, Entries, MaxEntries;
  UINT32 Candidate = 0xffffffffU;
  if (TreeSize < 12 || Le16(Tree) != 0xf30a ||
      Le16(Tree + 6) != Depth || Depth > 5)
    return EFI_VOLUME_CORRUPTED;
  Entries = Le16(Tree + 2);
  MaxEntries = (TreeSize - 12) / 12;
  if (Le16(Tree + 4) > MaxEntries || Entries > Le16(Tree + 4))
    return EFI_VOLUME_CORRUPTED;
  for (I = 0; I < Entries; I++) {
    CONST UINT8 *E = Tree + 12 + I * 12;
    UINT32 Start = Le32(E);
    if (I && Start <= Le32(E - 12)) return EFI_VOLUME_CORRUPTED;
    if (Start <= Logical) Candidate = (UINT32)I;
    else break;
  }
  if (Candidate == 0xffffffffU) return EFI_SUCCESS;
  {
    CONST UINT8 *E = Tree + 12 + (UINTN)Candidate * 12;
    if (Depth == 0) {
      UINT32 Length = Le16(E + 4);
      BOOLEAN Unwritten = Length > 32768;
      UINT64 StartBlock = (UINT64)Le32(E + 8) |
                          ((UINT64)Le16(E + 6) << 32);
      UINT64 Delta = Logical - Le32(E);
      if (Unwritten) Length -= 32768;
      if (!Length || Delta >= Length) return EFI_SUCCESS;
      if (Unwritten) return EFI_SUCCESS;
      if (StartBlock >= V->BlockCount ||
          Delta >= V->BlockCount - StartBlock) return EFI_VOLUME_CORRUPTED;
      *Physical = StartBlock + Delta;
      *Hole = FALSE;
      return EFI_SUCCESS;
    } else {
      UINT64 Child = Le32(E + 4) | ((UINT64)Le16(E + 8) << 32);
      UINT8 Page[EXT4_MAX_BLOCK];
      EFI_STATUS Status;
      if (Child == 0 || Child >= V->BlockCount) return EFI_VOLUME_CORRUPTED;
      Status = ReadDisk(V, Child * V->BlockSize, V->BlockSize, Page);
      if (EFI_ERROR(Status)) return Status;
      return ExtentMap(V, Page, V->BlockSize, Depth - 1,
                       Logical, Physical, Hole);
    }
  }
}

STATIC EFI_STATUS MapBlock (EXT4_VOLUME *V, CONST EXT4_NODE *N,
                            UINT64 Logical, UINT64 *Physical,
                            BOOLEAN *Hole) {
  UINT64 Index, First, Span;
  UINT32 Per, Pointer;
  UINTN Levels, Level;
  EFI_STATUS Status;
  *Hole = TRUE;
  *Physical = 0;
  if (Logical > 0xffffffffU) return EFI_UNSUPPORTED;
  if (N->Flags & EXT4_EXTENTS)
    return ExtentMap(V, N->BlockMap, sizeof(N->BlockMap),
                     Le16(N->BlockMap + 6), (UINT32)Logical,
                     Physical, Hole);
  if (Logical < 12) {
    Pointer = Le32(N->BlockMap + 4 * (UINTN)Logical);
    if (Pointer) { *Physical = Pointer; *Hole = FALSE; }
  } else {
    Per = V->BlockSize / 4;
    Index = Logical - 12;
    Levels = 1;
    Span = Per;
    while (Index >= Span && Levels < 3) {
      Index -= Span;
      Span *= Per;
      Levels++;
    }
    if (Index >= Span) return EFI_UNSUPPORTED;
    Pointer = Le32(N->BlockMap + 4 * (11 + Levels));
    for (Level = Levels; Level && Pointer; Level--) {
      UINT8 Raw[4];
      UINT64 Entry;
      if (Pointer >= V->BlockCount) return EFI_VOLUME_CORRUPTED;
      Span /= Per;
      Entry = Index / Span;
      Index %= Span;
      Status = ReadDisk(V, (UINT64)Pointer * V->BlockSize + Entry * 4,
                        sizeof(Raw), Raw);
      if (EFI_ERROR(Status)) return Status;
      Pointer = Le32(Raw);
    }
    if (Pointer) { *Physical = Pointer; *Hole = FALSE; }
  }
  if (!*Hole && (*Physical == 0 || *Physical >= V->BlockCount))
    return EFI_VOLUME_CORRUPTED;
  return EFI_SUCCESS;
}

EFI_STATUS Ext4ReadFile (EXT4_VOLUME *V, CONST EXT4_NODE *N,
                         UINT64 Offset, UINTN Length, VOID *Buffer) {
  UINT8 *Out = Buffer;
  if (!V || V->Signature != EXT4_SIGNATURE || !N ||
      (Length && !Out) || Offset > N->Size ||
      Length > N->Size - Offset) return EFI_INVALID_PARAMETER;
  while (Length) {
    UINTN Inside = (UINTN)(Offset % V->BlockSize);
    UINTN Chunk = MinN(Length, V->BlockSize - Inside);
    UINT64 Physical;
    BOOLEAN Hole;
    EFI_STATUS Status = MapBlock(V, N, Offset / V->BlockSize, &Physical, &Hole);
    if (EFI_ERROR(Status)) return Status;
    if (Hole) ZeroMem(Out, Chunk);
    else {
      Status = ReadDisk(V, Physical * V->BlockSize + Inside, Chunk, Out);
      if (EFI_ERROR(Status)) return Status;
    }
    Out += Chunk;
    Offset += Chunk;
    Length -= Chunk;
  }
  return EFI_SUCCESS;
}

/* Cursor-based directory scan. Cache is owned by the caller; no disk writes,
   including journal replay. Check every ext4_dir_entry_2 record boundary. */
EFI_STATUS Ext4DirNext (EXT4_VOLUME *V, CONST EXT4_NODE *Dir,
                        UINT64 *Position, UINT64 *CachedBlock,
                        UINT8 Cache[EXT4_MAX_BLOCK],
                        UINT32 *Inode, CHAR16 Name[EXT4_NAME_MAX + 1]) {
  if (!V || !Dir || (Dir->Mode & 0xf000) != INODE_DIR ||
      !Position || !CachedBlock || !Cache || !Inode || !Name)
    return EFI_INVALID_PARAMETER;
  while (*Position < Dir->Size) {
    UINT64 Block = *Position / V->BlockSize;
    UINTN Inside = (UINTN)(*Position % V->BlockSize);
    UINT8 *E;
    UINTN Rec, NameSize, I, ToRead;
    EFI_STATUS Status;
    if (Block != *CachedBlock) {
      if (Block > MAX_UINT64 / V->BlockSize) return EFI_VOLUME_CORRUPTED;
      ToRead = (UINTN)MinN(V->BlockSize,
                          (UINTN)(Dir->Size - Block * V->BlockSize));
      ZeroMem(Cache, V->BlockSize);
      Status = Ext4ReadFile(V, Dir, Block * V->BlockSize, ToRead, Cache);
      if (EFI_ERROR(Status)) return Status;
      *CachedBlock = Block;
    }
    if (Inside + 8 > V->BlockSize || *Position + 8 > Dir->Size)
      return EFI_VOLUME_CORRUPTED;
    E = Cache + Inside;
    Rec = Le16(E + 4);
    NameSize = E[6];
    if (Rec < 8 || (Rec & 3) || Rec > V->BlockSize - Inside ||
        NameSize > Rec - 8 || Rec > Dir->Size - *Position)
      return EFI_VOLUME_CORRUPTED;
    *Position += Rec;
    if (Le32(E) == 0 || NameSize == 0) continue;
    for (I = 0; I < NameSize; I++) {
      /* Filesystem byte names are UTF-8. The initial safe driver exposes
         ASCII names; others are escaped rather than interpreted incorrectly. */
      Name[I] = E[8 + I] < 0x80 ? E[8 + I] : (CHAR16)'?';
    }
    Name[NameSize] = 0;
    *Inode = Le32(E);
    if (*Inode > V->InodeCount) return EFI_VOLUME_CORRUPTED;
    return EFI_SUCCESS;
  }
  return EFI_NOT_FOUND;
}

EFI_STATUS Ext4Lookup (EXT4_VOLUME *V, CONST EXT4_NODE *Dir,
                       CONST CHAR16 *Name, UINT32 *Number) {
  UINT8 Cache[EXT4_MAX_BLOCK];
  UINT64 Position = 0, Cached = MAX_UINT64;
  CHAR16 Found[EXT4_NAME_MAX + 1];
  EFI_STATUS Status;
  if (!Name || !Number) return EFI_INVALID_PARAMETER;
  for (;;) {
    UINTN I;
    UINT32 Inode;
    Status = Ext4DirNext(V, Dir, &Position, &Cached, Cache, &Inode, Found);
    if (EFI_ERROR(Status)) return Status;
    for (I = 0; Found[I] && Name[I] && Found[I] == Name[I]; I++) {}
    if (!Found[I] && !Name[I]) { *Number = Inode; return EFI_SUCCESS; }
  }
}
