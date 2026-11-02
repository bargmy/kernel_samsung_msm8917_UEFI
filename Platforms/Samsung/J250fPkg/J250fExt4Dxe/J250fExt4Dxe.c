/** @file
  J250F read-only ext4 EFI Simple File System driver.
  Installs on GPT logical Block I/O handles; never calls WriteBlocks.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include "Ext4Read.h"
#include <Protocol/DriverBinding.h>
#include <Protocol/DevicePath.h>
#include <Guid/FileInfo.h>
#include <Guid/FileSystemInfo.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>

STATIC EFI_GUID mExt4VolumeGuid = {
  0x7cd5d62a, 0x8f37, 0x4eb6,
  {0x9d,0x10,0x3d,0xeb,0xb3,0x85,0xaf,0x64}
};

#define EXT4_DIR_MODE 0x4000
#define EXT4_FILE_MODE 0x8000

typedef struct {
  EFI_FILE_PROTOCOL File;
  EXT4_VOLUME *Volume;
  EXT4_NODE Node;
  UINT64 Position;
  UINT64 CachedBlock;
  CHAR16 Name[EXT4_NAME_MAX + 1];
  UINT8 DirCache[EXT4_MAX_BLOCK];
} EXT4_FILE;

STATIC EFI_STATUS EFIAPI FileOpen (EFI_FILE_PROTOCOL *, EFI_FILE_PROTOCOL **,
                                   CHAR16 *, UINT64, UINT64);
STATIC EFI_STATUS EFIAPI FileClose (EFI_FILE_PROTOCOL *);
STATIC EFI_STATUS EFIAPI FileDelete (EFI_FILE_PROTOCOL *);
STATIC EFI_STATUS EFIAPI FileRead (EFI_FILE_PROTOCOL *, UINTN *, VOID *);
STATIC EFI_STATUS EFIAPI FileWrite (EFI_FILE_PROTOCOL *, UINTN *, VOID *);
STATIC EFI_STATUS EFIAPI FileGetPosition (EFI_FILE_PROTOCOL *, UINT64 *);
STATIC EFI_STATUS EFIAPI FileSetPosition (EFI_FILE_PROTOCOL *, UINT64);
STATIC EFI_STATUS EFIAPI FileGetInfo (EFI_FILE_PROTOCOL *, EFI_GUID *,
                                      UINTN *, VOID *);
STATIC EFI_STATUS EFIAPI FileSetInfo (EFI_FILE_PROTOCOL *, EFI_GUID *,
                                      UINTN, VOID *);
STATIC EFI_STATUS EFIAPI FileFlush (EFI_FILE_PROTOCOL *);

STATIC BOOLEAN IsDirectory (EXT4_FILE *F) {
  return (F->Node.Mode & 0xf000) == EXT4_DIR_MODE;
}
STATIC EFI_STATUS NewFile (EXT4_VOLUME *V, UINT32 Inode,
                           CONST CHAR16 *Name, EFI_FILE_PROTOCOL **Result) {
  EXT4_FILE *F;
  EFI_STATUS Status;
  UINTN I;
  if (!Result) return EFI_INVALID_PARAMETER;
  *Result = NULL;
  Status = gBS->AllocatePool(EfiBootServicesData, sizeof(*F), (VOID **)&F);
  if (EFI_ERROR(Status)) return Status;
  ZeroMem(F, sizeof(*F));
  F->CachedBlock = MAX_UINT64;
  F->Volume = V;
  Status = Ext4LoadInode(V, Inode, &F->Node);
  if (EFI_ERROR(Status)) { gBS->FreePool(F); return Status; }
  for (I = 0; I < EXT4_NAME_MAX && Name[I]; I++) F->Name[I] = Name[I];
  F->Name[I] = 0;
  F->File.Revision = EFI_FILE_PROTOCOL_REVISION;
  F->File.Open = FileOpen;
  F->File.Close = FileClose;
  F->File.Delete = FileDelete;
  F->File.Read = FileRead;
  F->File.Write = FileWrite;
  F->File.GetPosition = FileGetPosition;
  F->File.SetPosition = FileSetPosition;
  F->File.GetInfo = FileGetInfo;
  F->File.SetInfo = FileSetInfo;
  F->File.Flush = FileFlush;
  V->OpenCount++;
  *Result = &F->File;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI OpenVolume (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
                                     EFI_FILE_PROTOCOL **Root) {
  EXT4_VOLUME *V;
  if (!This || !Root) return EFI_INVALID_PARAMETER;
  V = (EXT4_VOLUME *)((UINT8 *)This - OFFSET_OF(EXT4_VOLUME, Simple));
  if (V->Signature != EXT4_SIGNATURE) return EFI_INVALID_PARAMETER;
  return NewFile(V, 2, L"\\", Root);
}

STATIC EFI_STATUS EFIAPI FileOpen (EFI_FILE_PROTOCOL *This,
                                   EFI_FILE_PROTOCOL **NewHandle,
                                   CHAR16 *Path, UINT64 OpenMode,
                                   UINT64 Attributes) {
  EXT4_FILE *Base = (EXT4_FILE *)This;
  EXT4_VOLUME *V;
  EXT4_NODE Current;
  CONST CHAR16 *P;
  CHAR16 Part[EXT4_NAME_MAX + 1], Final[EXT4_NAME_MAX + 1];
  UINT32 Id;
  EFI_STATUS Status;
  if (!Base || !NewHandle || !Path || !Base->Volume)
    return EFI_INVALID_PARAMETER;
  *NewHandle = NULL;
  if (OpenMode != EFI_FILE_MODE_READ || Attributes != 0)
    return EFI_WRITE_PROTECTED;
  V = Base->Volume;
  if (V->Signature != EXT4_SIGNATURE) return EFI_INVALID_PARAMETER;
  P = Path;
  if (*P == L'\\' || *P == L'/') {
    Status = Ext4LoadInode(V, 2, &Current);
    if (EFI_ERROR(Status)) return Status;
    Id = 2;
  } else {
    Current = Base->Node;
    Id = Current.Number;
  }
  CopyMem(Final, L"\\", sizeof(CHAR16) * 2);
  while (*P) {
    UINTN Count = 0, I;
    while (*P == L'\\' || *P == L'/') P++;
    if (!*P) break;
    if ((Current.Mode & 0xf000) != EXT4_DIR_MODE) return EFI_NOT_FOUND;
    while (P[Count] && P[Count] != L'\\' && P[Count] != L'/') {
      if (Count == EXT4_NAME_MAX || P[Count] > 0x7f ||
          P[Count] < 0x20) return EFI_UNSUPPORTED;
      Count++;
    }
    if (!Count) break;
    for (I = 0; I < Count; I++) Part[I] = P[I];
    Part[Count] = 0;
    P += Count;
    if (Count == 1 && Part[0] == L'.') continue;
    if (Count == 2 && Part[0] == L'.' && Part[1] == L'.')
      return EFI_ACCESS_DENIED; /* no parent traversal */
    Status = Ext4Lookup(V, &Current, Part, &Id);
    if (EFI_ERROR(Status)) return Status;
    Status = Ext4LoadInode(V, Id, &Current);
    if (EFI_ERROR(Status)) return Status;
    CopyMem(Final, Part, (Count + 1) * sizeof(CHAR16));
  }
  return NewFile(V, Id, Final, NewHandle);
}

STATIC EFI_STATUS EFIAPI FileClose (EFI_FILE_PROTOCOL *This) {
  EXT4_FILE *F = (EXT4_FILE *)This;
  if (!F || !F->Volume || F->Volume->Signature != EXT4_SIGNATURE)
    return EFI_INVALID_PARAMETER;
  F->Volume->OpenCount--;
  gBS->FreePool(F);
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI FileDelete (EFI_FILE_PROTOCOL *This) {
  EFI_STATUS Status = FileClose(This);
  return EFI_ERROR(Status) ? Status : EFI_WARN_DELETE_FAILURE;
}
STATIC EFI_STATUS FileInfoFromNode (CONST EXT4_NODE *Node,
                                    CONST CHAR16 *Name,
                                    UINTN *Size, VOID *Buffer) {
  UINTN NameLen = 0, Required;
  EFI_FILE_INFO *Info;
  if (!Size) return EFI_INVALID_PARAMETER;
  while (Name[NameLen] && NameLen < EXT4_NAME_MAX) NameLen++;
  Required = OFFSET_OF(EFI_FILE_INFO, FileName) +
             (NameLen + 1) * sizeof(CHAR16);
  if (*Size < Required) { *Size = Required; return EFI_BUFFER_TOO_SMALL; }
  if (!Buffer) return EFI_INVALID_PARAMETER;
  ZeroMem(Buffer, Required);
  Info = (EFI_FILE_INFO *)Buffer;
  Info->Size = Required;
  Info->FileSize = Node->Size;
  Info->PhysicalSize = Node->Size; /* sparse allocation not inferred */
  Info->Attribute = EFI_FILE_READ_ONLY;
  if ((Node->Mode & 0xf000) == EXT4_DIR_MODE)
    Info->Attribute |= EFI_FILE_DIRECTORY;
  CopyMem(Info->FileName, Name, (NameLen + 1) * sizeof(CHAR16));
  *Size = Required;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI FileRead (EFI_FILE_PROTOCOL *This,
                                   UINTN *Size, VOID *Buffer) {
  EXT4_FILE *F = (EXT4_FILE *)This;
  EFI_STATUS Status;
  UINTN Length;
  if (!F || !F->Volume || !Size || (*Size && !Buffer))
    return EFI_INVALID_PARAMETER;
  if (IsDirectory(F)) {
    UINT64 NextPos = F->Position;
    UINT32 Ino;
    CHAR16 Name[EXT4_NAME_MAX + 1];
    EXT4_NODE Node;
    while (TRUE) {
      Status = Ext4DirNext(F->Volume, &F->Node, &NextPos, &F->CachedBlock,
                           F->DirCache, &Ino, Name);
      if (Status == EFI_NOT_FOUND) { *Size = 0; return EFI_SUCCESS; }
      if (EFI_ERROR(Status)) return Status;
      if ((Name[0] == L'.' && Name[1] == 0) ||
          (Name[0] == L'.' && Name[1] == L'.' && Name[2] == 0))
        continue;
      Status = Ext4LoadInode(F->Volume, Ino, &Node);
      if (EFI_ERROR(Status)) return Status;
      Status = FileInfoFromNode(&Node, Name, Size, Buffer);
      if (!EFI_ERROR(Status)) F->Position = NextPos;
      return Status;
    }
  }
  if (F->Position >= F->Node.Size) { *Size = 0; return EFI_SUCCESS; }
  Length = *Size;
  if ((UINT64)Length > F->Node.Size - F->Position)
    Length = (UINTN)(F->Node.Size - F->Position);
  Status = Ext4ReadFile(F->Volume, &F->Node, F->Position, Length, Buffer);
  if (!EFI_ERROR(Status)) { F->Position += Length; *Size = Length; }
  return Status;
}
STATIC EFI_STATUS EFIAPI FileWrite (EFI_FILE_PROTOCOL *This,
                                    UINTN *Size, VOID *Buffer) {
  (VOID)This; (VOID)Size; (VOID)Buffer;
  return EFI_WRITE_PROTECTED;
}
STATIC EFI_STATUS EFIAPI FileGetPosition (EFI_FILE_PROTOCOL *This,
                                          UINT64 *Position) {
  EXT4_FILE *F = (EXT4_FILE *)This;
  if (!F || !Position) return EFI_INVALID_PARAMETER;
  if (IsDirectory(F)) return EFI_UNSUPPORTED;
  *Position = F->Position;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI FileSetPosition (EFI_FILE_PROTOCOL *This,
                                          UINT64 Position) {
  EXT4_FILE *F = (EXT4_FILE *)This;
  if (!F) return EFI_INVALID_PARAMETER;
  if (IsDirectory(F)) {
    if (Position) return EFI_UNSUPPORTED;
    F->Position = 0;
    F->CachedBlock = MAX_UINT64;
    return EFI_SUCCESS;
  }
  F->Position = Position == MAX_UINT64 ? F->Node.Size : Position;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI FileGetInfo (EFI_FILE_PROTOCOL *This,
                                      EFI_GUID *InformationType,
                                      UINTN *Size, VOID *Buffer) {
  EXT4_FILE *F = (EXT4_FILE *)This;
  EXT4_VOLUME *V;
  if (!F || !F->Volume || !InformationType || !Size)
    return EFI_INVALID_PARAMETER;
  V = F->Volume;
  if (CompareGuid(InformationType, &gEfiFileInfoGuid))
    return FileInfoFromNode(&F->Node, F->Name, Size, Buffer);
  if (CompareGuid(InformationType, &gEfiFileSystemInfoGuid)) {
    UINTN LabelLen = 0, I, Needed;
    EFI_FILE_SYSTEM_INFO *Info;
    while (LabelLen < 16 && V->Super[120 + LabelLen]) LabelLen++;
    Needed = OFFSET_OF(EFI_FILE_SYSTEM_INFO, VolumeLabel) +
             (LabelLen + 1) * sizeof(CHAR16);
    if (*Size < Needed) { *Size = Needed; return EFI_BUFFER_TOO_SMALL; }
    if (!Buffer) return EFI_INVALID_PARAMETER;
    ZeroMem(Buffer, Needed);
    Info = (EFI_FILE_SYSTEM_INFO *)Buffer;
    Info->Size = Needed;
    Info->ReadOnly = TRUE;
    Info->VolumeSize = V->BlockCount * V->BlockSize;
    Info->FreeSpace = V->FreeBlocks * V->BlockSize;
    Info->BlockSize = V->BlockSize;
    for (I = 0; I < LabelLen; I++)
      Info->VolumeLabel[I] = V->Super[120 + I] < 0x80
                              ? V->Super[120 + I] : L'?';
    *Size = Needed;
    return EFI_SUCCESS;
  }
  return EFI_UNSUPPORTED;
}
STATIC EFI_STATUS EFIAPI FileSetInfo (EFI_FILE_PROTOCOL *This,
                                      EFI_GUID *Type,
                                      UINTN Size, VOID *Buffer) {
  (VOID)This; (VOID)Type; (VOID)Size; (VOID)Buffer;
  return EFI_WRITE_PROTECTED;
}
STATIC EFI_STATUS EFIAPI FileFlush (EFI_FILE_PROTOCOL *This) {
  return This ? EFI_SUCCESS : EFI_INVALID_PARAMETER; /* no dirty buffers */
}

STATIC EFI_STATUS OpenBlock (EFI_DRIVER_BINDING_PROTOCOL *This,
                             EFI_HANDLE Controller,
                             EFI_BLOCK_IO_PROTOCOL **Block) {
  return gBS->OpenProtocol(Controller, &gEfiBlockIoProtocolGuid,
                          (VOID **)Block, This->DriverBindingHandle,
                          Controller, EFI_OPEN_PROTOCOL_BY_DRIVER);
}
STATIC VOID FreeVolume (EXT4_VOLUME *V) {
  if (V) {
    if (V->ScratchAllocation) gBS->FreePool(V->ScratchAllocation);
    gBS->FreePool(V);
  }
}
STATIC EFI_STATUS EFIAPI DriverSupported (
  EFI_DRIVER_BINDING_PROTOCOL *This, EFI_HANDLE Controller,
  EFI_DEVICE_PATH_PROTOCOL *RemainingDevicePath
) {
  EFI_BLOCK_IO_PROTOCOL *Block = NULL;
  EXT4_VOLUME Probe;
  EFI_STATUS Status;
  (VOID)RemainingDevicePath;
  Status = OpenBlock(This, Controller, &Block);
  if (EFI_ERROR(Status)) return Status;
  ZeroMem(&Probe, sizeof(Probe));
  Probe.Disk = Block;
  Status = Ext4Probe(&Probe);
  if (Probe.ScratchAllocation) gBS->FreePool(Probe.ScratchAllocation);
  gBS->CloseProtocol(Controller, &gEfiBlockIoProtocolGuid,
                     This->DriverBindingHandle, Controller);
  return Status;
}
STATIC EFI_STATUS EFIAPI DriverStart (
  EFI_DRIVER_BINDING_PROTOCOL *This, EFI_HANDLE Controller,
  EFI_DEVICE_PATH_PROTOCOL *RemainingDevicePath
) {
  EFI_STATUS Status;
  EXT4_VOLUME *V = NULL;
  EFI_BLOCK_IO_PROTOCOL *Block = NULL;
  (VOID)RemainingDevicePath;
  Status = OpenBlock(This, Controller, &Block);
  if (EFI_ERROR(Status)) return Status;
  Status = gBS->AllocatePool(EfiBootServicesData, sizeof(*V), (VOID **)&V);
  if (EFI_ERROR(Status)) goto Fail;
  ZeroMem(V, sizeof(*V));
  V->Controller = Controller;
  V->Disk = Block;
  Status = Ext4Probe(V);
  if (EFI_ERROR(Status)) goto Fail;
  V->Simple.Revision = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION;
  V->Simple.OpenVolume = OpenVolume;
  Status = gBS->InstallMultipleProtocolInterfaces(&Controller,
                      &gEfiSimpleFileSystemProtocolGuid, &V->Simple,
                      &mExt4VolumeGuid, V, NULL);
  if (!EFI_ERROR(Status))
    DEBUG((DEBUG_INFO, "J250F ext4: read-only filesystem mounted on %p\n",
           Controller));
  if (!EFI_ERROR(Status)) return EFI_SUCCESS;
Fail:
  FreeVolume(V);
  gBS->CloseProtocol(Controller, &gEfiBlockIoProtocolGuid,
                     This->DriverBindingHandle, Controller);
  return Status;
}
STATIC EFI_STATUS EFIAPI DriverStop (
  EFI_DRIVER_BINDING_PROTOCOL *This, EFI_HANDLE Controller,
  UINTN NumberOfChildren, EFI_HANDLE *ChildHandleBuffer
) {
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Simple = NULL;
  EXT4_VOLUME *V;
  EFI_STATUS Status;
  (VOID)ChildHandleBuffer;
  if (NumberOfChildren) return EFI_UNSUPPORTED;
  Status = gBS->HandleProtocol(Controller, &gEfiSimpleFileSystemProtocolGuid,
                               (VOID **)&Simple);
  if (EFI_ERROR(Status)) return Status;
  V = (EXT4_VOLUME *)((UINT8 *)Simple - OFFSET_OF(EXT4_VOLUME, Simple));
  if (V->Signature != EXT4_SIGNATURE || V->OpenCount) return EFI_ACCESS_DENIED;
  Status = gBS->UninstallMultipleProtocolInterfaces(Controller,
               &gEfiSimpleFileSystemProtocolGuid, &V->Simple,
               &mExt4VolumeGuid, V, NULL);
  if (EFI_ERROR(Status)) return Status;
  gBS->CloseProtocol(Controller, &gEfiBlockIoProtocolGuid,
                     This->DriverBindingHandle, Controller);
  FreeVolume(V);
  return EFI_SUCCESS;
}
STATIC EFI_DRIVER_BINDING_PROTOCOL mBinding = {
  DriverSupported, DriverStart, DriverStop, 0x20, NULL, NULL
};

EFI_STATUS EFIAPI J250fExt4Entry (EFI_HANDLE ImageHandle,
                                   EFI_SYSTEM_TABLE *SystemTable) {
  (VOID)SystemTable;
  mBinding.ImageHandle = ImageHandle;
  mBinding.DriverBindingHandle = ImageHandle;
  return gBS->InstallProtocolInterface(&ImageHandle,
             &gEfiDriverBindingProtocolGuid, EFI_NATIVE_INTERFACE, &mBinding);
}
