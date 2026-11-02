/** @file
  Bounded, read-only ADB listing of mounted ext4 SimpleFileSystem handles.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Uefi.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>

/* Matches the private ext4-volume marker installed by J250fExt4Dxe. */
STATIC EFI_GUID mExt4VolumeGuid = {
  0x7cd5d62a, 0x8f37, 0x4eb6, {0x9d,0x10,0x3d,0xeb,0xb3,0x85,0xaf,0x64}
};
STATIC BOOLEAN Prefix (CONST CHAR8 *Text, CONST CHAR8 *Start) {
  return AsciiStrnCmp(Text, Start, AsciiStrLen(Start)) == 0;
}
STATIC UINTN Append (CHAR8 *Out, UINTN Cap, UINTN Used,
                      CONST CHAR8 *Format, UINT32 Number,
                      CONST CHAR8 *Word) {
  if (Used < Cap)
    Used += AsciiSPrint(Out + Used, Cap - Used, Format, Number, Word);
  return Used;
}

BOOLEAN EFIAPI J250fExt4Command(CONST CHAR8 *Cmd, CHAR8 *Out,
                                UINTN Cap, UINTN *Used) {
  EFI_STATUS Status;
  EFI_HANDLE *Handles = NULL;
  UINTN Count = 0, I, Selected = 0, Pos;
  BOOLEAN Listing;
  CHAR16 Path[256];
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs = NULL;
  EFI_FILE_PROTOCOL *Root = NULL, *Opened = NULL;
  if (!Prefix(Cmd, "ext4 ") && AsciiStrCmp(Cmd, "ext4") != 0)
    return FALSE;
  if (!Out || !Used || !Cap) return TRUE;
  *Used = 0;
  Status = gBS->LocateHandleBuffer(ByProtocol, &mExt4VolumeGuid,
                                   NULL, &Count, &Handles);
  if (EFI_ERROR(Status)) {
    *Used = AsciiSPrint(Out, Cap, "ext4: no mounted volumes (%r)\r\n", Status);
    return TRUE;
  }
  if (AsciiStrCmp(Cmd, "ext4 volumes") == 0 || AsciiStrCmp(Cmd, "ext4") == 0) {
    *Used = AsciiSPrint(Out, Cap, "Mounted read-only ext4 volumes: %u\r\n",
                         (UINT32)Count);
    for (I = 0; I < Count; I++)
      *Used = Append(Out, Cap, *Used,
                     "ext4 volume %u (read-only) %a\r\n", (UINT32)I, "");
    gBS->FreePool(Handles);
    return TRUE;
  }
  Listing = Prefix(Cmd, "ext4 ls ");
  if (!Listing && !Prefix(Cmd, "ext4 cat ")) {
    *Used = AsciiSPrint(Out, Cap, "Usage: ext4 volumes | ls <index> <path> | cat <index> <path>\r\n");
    gBS->FreePool(Handles);
    return TRUE;
  }
  Pos = Listing ? 8 : 9;
  if (Cmd[Pos] < '0' || Cmd[Pos] > '9') {
    Status = EFI_INVALID_PARAMETER;
    goto Done;
  }
  while (Cmd[Pos] >= '0' && Cmd[Pos] <= '9') {
    Selected = Selected * 10 + (UINTN)(Cmd[Pos] - '0');
    if (Selected >= Count) { Status = EFI_NOT_FOUND; goto Done; }
    Pos++;
  }
  if (Cmd[Pos] == 0) {
    Path[0] = L'\\'; Path[1] = 0;
  } else {
    UINTN N = 0;
    if (Cmd[Pos++] != ' ') { Status = EFI_INVALID_PARAMETER; goto Done; }
    while (Cmd[Pos] && N < 255) {
      UINT8 Ch = (UINT8)Cmd[Pos++];
      if (Ch < 0x20 || Ch > 0x7e) { Status = EFI_UNSUPPORTED; goto Done; }
      Path[N++] = (CHAR16)Ch;
    }
    if (Cmd[Pos]) { Status = EFI_INVALID_PARAMETER; goto Done; }
    Path[N] = 0;
  }
  Status = gBS->HandleProtocol(Handles[Selected],
                  &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
  if (EFI_ERROR(Status)) goto Done;
  Status = Fs->OpenVolume(Fs, &Root);
  if (EFI_ERROR(Status)) goto Done;
  Status = Root->Open(Root, &Opened, Path, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR(Status)) goto Done;
  if (Listing) {
    UINT8 InfoBytes[768];
    EFI_FILE_INFO *Info = (EFI_FILE_INFO *)InfoBytes;
    UINTN Entries = 0;
    /* Never stream unbounded logs or directory data over ADB. */
    while (*Used + 90 < Cap && Entries++ < 100) {
      UINTN Size = sizeof(InfoBytes);
      CHAR8 Name[256];
      UINTN J;
      Status = Opened->Read(Opened, &Size, Info);
      if (EFI_ERROR(Status) || Size == 0) break;
      for (J = 0; J < 255 && Info->FileName[J]; J++) {
        UINT16 Ch = Info->FileName[J];
        Name[J] = (Ch >= 0x20 && Ch <= 0x7e) ? (CHAR8)Ch : '?';
      }
      Name[J] = 0;
      *Used = Append(Out, Cap, *Used,
                     "%u %a\r\n",
                     (UINT32)((Info->Attribute & EFI_FILE_DIRECTORY) != 0),
                     Name);
    }
    if (Status == EFI_SUCCESS)
      *Used = Append(Out, Cap, *Used, "%u %a\r\n", (UINT32)Entries,
                    "(max 100 entries, output may be truncated)");
  } else {
    UINT8 Bytes[1024];
    UINTN Size = sizeof(Bytes);
    UINTN J;
    Status = Opened->Read(Opened, &Size, Bytes);
    if (!EFI_ERROR(Status)) {
      for (J = 0; J < Size && *Used + 4 < Cap; J++) {
        UINT8 Ch = Bytes[J];
        if (Ch == '\n') {
          Out[(*Used)++] = '\r'; Out[(*Used)++] = '\n';
        } else if (Ch == '\r' || Ch == '\t' ||
                   (Ch >= 0x20 && Ch <= 0x7e)) {
          Out[(*Used)++] = (CHAR8)Ch;
        } else {
          Out[(*Used)++] = '.';
        }
      }
      Out[*Used] = 0;
      *Used = Append(Out, Cap, *Used,
                     "\r\n[displayed %u bytes; %a]\r\n", (UINT32)Size,
                     "text preview only");
    }
  }
Done:
  if (Opened) Opened->Close(Opened);
  if (Root) Root->Close(Root);
  if (Handles) gBS->FreePool(Handles);
  if (EFI_ERROR(Status))
    *Used = Append(Out, Cap, *Used,
                   "ext4: operation failed: %u %a\r\n", (UINT32)Status,
                   "(see firmware status)");
  return TRUE;
}
