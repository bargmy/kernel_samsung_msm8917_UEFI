/** @file
  Private read-only userdata ext4 backing for the WP81 virtual disk.
  Optional DEBUG logging has a separate, bounded preallocated-file writer;
  the backing interface and general filesystem remain read-only.
  The physical eMMC EFI protocols are removed only after all inodes are staged.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Uefi.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/DiskIo.h>
#include <Protocol/PartitionInfo.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>

/* Reuse the exact ext4 parser from the working read-only driver in this
   module, WITHOUT publishing its SimpleFileSystem protocol. Its functions
   are linked once into this EFI image, independently of J250fExt4Dxe. */
#include "../J250fExt4Dxe/Ext4Read.c"

#define WP_FILE_COUNT 1U
#define WP_SECTOR_SIZE 512U

STATIC EFI_GUID mPhysicalDiskPathGuid = {
  0x98074a7c, 0x2be4, 0x47dc, {0xb7,0xa2,0x17,0x75,0xac,0xfb,0x10,0x91}
};
STATIC EFI_GUID mExt4VolumeMarker = {
  0x7cd5d62a, 0x8f37, 0x4eb6, {0x9d,0x10,0x3d,0xeb,0xb3,0x85,0xaf,0x64}
};

typedef struct {
  EFI_BLOCK_IO_PROTOCOL Io;
  EFI_BLOCK_IO_MEDIA Media;
  EFI_BLOCK_IO_PROTOCOL *Raw;
  EFI_HANDLE RawHandle;
  EFI_DEVICE_PATH_PROTOCOL *RawPath;
  EXT4_VOLUME Ext4;
  EXT4_NODE Files[WP_FILE_COUNT];
  UINT64 PhysicalStart;
  BOOLEAN Staged;
  BOOLEAN Concealed;
} WP_BACKING;
STATIC WP_BACKING mBacking;

#include "J250fLogStorage.inc"

STATIC BOOLEAN PathOnPhysical (EFI_DEVICE_PATH_PROTOCOL *Path) {
  VENDOR_DEVICE_PATH *Vendor;
  if (!Path || Path->Type != HARDWARE_DEVICE_PATH ||
      Path->SubType != HW_VENDOR_DP ||
      Path->Length[0] != sizeof(VENDOR_DEVICE_PATH) ||
      Path->Length[1] != 0)
    return FALSE;
  Vendor = (VENDOR_DEVICE_PATH *)Path;
  return CompareGuid(&Vendor->Guid, &mPhysicalDiskPathGuid);
}
STATIC BOOLEAN GptName (CONST CHAR16 *Entry, CONST CHAR16 *Wanted) {
  UINTN Index;
  for (Index = 0; Index < 36; ++Index) {
    if (Entry[Index] != Wanted[Index]) return FALSE;
    if (Entry[Index] == 0) return TRUE;
  }
  return FALSE;
}
STATIC EFI_STATUS EFIAPI BackingReset (EFI_BLOCK_IO_PROTOCOL *Io,
                                       BOOLEAN Verify) {
  (VOID)Verify;
  return Io == &mBacking.Io ? EFI_SUCCESS : EFI_INVALID_PARAMETER;
}
STATIC EFI_STATUS EFIAPI BackingWrite (EFI_BLOCK_IO_PROTOCOL *Io,
  UINT32 MediaId, EFI_LBA Lba, UINTN Size, VOID *Data) {
  (VOID)Io; (VOID)MediaId; (VOID)Lba; (VOID)Size; (VOID)Data;
  return EFI_WRITE_PROTECTED;
}
STATIC EFI_STATUS EFIAPI BackingFlush (EFI_BLOCK_IO_PROTOCOL *Io) {
  return Io == &mBacking.Io ? EFI_SUCCESS : EFI_INVALID_PARAMETER;
}
STATIC EFI_STATUS EFIAPI BackingRead (EFI_BLOCK_IO_PROTOCOL *Io,
  UINT32 MediaId, EFI_LBA Lba, UINTN Size, VOID *Data) {
  UINTN Sectors;
  EFI_BLOCK_IO_PROTOCOL *Raw = mBacking.Raw;
  if (Io != &mBacking.Io || !Raw || !Raw->Media ||
      MediaId != mBacking.Media.MediaId)
    return EFI_INVALID_PARAMETER;
  if (!Size) return EFI_SUCCESS;
  if (!Data || Size % WP_SECTOR_SIZE) return EFI_BAD_BUFFER_SIZE;
  if (mBacking.Media.IoAlign > 1 &&
      ((UINTN)Data & (mBacking.Media.IoAlign - 1)))
    return EFI_INVALID_PARAMETER;
  Sectors = Size / WP_SECTOR_SIZE;
  if (Lba > mBacking.Media.LastBlock ||
      Sectors - 1 > mBacking.Media.LastBlock - Lba ||
      mBacking.PhysicalStart > Raw->Media->LastBlock - Lba)
    return EFI_INVALID_PARAMETER;
  /* The pointer is private: the original eMMC DXE retains its static
     callback and disk state after its public protocol is uninstalled. */
  return Raw->ReadBlocks(Raw, Raw->Media->MediaId,
                         mBacking.PhysicalStart + Lba, Size, Data);
}

/* Only select the J250F's published real disk by its explicit vendor path.
   Never choose an arbitrary Block I/O handle or assume disk index zero. */
STATIC EFI_STATUS LocateRawDisk (VOID) {
  EFI_HANDLE *Handles = NULL;
  EFI_STATUS Status;
  UINTN Count = 0, I;
  Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiBlockIoProtocolGuid,
                                  NULL, &Count, &Handles);
  if (EFI_ERROR(Status)) return Status;
  for (I = 0; I < Count; ++I) {
    EFI_DEVICE_PATH_PROTOCOL *Path = NULL;
    EFI_BLOCK_IO_PROTOCOL *Raw = NULL;
    if (EFI_ERROR(gBS->HandleProtocol(Handles[I],
                    &gEfiDevicePathProtocolGuid, (VOID **)&Path)) ||
        !PathOnPhysical(Path) ||
        EFI_ERROR(gBS->HandleProtocol(Handles[I],
                    &gEfiBlockIoProtocolGuid, (VOID **)&Raw)) ||
        !Raw || !Raw->Media || !Raw->ReadBlocks ||
        !Raw->Media->MediaPresent || Raw->Media->LogicalPartition ||
        !Raw->Media->ReadOnly || Raw->Media->BlockSize != WP_SECTOR_SIZE ||
        Raw->Media->IoAlign > 65536 ||
        (Raw->Media->IoAlign &&
         (Raw->Media->IoAlign & (Raw->Media->IoAlign - 1))))
      continue;
    if (mBacking.Raw) { Status = EFI_ACCESS_DENIED; goto Done; }
    mBacking.Raw = Raw;
    mBacking.RawHandle = Handles[I];
    mBacking.RawPath = Path;
  }
  Status = mBacking.Raw ? EFI_SUCCESS : EFI_NOT_FOUND;
Done:
  gBS->FreePool(Handles);
  return Status;
}

STATIC EFI_STATUS LocateUserdata (UINT64 *Start, UINT64 *End) {
  EFI_HANDLE *Handles = NULL;
  EFI_STATUS Status;
  UINTN Count = 0, I;
  BOOLEAN Found = FALSE;
  Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiPartitionInfoProtocolGuid,
                                  NULL, &Count, &Handles);
  if (EFI_ERROR(Status)) return Status;
  for (I = 0; I < Count; ++I) {
    EFI_PARTITION_INFO_PROTOCOL *Part = NULL;
    EFI_BLOCK_IO_PROTOCOL *Io = NULL;
    EFI_DEVICE_PATH_PROTOCOL *Path = NULL;
    VOID *Ext4Marker = NULL;
    if (EFI_ERROR(gBS->HandleProtocol(Handles[I],
            &gEfiPartitionInfoProtocolGuid, (VOID **)&Part)) ||
        !Part || Part->Type != PARTITION_TYPE_GPT ||
        !GptName(Part->Info.Gpt.PartitionName, L"userdata") ||
        EFI_ERROR(gBS->HandleProtocol(Handles[I],
            &gEfiDevicePathProtocolGuid, (VOID **)&Path)) ||
        !PathOnPhysical(Path) ||
        EFI_ERROR(gBS->HandleProtocol(Handles[I],
            &mExt4VolumeMarker, &Ext4Marker)) ||
        EFI_ERROR(gBS->HandleProtocol(Handles[I],
            &gEfiBlockIoProtocolGuid, (VOID **)&Io)) ||
        !Io || !Io->Media || !Io->Media->ReadOnly ||
        !Io->Media->LogicalPartition ||
        Io->Media->BlockSize != WP_SECTOR_SIZE)
      continue;
    if (Found) { Status = EFI_ACCESS_DENIED; goto Done; }
    *Start = Part->Info.Gpt.StartingLBA;
    *End = Part->Info.Gpt.EndingLBA;
    if (*Start > *End || *End > mBacking.Raw->Media->LastBlock ||
        Io->Media->LastBlock != *End - *Start) {
      Status = EFI_VOLUME_CORRUPTED; goto Done;
    }
    Found = TRUE;
  }
  Status = Found ? EFI_SUCCESS : EFI_NOT_FOUND;
Done:
  gBS->FreePool(Handles);
  return Status;
}

VOID EFIAPI J250fWpPrivateRelease (VOID) {
  if (mBacking.Concealed) return; /* retained backend must stay alive */
  mLogMapped = FALSE;
  mLogNext = LOG_SECTORS;
  ZeroMem(mLogLbas, sizeof(mLogLbas));
  if (mBacking.Ext4.ScratchAllocation)
    gBS->FreePool(mBacking.Ext4.ScratchAllocation);
  ZeroMem(&mBacking, sizeof(mBacking));
}

EFI_STATUS EFIAPI J250fWpPrivateStage (VOID) {
  EFI_STATUS Status;
  UINT64 Start = 0, End = 0;
  EXT4_NODE Root, Folder;
  UINT32 Inode, I;
  UINT8 Header[WP_SECTOR_SIZE];
  STATIC CONST CHAR16 *Names[WP_FILE_COUNT] = {
    L"26.EFIESP.fat"
  };
  STATIC CONST UINT64 Sizes[WP_FILE_COUNT] = {
    33554432ULL
  };
  if (mBacking.Staged) return EFI_ALREADY_STARTED;
  J250fWpPrivateRelease();
  Status = LocateRawDisk();
  if (EFI_ERROR(Status)) goto Fail;
  Status = LocateUserdata(&Start, &End);
  if (EFI_ERROR(Status)) goto Fail;
  mBacking.PhysicalStart = Start;
  mBacking.Media.MediaId = 1;
  mBacking.Media.MediaPresent = TRUE;
  mBacking.Media.LogicalPartition = TRUE;
  mBacking.Media.ReadOnly = TRUE;
  mBacking.Media.BlockSize = WP_SECTOR_SIZE;
  mBacking.Media.IoAlign = mBacking.Raw->Media->IoAlign;
  mBacking.Media.LastBlock = End - Start;
  mBacking.Io.Revision = EFI_BLOCK_IO_PROTOCOL_REVISION;
  mBacking.Io.Media = &mBacking.Media;
  mBacking.Io.Reset = BackingReset;
  mBacking.Io.ReadBlocks = BackingRead;
  mBacking.Io.WriteBlocks = BackingWrite;
  mBacking.Io.FlushBlocks = BackingFlush;
  mBacking.Ext4.Disk = &mBacking.Io;
  Status = Ext4Probe(&mBacking.Ext4);
  if (EFI_ERROR(Status)) goto Fail;
  Status = Ext4LoadInode(&mBacking.Ext4, 2, &Root);
  if (EFI_ERROR(Status)) goto Fail;
  Status = Ext4Lookup(&mBacking.Ext4, &Root, L"wp81", &Inode);
  if (EFI_ERROR(Status)) goto Fail;
  Status = Ext4LoadInode(&mBacking.Ext4, Inode, &Folder);
  if (EFI_ERROR(Status)) goto Fail;
  if ((Folder.Mode & 0xf000) != 0x4000) { Status = EFI_UNSUPPORTED; goto Fail; }
  for (I = 0; I < WP_FILE_COUNT; ++I) {
    Status = Ext4Lookup(&mBacking.Ext4, &Folder, Names[I], &Inode);
    if (EFI_ERROR(Status)) goto Fail;
    Status = Ext4LoadInode(&mBacking.Ext4, Inode, &mBacking.Files[I]);
    if (EFI_ERROR(Status)) goto Fail;
    if ((mBacking.Files[I].Mode & 0xf000) != 0x8000 ||
        mBacking.Files[I].Size != Sizes[I]) {
      Status = EFI_UNSUPPORTED; goto Fail;
    }
    Status = Ext4ReadFile(&mBacking.Ext4, &mBacking.Files[I],
                          0, sizeof(Header), Header);
    if (EFI_ERROR(Status)) goto Fail;
    if (Header[510] != 0x55 || Header[511] != 0xaa ||
        (I == 0 ? !(Header[0] == 0xeb || Header[0] == 0xe9) :
         CompareMem(Header + 3, "NTFS    ", 8) != 0)) {
      Status = EFI_VOLUME_CORRUPTED; goto Fail;
    }
  }
  mBacking.Staged = TRUE;
  Status = LogStage(&mBacking.Ext4, &Folder);
  DEBUG((DEBUG_WARN, "UEFI LOG /wp81/uefi.log preparation: %r\n", Status));
  return EFI_SUCCESS;
Fail:
  J250fWpPrivateRelease();
  return Status;
}

EFI_STATUS EFIAPI J250fWpPrivateRead (UINT32 Index, UINT64 Offset,
                                     UINTN Size, VOID *Out) {
  if (!mBacking.Staged || Index >= WP_FILE_COUNT) return EFI_NOT_READY;
  return Ext4ReadFile(&mBacking.Ext4, &mBacking.Files[Index],
                      Offset, Size, Out);
}

/* Refuse to continue if ANY physical or unrelated UEFI disk survives.
   This checks EFI handles, not Windows hardware MMIO capabilities. */
STATIC EFI_STATUS NoPublicDisks (VOID) {
  EFI_HANDLE *Handles = NULL;
  UINTN Count = 0;
  EFI_STATUS Status = gBS->LocateHandleBuffer(
    ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles);
  if (!EFI_ERROR(Status)) {
    gBS->FreePool(Handles);
    return EFI_ACCESS_DENIED;
  }
  if (Status != EFI_NOT_FOUND) return Status;
  /* DiskIo must also disappear with the real controller, or its handle
     could still expose raw sectors without BlockIo. */
  Status = gBS->LocateHandleBuffer(
    ByProtocol, &gEfiDiskIoProtocolGuid, NULL, &Count, &Handles);
  if (!EFI_ERROR(Status)) {
    gBS->FreePool(Handles);
    return EFI_ACCESS_DENIED;
  }
  if (Status != EFI_NOT_FOUND) return Status;
  Status = gBS->LocateHandleBuffer(
    ByProtocol, &gEfiPartitionInfoProtocolGuid, NULL, &Count, &Handles);
  if (!EFI_ERROR(Status)) {
    gBS->FreePool(Handles);
    return EFI_ACCESS_DENIED;
  }
  return Status == EFI_NOT_FOUND ? EFI_SUCCESS : Status;
}

EFI_STATUS EFIAPI J250fWpPrivateConceal (VOID) {
  EFI_STATUS Status;
  if (!mBacking.Staged || !mBacking.RawHandle || mBacking.Concealed)
    return EFI_NOT_READY;
  Status = gBS->DisconnectController(mBacking.RawHandle, NULL, NULL);
  if (EFI_ERROR(Status)) return Status;
  Status = gBS->UninstallMultipleProtocolInterfaces(mBacking.RawHandle,
             &gEfiBlockIoProtocolGuid, mBacking.Raw,
             &gEfiDevicePathProtocolGuid, mBacking.RawPath, NULL);
  if (EFI_ERROR(Status)) return Status;
  mBacking.Concealed = TRUE;
  Status = NoPublicDisks();
  if (!EFI_ERROR(Status)) LogStart();
  return Status;
}
