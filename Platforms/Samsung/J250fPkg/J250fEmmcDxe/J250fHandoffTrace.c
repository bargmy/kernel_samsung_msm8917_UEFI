/** Pre-OS handoff trace; never suppresses a reset or changes image policy.
    SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Protocol/LoadedImage.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

STATIC EFI_IMAGE_LOAD mLoad;
STATIC EFI_IMAGE_START mStart;
STATIC EFI_EXIT_BOOT_SERVICES mExit;
STATIC EFI_RESET_SYSTEM mReset;
STATIC BOOLEAN mInstalled;
STATIC BOOLEAN mLogging;
STATIC BOOLEAN mReadySeen;
STATIC EFI_GUID mReadyGuid = {
  0xd7f18d20,0x49b1,0x4e41,{0x8d,0x1a,0x91,0x46,0x6c,0x7e,0x37,0xa2}
};

#define HANDOFF_MESSAGE(...) do { \
  CHAR8 Text[256]; \
  UINTN Length = AsciiSPrint(Text, sizeof(Text), __VA_ARGS__); \
  SerialPortWrite((UINT8 *)Text, Length); \
} while (0)

STATIC BOOLEAN Active(VOID) {
  VOID *Marker = NULL;
  BOOLEAN Found;
  if (!mInstalled || mLogging) return FALSE;
  Found = !EFI_ERROR(gBS->LocateProtocol(&mReadyGuid, NULL, &Marker)) && Marker != NULL;
  if (Found) mReadySeen = TRUE;
  return Found;
}

STATIC EFI_STATUS EFIAPI TraceLoad(BOOLEAN Policy, EFI_HANDLE Parent,
  EFI_DEVICE_PATH_PROTOCOL *Path, VOID *Source, UINTN Bytes, EFI_HANDLE *Image) {
  EFI_STATUS Status;
  BOOLEAN Trace = Active();
  if (Trace) {
    mLogging = TRUE;
    HANDOFF_MESSAGE("[WP81 HANDOFF] LoadImage ENTER policy=%u bytes=%u path=%p\r\n",
                    (UINT32)Policy, (UINT32)Bytes, Path);
    mLogging = FALSE;
  }
  Status = mLoad(Policy, Parent, Path, Source, Bytes, Image);
  if (Trace && mInstalled) {
    mLogging = TRUE;
    HANDOFF_MESSAGE("[WP81 HANDOFF] LoadImage RETURN %r image=%p\r\n",
                    Status, Image && (!EFI_ERROR(Status) || Status == EFI_SECURITY_VIOLATION) ? *Image : NULL);
    mLogging = FALSE;
  }
  return Status;
}

STATIC EFI_STATUS EFIAPI TraceStart(EFI_HANDLE Image, UINTN *ExitBytes, CHAR16 **ExitData) {
  EFI_STATUS Status;
  BOOLEAN Trace = Active();
  if (Trace) {
    EFI_LOADED_IMAGE_PROTOCOL *Info = NULL;
    EFI_STATUS InfoStatus;
    mLogging = TRUE;
    InfoStatus = gBS->HandleProtocol(Image, &gEfiLoadedImageProtocolGuid, (VOID **)&Info);
    HANDOFF_MESSAGE("[WP81 HANDOFF] StartImage ENTER image=%p info=%r base=%p size=%Lu\r\n",
                    Image, InfoStatus, Info ? Info->ImageBase : NULL,
                    Info ? Info->ImageSize : 0ULL);
    mLogging = FALSE;
  }
  Status = mStart(Image, ExitBytes, ExitData);
  /* A successful ExitBootServices invalidates boot-service logging. */
  if (Trace && mInstalled) {
    mLogging = TRUE;
    HANDOFF_MESSAGE("[WP81 HANDOFF] StartImage RETURN %r\r\n", Status);
    mLogging = FALSE;
  }
  return Status;
}

STATIC EFI_STATUS EFIAPI TraceExit(EFI_HANDLE Image, UINTN MapKey) {
  EFI_STATUS Status;
  BOOLEAN Trace = Active();
  if (Trace) {
    mLogging = TRUE;
    /* The existing logger flush uses no pool/page allocation. Never log after
       successful EBS; its callback restores these hooks before OS ownership. */
    HANDOFF_MESSAGE("[WP81 HANDOFF] ExitBootServices ENTER image=%p key=%u\r\n",
                    Image, (UINT32)MapKey);
    mLogging = FALSE;
  }
  Status = mExit(Image, MapKey);
  if (EFI_ERROR(Status) && Trace && mInstalled) {
    mLogging = TRUE;
    HANDOFF_MESSAGE("[WP81 HANDOFF] ExitBootServices RETURN %r\r\n", Status);
    mLogging = FALSE;
  }
  return Status;
}

STATIC VOID EFIAPI TraceReset(EFI_RESET_TYPE Type, EFI_STATUS Status,
                              UINTN Bytes, VOID *Data) {
  /* ResetSystem permits high TPL/IRQ-masked callers. Unlike image services,
     do not call LocateProtocol here: use the marker observed during loading. */
  if (mInstalled && mReadySeen && !mLogging) {
    VOID *Caller = NULL;
#if defined(__GNUC__) || defined(__clang__)
    Caller = __builtin_extract_return_addr(__builtin_return_address(0));
#endif
    mLogging = TRUE;
    /* Do not dereference ResetData or stop the reset. Caller can be matched
       with the loaded image range to distinguish bootmgr and firmware. */
    HANDOFF_MESSAGE("[WP81 HANDOFF] ResetSystem REQUEST type=%u status=%r caller=%p dataBytes=%u\r\n",
                    (UINT32)Type, Status, Caller, (UINT32)Bytes);
    mLogging = FALSE;
  }
  mReset(Type, Status, Bytes, Data);
}

STATIC EFI_STATUS Crc(EFI_TABLE_HEADER *Header) {
  Header->CRC32 = 0;
  return gBS->CalculateCrc32(Header, Header->HeaderSize, &Header->CRC32);
}

EFI_STATUS EFIAPI J250fHandoffTraceInstall(VOID) {
  EFI_TPL Tpl;
  EFI_STATUS Status;
  UINT32 BsCrc, RtCrc;
  if (mInstalled) return EFI_ALREADY_STARTED;
  if (!gBS || !gRT || !gBS->LoadImage || !gBS->StartImage ||
      !gBS->ExitBootServices || !gRT->ResetSystem || !gBS->CalculateCrc32)
    return EFI_NOT_READY;
  Tpl = gBS->RaiseTPL(TPL_HIGH_LEVEL);
  mReadySeen = FALSE;
  BsCrc = gBS->Hdr.CRC32; RtCrc = gRT->Hdr.CRC32;
  mLoad = gBS->LoadImage; mStart = gBS->StartImage;
  mExit = gBS->ExitBootServices; mReset = gRT->ResetSystem;
  gBS->LoadImage = TraceLoad; gBS->StartImage = TraceStart;
  gBS->ExitBootServices = TraceExit; gRT->ResetSystem = TraceReset;
  Status = Crc(&gBS->Hdr);
  if (!EFI_ERROR(Status)) Status = Crc(&gRT->Hdr);
  if (EFI_ERROR(Status)) {
    gBS->LoadImage = mLoad; gBS->StartImage = mStart;
    gBS->ExitBootServices = mExit; gRT->ResetSystem = mReset;
    gBS->Hdr.CRC32 = BsCrc; gRT->Hdr.CRC32 = RtCrc;
  } else {
    mInstalled = TRUE;
  }
  gBS->RestoreTPL(Tpl);
  return Status;
}

/* Resident until EBS, never a runtime hook. No allocation or disk I/O here. */
VOID EFIAPI J250fHandoffTraceStop(VOID) {
  BOOLEAN BsChanged = FALSE, RtChanged = FALSE;
  if (!mInstalled) return;
  mInstalled = FALSE;
  mReadySeen = FALSE;
  if (gBS->LoadImage == TraceLoad) { gBS->LoadImage = mLoad; BsChanged = TRUE; }
  if (gBS->StartImage == TraceStart) { gBS->StartImage = mStart; BsChanged = TRUE; }
  if (gBS->ExitBootServices == TraceExit) { gBS->ExitBootServices = mExit; BsChanged = TRUE; }
  if (gRT->ResetSystem == TraceReset) { gRT->ResetSystem = mReset; RtChanged = TRUE; }
  if (BsChanged) (VOID)Crc(&gBS->Hdr);
  if (RtChanged) (VOID)Crc(&gRT->Hdr);
}
