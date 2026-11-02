/** @file
  Read-only ext4 on an EFI Block I/O logical partition.
  No journaling, recovery, updates, or block writes.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#ifndef J250F_EXT4_READ_H
#define J250F_EXT4_READ_H
#include <Uefi.h>
#include <Protocol/BlockIo.h>
#include <Protocol/SimpleFileSystem.h>

#define EXT4_SIGNATURE SIGNATURE_32('E','4','R','O')
#define EXT4_MAX_BLOCK 4096U
#define EXT4_NAME_MAX 255U
typedef struct {
  UINT32 Number;
  UINT16 Mode;
  UINT32 Flags;
  UINT64 Size;
  UINT8 BlockMap[60];
} EXT4_NODE;
typedef struct {
  UINT32 Signature;
  EFI_HANDLE Controller;
  EFI_BLOCK_IO_PROTOCOL *Disk;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL Simple;
  UINT32 SectorSize;
  UINT32 BlockSize;
  UINT32 InodeSize;
  UINT32 InodesPerGroup;
  UINT32 BlocksPerGroup;
  UINT32 DescriptorSize;
  UINT32 Groups;
  UINT64 BlockCount;
  UINT64 InodeCount;
  UINT64 FreeBlocks;
  UINT64 TableOffset;
  UINT32 Incompat;
  UINT32 OpenCount;
  UINT8 Super[1024];
  VOID *ScratchAllocation;
  UINT8 *Scratch;
} EXT4_VOLUME;

EFI_STATUS Ext4Probe (EXT4_VOLUME *Volume);
EFI_STATUS Ext4LoadInode (EXT4_VOLUME *Volume, UINT32 Number, EXT4_NODE *Node);
EFI_STATUS Ext4ReadFile (EXT4_VOLUME *Volume, CONST EXT4_NODE *Node,
                         UINT64 Offset, UINTN Size, VOID *Buffer);
EFI_STATUS Ext4DirNext (EXT4_VOLUME *Volume, CONST EXT4_NODE *Dir,
                        UINT64 *Position, UINT64 *CachedBlock,
                        UINT8 Cache[EXT4_MAX_BLOCK],
                        UINT32 *Inode, CHAR16 Name[EXT4_NAME_MAX + 1]);
EFI_STATUS Ext4Lookup (EXT4_VOLUME *Volume, CONST EXT4_NODE *Dir,
                       CONST CHAR16 *Name, UINT32 *Number);
#endif
