/* SimpleFbDxe: Simple FrameBuffer */
#include <PiDxe.h>
#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/FrameBufferBltLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Protocol/GraphicsOutput.h>

/// Defines
/*
 * Convert enum video_log2_bpp to bytes and bits. Note we omit the outer
 * brackets to allow multiplication by fractional pixels.
 */
#define VNBYTES(bpix) (1 << (bpix)) / 8
#define VNBITS(bpix) (1 << (bpix))

#define POS_TO_FB(posX, posY)                                                  \
  ((UINT8                                                                      \
        *)((UINTN)This->Mode->FrameBufferBase + (posY)*This->Mode->Info->PixelsPerScanLine * FB_BYTES_PER_PIXEL + (posX)*FB_BYTES_PER_PIXEL))

#define FB_BITS_PER_PIXEL (32)
#define FB_BYTES_PER_PIXEL (FB_BITS_PER_PIXEL / 8)
#define DISPLAYDXE_PHYSICALADDRESS32(_x_) (UINTN)((_x_)&0xFFFFFFFF)

#define DISPLAYDXE_RED_MASK 0xFF0000
#define DISPLAYDXE_GREEN_MASK 0x00FF00
#define DISPLAYDXE_BLUE_MASK 0x0000FF
#define DISPLAYDXE_ALPHA_MASK 0x000000

/*
 * Bits per pixel selector. Each value n is such that the bits-per-pixel is
 * 2 ^ n
 */
enum video_log2_bpp {
  VIDEO_BPP1 = 0,
  VIDEO_BPP2,
  VIDEO_BPP4,
  VIDEO_BPP8,
  VIDEO_BPP16,
  VIDEO_BPP32,
};

typedef struct {
  VENDOR_DEVICE_PATH DisplayDevicePath;
  EFI_DEVICE_PATH    EndDevicePath;
} DISPLAY_DEVICE_PATH;

DISPLAY_DEVICE_PATH mDisplayDevicePath = {
    {{HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      {
          (UINT8)(sizeof(VENDOR_DEVICE_PATH)),
          (UINT8)((sizeof(VENDOR_DEVICE_PATH)) >> 8),
      }},
     EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID},
    {END_DEVICE_PATH_TYPE,
     END_ENTIRE_DEVICE_PATH_SUBTYPE,
     {sizeof(EFI_DEVICE_PATH_PROTOCOL), 0}}};

/// Declares

STATIC FRAME_BUFFER_CONFIGURE *mFrameBufferBltLibConfigure;
STATIC UINTN mFrameBufferBltLibConfigureSize;

STATIC UINT8  *mFbBase;
STATIC UINT32 mFbWidth;
STATIC UINT32 mFbHeight;
STATIC UINT32 mFbStride;
STATIC UINT32 mFbBpp;

STATIC
EFI_STATUS
EFIAPI
DisplayQueryMode(
    IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This, IN UINT32 ModeNumber,
    OUT UINTN *SizeOfInfo, OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);

STATIC
EFI_STATUS
EFIAPI
DisplaySetMode(IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This, IN UINT32 ModeNumber);

STATIC
EFI_STATUS
EFIAPI
DisplayBlt(
    IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL *BltBuffer,
    OPTIONAL IN EFI_GRAPHICS_OUTPUT_BLT_OPERATION BltOperation,
    IN UINTN SourceX, IN UINTN SourceY, IN UINTN DestinationX,
    IN UINTN DestinationY, IN UINTN Width, IN UINTN Height,
    IN UINTN Delta OPTIONAL);

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL mDisplay = {
    DisplayQueryMode, DisplaySetMode, DisplayBlt, NULL};

STATIC
EFI_STATUS
EFIAPI
DisplayQueryMode(
    IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This, IN UINT32 ModeNumber,
    OUT UINTN *SizeOfInfo, OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info)
{
  EFI_STATUS Status;
  Status = gBS->AllocatePool(
      EfiBootServicesData, sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION),
      (VOID **)Info);

  ASSERT_EFI_ERROR(Status);

  *SizeOfInfo                   = sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION);
  (*Info)->Version              = This->Mode->Info->Version;
  (*Info)->HorizontalResolution = This->Mode->Info->HorizontalResolution;
  (*Info)->VerticalResolution   = This->Mode->Info->VerticalResolution;
  (*Info)->PixelFormat          = This->Mode->Info->PixelFormat;
  (*Info)->PixelsPerScanLine    = This->Mode->Info->PixelsPerScanLine;

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
DisplaySetMode(IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This, IN UINT32 ModeNumber)
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
DisplayBlt(
    IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL *BltBuffer,
    OPTIONAL IN EFI_GRAPHICS_OUTPUT_BLT_OPERATION BltOperation,
    IN UINTN SourceX, IN UINTN SourceY, IN UINTN DestinationX,
    IN UINTN DestinationY, IN UINTN Width, IN UINTN Height,
    IN UINTN Delta OPTIONAL)
{
  EFI_TPL Tpl;

  if (mFbBpp != 24) {
    Tpl    = gBS->RaiseTPL(TPL_NOTIFY);
    RETURN_STATUS Status = FrameBufferBlt(
        mFrameBufferBltLibConfigure, BltBuffer, BltOperation, SourceX, SourceY,
        DestinationX, DestinationY, Width, Height, Delta);
    gBS->RestoreTPL(Tpl);
    return RETURN_ERROR(Status) ? EFI_INVALID_PARAMETER : EFI_SUCCESS;
  }

  // Native 24bpp packed RGB888 Blt handler
  if (mFbBase == NULL || Width == 0 || Height == 0) {
    return EFI_INVALID_PARAMETER;
  }

  if (BltOperation == EfiBltVideoFill || BltOperation == EfiBltBufferToVideo) {
    if (DestinationX + Width > mFbWidth || DestinationY + Height > mFbHeight) {
      return EFI_INVALID_PARAMETER;
    }
  } else if (BltOperation == EfiBltVideoToBltBuffer) {
    if (SourceX + Width > mFbWidth || SourceY + Height > mFbHeight) {
      return EFI_INVALID_PARAMETER;
    }
  } else if (BltOperation == EfiBltVideoToVideo) {
    if (SourceX + Width > mFbWidth || SourceY + Height > mFbHeight ||
        DestinationX + Width > mFbWidth || DestinationY + Height > mFbHeight) {
      return EFI_INVALID_PARAMETER;
    }
  } else {
    return EFI_INVALID_PARAMETER;
  }

  if (Delta == 0) {
    Delta = Width * sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL);
  }

  UINTN LinePitch = mFbStride * 3;
  UINTN x, y;

  Tpl = gBS->RaiseTPL(TPL_NOTIFY);

  switch (BltOperation) {
  case EfiBltVideoFill:
    {
      UINT8 R = BltBuffer->Red;
      UINT8 G = BltBuffer->Green;
      UINT8 B = BltBuffer->Blue;

      for (y = 0; y < Height; y++) {
        UINT8 *DstRow = mFbBase + ((DestinationY + y) * LinePitch) + (DestinationX * 3);
        for (x = 0; x < Width; x++) {
          DstRow[x * 3 + 0] = R;
          DstRow[x * 3 + 1] = G;
          DstRow[x * 3 + 2] = B;
        }
      }
      WriteBackInvalidateDataCacheRange(
          mFbBase + (DestinationY * LinePitch),
          Height * LinePitch);
      break;
    }

  case EfiBltBufferToVideo:
    {
      for (y = 0; y < Height; y++) {
        EFI_GRAPHICS_OUTPUT_BLT_PIXEL *SrcRow = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)
            ((UINT8 *)BltBuffer + ((SourceY + y) * Delta) + (SourceX * sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL)));
        UINT8 *DstRow = mFbBase + ((DestinationY + y) * LinePitch) + (DestinationX * 3);

        for (x = 0; x < Width; x++) {
          DstRow[x * 3 + 0] = SrcRow[x].Red;
          DstRow[x * 3 + 1] = SrcRow[x].Green;
          DstRow[x * 3 + 2] = SrcRow[x].Blue;
        }
      }
      WriteBackInvalidateDataCacheRange(
          mFbBase + (DestinationY * LinePitch),
          Height * LinePitch);
      break;
    }

  case EfiBltVideoToBltBuffer:
    {
      for (y = 0; y < Height; y++) {
        UINT8 *SrcRow = mFbBase + ((SourceY + y) * LinePitch) + (SourceX * 3);
        EFI_GRAPHICS_OUTPUT_BLT_PIXEL *DstRow = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)
            ((UINT8 *)BltBuffer + ((DestinationY + y) * Delta) + (DestinationX * sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL)));

        for (x = 0; x < Width; x++) {
          DstRow[x].Red      = SrcRow[x * 3 + 0];
          DstRow[x].Green    = SrcRow[x * 3 + 1];
          DstRow[x].Blue     = SrcRow[x * 3 + 2];
          DstRow[x].Reserved = 0xFF;
        }
      }
      break;
    }

  case EfiBltVideoToVideo:
    {
      if (DestinationY > SourceY) {
        for (INTN dy = (INTN)Height - 1; dy >= 0; dy--) {
          UINT8 *SrcRow = mFbBase + ((SourceY + dy) * LinePitch) + (SourceX * 3);
          UINT8 *DstRow = mFbBase + ((DestinationY + dy) * LinePitch) + (DestinationX * 3);
          CopyMem (DstRow, SrcRow, Width * 3);
        }
      } else {
        for (y = 0; y < Height; y++) {
          UINT8 *SrcRow = mFbBase + ((SourceY + y) * LinePitch) + (SourceX * 3);
          UINT8 *DstRow = mFbBase + ((DestinationY + y) * LinePitch) + (DestinationX * 3);
          CopyMem (DstRow, SrcRow, Width * 3);
        }
      }
      UINTN FlushTop = (DestinationY < SourceY) ? DestinationY : SourceY;
      UINTN FlushH   = Height + ((DestinationY > SourceY) ? (DestinationY - SourceY) : (SourceY - DestinationY));
      WriteBackInvalidateDataCacheRange(
          mFbBase + (FlushTop * LinePitch),
          FlushH * LinePitch);
      break;
    }

  default:
    gBS->RestoreTPL(Tpl);
    return EFI_INVALID_PARAMETER;
  }

  gBS->RestoreTPL(Tpl);
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
SimpleFbDxeInitialize(
    IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{

  EFI_STATUS Status             = EFI_SUCCESS;
  EFI_HANDLE hUEFIDisplayHandle = NULL;

  /* Retrieve simple frame buffer from pre-SEC bootloader */
  DEBUG(
      (EFI_D_INFO,
       "SimpleFbDxe: Retrieve MIPI FrameBuffer parameters from PCD\n"));
  ARM_MEMORY_REGION_DESCRIPTOR_EX DisplayMemoryRegion;
  LocateMemoryMapAreaByName("Display Reserved", &DisplayMemoryRegion);
  UINT32 MipiFrameBufferAddr   = DisplayMemoryRegion.Address;
  UINT32 MipiFrameBufferWidth  = FixedPcdGet32(PcdMipiFrameBufferWidth);
  UINT32 MipiFrameBufferHeight = FixedPcdGet32(PcdMipiFrameBufferHeight);
  UINT32 MipiFrameBufferBpp    = FixedPcdGet32(PcdMipiFrameBufferPixelBpp);
  UINT32 MipiFrameBufferStride = FixedPcdGet32(PcdMipiFrameBufferPixelsPerScanLine);
  UINT64 CustomFbAddr          = FixedPcdGet64(PcdMipiFrameBufferAddress);

  if (CustomFbAddr != 0) {
    MipiFrameBufferAddr = (UINT32)CustomFbAddr;
  }

  if (MipiFrameBufferStride == 0) {
    MipiFrameBufferStride = MipiFrameBufferWidth;
  }
  if (MipiFrameBufferBpp == 0) {
    MipiFrameBufferBpp = 32;
  }

  /* Sanity check */
  if (MipiFrameBufferAddr == 0 || MipiFrameBufferWidth == 0 ||
      MipiFrameBufferHeight == 0) {
    DEBUG((EFI_D_ERROR, "SimpleFbDxe: Invalid FrameBuffer parameters\n"));
    return EFI_DEVICE_ERROR;
  }

  mFbBase   = (UINT8 *)(UINTN)MipiFrameBufferAddr;
  mFbWidth  = MipiFrameBufferWidth;
  mFbHeight = MipiFrameBufferHeight;
  mFbStride = MipiFrameBufferStride;
  mFbBpp    = MipiFrameBufferBpp;

  /* Prepare struct */
  if (mDisplay.Mode == NULL) {
    Status = gBS->AllocatePool(
        EfiBootServicesData, sizeof(EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE),
        (VOID **)&mDisplay.Mode);

    ASSERT_EFI_ERROR(Status);
    if (EFI_ERROR(Status))
      return Status;

    ZeroMem(mDisplay.Mode, sizeof(EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE));
  }

  if (mDisplay.Mode->Info == NULL) {
    Status = gBS->AllocatePool(
        EfiBootServicesData, sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION),
        (VOID **)&mDisplay.Mode->Info);

    ASSERT_EFI_ERROR(Status);
    if (EFI_ERROR(Status))
      return Status;

    ZeroMem(mDisplay.Mode->Info, sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION));
  }

  /* Set information */
  mDisplay.Mode->MaxMode       = 1;
  mDisplay.Mode->Mode          = 0;
  mDisplay.Mode->Info->Version = 0;

  mDisplay.Mode->Info->HorizontalResolution = MipiFrameBufferWidth;
  mDisplay.Mode->Info->VerticalResolution   = MipiFrameBufferHeight;
  mDisplay.Mode->Info->PixelsPerScanLine    = MipiFrameBufferStride;

  UINT32               BytesPerPixel      = MipiFrameBufferBpp / 8;
  UINT32               LineLength         = MipiFrameBufferStride * BytesPerPixel;
  UINT32               FrameBufferSize    = LineLength * MipiFrameBufferHeight;
  EFI_PHYSICAL_ADDRESS FrameBufferAddress = MipiFrameBufferAddr;

  if (MipiFrameBufferBpp == 24) {
    // 24bpp packed RGB888: Use PixelBltOnly so all callers use our custom DisplayBlt
    mDisplay.Mode->Info->PixelFormat = PixelBltOnly;
    mDisplay.Mode->FrameBufferBase   = 0;
    mDisplay.Mode->FrameBufferSize   = 0;
  } else {
#if DISPLAY_USES_RGBA == 1
    mDisplay.Mode->Info->PixelFormat = PixelRedGreenBlueReserved8BitPerColor;
#else
    mDisplay.Mode->Info->PixelFormat = PixelBlueGreenRedReserved8BitPerColor;
#endif
    mDisplay.Mode->FrameBufferBase = FrameBufferAddress;
    mDisplay.Mode->FrameBufferSize = FrameBufferSize;

    /* Create the FrameBufferBltLib configuration for 32bpp fallback. */
    Status = FrameBufferBltConfigure(
        (VOID *)(UINTN)mDisplay.Mode->FrameBufferBase, mDisplay.Mode->Info,
        mFrameBufferBltLibConfigure, &mFrameBufferBltLibConfigureSize);

    if (Status == RETURN_BUFFER_TOO_SMALL) {
      mFrameBufferBltLibConfigure = AllocatePool(mFrameBufferBltLibConfigureSize);
      if (mFrameBufferBltLibConfigure != NULL) {
        Status = FrameBufferBltConfigure(
            (VOID *)(UINTN)mDisplay.Mode->FrameBufferBase, mDisplay.Mode->Info,
            mFrameBufferBltLibConfigure, &mFrameBufferBltLibConfigureSize);
      }
    }
    ASSERT_EFI_ERROR(Status);
  }

  mDisplay.Mode->SizeOfInfo = sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION);

  /* Register handle */
  Status = gBS->InstallMultipleProtocolInterfaces(
      &hUEFIDisplayHandle, &gEfiDevicePathProtocolGuid, &mDisplayDevicePath,
      &gEfiGraphicsOutputProtocolGuid, &mDisplay, NULL);

  ASSERT_EFI_ERROR(Status);

  return Status;
}
