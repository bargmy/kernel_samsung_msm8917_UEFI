/** @file
  ESP-only Windows Phone 8.1 virtual GPT for normal BDS.
  EFI-visible storage only: EFIESP. No physical eMMC handles
  remain after a successful mount. All virtual writes fail protected.
  This does NOT restrict Windows MMIO access after ExitBootServices.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Uefi.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/GlobalVariable.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Uefi/UefiGpt.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define WP_SECTOR 512U
#define WP_PARTITIONS 1U
#define WP_GPT_ENTRIES 128U
#define WP_ENTRY_BLOCKS 32U
#define WP_READ_MAX 32768U
#define WP_SMALLER(A,B) ((A) < (B) ? (A) : (B))

extern EFI_STATUS EFIAPI J250fWpPrivateStage (VOID);
extern EFI_STATUS EFIAPI J250fWpPrivateRead (UINT32, UINT64, UINTN, VOID *);
extern EFI_STATUS EFIAPI J250fWpPrivateConceal (VOID);
extern VOID EFIAPI J250fWpPrivateRelease (VOID);

typedef struct {
  VENDOR_DEVICE_PATH Vendor;
  EFI_DEVICE_PATH_PROTOCOL End;
} WP_DEVICE_PATH;
typedef struct {
  CONST CHAR16 *Name;
  UINT64 Length;
  EFI_LBA First;
  EFI_LBA Sectors;
} WP_IMAGE;
typedef struct {
  EFI_BLOCK_IO_PROTOCOL Io;
  EFI_BLOCK_IO_MEDIA Media;
  WP_DEVICE_PATH Path;
  EFI_HANDLE Handle;
  BOOLEAN Active;
  BOOLEAN Isolated;
  EFI_LBA Last;
  WP_IMAGE Image[WP_PARTITIONS];
  EFI_PARTITION_ENTRY Entries[WP_GPT_ENTRIES];
  UINT8 Mbr[WP_SECTOR];
  UINT8 Primary[WP_SECTOR];
  UINT8 Backup[WP_SECTOR];
} WP_DISK;
STATIC WP_DISK mWp;
STATIC EFI_HANDLE mImageHandle;
/* Contract with QcomPkg DeviceBootManagerLib: published only after BDS
   BootFF10 is installed and ESP-only virtual topology has been verified.
   This is intentionally an in-memory marker, never an eMMC write. */
STATIC EFI_GUID mWpEspReadyGuid = {
  0xd7f18d20,0x49b1,0x4e41,{0x8d,0x1a,0x91,0x46,0x6c,0x7e,0x37,0xa2}
};
STATIC EFI_GUID mWpPathGuid = {
  0xe7b5ba2a,0xd586,0x4df0,{0x8e,0x28,0x2a,0xd5,0x5d,0x85,0x9c,0x81}
};
STATIC CONST EFI_GUID mWpDiskGuid = {
  0xae420040,0x13dd,0x41f2,{0xae,0x7f,0x0d,0xc3,0x58,0x54,0xc8,0xd7}
};
STATIC CONST EFI_GUID mEspTypeGuid = {
  0xc12a7328,0xf81f,0x11d2,{0xba,0x4b,0x00,0xa0,0xc9,0x3e,0xc9,0x3b}
};
STATIC CONST EFI_GUID mBasicTypeGuid = {
  0xebd0a0a2,0xb9e5,0x4433,{0x87,0xc0,0x68,0xb6,0xb7,0x26,0x99,0xc7}
};
STATIC VOID Set32 (UINT8 *P, UINT32 V) {
  P[0]=(UINT8)V;P[1]=(UINT8)(V>>8);P[2]=(UINT8)(V>>16);P[3]=(UINT8)(V>>24);
}
STATIC BOOLEAN NameIs (CONST CHAR16 *A, CONST CHAR16 *B) {
  UINTN I;
  for (I = 0; I < 36; I++) {
    if (A[I] != B[I]) return FALSE;
    if (!A[I]) return TRUE;
  }
  return FALSE;
}
STATIC BOOLEAN PathIsVirtual (CONST EFI_DEVICE_PATH_PROTOCOL *Path) {
  CONST VENDOR_DEVICE_PATH *Vendor;
  if (!Path || Path->Type != HARDWARE_DEVICE_PATH ||
      Path->SubType != HW_VENDOR_DP ||
      Path->Length[0] != sizeof(VENDOR_DEVICE_PATH) ||
      Path->Length[1] != 0) return FALSE;
  Vendor = (CONST VENDOR_DEVICE_PATH *)Path;
  return CompareGuid(&Vendor->Guid, &mWpPathGuid);
}
STATIC EFI_STATUS EFIAPI WpReset (EFI_BLOCK_IO_PROTOCOL *This,
                                  BOOLEAN Verify) {
  (VOID)Verify;
  return This == &mWp.Io && mWp.Active ? EFI_SUCCESS : EFI_NO_MEDIA;
}
STATIC EFI_STATUS EFIAPI WpWrite (EFI_BLOCK_IO_PROTOCOL *This,
    UINT32 MediaId, EFI_LBA Lba, UINTN Bytes, VOID *Buffer) {
  (VOID)MediaId;(VOID)Lba;(VOID)Bytes;(VOID)Buffer;
  return This == &mWp.Io ? EFI_WRITE_PROTECTED : EFI_INVALID_PARAMETER;
}
STATIC EFI_STATUS EFIAPI WpFlush (EFI_BLOCK_IO_PROTOCOL *This) {
  return This == &mWp.Io && mWp.Active ? EFI_SUCCESS : EFI_NO_MEDIA;
}
STATIC EFI_STATUS EFIAPI WpRead (EFI_BLOCK_IO_PROTOCOL *This,
    UINT32 MediaId, EFI_LBA Lba, UINTN Bytes, VOID *Buffer) {
  UINT8 *Out = (UINT8 *)Buffer;
  UINTN Sectors;
  if (This != &mWp.Io || !mWp.Active || !mWp.Isolated)
    return EFI_NO_MEDIA;
  if (MediaId != mWp.Media.MediaId) return EFI_MEDIA_CHANGED;
  if (!Bytes) return EFI_SUCCESS;
  if (!Out) return EFI_INVALID_PARAMETER;
  if (Bytes % WP_SECTOR) return EFI_BAD_BUFFER_SIZE;
  Sectors = Bytes / WP_SECTOR;
  if (Lba > mWp.Last || Sectors - 1 > mWp.Last - Lba)
    return EFI_INVALID_PARAMETER;
  while (Sectors) {
    CONST UINT8 *Fixed = NULL;
    UINTN I;
    for (I = 0; I < WP_PARTITIONS; I++) {
      WP_IMAGE *File = &mWp.Image[I];
      if (Lba >= File->First && Lba - File->First < File->Sectors) {
        UINTN Chunk = (UINTN)WP_SMALLER((UINT64)Sectors,
                                   File->Sectors - (Lba - File->First));
        EFI_STATUS Status;
        Chunk = WP_SMALLER(Chunk, WP_READ_MAX / WP_SECTOR);
        Status = J250fWpPrivateRead((UINT32)I,
                (Lba - File->First) * WP_SECTOR, Chunk * WP_SECTOR, Out);
        if (EFI_ERROR(Status)) return Status;
        Out += Chunk * WP_SECTOR;
        Lba += Chunk;
        Sectors -= Chunk;
        break;
      }
    }
    if (I < WP_PARTITIONS) continue;
    if (Lba == 0) Fixed = mWp.Mbr;
    else if (Lba == 1) Fixed = mWp.Primary;
    else if (Lba >= 2 && Lba < 2 + WP_ENTRY_BLOCKS)
      Fixed = (CONST UINT8 *)mWp.Entries + (UINTN)(Lba - 2) * WP_SECTOR;
    else if (Lba == mWp.Last) Fixed = mWp.Backup;
    else if (Lba >= mWp.Last - WP_ENTRY_BLOCKS && Lba < mWp.Last)
      Fixed = (CONST UINT8 *)mWp.Entries +
              (UINTN)(Lba - (mWp.Last - WP_ENTRY_BLOCKS)) * WP_SECTOR;
    if (Fixed) CopyMem(Out, Fixed, WP_SECTOR);
    else ZeroMem(Out, WP_SECTOR);
    Out += WP_SECTOR;
    Lba++;
    Sectors--;
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS MakeGpt (VOID) {
  EFI_PARTITION_TABLE_HEADER Header;
  EFI_STATUS Status;
  UINT64 Next = 2048;
  UINT32 Crc;
  UINTN I, J;
  mWp.Image[0].Name = L"EFIESP";
  mWp.Image[0].Length = 33554432ULL;
  ZeroMem(mWp.Entries, sizeof(mWp.Entries));
  if (sizeof(EFI_PARTITION_ENTRY) != 128 ||
      sizeof(mWp.Entries) != WP_ENTRY_BLOCKS * WP_SECTOR)
    return EFI_UNSUPPORTED;
  for (I = 0; I < WP_PARTITIONS; I++) {
    EFI_PARTITION_ENTRY *Entry = &mWp.Entries[I];
    WP_IMAGE *File = &mWp.Image[I];
    if (File->Length % WP_SECTOR ||
        File->Length / WP_SECTOR > MAX_UINT64 - Next - 4096)
      return EFI_INVALID_PARAMETER;
    File->First = Next;
    File->Sectors = File->Length / WP_SECTOR;
    Entry->PartitionTypeGUID = I == 0 ? mEspTypeGuid : mBasicTypeGuid;
    Entry->UniquePartitionGUID = mWpDiskGuid;
    Entry->UniquePartitionGUID.Data1 += (UINT32)I + 1;
    Entry->StartingLBA = Next;
    Entry->EndingLBA = Next + File->Sectors - 1;
    for (J=0; J<35 && File->Name[J]; J++)
      Entry->PartitionName[J] = File->Name[J];
    Next = (Next + File->Sectors + 2047ULL) & ~2047ULL;
  }
  mWp.Last = Next + 33;
  Status = gBS->CalculateCrc32(mWp.Entries,sizeof(mWp.Entries),&Crc);
  if (EFI_ERROR(Status)) return Status;
  ZeroMem(&Header,sizeof(Header));
  Header.Header.Signature=EFI_PTAB_HEADER_ID;
  Header.Header.Revision=0x00010000;
  Header.Header.HeaderSize=sizeof(Header);
  Header.MyLBA=1;
  Header.AlternateLBA=mWp.Last;
  Header.FirstUsableLBA=34;
  Header.LastUsableLBA=mWp.Last-33;
  Header.DiskGUID=mWpDiskGuid;
  Header.PartitionEntryLBA=2;
  Header.NumberOfPartitionEntries=WP_GPT_ENTRIES;
  Header.SizeOfPartitionEntry=sizeof(EFI_PARTITION_ENTRY);
  Header.PartitionEntryArrayCRC32=Crc;
  Status=gBS->CalculateCrc32(&Header,sizeof(Header),&Crc);
  if (EFI_ERROR(Status)) return Status;
  Header.Header.CRC32=Crc;
  ZeroMem(mWp.Primary,WP_SECTOR);
  CopyMem(mWp.Primary,&Header,sizeof(Header));
  Header.Header.CRC32=0;
  Header.MyLBA=mWp.Last;
  Header.AlternateLBA=1;
  Header.PartitionEntryLBA=mWp.Last-WP_ENTRY_BLOCKS;
  Status=gBS->CalculateCrc32(&Header,sizeof(Header),&Crc);
  if (EFI_ERROR(Status)) return Status;
  Header.Header.CRC32=Crc;
  ZeroMem(mWp.Backup,WP_SECTOR);
  CopyMem(mWp.Backup,&Header,sizeof(Header));
  ZeroMem(mWp.Mbr,WP_SECTOR);
  mWp.Mbr[446+4]=0xee;
  Set32(mWp.Mbr+446+8,1);
  Set32(mWp.Mbr+446+12,mWp.Last>MAX_UINT32?MAX_UINT32:(UINT32)mWp.Last);
  mWp.Mbr[510]=0x55;mWp.Mbr[511]=0xaa;
  return EFI_SUCCESS;
}

/* A valid view has one virtual physical disk and exactly one logical GPT
   partition handles, all marked read-only. No other Block I/O/PartitionInfo
   handles may be present, even if they are not from eMMC. */
STATIC EFI_STATUS VerifyTopology (UINTN *Disks, UINTN *Partitions) {
  EFI_HANDLE *Handles = NULL;
  EFI_STATUS Status;
  UINTN Count=0,I,Raw=0,PartCount=0;
  BOOLEAN Seen[WP_PARTITIONS]={FALSE};
  if (!mWp.Active || !mWp.Isolated) return EFI_NOT_READY;
  Status=gBS->LocateHandleBuffer(ByProtocol,&gEfiBlockIoProtocolGuid,
                                  NULL,&Count,&Handles);
  if (EFI_ERROR(Status)) return Status;
  for (I=0;I<Count;I++) {
    EFI_BLOCK_IO_PROTOCOL *Io=NULL;
    EFI_DEVICE_PATH_PROTOCOL *Path=NULL;
    if (EFI_ERROR(gBS->HandleProtocol(Handles[I],
                     &gEfiBlockIoProtocolGuid,(VOID **)&Io)) ||
        EFI_ERROR(gBS->HandleProtocol(Handles[I],
                     &gEfiDevicePathProtocolGuid,(VOID **)&Path)) ||
        !Io || !Io->Media || !PathIsVirtual(Path) || !Io->Media->ReadOnly) {
      Status=EFI_ACCESS_DENIED;goto Done;
    }
    if (!Io->Media->LogicalPartition) {
      if (Handles[I] != mWp.Handle || Io != &mWp.Io) {
        Status=EFI_ACCESS_DENIED;goto Done;
      }
      Raw++;
    } else PartCount++;
  }
  Status=(Count == WP_PARTITIONS+1 && Raw == 1 &&
          PartCount == WP_PARTITIONS) ? EFI_SUCCESS : EFI_ACCESS_DENIED;
Done:
  if (Disks) *Disks=Raw;
  if (Partitions) *Partitions=PartCount;
  gBS->FreePool(Handles);
  if (EFI_ERROR(Status)) return Status;
  Handles=NULL;Count=0;
  Status=gBS->LocateHandleBuffer(ByProtocol,&gEfiPartitionInfoProtocolGuid,
                                  NULL,&Count,&Handles);
  if (EFI_ERROR(Status)) return Status;
  if (Count != WP_PARTITIONS) Status=EFI_ACCESS_DENIED;
  for (I=0;I<Count && !EFI_ERROR(Status);I++) {
    EFI_PARTITION_INFO_PROTOCOL *Info=NULL;
    UINTN J;
    if (EFI_ERROR(gBS->HandleProtocol(Handles[I],
            &gEfiPartitionInfoProtocolGuid,(VOID **)&Info)) ||
        !Info || Info->Type != PARTITION_TYPE_GPT) {
      Status=EFI_ACCESS_DENIED;break;
    }
    for (J=0;J<WP_PARTITIONS;J++) {
      if (NameIs(Info->Info.Gpt.PartitionName,mWp.Image[J].Name) &&
          Info->Info.Gpt.StartingLBA == mWp.Image[J].First &&
          Info->Info.Gpt.EndingLBA ==
              mWp.Image[J].First+mWp.Image[J].Sectors-1 &&
          !Seen[J]) break;
    }
    if (J == WP_PARTITIONS) Status=EFI_ACCESS_DENIED;
    else Seen[J]=TRUE;
  }
  for (I=0;I<WP_PARTITIONS;I++)
    if (!Seen[I]) Status=EFI_ACCESS_DENIED;
  gBS->FreePool(Handles);
  return Status;
}

STATIC EFI_STATUS WpMount (VOID) {
  EFI_STATUS Status;
  if (mWp.Active) return EFI_ALREADY_STARTED;
  ZeroMem(&mWp,sizeof(mWp));
  /* Stage private read-only ext4 inode references BEFORE de-enumerating the
     original GPT. After this call the backing is independent of EFI handles. */
  Status=J250fWpPrivateStage();
  if (EFI_ERROR(Status)) return Status;
  Status=MakeGpt();
  if (EFI_ERROR(Status)) { J250fWpPrivateRelease();return Status; }
  /* Disconnect the physical controller and uninstall its public protocols.
     If any other disk/partition handle survives, fail closed. */
  Status=J250fWpPrivateConceal();
  if (EFI_ERROR(Status)) return Status;
  mWp.Isolated=TRUE;
  mWp.Media.MediaId=1;
  mWp.Media.MediaPresent=TRUE;
  mWp.Media.ReadOnly=TRUE;
  mWp.Media.RemovableMedia=FALSE;
  mWp.Media.LogicalPartition=FALSE;
  mWp.Media.WriteCaching=FALSE;
  mWp.Media.BlockSize=WP_SECTOR;
  mWp.Media.IoAlign=1;
  mWp.Media.LastBlock=mWp.Last;
  mWp.Io.Revision=EFI_BLOCK_IO_PROTOCOL_REVISION;
  mWp.Io.Media=&mWp.Media;
  mWp.Io.Reset=WpReset;
  mWp.Io.ReadBlocks=WpRead;
  mWp.Io.WriteBlocks=WpWrite;
  mWp.Io.FlushBlocks=WpFlush;
  mWp.Path.Vendor.Header.Type=HARDWARE_DEVICE_PATH;
  mWp.Path.Vendor.Header.SubType=HW_VENDOR_DP;
  mWp.Path.Vendor.Header.Length[0]=sizeof(VENDOR_DEVICE_PATH);
  mWp.Path.Vendor.Header.Length[1]=0;
  mWp.Path.Vendor.Guid=mWpPathGuid;
  mWp.Path.End.Type=END_DEVICE_PATH_TYPE;
  mWp.Path.End.SubType=END_ENTIRE_DEVICE_PATH_SUBTYPE;
  mWp.Path.End.Length[0]=sizeof(EFI_DEVICE_PATH_PROTOCOL);
  mWp.Path.End.Length[1]=0;
  mWp.Active=TRUE;
  Status=gBS->InstallMultipleProtocolInterfaces(&mWp.Handle,
           &gEfiBlockIoProtocolGuid,&mWp.Io,
           &gEfiDevicePathProtocolGuid,&mWp.Path,NULL);
  if (EFI_ERROR(Status)) {mWp.Active=FALSE;return Status;}
  (VOID)gBS->ConnectController(mWp.Handle,NULL,NULL,TRUE);
  return VerifyTopology(NULL,NULL);
}

STATIC EFI_HANDLE LocateVirtualEsp (VOID) {
  EFI_HANDLE *Handles=NULL,Result=NULL;
  EFI_STATUS Status;
  UINTN Count=0,I;
  Status=gBS->LocateHandleBuffer(ByProtocol,
                &gEfiSimpleFileSystemProtocolGuid,NULL,&Count,&Handles);
  if (EFI_ERROR(Status)) return NULL;
  for (I=0;I<Count;I++) {
    EFI_PARTITION_INFO_PROTOCOL *Info=NULL;
    EFI_DEVICE_PATH_PROTOCOL *Path=NULL;
    if (EFI_ERROR(gBS->HandleProtocol(Handles[I],
             &gEfiPartitionInfoProtocolGuid,(VOID **)&Info)) ||
        EFI_ERROR(gBS->HandleProtocol(Handles[I],
             &gEfiDevicePathProtocolGuid,(VOID **)&Path)) ||
        !Info || Info->Type != PARTITION_TYPE_GPT ||
        !NameIs(Info->Info.Gpt.PartitionName,L"EFIESP") ||
        !PathIsVirtual(Path)) continue;
    Result=Handles[I];break;
  }
  gBS->FreePool(Handles);
  return Result;
}

/* Register the ESP boot manager for normal Project Mu BDS.
   Do not call LoadImage/StartImage in DXE: BDS must run after EndOfDxe.
   The platform's emulated NV variable mode keeps Boot#### RAM-backed. */
STATIC EFI_STATUS RegisterBdsBootOption (VOID) {
  STATIC CONST CHAR16 *Loader=L"\\Windows\\BOOT\\EFI\\BOOTMGFW.EFI";
  STATIC CONST CHAR16 *Description=L"Windows Phone 8.1 EFI-only test";
  EFI_HANDLE Esp;
  EFI_DEVICE_PATH_PROTOCOL *Path;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
  EFI_FILE_PROTOCOL *Root=NULL,*File=NULL;
  EFI_STATUS Status;
  UINTN PathSize,DescriptionSize,OptionSize;
  UINT8 *Option;
  UINT32 Attributes=1U; /* LOAD_OPTION_ACTIVE */
  UINT16 FilePathLength;
  UINT16 BootOrder=0xff10;
  UINT32 VarAttrs=EFI_VARIABLE_NON_VOLATILE |
    EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS;
  Status=VerifyTopology(NULL,NULL);
  if (EFI_ERROR(Status)) return Status;
  Esp=LocateVirtualEsp();
  if (!Esp) return EFI_NOT_FOUND;
  Status=gBS->HandleProtocol(Esp,&gEfiSimpleFileSystemProtocolGuid,(VOID **)&Fs);
  if (EFI_ERROR(Status)) return Status;
  Status=Fs->OpenVolume(Fs,&Root);
  if (EFI_ERROR(Status)) return Status;
  Status=Root->Open(Root,&File,(CHAR16 *)Loader,EFI_FILE_MODE_READ,0);
  if (EFI_ERROR(Status)) {Root->Close(Root);return Status;}
  File->Close(File);
  Root->Close(Root);
  Path=FileDevicePath(Esp,(CHAR16 *)Loader);
  if (!Path) return EFI_OUT_OF_RESOURCES;
  PathSize=GetDevicePathSize(Path);
  DescriptionSize=StrSize(Description);
  if (PathSize > MAX_UINT16 ||
      PathSize > MAX_UINTN-DescriptionSize-6) {
    gBS->FreePool(Path);
    return EFI_INVALID_PARAMETER;
  }
  FilePathLength=(UINT16)PathSize;
  OptionSize=6+DescriptionSize+PathSize;
  Status=gBS->AllocatePool(EfiBootServicesData,OptionSize,(VOID **)&Option);
  if (EFI_ERROR(Status)) {gBS->FreePool(Path);return Status;}
  CopyMem(Option,&Attributes,sizeof(Attributes));
  CopyMem(Option+4,&FilePathLength,sizeof(FilePathLength));
  CopyMem(Option+6,Description,DescriptionSize);
  CopyMem(Option+6+DescriptionSize,Path,PathSize);
  gBS->FreePool(Path);
  Status=gRT->SetVariable(L"BootFF10",&gEfiGlobalVariableGuid,
                           VarAttrs,OptionSize,Option);
  gBS->FreePool(Option);
  if (EFI_ERROR(Status)) return Status;
  return gRT->SetVariable(L"BootOrder",&gEfiGlobalVariableGuid,
                           VarAttrs,sizeof(BootOrder),&BootOrder);
}
EFI_STATUS EFIAPI J250fWpPrepareBds (VOID) {
  EFI_STATUS Status=WpMount();
  if (EFI_ERROR(Status)) return Status;
  Status=RegisterBdsBootOption();
  if (EFI_ERROR(Status)) return Status;
  /* The marker must never be published if a mount or boot option fails. */
  return gBS->InstallProtocolInterface(&mWp.Handle,
            &mWpEspReadyGuid, EFI_NATIVE_INTERFACE, (VOID *)&mWp);
}

STATIC EFI_STATUS TryBootManager (BOOLEAN Execute,CHAR8 *Out,UINTN Cap) {
  STATIC CONST CHAR16 *Paths[]={
    L"\\Windows\\BOOT\\EFI\\BOOTMGFW.EFI",
    L"\\EFI\\BOOT\\BOOTARM.EFI"
  };
  EFI_HANDLE Esp,Image=NULL;
  EFI_STATUS Status=EFI_NOT_FOUND;
  UINTN I;
  Status=VerifyTopology(NULL,NULL);
  if (EFI_ERROR(Status)) return Status;
  Esp=LocateVirtualEsp();
  if (!Esp) return EFI_NOT_FOUND;
  for (I=0;I<sizeof(Paths)/sizeof(Paths[0]);I++) {
    EFI_DEVICE_PATH_PROTOCOL *Path=FileDevicePath(Esp,(CHAR16 *)Paths[I]);
    if (!Path) return EFI_OUT_OF_RESOURCES;
    Status=gBS->LoadImage(FALSE,mImageHandle,Path,NULL,0,&Image);
    gBS->FreePool(Path);
    if (!EFI_ERROR(Status)) break;
  }
  if (EFI_ERROR(Status)) return Status;
  if (!Execute) return gBS->UnloadImage(Image);
  /* UEFI protocols are hidden, but the guest OS can still access the eMMC
     controller directly. There is no Windows boot-critical storage driver. */
  (VOID)AsciiSPrint(Out,Cap,"Starting WP8.1 Boot Manager (NO KERNEL ISOLATION).\r\n");
  Status=gBS->StartImage(Image,NULL,NULL);
  (VOID)gBS->UnloadImage(Image);
  return Status;
}
VOID EFIAPI J250fWpInit (EFI_HANDLE Parent) {mImageHandle=Parent;}
BOOLEAN EFIAPI J250fWpCommand (CONST CHAR8 *Cmd,CHAR8 *Out,
                               UINTN Cap,UINTN *Used) {
  EFI_STATUS Status;
  UINTN DiskCount=0,PartitionCount=0;
  if (AsciiStrCmp(Cmd,"wp") != 0 && AsciiStrnCmp(Cmd,"wp ",3) != 0)
    return FALSE;
  if (!Out || !Cap || !Used) return TRUE;
  if (AsciiStrCmp(Cmd,"wp")==0 || AsciiStrCmp(Cmd,"wp status")==0) {
    *Used=AsciiSPrint(Out,Cap,
      "WP81 virtual disk: %a; UEFI eMMC concealed: %a\r\n"
      "ESP-only BDS test: EFIESP is the sole published partition.\r\n"
      "No Windows kernel MMIO isolation.\r\n"
      "Commands: wp mount | wp verify | wp probe | wp boot CONFIRM\r\n",
      mWp.Active ? "MOUNTED" : "not mounted",
      mWp.Isolated ? "YES" : "NO");
    return TRUE;
  }
  if (AsciiStrCmp(Cmd,"wp mount")==0) {
    Status=WpMount();
    *Used=AsciiSPrint(Out,Cap,
      "WP81 virtual GPT mount: %r; public UEFI eMMC concealed: %a\r\n",
      Status,mWp.Isolated ? "YES" : "NO");
    return TRUE;
  }
  if (AsciiStrCmp(Cmd,"wp verify")==0) {
    Status=VerifyTopology(&DiskCount,&PartitionCount);
    *Used=AsciiSPrint(Out,Cap,
      "WP81 UEFI-only GPT: %r; disks=%u partitions=%u\r\n",
      Status,(UINT32)DiskCount,(UINT32)PartitionCount);
    return TRUE;
  }
  if (AsciiStrCmp(Cmd,"wp probe")==0 ||
      AsciiStrCmp(Cmd,"wp boot CONFIRM")==0) {
    BOOLEAN Execute=AsciiStrCmp(Cmd,"wp boot CONFIRM")==0;
    Status=TryBootManager(Execute,Out,Cap);
    *Used=AsciiSPrint(Out,Cap,"WP81 %a: %r\r\n",
        Execute ? "StartImage" : "LoadImage/UnloadImage",Status);
    return TRUE;
  }
  *Used=AsciiSPrint(Out,Cap,
    "Usage: wp status | wp mount | wp verify | wp probe | wp boot CONFIRM\r\n"
    "boot CONFIRM is not safe against Windows kernel eMMC MMIO writes.\r\n");
  return TRUE;
}
