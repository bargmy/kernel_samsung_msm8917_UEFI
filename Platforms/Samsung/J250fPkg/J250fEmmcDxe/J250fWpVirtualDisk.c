/** @file
  Windows Phone 8.1 virtual GPT preview. Read-only, opt-in, fail-closed.
  Backed ONLY by three fixed regular files on the physical userdata ext4
  partition. The device never issues physical WriteBlocks.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Uefi.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>
#include <Uefi/UefiGpt.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define WP_SECTOR 512U
#define WP_GPT_ENTRIES 128U
#define WP_GPT_ARRAY_SECTORS 32U
#define WP_IMAGES 3U
#define WP_IO_MAX (32U * 1024U)
#define WP_MIN(A,B) ((A) < (B) ? (A) : (B))

typedef struct {
  CONST CHAR16 *Path;
  CONST CHAR16 *Name;
  UINT64 ExpectedSize;
  EFI_FILE_PROTOCOL *File;
  UINT64 FirstLba;
  UINT64 SectorCount;
} WP_IMAGE;

typedef struct {
  VENDOR_DEVICE_PATH Vendor;
  EFI_DEVICE_PATH_PROTOCOL End;
} WP_PATH;

typedef struct {
  EFI_BLOCK_IO_PROTOCOL Io;
  EFI_BLOCK_IO_MEDIA Media;
  WP_PATH Path;
  EFI_HANDLE Handle;
  BOOLEAN Active;
  UINT64 LastBlock;
  WP_IMAGE Images[WP_IMAGES];
  EFI_PARTITION_ENTRY Entries[WP_GPT_ENTRIES];
  UINT8 PrimaryHeader[WP_SECTOR];
  UINT8 BackupHeader[WP_SECTOR];
  UINT8 Mbr[WP_SECTOR];
} WP_DISK;

STATIC WP_DISK mWp;
STATIC EFI_HANDLE mParent;
STATIC CONST EFI_GUID mWpDiskPathGuid =
  { 0xe7b5ba2a, 0xd586, 0x4df0, {0x8e,0x28,0x2a,0xd5,0x5d,0x85,0x9c,0x81} };
STATIC CONST EFI_GUID mExt4MarkerGuid =
  { 0x7cd5d62a, 0x8f37, 0x4eb6, {0x9d,0x10,0x3d,0xeb,0xb3,0x85,0xaf,0x64} };
STATIC CONST EFI_GUID mVirtualDiskGuid =
  { 0xae420040, 0x13dd, 0x41f2, {0xae,0x7f,0x0d,0xc3,0x58,0x54,0xc8,0xd7} };
STATIC CONST EFI_GUID mEspType =
  { 0xc12a7328, 0xf81f, 0x11d2, {0xba,0x4b,0x00,0xa0,0xc9,0x3e,0xc9,0x3b} };
STATIC CONST EFI_GUID mBasicType =
  { 0xebd0a0a2, 0xb9e5, 0x4433, {0x87,0xc0,0x68,0xb6,0xb7,0x26,0x99,0xc7} };

STATIC VOID PutLe32 (UINT8 *P, UINT32 V) {
  P[0] = (UINT8)V; P[1] = (UINT8)(V >> 8);
  P[2] = (UINT8)(V >> 16); P[3] = (UINT8)(V >> 24);
}
STATIC BOOLEAN Named (CONST CHAR16 *A, CONST CHAR16 *B) {
  UINTN I;
  for (I = 0; I < 36; ++I) {
    if (A[I] != B[I]) return FALSE;
    if (!A[I]) return TRUE;
  }
  return FALSE;
}
STATIC UINT64 Align2048 (UINT64 Value) {
  return (Value + 2047ULL) & ~2047ULL;
}
STATIC EFI_STATUS EFIAPI WpReset (EFI_BLOCK_IO_PROTOCOL *This,
                                  BOOLEAN Verify) {
  (VOID)Verify;
  return This == &mWp.Io && mWp.Active ? EFI_SUCCESS : EFI_NO_MEDIA;
}
STATIC EFI_STATUS EFIAPI WpWrite (EFI_BLOCK_IO_PROTOCOL *This,
  UINT32 MediaId, EFI_LBA Lba, UINTN Bytes, VOID *Buffer) {
  (VOID)MediaId; (VOID)Lba; (VOID)Bytes; (VOID)Buffer;
  return This == &mWp.Io ? EFI_WRITE_PROTECTED : EFI_INVALID_PARAMETER;
}
STATIC EFI_STATUS EFIAPI WpFlush (EFI_BLOCK_IO_PROTOCOL *This) {
  return This == &mWp.Io && mWp.Active ? EFI_SUCCESS : EFI_NO_MEDIA;
}
STATIC EFI_STATUS WpReadImage (WP_IMAGE *Image, UINT64 Offset,
                               UINTN Bytes, UINT8 *Out) {
  EFI_STATUS Status;
  UINTN Got = Bytes;
  if (!Image || !Image->File || !Bytes ||
      Offset > Image->ExpectedSize ||
      Bytes > Image->ExpectedSize - Offset) return EFI_INVALID_PARAMETER;
  Status = Image->File->SetPosition(Image->File, Offset);
  if (EFI_ERROR(Status)) return Status;
  Status = Image->File->Read(Image->File, &Got, Out);
  if (EFI_ERROR(Status)) return Status;
  return Got == Bytes ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}
STATIC EFI_STATUS EFIAPI WpRead (EFI_BLOCK_IO_PROTOCOL *This,
  UINT32 MediaId, EFI_LBA Lba, UINTN Bytes, VOID *Buffer) {
  UINT8 *Out = (UINT8 *)Buffer;
  UINTN Count, N;
  if (This != &mWp.Io || !mWp.Active) return EFI_NO_MEDIA;
  if (MediaId != mWp.Media.MediaId) return EFI_MEDIA_CHANGED;
  if (Bytes == 0) return EFI_SUCCESS;
  if (!Out) return EFI_INVALID_PARAMETER;
  if (Bytes % WP_SECTOR) return EFI_BAD_BUFFER_SIZE;
  Count = Bytes / WP_SECTOR;
  if (Lba > mWp.LastBlock || Count - 1 > mWp.LastBlock - Lba)
    return EFI_INVALID_PARAMETER;
  while (Count) {
    CONST UINT8 *Sector = NULL;
    UINTN I;
    for (I = 0; I < WP_IMAGES; I++) {
      WP_IMAGE *Image = &mWp.Images[I];
      if (Lba >= Image->FirstLba &&
          Lba - Image->FirstLba < Image->SectorCount) {
        UINTN Available = (UINTN)WP_MIN((UINT64)Count,
                                Image->SectorCount - (Lba - Image->FirstLba));
        EFI_STATUS Status;
        N = WP_MIN(Available, WP_IO_MAX / WP_SECTOR);
        Status = WpReadImage(Image,
                     (Lba - Image->FirstLba) * WP_SECTOR,
                     N * WP_SECTOR, Out);
        if (EFI_ERROR(Status)) return Status;
        Out += N * WP_SECTOR;
        Lba += N;
        Count -= N;
        break;
      }
    }
    if (I < WP_IMAGES) continue;
    if (Lba == 0) Sector = mWp.Mbr;
    else if (Lba == 1) Sector = mWp.PrimaryHeader;
    else if (Lba >= 2 && Lba < 2 + WP_GPT_ARRAY_SECTORS)
      Sector = (CONST UINT8 *)mWp.Entries + (UINTN)(Lba - 2) * WP_SECTOR;
    else if (Lba == mWp.LastBlock) Sector = mWp.BackupHeader;
    else if (Lba >= mWp.LastBlock - WP_GPT_ARRAY_SECTORS &&
             Lba < mWp.LastBlock)
      Sector = (CONST UINT8 *)mWp.Entries +
               (UINTN)(Lba - (mWp.LastBlock - WP_GPT_ARRAY_SECTORS)) * WP_SECTOR;
    if (Sector) CopyMem(Out, Sector, WP_SECTOR);
    else ZeroMem(Out, WP_SECTOR);
    Out += WP_SECTOR; Lba++; Count--;
  }
  return EFI_SUCCESS;
}
STATIC VOID WpCloseImages (VOID) {
  UINTN I;
  for (I = 0; I < WP_IMAGES; I++) {
    if (mWp.Images[I].File) {
      mWp.Images[I].File->Close(mWp.Images[I].File);
      mWp.Images[I].File = NULL;
    }
  }
}
STATIC EFI_STATUS WpOpenImage (EFI_FILE_PROTOCOL *Root, WP_IMAGE *Image,
                               UINT8 *FirstSector) {
  EFI_STATUS Status;
  EFI_FILE_PROTOCOL *File = NULL;
  union {
    EFI_FILE_INFO Info;
    UINT8 Buffer[512];
  } InfoBuf;
  UINTN InfoSize = sizeof(InfoBuf);
  UINTN ReadSize = WP_SECTOR;
  Status = Root->Open(Root, &File, (CHAR16 *)Image->Path,
                      EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR(Status)) return Status;
  Status = File->GetInfo(File, &gEfiFileInfoGuid, &InfoSize, &InfoBuf);
  if (EFI_ERROR(Status) ||
      (InfoBuf.Info.Attribute & EFI_FILE_DIRECTORY) ||
      InfoBuf.Info.FileSize != Image->ExpectedSize) {
    File->Close(File);
    return EFI_UNSUPPORTED;
  }
  Status = File->Read(File, &ReadSize, FirstSector);
  if (EFI_ERROR(Status) || ReadSize != WP_SECTOR) {
    File->Close(File);
    return EFI_VOLUME_CORRUPTED;
  }
  /* Only accept a FAT boot sector for EFIESP and NTFS for the other images. */
  if (FirstSector[510] != 0x55 || FirstSector[511] != 0xaa ||
      (Image == &mWp.Images[0] ?
       !(FirstSector[0] == 0xeb || FirstSector[0] == 0xe9) :
       CompareMem(FirstSector + 3, "NTFS    ", 8) != 0)) {
    File->Close(File);
    return EFI_UNSUPPORTED;
  }
  Image->File = File;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS WpFindUserdata (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL **Fs) {
  EFI_STATUS Status;
  EFI_HANDLE *Handles = NULL;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Found = NULL;
  UINTN Count = 0, I;
  Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiPartitionInfoProtocolGuid,
                                    NULL, &Count, &Handles);
  if (EFI_ERROR(Status)) return Status;
  for (I = 0; I < Count; I++) {
    EFI_PARTITION_INFO_PROTOCOL *Part = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Candidate = NULL;
    EFI_BLOCK_IO_PROTOCOL *Block = NULL;
    VOID *Marker = NULL;
    if (EFI_ERROR(gBS->HandleProtocol(Handles[I], &gEfiPartitionInfoProtocolGuid,
                                     (VOID **)&Part)) ||
        !Part || Part->Type != PARTITION_TYPE_GPT ||
        !Named(Part->Info.Gpt.PartitionName, L"userdata"))
      continue;
    if (EFI_ERROR(gBS->HandleProtocol(Handles[I], &mExt4MarkerGuid, &Marker)) ||
        EFI_ERROR(gBS->HandleProtocol(Handles[I],
                    &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Candidate)) ||
        EFI_ERROR(gBS->HandleProtocol(Handles[I],
                    &gEfiBlockIoProtocolGuid, (VOID **)&Block)) ||
        !Candidate || !Block || !Block->Media ||
        !Block->Media->LogicalPartition || !Block->Media->ReadOnly)
      continue;
    if (Found) {
      gBS->FreePool(Handles);
      return EFI_ACCESS_DENIED; /* never guess between two userdatas */
    }
    Found = Candidate;
  }
  gBS->FreePool(Handles);
  if (!Found) return EFI_NOT_FOUND;
  *Fs = Found;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS WpBuildGpt (VOID) {
  EFI_PARTITION_TABLE_HEADER Header;
  EFI_STATUS Status;
  UINT32 Crc;
  UINTN I, J;
  UINT64 Next = 2048;
  ZeroMem(mWp.Entries, sizeof(mWp.Entries));
  for (I = 0; I < WP_IMAGES; I++) {
    EFI_PARTITION_ENTRY *E = &mWp.Entries[I];
    WP_IMAGE *Img = &mWp.Images[I];
    if (Img->ExpectedSize % WP_SECTOR ||
        Img->ExpectedSize < WP_SECTOR ||
        Next > MAX_UINT64 - Img->ExpectedSize / WP_SECTOR - 4096)
      return EFI_INVALID_PARAMETER;
    Img->FirstLba = Next;
    Img->SectorCount = Img->ExpectedSize / WP_SECTOR;
    E->PartitionTypeGUID = I == 0 ? mEspType : mBasicType;
    E->UniquePartitionGUID = mVirtualDiskGuid;
    /* Stable, distinct IDs until the original BLU GPT entries are available. */
    E->UniquePartitionGUID.Data1 += (UINT32)(I + 1);
    E->StartingLBA = Next;
    E->EndingLBA = Next + Img->SectorCount - 1;
    for (J = 0; J < 35 && Img->Name[J]; J++)
      E->PartitionName[J] = Img->Name[J];
    Next = Align2048(Next + Img->SectorCount);
  }
  mWp.LastBlock = Next + 33;
  Status = gBS->CalculateCrc32(mWp.Entries, sizeof(mWp.Entries), &Crc);
  if (EFI_ERROR(Status)) return Status;
  ZeroMem(&Header, sizeof(Header));
  Header.Header.Signature = EFI_PTAB_HEADER_ID;
  Header.Header.Revision = 0x00010000;
  Header.Header.HeaderSize = sizeof(Header);
  Header.MyLBA = 1;
  Header.AlternateLBA = mWp.LastBlock;
  Header.FirstUsableLBA = 34;
  Header.LastUsableLBA = mWp.LastBlock - 33;
  Header.DiskGUID = mVirtualDiskGuid;
  Header.PartitionEntryLBA = 2;
  Header.NumberOfPartitionEntries = WP_GPT_ENTRIES;
  Header.SizeOfPartitionEntry = sizeof(EFI_PARTITION_ENTRY);
  Header.PartitionEntryArrayCRC32 = Crc;
  Status = gBS->CalculateCrc32(&Header, sizeof(Header), &Crc);
  if (EFI_ERROR(Status)) return Status;
  Header.Header.CRC32 = Crc;
  ZeroMem(mWp.PrimaryHeader, WP_SECTOR);
  CopyMem(mWp.PrimaryHeader, &Header, sizeof(Header));
  Header.Header.CRC32 = 0;
  Header.MyLBA = mWp.LastBlock;
  Header.AlternateLBA = 1;
  Header.PartitionEntryLBA = mWp.LastBlock - WP_GPT_ARRAY_SECTORS;
  Status = gBS->CalculateCrc32(&Header, sizeof(Header), &Crc);
  if (EFI_ERROR(Status)) return Status;
  Header.Header.CRC32 = Crc;
  ZeroMem(mWp.BackupHeader, WP_SECTOR);
  CopyMem(mWp.BackupHeader, &Header, sizeof(Header));
  ZeroMem(mWp.Mbr, WP_SECTOR);
  mWp.Mbr[446 + 4] = 0xee;
  PutLe32(mWp.Mbr + 446 + 8, 1);
  PutLe32(mWp.Mbr + 446 + 12,
          mWp.LastBlock > MAX_UINT32 ? MAX_UINT32 : (UINT32)mWp.LastBlock);
  mWp.Mbr[510] = 0x55; mWp.Mbr[511] = 0xaa;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS WpMount (VOID) {
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs = NULL;
  EFI_FILE_PROTOCOL *Root = NULL;
  EFI_STATUS Status;
  UINTN I;
  UINT8 FirstSector[WP_SECTOR];
  if (mWp.Active) return EFI_ALREADY_STARTED;
  ZeroMem(&mWp, sizeof(mWp));
  mWp.Images[0].Path = L"\\wp81\\26.EFIESP.fat";
  mWp.Images[0].Name = L"EFIESP";
  mWp.Images[0].ExpectedSize = 33554432ULL;
  mWp.Images[1].Path = L"\\wp81\\27.MainOS.ntfs";
  mWp.Images[1].Name = L"MainOS";
  mWp.Images[1].ExpectedSize = 2648719360ULL;
  mWp.Images[2].Path = L"\\wp81\\28.Data.ntfs";
  mWp.Images[2].Name = L"Data";
  mWp.Images[2].ExpectedSize = 4552916992ULL;
  Status = WpFindUserdata(&Fs);
  if (EFI_ERROR(Status)) return Status;
  Status = Fs->OpenVolume(Fs, &Root);
  if (EFI_ERROR(Status)) return Status;
  for (I = 0; I < WP_IMAGES; I++) {
    Status = WpOpenImage(Root, &mWp.Images[I], FirstSector);
    if (EFI_ERROR(Status)) break;
  }
  Root->Close(Root);
  if (EFI_ERROR(Status)) { WpCloseImages(); return Status; }
  Status = WpBuildGpt();
  if (EFI_ERROR(Status)) { WpCloseImages(); return Status; }
  mWp.Media.MediaId = 1;
  mWp.Media.MediaPresent = TRUE;
  mWp.Media.RemovableMedia = FALSE;
  mWp.Media.LogicalPartition = FALSE;
  mWp.Media.ReadOnly = TRUE;
  mWp.Media.WriteCaching = FALSE;
  mWp.Media.BlockSize = WP_SECTOR;
  mWp.Media.IoAlign = 1;
  mWp.Media.LastBlock = mWp.LastBlock;
  mWp.Io.Revision = EFI_BLOCK_IO_PROTOCOL_REVISION;
  mWp.Io.Media = &mWp.Media;
  mWp.Io.Reset = WpReset;
  mWp.Io.ReadBlocks = WpRead;
  mWp.Io.WriteBlocks = WpWrite;
  mWp.Io.FlushBlocks = WpFlush;
  mWp.Path.Vendor.Header.Type = HARDWARE_DEVICE_PATH;
  mWp.Path.Vendor.Header.SubType = HW_VENDOR_DP;
  mWp.Path.Vendor.Header.Length[0] = sizeof(VENDOR_DEVICE_PATH);
  mWp.Path.Vendor.Header.Length[1] = 0;
  mWp.Path.Vendor.Guid = mWpDiskPathGuid;
  mWp.Path.End.Type = END_DEVICE_PATH_TYPE;
  mWp.Path.End.SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
  mWp.Path.End.Length[0] = sizeof(EFI_DEVICE_PATH_PROTOCOL);
  mWp.Path.End.Length[1] = 0;
  mWp.Active = TRUE;
  Status = gBS->InstallMultipleProtocolInterfaces(&mWp.Handle,
    &gEfiBlockIoProtocolGuid, &mWp.Io,
    &gEfiDevicePathProtocolGuid, &mWp.Path, NULL);
  if (EFI_ERROR(Status)) {
    mWp.Active = FALSE;
    WpCloseImages();
    return Status;
  }
  /* The partition and FAT drivers run before the persistent ADB session. */
  (VOID)gBS->ConnectController(mWp.Handle, NULL, NULL, TRUE);
  return EFI_SUCCESS;
}
STATIC EFI_HANDLE WpFindEsp (VOID) {
  EFI_HANDLE *Handles = NULL, Result = NULL;
  UINTN Count = 0, I;
  EFI_STATUS Status = gBS->LocateHandleBuffer(
    ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR(Status)) return NULL;
  for (I = 0; I < Count; I++) {
    EFI_PARTITION_INFO_PROTOCOL *Part = NULL;
    EFI_DEVICE_PATH_PROTOCOL *Path = NULL;
    VENDOR_DEVICE_PATH *Vendor;
    if (EFI_ERROR(gBS->HandleProtocol(Handles[I],
             &gEfiPartitionInfoProtocolGuid, (VOID **)&Part)) ||
        EFI_ERROR(gBS->HandleProtocol(Handles[I],
             &gEfiDevicePathProtocolGuid, (VOID **)&Path)) ||
        !Part || Part->Type != PARTITION_TYPE_GPT || !Path ||
        !Named(Part->Info.Gpt.PartitionName, L"EFIESP"))
      continue;
    if (Path->Type != HARDWARE_DEVICE_PATH ||
        Path->SubType != HW_VENDOR_DP ||
        Path->Length[0] != sizeof(VENDOR_DEVICE_PATH) ||
        Path->Length[1] != 0)
      continue;
    Vendor = (VENDOR_DEVICE_PATH *)Path;
    if (CompareGuid(&Vendor->Guid, &mWpDiskPathGuid)) { Result = Handles[I]; break; }
  }
  gBS->FreePool(Handles);
  return Result;
}
STATIC EFI_STATUS WpTryLoader (BOOLEAN Start, CHAR8 *Out, UINTN Cap) {
  STATIC CONST CHAR16 *Names[] = {
    L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi",
    L"\\EFI\\BOOT\\BOOTARM.EFI",
    L"\\EFI\\Microsoft\\Boot\\bootarm.efi"
  };
  EFI_HANDLE Esp, Image = NULL;
  EFI_STATUS Status = EFI_NOT_FOUND;
  UINTN I;
  if (!mWp.Active) return EFI_NOT_READY;
  Esp = WpFindEsp();
  if (!Esp) return EFI_NOT_FOUND;
  for (I = 0; I < sizeof(Names) / sizeof(Names[0]); ++I) {
    EFI_DEVICE_PATH_PROTOCOL *Path = FileDevicePath(Esp, (CHAR16 *)Names[I]);
    if (!Path) return EFI_OUT_OF_RESOURCES;
    Status = gBS->LoadImage(FALSE, mParent, Path, NULL, 0, &Image);
    gBS->FreePool(Path);
    if (!EFI_ERROR(Status)) break;
  }
  if (EFI_ERROR(Status)) return Status;
  if (!Start) {
    Status = gBS->UnloadImage(Image);
    return Status;
  }
  /* WARNING: after ExitBootServices a guest storage driver could reach raw
     eMMC MMIO, outside this read-only UEFI isolation. Start only explicitly. */
  AsciiSPrint(Out, Cap, "StartImage: Windows boot manager starting now.\r\n");
  Status = gBS->StartImage(Image, NULL, NULL);
  (VOID)gBS->UnloadImage(Image);
  return Status;
}
VOID EFIAPI J250fWpInit (EFI_HANDLE Parent) { mParent = Parent; }
BOOLEAN EFIAPI J250fWpCommand (CONST CHAR8 *Cmd, CHAR8 *Out,
                                UINTN Cap, UINTN *Used) {
  EFI_STATUS Status;
  if (AsciiStrnCmp(Cmd, "wp ", 3) != 0 &&
      AsciiStrCmp(Cmd, "wp") != 0) return FALSE;
  if (!Out || !Cap || !Used) return TRUE;
  if (AsciiStrCmp(Cmd, "wp") == 0 || AsciiStrCmp(Cmd, "wp status") == 0) {
    *Used = AsciiSPrint(Out, Cap,
      "WP81 virtual GPT: %a (read-only)\r\n"
      "Backing: userdata ext4 /wp81/{26.EFIESP.fat,27.MainOS.ntfs,28.Data.ntfs}\r\n"
      "Virtual disk sectors: %Lu\r\n"
      "Commands: wp mount | wp probe | wp boot CONFIRM\r\n"
      "WARNING: Windows kernel is NOT isolated from physical eMMC.\r\n",
      mWp.Active ? "MOUNTED" : "not mounted",
      mWp.Active ? mWp.LastBlock + 1 : 0ULL);
    return TRUE;
  }
  if (AsciiStrCmp(Cmd, "wp mount") == 0) {
    Status = WpMount();
    *Used = AsciiSPrint(Out, Cap,
      "WP81 read-only virtual GPT mount: %r\r\n"
      "Missing files, wrong sizes or no ext4 userdata cause safe failure.\r\n",
      Status);
    return TRUE;
  }
  if (AsciiStrCmp(Cmd, "wp probe") == 0 ||
      AsciiStrCmp(Cmd, "wp boot CONFIRM") == 0) {
    BOOLEAN Start = AsciiStrCmp(Cmd, "wp boot CONFIRM") == 0;
    Status = WpTryLoader(Start, Out, Cap);
    *Used = AsciiSPrint(Out, Cap, "WP81 %a: %r\r\n",
                        Start ? "StartImage" : "LoadImage/UnloadImage", Status);
    return TRUE;
  }
  *Used = AsciiSPrint(Out, Cap,
    "Usage: wp status | wp mount | wp probe | wp boot CONFIRM\r\n"
    "wp boot CONFIRM can execute the Windows kernel, which is NOT eMMC-isolated.\r\n");
  return TRUE;
}
