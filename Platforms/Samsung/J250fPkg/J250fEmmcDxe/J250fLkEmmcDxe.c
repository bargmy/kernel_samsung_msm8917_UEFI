/** @file
  Safe, read-only Project Mu/EDK II Block I/O frontend for a Qualcomm LK
  MMC backend. Uses the integrated controller backend and RPM power provider.
  SPDX-License-Identifier: BSD-2-Clause
**/
#include <Uefi.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/FrameBufferSerialPortLib.h>
#include <Protocol/GraphicsOutput.h>
#include <J250fGopSupportLib.h>
extern VOID EFIAPI J250fGopDiagRecord(
  EFI_STATUS PreStatus, EFI_STATUS PublishStatus,
  EFI_STATUS PostStatus, BOOLEAN Attempted);

#include "J250fLkPlatform.h"
extern EFI_STATUS EFIAPI J250fLogBoardWrite(EFI_LBA Lba, CONST VOID *Buffer);
#include "J250fPowerProtocol.h"
#include <Library/IoLib.h>
#include "J250fUsbGadget.h"
#include "J250fAdbShell.h"
extern EFI_STATUS EFIAPI J250fUsbDiagCommand(UINT16,UINT16,UINT16,VOID*,UINTN*);
extern VOID EFIAPI J250fWpInit(EFI_HANDLE Parent);
extern EFI_STATUS EFIAPI J250fWpPrepareBds(VOID);

typedef struct {
  VENDOR_DEVICE_PATH Vendor;
  EFI_DEVICE_PATH_PROTOCOL End;
} J250F_LK_PATH;

typedef struct {
  EFI_HANDLE Handle;
  EFI_BLOCK_IO_PROTOCOL Io;
  EFI_BLOCK_IO_MEDIA Media;
  J250F_LK_PATH Path;
  J250F_LK_DISK Lk;
  BOOLEAN Busy;
} J250F_LK_PRIVATE;
static J250F_LK_PRIVATE mDisk;
static BOOLEAN mPublished;

#define J250F_LK_SECTOR_BYTES 512
static CONST EFI_GUID mDiskPathGuid = {
  0x98074a7c, 0x2be4, 0x47dc, {0xb7,0xa2,0x17,0x75,0xac,0xfb,0x10,0x91}
};

static BOOLEAN Enter (VOID) {
  EFI_TPL Previous = gBS->RaiseTPL (TPL_NOTIFY);
  BOOLEAN Ok = !mDisk.Busy;
  if (Ok) mDisk.Busy = TRUE;
  gBS->RestoreTPL (Previous);
  return Ok;
}
static VOID Leave (VOID) {
  EFI_TPL Previous = gBS->RaiseTPL (TPL_NOTIFY);
  mDisk.Busy = FALSE;
  gBS->RestoreTPL (Previous);
}

/* Private logger uses the same serialization as physical reads, including
   reads retained after public BlockIo removal. No write protocol is exposed. */
EFI_STATUS EFIAPI J250fLogBackendWrite(EFI_LBA Lba, CONST VOID *Buffer) {
  EFI_STATUS Status;
  if (!mPublished) return EFI_NO_MEDIA;
  if (Lba > mDisk.Media.LastBlock || !Buffer) return EFI_INVALID_PARAMETER;
  if (!Enter()) return EFI_NOT_READY;
  Status = J250fLogBoardWrite(Lba, Buffer);
  Leave();
  return Status;
}

static EFI_STATUS EFIAPI LkReset (
  IN EFI_BLOCK_IO_PROTOCOL *This, IN BOOLEAN ExtendedVerification
) {
  (VOID)ExtendedVerification;
  if (This != &mDisk.Io) return EFI_INVALID_PARAMETER;
  // A destructive controller reset requires a platform-specific recovery path.
  // Extended verification performs an actual read of LBA 0 instead of a
  // misleading unconditional-success reset. No eMMC writes are attempted.
  if (!mDisk.Media.MediaPresent) return EFI_NO_MEDIA;
  if (ExtendedVerification) {
    UINT8 Sector[J250F_LK_SECTOR_BYTES];
    EFI_STATUS Status;
    if (!Enter()) return EFI_NOT_READY;
    Status=mDisk.Lk.ReadBlocks(mDisk.Lk.Device,0,1,Sector);
    Leave();
    return Status;
  }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI LkRead (
  IN EFI_BLOCK_IO_PROTOCOL *This, IN UINT32 MediaId, IN EFI_LBA Lba,
  IN UINTN BufferSize, OUT VOID *Buffer
) {
  EFI_STATUS Status;
  UINTN Count;
  if (This != &mDisk.Io) return EFI_INVALID_PARAMETER;
  if (MediaId != mDisk.Media.MediaId) return EFI_MEDIA_CHANGED;
  if (!mDisk.Media.MediaPresent) return EFI_NO_MEDIA;
  if (BufferSize == 0) return EFI_SUCCESS;
  if (Buffer == NULL) return EFI_INVALID_PARAMETER;
  if ((BufferSize % mDisk.Media.BlockSize) != 0) return EFI_BAD_BUFFER_SIZE;
  if (mDisk.Media.IoAlign > 1 &&
      (((UINTN)Buffer & (mDisk.Media.IoAlign - 1)) != 0))
    return EFI_INVALID_PARAMETER;
  Count = BufferSize / mDisk.Media.BlockSize;
  if (Count == 0 || Lba > mDisk.Media.LastBlock ||
      (EFI_LBA)(Count - 1) > mDisk.Media.LastBlock - Lba)
    return EFI_INVALID_PARAMETER;
  if (!Enter ()) return EFI_NOT_READY;
  Status = mDisk.Lk.ReadBlocks (mDisk.Lk.Device, Lba, Count, Buffer);
  Leave ();
  return Status;
}
static EFI_STATUS EFIAPI LkWrite (
  IN EFI_BLOCK_IO_PROTOCOL *This, IN UINT32 MediaId, IN EFI_LBA Lba,
  IN UINTN BufferSize, IN VOID *Buffer
) {
  (VOID)Lba; (VOID)BufferSize; (VOID)Buffer;
  if (This != &mDisk.Io) return EFI_INVALID_PARAMETER;
  if (MediaId != mDisk.Media.MediaId) return EFI_MEDIA_CHANGED;
  return mDisk.Media.MediaPresent ? EFI_WRITE_PROTECTED : EFI_NO_MEDIA;
}
static EFI_STATUS EFIAPI LkFlush (IN EFI_BLOCK_IO_PROTOCOL *This) {
  if (This != &mDisk.Io) return EFI_INVALID_PARAMETER;
  return mDisk.Media.MediaPresent ? EFI_SUCCESS : EFI_NO_MEDIA;
}

STATIC EFI_STATUS
InstallDisk(VOID) {
  EFI_STATUS Status;
  if (mPublished) return EFI_SUCCESS;
  ZeroMem(&mDisk,sizeof(mDisk));
  Status=J250fLkBoardOpen(&mDisk.Lk);
  if (EFI_ERROR(Status)) {
    DEBUG((DEBUG_ERROR,"J250F eMMC: not published: %r\n",Status));
    return Status;
  }
  if (mDisk.Lk.Device==NULL || mDisk.Lk.ReadBlocks==NULL ||
      mDisk.Lk.NumberOfBlocks==0 || mDisk.Lk.BlockSize!=512 ||
      mDisk.Lk.IoAlignment==0 ||
      (mDisk.Lk.IoAlignment & (mDisk.Lk.IoAlignment-1))!=0)
    return EFI_DEVICE_ERROR;
  mDisk.Media.MediaId=1;
  mDisk.Media.MediaPresent=TRUE;
  mDisk.Media.RemovableMedia=FALSE;
  mDisk.Media.LogicalPartition=FALSE;
  mDisk.Media.ReadOnly=TRUE;
  mDisk.Media.WriteCaching=FALSE;
  mDisk.Media.BlockSize=mDisk.Lk.BlockSize;
  mDisk.Media.IoAlign=mDisk.Lk.IoAlignment;
  mDisk.Media.LastBlock=mDisk.Lk.NumberOfBlocks-1;
  mDisk.Media.LogicalBlocksPerPhysicalBlock=1;
  mDisk.Media.OptimalTransferLengthGranularity=1;
  mDisk.Io.Revision=EFI_BLOCK_IO_PROTOCOL_REVISION3;
  mDisk.Io.Media=&mDisk.Media;
  mDisk.Io.Reset=LkReset;
  mDisk.Io.ReadBlocks=LkRead;
  mDisk.Io.WriteBlocks=LkWrite;
  mDisk.Io.FlushBlocks=LkFlush;
  mDisk.Path.Vendor.Header.Type=HARDWARE_DEVICE_PATH;
  mDisk.Path.Vendor.Header.SubType=HW_VENDOR_DP;
  mDisk.Path.Vendor.Header.Length[0]=sizeof(VENDOR_DEVICE_PATH);
  mDisk.Path.Vendor.Header.Length[1]=0;
  CopyMem(&mDisk.Path.Vendor.Guid,&mDiskPathGuid,sizeof(EFI_GUID));
  mDisk.Path.End.Type=END_DEVICE_PATH_TYPE;
  mDisk.Path.End.SubType=END_ENTIRE_DEVICE_PATH_SUBTYPE;
  mDisk.Path.End.Length[0]=sizeof(EFI_DEVICE_PATH_PROTOCOL);
  mDisk.Path.End.Length[1]=0;
  Status=gBS->InstallMultipleProtocolInterfaces(
    &mDisk.Handle,
    &gEfiBlockIoProtocolGuid,&mDisk.Io,
    &gEfiDevicePathProtocolGuid,&mDisk.Path,NULL);
  if (!EFI_ERROR(Status)) {
    mPublished=TRUE;
    DEBUG((DEBUG_INFO,"J250F eMMC: UEFI BlockIo published: %Lu sectors\n",
      (UINT64)mDisk.Lk.NumberOfBlocks));
  }
  return Status;
}

EFI_STATUS EFIAPI J250fLkEmmcEntry(
  IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable
) {
  EFI_STATUS Status;
  CHAR8 Text[256];
  UINTN Len, Second;
  (VOID)SystemTable;
  J250fAdbShellInit(ImageHandle);
  J250fWpInit(ImageHandle);
  // DEPEX guarantees CPU/GOP/power readiness. No callback can outlive this
  // image if initialization returns an error and DXE unloads it.
  SerialPortInitialize();
  // DEBUG uses the framebuffer SerialPortLib, which exports FbConStartScreen.
  // RELEASE links BaseSerialPortLibNull, which does not export it.  Keep the
  // debug-only banner without introducing an undefined symbol in RELEASE.
#if defined(J250F_ADB_RAM_EXECUTION)
  FbConStartScreen();
#endif

  /* Recovery for firmware where the early GOP DXE was not dispatched or its
     protocol disappeared. ADB diagnostics proved zero GOP handles even while
     Block I/O was running. Install the same GOP implementation here, BEFORE
     entering the persistent USB session, so graphics can be tested in ADB.
     This is protocol-only; no eMMC media or partitions are written. */
  {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop = NULL;
    EFI_STATUS GopStatus = gBS->LocateProtocol(
      &gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
    EFI_STATUS BeforeStatus = GopStatus;
    EFI_STATUS InstallStatus = EFI_NOT_READY;
    BOOLEAN FallbackAttempted = FALSE;
    if (EFI_ERROR(GopStatus) || Gop == NULL) {
      FallbackAttempted = TRUE;
      /* ADB recovery must preserve Samsung's working framebuffer. It must
         never experiment with MDP or 32-bit scanout while debugging GOP. */
      InstallStatus = J250fGopPublish(ImageHandle, SystemTable, FALSE);
    } else {
      InstallStatus = EFI_ALREADY_STARTED;
    }
    Gop = NULL;
    GopStatus = gBS->LocateProtocol(
      &gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
    if (!EFI_ERROR(GopStatus) && Gop == NULL)
      GopStatus = EFI_NOT_FOUND;
    J250fGopDiagRecord(BeforeStatus, InstallStatus, GopStatus,
                       FallbackAttempted);
    Len = AsciiSPrint(Text, sizeof(Text),
      "J250F GOP: fallback=%u, before=%r install=%r after=%r, pointer=%p\r\n",
      (UINT32)FallbackAttempted, BeforeStatus, InstallStatus, GopStatus, Gop);
    SerialPortWrite((UINT8 *)Text, Len);
  }
  Len=AsciiSPrint(Text,sizeof(Text),"J250F: initializing eMMC (one-bit, 400kHz)...\r\n");
  SerialPortWrite((UINT8*)Text,Len);
  {
    EFI_STATUS UsbStatus=J250fUsbGadgetStart(J250fUsbDiagCommand);
    Len=AsciiSPrint(Text,sizeof(Text),"ADB DEBUG start: %r\r\n",UsbStatus);
    SerialPortWrite((UINT8*)Text,Len);
    Len=AsciiSPrint(Text,sizeof(Text),"USB GCC CORE=%08x AHB=%08x USBMODE=%08x\r\n",
      MmioRead32(0x01841004u),MmioRead32(0x01841008u),MmioRead32(0x078DB1A8u));
    SerialPortWrite((UINT8*)Text,Len);
  }
  Status=InstallDisk();
  /* DiskIo, GPT PartitionDxe and ext4 are DXE APRIORI drivers. This entry
     runs a persistent ADB loop, so DXE would not dispatch more drivers until
     firmware exit. Connect the read-only physical disk recursively first. */
  if (!EFI_ERROR(Status) && mPublished) {
    EFI_STATUS Connect = gBS->ConnectController(mDisk.Handle, NULL, NULL, TRUE);
    Len = AsciiSPrint(Text, sizeof(Text),
      "Read-only partition/filesystem connection: %r\r\n", Connect);
    SerialPortWrite((UINT8 *)Text, Len);
  }
  /* The normal boot path: stage only EFIESP, conceal public physical eMMC,
     create BootFF10, then RETURN from this DXE entry. EndOfDxe and BDS must
     run before Windows Boot Manager is loaded. If staging fails, retain ADB
     diagnostics and do not enter BDS with the original disk visible. */
  if (!EFI_ERROR(Status) && mPublished) {
    EFI_STATUS BootStatus=J250fWpPrepareBds();
    Len=AsciiSPrint(Text,sizeof(Text),
      "WP81 EFI-only BDS preparation: %r\r\n",BootStatus);
    SerialPortWrite((UINT8*)Text,Len);
    if (!EFI_ERROR(BootStatus)) {
      J250fUsbGadgetStop();
      J250fAdbShellFinish();
      return EFI_SUCCESS;
    }
  }
  Len=AsciiSPrint(Text,sizeof(Text),
    "eMMC disk initialization: %r\r\nBlock IO published: %a\r\n"
    "ADB session stays available until firmware exit or reboot.\r\n",Status,mPublished?"YES (read-only)":"NO");
  SerialPortWrite((UINT8*)Text,Len);
  while(!J250fAdbShellExitRequested()) {
    EFI_STATUS Usb=J250fUsbGadgetPoll();
    if(EFI_ERROR(Usb)){
      J250fUsbGadgetStop();gBS->Stall(1000000);
      (VOID)J250fUsbGadgetStart(J250fUsbDiagCommand);
    }
    gBS->Stall(1000);
  }
  /* Allow final WRTE/CLSE responses to drain before disconnecting. */
  for(Second=0;Second<500;++Second){(VOID)J250fUsbGadgetPoll();gBS->Stall(1000);}
  J250fUsbGadgetStop();
  J250fAdbShellFinish();
  return EFI_SUCCESS;
}
