/** @file
  J250F ADB firmware inspection interface (DEBUG builds).

  Uses native DXE Boot Services and Runtime Services. Broad, generic,
  read-only introspection, rather than one hand-written command per problem.
  Paged output fits the 8 KiB JADB shell transport. No storage / MMIO writes.
  WARNING: the existing ADB transport is not authenticated. Keep a physical
  USB-only development build, never publish on a production handset.
  SPDX-License-Identifier: GPL-2.0-only
**/
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/LoadedImage.h>

#define DBG_OUT_LIMIT  5120u
#define DBG_PAGE       8u
#define DBG_READ_LIMIT 128u

typedef struct {
  CHAR8 *Out;
  UINTN Cap;
  UINTN Used;
} DBG_OUTPUT;

STATIC VOID
Add(DBG_OUTPUT *O, CONST CHAR8 *Text)
{
  UINTN Length;
  if (O->Used >= O->Cap) return;
  Length = AsciiStrLen(Text);
  if (Length >= O->Cap - O->Used)
    Length = O->Cap - O->Used - 1u;
  CopyMem(O->Out + O->Used, Text, Length);
  O->Used += Length;
  O->Out[O->Used] = 0;
}

STATIC VOID
GuidLine(DBG_OUTPUT *O, UINTN Index, CONST EFI_GUID *G, VOID *Ptr)
{
  CHAR8 Line[128];
  AsciiSPrint(Line, sizeof(Line), "[%u] %g ptr=%p\r\n",
              (UINT32)Index, G, Ptr);
  Add(O, Line);
}

STATIC BOOLEAN
Number(CONST CHAR8 *S, UINTN *Value)
{
  UINTN Acc = 0, Digit, Base = 10;
  if (S == NULL || Value == NULL || !S[0]) return FALSE;
  if (S[0] == '0' && (S[1] == 'x' || S[1] == 'X')) {
    S += 2;
    Base = 16;
    if (!*S) return FALSE;
  }
  for (; *S; ++S) {
    if (*S >= '0' && *S <= '9') Digit = (UINTN)(*S - '0');
    else if (*S >= 'a' && *S <= 'f') Digit = (UINTN)(*S - 'a' + 10);
    else if (*S >= 'A' && *S <= 'F') Digit = (UINTN)(*S - 'A' + 10);
    else return FALSE;
    if (Digit >= Base || Acc > (MAX_UINTN - Digit) / Base)
      return FALSE;
    Acc = Acc * Base + Digit;
  }
  *Value = Acc;
  return TRUE;
}

STATIC BOOLEAN
ParseGuid(CONST CHAR8 *Text, EFI_GUID *G)
{
  STATIC CONST UINT8 DashPos[] = {8, 13, 18, 23};
  UINT8 Bytes[16];
  UINTN I, Nib = 0, V = 0, Dash = 0;
  UINTN B = 0;
  if (Text == NULL || G == NULL || AsciiStrLen(Text) != 36) return FALSE;
  for (I = 0; I < 36; ++I) {
    CHAR8 Ch = Text[I];
    UINTN Dig;
    if (Dash < 4 && I == DashPos[Dash]) { if (Ch != '-') return FALSE; ++Dash; continue; }
    if (Ch >= '0' && Ch <= '9') Dig = (UINTN)(Ch - '0');
    else if (Ch >= 'a' && Ch <= 'f') Dig = (UINTN)(Ch - 'a' + 10);
    else if (Ch >= 'A' && Ch <= 'F') Dig = (UINTN)(Ch - 'A' + 10);
    else return FALSE;
    V = (V << 4) | Dig;
    if (++Nib == 2) {
      if (B >= sizeof(Bytes)) return FALSE;
      Bytes[B++] = (UINT8)V;
      V = 0; Nib = 0;
    }
  }
  if (B != 16 || Dash != 4) return FALSE;
  G->Data1 = (UINT32)Bytes[0] << 24 | (UINT32)Bytes[1] << 16 |
             (UINT32)Bytes[2] << 8 | Bytes[3];
  G->Data2 = (UINT16)((UINT16)Bytes[4] << 8 | Bytes[5]);
  G->Data3 = (UINT16)((UINT16)Bytes[6] << 8 | Bytes[7]);
  CopyMem(G->Data4, Bytes + 8, 8);
  return TRUE;
}

STATIC BOOLEAN
IsReadRamType(EFI_MEMORY_TYPE Type)
{
  /* Physical memory reads are limited to UEFI-managed, non-MMIO RAM.
     No EfiRuntimeServices*, EfiReservedMemoryType or MMIO reads here. */
  return Type == EfiConventionalMemory ||
         Type == EfiBootServicesCode || Type == EfiBootServicesData ||
         Type == EfiLoaderCode || Type == EfiLoaderData;
}

STATIC EFI_STATUS
ReadMemoryMap(EFI_MEMORY_DESCRIPTOR **Map, UINTN *Size, UINTN *DescSize)
{
  UINTN Key, Version, Capacity;
  EFI_STATUS S;
  if (!Map || !Size || !DescSize) return EFI_INVALID_PARAMETER;
  *Map = NULL;
  *Size = 0;
  *DescSize = 0;
  S = gBS->GetMemoryMap(Size, *Map, &Key, DescSize, (UINT32 *)&Version);
  if (S != EFI_BUFFER_TOO_SMALL || *DescSize == 0) return S;
  if (*Size > 128 * 1024u) return EFI_OUT_OF_RESOURCES;
  Capacity = *Size + 32u * *DescSize;
  S = gBS->AllocatePool(EfiBootServicesData, Capacity, (VOID **)Map);
  if (EFI_ERROR(S)) return S;
  *Size = Capacity;
  S = gBS->GetMemoryMap(Size, *Map, &Key, DescSize, (UINT32 *)&Version);
  if (EFI_ERROR(S)) { gBS->FreePool(*Map); *Map = NULL; }
  return S;
}

STATIC BOOLEAN
MemoryRangeAllowed(UINTN Address, UINTN Bytes)
{
  EFI_MEMORY_DESCRIPTOR *Map = NULL, *D;
  UINTN Size = 0, DescSize = 0, Offset;
  EFI_STATUS S;
  BOOLEAN Allowed = FALSE;
  if (Bytes == 0 || Address > MAX_UINTN - Bytes) return FALSE;
  /* The phone's usable RAM is 0x80000000..0xDFFFFFFF, but
     EFI descriptors, not only these bounds, must authorize the read. */
  if (Address < 0x80000000u || Address + Bytes > 0xE0000000u) return FALSE;
  S = ReadMemoryMap(&Map, &Size, &DescSize);
  if (EFI_ERROR(S)) return FALSE;
  for (Offset = 0; Offset + DescSize <= Size; Offset += DescSize) {
    UINT64 Start, Length, End;
    D = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Offset);
    if (!IsReadRamType(D->Type)) continue;
    Start = D->PhysicalStart;
    Length = D->NumberOfPages * EFI_PAGE_SIZE;
    End = Start + Length;
    if (Length != 0 && End >= Start &&
        (UINT64)Address >= Start &&
        (UINT64)(Address + Bytes) <= End) {
      Allowed = TRUE; break;
    }
  }
  gBS->FreePool(Map);
  return Allowed;
}

STATIC BOOLEAN
MmioAllowed(UINTN Address, UINTN Words)
{
  UINTN End;
  if (Words == 0 || Words > 8 || Address > MAX_UINTN - Words*4u ||
      (Address & 3u) != 0) return FALSE;
  End = Address + Words*4u;
  /* Known J250F register blocks only. Some clock-gated MMIO can still
     fault or stall; MMIO is strictly opt-in and read-only. */
  return (Address >= 0x01A00000u && End <= 0x01B00000u) || // MDSS
         (Address >= 0x01800000u && End <= 0x01900000u) || // GCC
         (Address >= 0x07824000u && End <= 0x07826000u) || // SDCC1
         (Address >= 0x01000000u && End <= 0x01300000u);   // TLMM
}

STATIC UINTN
Tokenize(CHAR8 *Line, CHAR8 *Arg[5])
{
  UINTN Count = 0;
  CHAR8 *P = Line;
  while (*P && Count < 5) {
    while (*P == ' ' || *P == '\t') ++P;
    if (!*P) break;
    Arg[Count++] = P;
    while (*P && *P != ' ' && *P != '\t') ++P;
    if (!*P) break;
    *P++ = 0;
  }
  return Count;
}

STATIC VOID
DumpHandles(DBG_OUTPUT *O, UINTN Start, UINTN Mode, UINTN HandleIndex)
{
  EFI_HANDLE *Handles = NULL;
  EFI_STATUS S;
  UINTN Count = 0, I, End;
  CHAR8 Line[140];
  S = gBS->LocateHandleBuffer(AllHandles, NULL, NULL, &Count, &Handles);
  if (EFI_ERROR(S)) {
    AsciiSPrint(Line, sizeof(Line), "LocateHandleBuffer(AllHandles)=%r\r\n", S);
    Add(O, Line); return;
  }
  if (Mode == 0) {
    AsciiSPrint(Line, sizeof(Line), "UEFI handles: %u total, showing %u..%u\r\n",
                (UINT32)Count, (UINT32)Start, (UINT32)(Start + DBG_PAGE));
    Add(O, Line);
    End = Start + DBG_PAGE < Count ? Start + DBG_PAGE : Count;
    for (I = Start; I < End; ++I) {
      EFI_GUID **Protocols = NULL;
      UINTN ProCount = 0;
      S = gBS->ProtocolsPerHandle(Handles[I], &Protocols, &ProCount);
      AsciiSPrint(Line, sizeof(Line),
                  "[%u] handle=%p protocols=%u enumerate=%r\r\n",
                  (UINT32)I, Handles[I], (UINT32)ProCount, S);
      Add(O, Line);
      if (!EFI_ERROR(S) && Protocols != NULL) gBS->FreePool(Protocols);
    }
  } else if (HandleIndex >= Count) {
    Add(O, "Invalid handle index. Run dbg handles <start>.\r\n");
  } else {
    EFI_GUID **Protocols = NULL;
    UINTN ProCount = 0;
    S = gBS->ProtocolsPerHandle(Handles[HandleIndex], &Protocols, &ProCount);
    AsciiSPrint(Line, sizeof(Line),
                "Handle[%u]=%p ProtocolsPerHandle=%r count=%u\r\n",
                (UINT32)HandleIndex, Handles[HandleIndex], S, (UINT32)ProCount);
    Add(O, Line);
    if (!EFI_ERROR(S) && Protocols != NULL) {
      End = Start + DBG_PAGE < ProCount ? Start + DBG_PAGE : ProCount;
      for (I = Start; I < End; ++I) {
        VOID *Interface = NULL;
        EFI_STATUS P = gBS->HandleProtocol(Handles[HandleIndex],
                                          Protocols[I], &Interface);
        GuidLine(O, I, Protocols[I], EFI_ERROR(P) ? NULL : Interface);
      }
      gBS->FreePool(Protocols);
    }
  }
  if (Handles) gBS->FreePool(Handles);
}

STATIC VOID
DumpMemory(DBG_OUTPUT *O, UINTN Start)
{
  EFI_MEMORY_DESCRIPTOR *Map = NULL, *D;
  UINTN Size = 0, DescSize = 0, Off, Index = 0;
  EFI_STATUS S = ReadMemoryMap(&Map, &Size, &DescSize);
  CHAR8 Line[150];
  if (EFI_ERROR(S)) {
    AsciiSPrint(Line, sizeof(Line), "GetMemoryMap=%r\r\n", S);
    Add(O, Line); return;
  }
  AsciiSPrint(Line, sizeof(Line), "UEFI memory map: %u records, entry=%u bytes\r\n",
              (UINT32)(Size / DescSize), (UINT32)DescSize);
  Add(O, Line);
  for (Off = 0; Off + DescSize <= Size; Off += DescSize, ++Index) {
    if (Index < Start) continue;
    if (Index >= Start + DBG_PAGE) break;
    D = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Off);
    AsciiSPrint(Line, sizeof(Line),
      "[%u] type=%u phys=0x%Lx pages=0x%Lx attr=0x%Lx\r\n",
      (UINT32)Index, (UINT32)D->Type, D->PhysicalStart,
      D->NumberOfPages, D->Attribute);
    Add(O, Line);
  }
  gBS->FreePool(Map);
}

STATIC VOID
DumpBlock(DBG_OUTPUT *O)
{
  EFI_HANDLE *Handles = NULL;
  UINTN I, Count = 0;
  EFI_STATUS S;
  CHAR8 Line[165];
  S = gBS->LocateHandleBuffer(ByProtocol, &gEfiBlockIoProtocolGuid,
                               NULL, &Count, &Handles);
  AsciiSPrint(Line, sizeof(Line), "BlockIo: %r handles=%u\r\n",
              S, (UINT32)Count);
  Add(O, Line);
  if (EFI_ERROR(S)) return;
  for (I = 0; I < Count && I < 12; ++I) {
    EFI_BLOCK_IO_PROTOCOL *B = NULL;
    S = gBS->HandleProtocol(Handles[I], &gEfiBlockIoProtocolGuid, (VOID **)&B);
    if (EFI_ERROR(S) || B == NULL || B->Media == NULL) continue;
    AsciiSPrint(Line, sizeof(Line),
      "[%u] handle=%p present=%u readonly=%u blk=%u lastLBA=0x%Lx\r\n",
      (UINT32)I, Handles[I], (UINT32)B->Media->MediaPresent,
      (UINT32)B->Media->ReadOnly, B->Media->BlockSize, B->Media->LastBlock);
    Add(O, Line);
  }
  gBS->FreePool(Handles);
}

STATIC VOID
DumpImages(DBG_OUTPUT *O, UINTN Start)
{
  EFI_HANDLE *Handles = NULL;
  UINTN Count = 0, I, End;
  EFI_STATUS S;
  CHAR8 Line[135];
  S = gBS->LocateHandleBuffer(ByProtocol, &gEfiLoadedImageProtocolGuid,
                              NULL, &Count, &Handles);
  AsciiSPrint(Line, sizeof(Line), "LoadedImages=%r count=%u\r\n", S, (UINT32)Count);
  Add(O, Line);
  if (EFI_ERROR(S)) return;
  End = Start + DBG_PAGE < Count ? Start + DBG_PAGE : Count;
  for (I = Start; I < End; ++I) {
    EFI_LOADED_IMAGE_PROTOCOL *Image = NULL;
    S = gBS->HandleProtocol(Handles[I], &gEfiLoadedImageProtocolGuid,
                            (VOID **)&Image);
    if (EFI_ERROR(S) || Image == NULL) continue;
    AsciiSPrint(Line, sizeof(Line),
      "[%u] handle=%p base=%p size=0x%Lx parent=%p\r\n",
      (UINT32)I, Handles[I], Image->ImageBase,
      (UINT64)Image->ImageSize, Image->ParentHandle);
    Add(O, Line);
  }
  gBS->FreePool(Handles);
}

STATIC VOID
DumpVariables(DBG_OUTPUT *O, UINTN Start)
{
  CHAR16 Name[256];
  EFI_GUID Vendor;
  EFI_STATUS S;
  UINTN Size, I = 0, Printed = 0;
  CHAR8 Line[240];
  ZeroMem(&Vendor, sizeof(Vendor));
  Name[0] = 0;
  for (I = 0; I < Start + DBG_PAGE && I < 256u; ++I) {
    Size = sizeof(Name);
    S = gRT->GetNextVariableName(&Size, Name, &Vendor);
    if (EFI_ERROR(S)) {
      AsciiSPrint(Line, sizeof(Line),
        "GetNextVariableName stopped at %u: %r\r\n", (UINT32)I, S);
      Add(O, Line); break;
    }
    if (I >= Start) {
      AsciiSPrint(Line, sizeof(Line), "[%u] vendor=%g name=%s\r\n",
                  (UINT32)I, &Vendor, Name);
      Add(O, Line);
      ++Printed;
    }
  }
  if (Printed == 0) Add(O, "No entries on this page.\r\n");
}

STATIC VOID
DumpBytes(DBG_OUTPUT *O, UINTN Addr, UINTN Len)
{
  UINTN I, J;
  CHAR8 Line[128], *Ptr;
  for (I = 0; I < Len; I += 16) {
    UINTN Used = 0;
    Used += AsciiSPrint(Line, sizeof(Line), "%08lx:", Addr + I);
    for (J = 0; J < 16 && I + J < Len; ++J) {
      Ptr = Line + Used;
      Used += AsciiSPrint(Ptr, sizeof(Line) - Used, " %02x",
                          *((volatile UINT8 *)(Addr + I + J)));
    }
    Add(O, Line); Add(O, "\r\n");
  }
}

UINTN EFIAPI
J250fDeepDebugCommand(CONST CHAR8 *Args, CHAR8 *Output, UINTN Capacity)
{
  CHAR8 Buf[224], Line[180];
  CHAR8 *Arg[5] = {0};
  UINTN N, Start = 0, Index = 0, Length = 0;
  DBG_OUTPUT O;
  EFI_STATUS S;
  if (Output == NULL || Capacity == 0) return 0;
  O.Out = Output; O.Cap = Capacity > DBG_OUT_LIMIT ? DBG_OUT_LIMIT : Capacity;
  O.Used = 0;
  O.Out[0] = 0;
  if (Args == NULL || AsciiStrLen(Args) >= sizeof(Buf)) {
    Add(&O, "Command too long.\r\n"); return O.Used;
  }
  AsciiStrCpyS(Buf, sizeof(Buf), Args);
  N = Tokenize(Buf, Arg);
  if (N == 0 || AsciiStrCmp(Arg[0], "help") == 0) {
    Add(&O,
      "J250F DBG/1: firmware-introspection shell (read-only)\r\n"
      "dbg system                      EFI system and runtime pointers\r\n"
      "dbg tables [start]              EFI configuration table GUIDs\r\n"
      "dbg handles [start]             Enumerate all EFI handles (8/page)\r\n"
      "dbg protocols <handle> [start]  GUIDs attached to a handle\r\n"
      "dbg locate <guid>               Locate any EFI protocol by GUID\r\n"
      "dbg images [start]              Loaded EFI images and addresses\r\n"
      "dbg disks                       BlockIo attributes, never writes\r\n"
      "dbg memory [start]              EFI memory descriptors (8/page)\r\n"
      "dbg ram <addr> <bytes>          EFI-managed DRAM dump (max 128)\r\n"
      "dbg fb <offset> <bytes>         Inherited framebuffer dump (max 128)\r\n"
      "dbg mmio <addr> [words]         Known chipset MMIO read (max 8)\r\n"
      "dbg vars [start]                UEFI variable metadata, 8/page\r\n"
      "Existing: gop status|repair|test; emmc ...; efi run /ram/tool.efi\r\n"
      "Dangerous operations, arbitrary MMIO and storage writes are disabled.\r\n");
    return O.Used;
  }
  if (AsciiStrCmp(Arg[0], "system") == 0) {
    AsciiSPrint(Line, sizeof(Line),
      "SystemTable=%p BS=%p RT=%p firmwareRevision=%u hdrRev=0x%x\r\n",
      gST, gBS, gRT, (UINT32)gST->FirmwareRevision,
      (UINT32)gST->Hdr.Revision);
    Add(&O, Line);
    AsciiSPrint(Line, sizeof(Line),
      "BootServicesRev=0x%x RuntimeServicesRev=0x%x cfgTables=%u\r\n",
      (UINT32)gBS->Hdr.Revision, (UINT32)gRT->Hdr.Revision,
      (UINT32)gST->NumberOfTableEntries);
    Add(&O, Line); return O.Used;
  }
  if (AsciiStrCmp(Arg[0], "disks") == 0) {
    DumpBlock(&O); return O.Used;
  }
  if (N > 1 && AsciiStrCmp(Arg[0], "locate") == 0) {
    EFI_GUID Guid;
    EFI_HANDLE *Handles = NULL;
    UINTN Count = 0;
    VOID *Found = NULL;
    if (!ParseGuid(Arg[1], &Guid)) {
      Add(&O, "Expected GUID xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx.\r\n");
      return O.Used;
    }
    S = gBS->LocateProtocol(&Guid, NULL, &Found);
    AsciiSPrint(Line, sizeof(Line), "LocateProtocol(%g)=%r interface=%p\r\n",
                &Guid, S, Found);
    Add(&O, Line);
    S = gBS->LocateHandleBuffer(ByProtocol, &Guid, NULL, &Count, &Handles);
    AsciiSPrint(Line, sizeof(Line), "LocateHandles=%r count=%u\r\n",
                S, (UINT32)Count);
    Add(&O, Line);
    if (!EFI_ERROR(S) && Handles != NULL) {
      for (Index = 0; Index < Count && Index < 12; ++Index) {
        AsciiSPrint(Line, sizeof(Line), "[%u] handle=%p\r\n",
                    (UINT32)Index, Handles[Index]);
        Add(&O, Line);
      }
      gBS->FreePool(Handles);
    }
    return O.Used;
  }
  if (AsciiStrCmp(Arg[0], "protocols") == 0) {
    if (N < 2 || !Number(Arg[1], &Index) ||
        (N > 2 && !Number(Arg[2], &Start))) {
      Add(&O, "Usage: dbg protocols <handle_index> [start]\r\n");
      return O.Used;
    }
    DumpHandles(&O, Start, 1, Index); return O.Used;
  }
  if (N > 1 && AsciiStrCmp(Arg[0], "ram") == 0) {
    if (N != 3 || !Number(Arg[1], &Index) ||
        !Number(Arg[2], &Length) || Length > DBG_READ_LIMIT ||
        !MemoryRangeAllowed(Index, Length)) {
      Add(&O, "RAM range rejected: must be EFI-managed RAM, 1..128 bytes.\r\n");
      return O.Used;
    }
    DumpBytes(&O, Index, Length); return O.Used;
  }
  if (N > 1 && AsciiStrCmp(Arg[0], "fb") == 0) {
    if (N != 3 || !Number(Arg[1], &Index) || !Number(Arg[2], &Length) ||
        Length == 0 || Length > DBG_READ_LIMIT ||
        Index > 0x00300000u - Length) {
      Add(&O, "Usage: dbg fb <0..0x2fffff> <1..128>\r\n");
      return O.Used;
    }
    DumpBytes(&O, 0xA8000000u + Index, Length); return O.Used;
  }
  if (N > 1 && AsciiStrCmp(Arg[0], "mmio") == 0) {
    UINTN Words = 1, I;
    if (N > 2 && !Number(Arg[2], &Words)) Words = 0;
    if (!Number(Arg[1], &Index) || !MmioAllowed(Index, Words)) {
      Add(&O, "MMIO blocked: only J250F MDSS/GCC/SDCC1/TLMM, aligned, 1..8 words.\r\n");
      return O.Used;
    }
    for (I = 0; I < Words; ++I) {
      AsciiSPrint(Line, sizeof(Line), "0x%08lx = 0x%08x\r\n",
                  Index + 4u * I, MmioRead32(Index + 4u * I));
      Add(&O, Line);
    }
    return O.Used;
  }
  if (N > 1 && !Number(Arg[1], &Start) &&
      AsciiStrCmp(Arg[0], "tables") == 0) {
    Add(&O, "Invalid start index.\r\n"); return O.Used;
  }
  if (N > 1 && !Number(Arg[1], &Start) &&
      (AsciiStrCmp(Arg[0], "handles") == 0 ||
       AsciiStrCmp(Arg[0], "memory") == 0 ||
       AsciiStrCmp(Arg[0], "images") == 0 ||
       AsciiStrCmp(Arg[0], "vars") == 0)) {
    Add(&O, "Invalid page index.\r\n"); return O.Used;
  }
  if (Start > 100000u) {
    Add(&O, "Page index too large.\r\n"); return O.Used;
  }
  if (AsciiStrCmp(Arg[0], "tables") == 0) {
    UINTN End = Start + DBG_PAGE;
    if (End > gST->NumberOfTableEntries) End = gST->NumberOfTableEntries;
    AsciiSPrint(Line, sizeof(Line), "EFI config tables count=%u\r\n",
                (UINT32)gST->NumberOfTableEntries);
    Add(&O, Line);
    for (Index = Start; Index < End; ++Index)
      GuidLine(&O, Index, &gST->ConfigurationTable[Index].VendorGuid,
               gST->ConfigurationTable[Index].VendorTable);
    return O.Used;
  }
  if (AsciiStrCmp(Arg[0], "handles") == 0) {
    DumpHandles(&O, Start, 0, 0); return O.Used;
  }
  if (AsciiStrCmp(Arg[0], "memory") == 0) {
    DumpMemory(&O, Start); return O.Used;
  }
  if (AsciiStrCmp(Arg[0], "images") == 0) {
    DumpImages(&O, Start); return O.Used;
  }
  if (AsciiStrCmp(Arg[0], "vars") == 0) {
    DumpVariables(&O, Start); return O.Used;
  }
  Add(&O, "Unknown dbg command. Run dbg help.\r\n");
  return O.Used;
}
