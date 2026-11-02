/** @file
  Samsung J250F inherited-framebuffer GOP.

  This is deliberately NOT a display controller mode-setting driver.  Samsung
  aboot initializes the DSI panel; this DXE exposes the scanned-out buffer.

  Known J250F handoff: 540x960 packed RGB888, at 0xA800035A.  UEFI requires
  PixelBltOnly for a format that cannot be described by standard 32-bit GOP
  pixel formats.  Never pretend packed 24-bit hardware is linear BGRX.

  J250fMdp32.c can attempt a STRICTLY-GUARDED conversion of an already
  running Samsung splash pipe to a newly reserved physical BGRX scanout.
  If register checks fail, the original 24-bit GOP remains active. This
  transition is experimental until physically tested on the J250F.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <PiDxe.h>

#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/GraphicsOutput.h>

#ifndef J250F_GOP_NATIVE_BGRX
#define J250F_GOP_NATIVE_BGRX 0
#endif

#define J250F_FB_REGION_BASE  0xA8000000u
#define J250F_FB_REGION_SIZE  0x00300000u

typedef struct {
  VENDOR_DEVICE_PATH      Vendor;
  EFI_DEVICE_PATH_PROTOCOL End;
} J250F_GOP_DEVICE_PATH;

STATIC J250F_GOP_DEVICE_PATH mDevicePath = {
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      { (UINT8)sizeof(VENDOR_DEVICE_PATH),
        (UINT8)(sizeof(VENDOR_DEVICE_PATH) >> 8) }
    },
    EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { (UINT8)sizeof(EFI_DEVICE_PATH_PROTOCOL), 0 }
  }
};

STATIC EFI_GRAPHICS_OUTPUT_MODE_INFORMATION mInfo;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE    mMode;
STATIC EFI_HANDLE                           mHandle;
STATIC UINT8                               *mPixels;
STATIC UINTN                                mWidth;
STATIC UINTN                                mHeight;
STATIC UINTN                                mStride;
STATIC UINTN                                mBytesPerPixel;
STATIC UINTN                                mFramebufferBytes;
STATIC BOOLEAN                              mNativeBgrx = (J250F_GOP_NATIVE_BGRX != 0);

EFI_STATUS EFIAPI J250fTryNativeBgrx(
  IN UINT64 OldAddress, IN UINT32 Width, IN UINT32 Height,
  IN UINT32 Stride, OUT EFI_PHYSICAL_ADDRESS *NewAddress);

STATIC EFI_STATUS EFIAPI J250fQueryMode(
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
  IN UINT32 ModeNumber,
  OUT UINTN *SizeOfInfo,
  OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info
  );

STATIC EFI_STATUS EFIAPI J250fSetMode(
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
  IN UINT32 ModeNumber
  );

STATIC EFI_STATUS EFIAPI J250fBlt(
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
  IN OUT EFI_GRAPHICS_OUTPUT_BLT_PIXEL *BltBuffer OPTIONAL,
  IN EFI_GRAPHICS_OUTPUT_BLT_OPERATION BltOperation,
  IN UINTN SourceX,
  IN UINTN SourceY,
  IN UINTN DestinationX,
  IN UINTN DestinationY,
  IN UINTN Width,
  IN UINTN Height,
  IN UINTN Delta OPTIONAL
  );

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL mGop = {
  J250fQueryMode,
  J250fSetMode,
  J250fBlt,
  &mMode
};

STATIC BOOLEAN
ScreenRectFits(UINTN X, UINTN Y, UINTN W, UINTN H)
{
  return W != 0 && H != 0 &&
         X < mWidth && Y < mHeight &&
         W <= mWidth - X && H <= mHeight - Y;
}

STATIC UINT8 *
PhysicalPixel(UINTN X, UINTN Y)
{
  return mPixels + (Y * mStride + X) * mBytesPerPixel;
}

/* The inherited 24-bit Samsung panel buffer uses R, G, B byte order.
   The optional 32-bit case must match the ACTUAL MDP scanout byte order. */
STATIC VOID
PutPixel(UINTN X, UINTN Y, CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL *P)
{
  UINT8 *Dst = PhysicalPixel(X, Y);
  if (mBytesPerPixel == 3) {
    Dst[0] = P->Red;
    Dst[1] = P->Green;
    Dst[2] = P->Blue;
  } else {
    if (mNativeBgrx) {
      Dst[0] = P->Blue;
      Dst[1] = P->Green;
      Dst[2] = P->Red;
    } else {
      Dst[0] = P->Red;
      Dst[1] = P->Green;
      Dst[2] = P->Blue;
    }
    Dst[3] = 0;
  }
}

STATIC VOID
GetPixel(UINTN X, UINTN Y, EFI_GRAPHICS_OUTPUT_BLT_PIXEL *P)
{
  CONST UINT8 *Src = PhysicalPixel(X, Y);
  if (mBytesPerPixel == 3) {
    P->Red = Src[0];
    P->Green = Src[1];
    P->Blue = Src[2];
  } else {
    if (mNativeBgrx) {
      P->Blue = Src[0];
      P->Green = Src[1];
      P->Red = Src[2];
    } else {
      P->Red = Src[0];
      P->Green = Src[1];
      P->Blue = Src[2];
    }
  }
  P->Reserved = 0;
}

STATIC VOID
FlushRect(UINTN X, UINTN Y, UINTN W, UINTN H)
{
  UINT8 *First = PhysicalPixel(X, Y);
  UINTN Size = (H - 1) * mStride * mBytesPerPixel +
               W * mBytesPerPixel;
  WriteBackInvalidateDataCacheRange(First, Size);
}

STATIC EFI_STATUS
CheckBufferLayout(UINTN BufferX, UINTN BufferY,
                  UINTN Width, UINTN Height, UINTN *Delta)
{
  UINTN RowBytes;
  UINTN LastRow;

  if (BufferX > MAX_UINTN / sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL) - Width)
    return EFI_INVALID_PARAMETER;

  RowBytes = (BufferX + Width) * sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL);
  if (*Delta == 0)
    *Delta = Width * sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL);
  if (*Delta < RowBytes || BufferY > MAX_UINTN - (Height - 1))
    return EFI_INVALID_PARAMETER;

  LastRow = BufferY + Height - 1;
  if (LastRow > (MAX_UINTN - RowBytes) / *Delta)
    return EFI_INVALID_PARAMETER;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
J250fQueryMode(IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
               IN UINT32 ModeNumber, OUT UINTN *SizeOfInfo,
               OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info)
{
  EFI_STATUS Status;
  if (This != &mGop || SizeOfInfo == NULL || Info == NULL)
    return EFI_INVALID_PARAMETER;
  *Info = NULL;
  if (ModeNumber != 0)
    return EFI_INVALID_PARAMETER;

  Status = gBS->AllocatePool(
    EfiBootServicesData, sizeof(mInfo), (VOID **)Info);
  if (EFI_ERROR(Status))
    return Status;
  CopyMem(*Info, &mInfo, sizeof(mInfo));
  *SizeOfInfo = sizeof(mInfo);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
J250fSetMode(IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This, IN UINT32 ModeNumber)
{
  if (This != &mGop || ModeNumber != 0)
    return EFI_INVALID_PARAMETER;
  /* No alternate timings: the original Samsung bootloader owns DSI init.
     Reprogramming MDSS without a board-specific sequence could blank panel. */
  mMode.Mode = 0;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
J250fBlt(IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
         IN OUT EFI_GRAPHICS_OUTPUT_BLT_PIXEL *BltBuffer OPTIONAL,
         IN EFI_GRAPHICS_OUTPUT_BLT_OPERATION Op,
         IN UINTN SourceX, IN UINTN SourceY,
         IN UINTN DestinationX, IN UINTN DestinationY,
         IN UINTN Width, IN UINTN Height, IN UINTN Delta OPTIONAL)
{
  UINTN Row, Col, BufferX, BufferY;
  EFI_STATUS Status;
  EFI_TPL OldTpl;

  if (This != &mGop || mPixels == NULL || Width == 0 || Height == 0 ||
      Op >= EfiGraphicsOutputBltOperationMax)
    return EFI_INVALID_PARAMETER;

  if (Op == EfiBltVideoFill || Op == EfiBltBufferToVideo) {
    if (!ScreenRectFits(DestinationX, DestinationY, Width, Height))
      return EFI_INVALID_PARAMETER;
  }
  if (Op == EfiBltVideoToBltBuffer || Op == EfiBltVideoToVideo) {
    if (!ScreenRectFits(SourceX, SourceY, Width, Height))
      return EFI_INVALID_PARAMETER;
  }
  if (Op == EfiBltVideoToVideo &&
      !ScreenRectFits(DestinationX, DestinationY, Width, Height))
    return EFI_INVALID_PARAMETER;

  if (Op != EfiBltVideoToVideo && BltBuffer == NULL)
    return EFI_INVALID_PARAMETER;

  if (Op == EfiBltBufferToVideo || Op == EfiBltVideoToBltBuffer) {
    BufferX = (Op == EfiBltBufferToVideo) ? SourceX : DestinationX;
    BufferY = (Op == EfiBltBufferToVideo) ? SourceY : DestinationY;
    Status = CheckBufferLayout(BufferX, BufferY, Width, Height, &Delta);
    if (EFI_ERROR(Status))
      return Status;
  }

  OldTpl = gBS->RaiseTPL(TPL_NOTIFY);
  switch (Op) {
  case EfiBltVideoFill:
    for (Row = 0; Row < Height; ++Row)
      for (Col = 0; Col < Width; ++Col)
        PutPixel(DestinationX + Col, DestinationY + Row, BltBuffer);
    FlushRect(DestinationX, DestinationY, Width, Height);
    break;

  case EfiBltBufferToVideo:
    for (Row = 0; Row < Height; ++Row) {
      CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Src =
        (CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)(
          (CONST UINT8 *)BltBuffer + (SourceY + Row) * Delta +
          SourceX * sizeof(*BltBuffer));
      for (Col = 0; Col < Width; ++Col)
        PutPixel(DestinationX + Col, DestinationY + Row, &Src[Col]);
    }
    FlushRect(DestinationX, DestinationY, Width, Height);
    break;

  case EfiBltVideoToBltBuffer:
    for (Row = 0; Row < Height; ++Row) {
      EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Dst =
        (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)(
          (UINT8 *)BltBuffer + (DestinationY + Row) * Delta +
          DestinationX * sizeof(*BltBuffer));
      for (Col = 0; Col < Width; ++Col)
        GetPixel(SourceX + Col, SourceY + Row, &Dst[Col]);
    }
    break;

  case EfiBltVideoToVideo:
    /* Copy backwards for overlapping moves, just like memmove. */
    for (Row = 0; Row < Height; ++Row) {
      UINTN R = (DestinationY > SourceY) ? Height - 1 - Row : Row;
      for (Col = 0; Col < Width; ++Col) {
        UINTN C = (DestinationY == SourceY && DestinationX > SourceX)
                    ? Width - 1 - Col : Col;
        UINT8 *Dst = PhysicalPixel(DestinationX + C, DestinationY + R);
        CONST UINT8 *Src = PhysicalPixel(SourceX + C, SourceY + R);
        CopyMem(Dst, Src, mBytesPerPixel);
      }
    }
    FlushRect(DestinationX, DestinationY, Width, Height);
    break;

  default:
    gBS->RestoreTPL(OldTpl);
    return EFI_INVALID_PARAMETER;
  }
  gBS->RestoreTPL(OldTpl);
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
J250fGopEntry(IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  UINT64 Address = FixedPcdGet64(PcdMipiFrameBufferAddress);
  UINT32 Bpp = FixedPcdGet32(PcdMipiFrameBufferPixelBpp);
  EFI_PHYSICAL_ADDRESS NativeFb = 0;
  UINTN Bytes;
  EFI_STATUS Status;

  (VOID)ImageHandle;
  (VOID)SystemTable;

  mWidth = FixedPcdGet32(PcdMipiFrameBufferWidth);
  mHeight = FixedPcdGet32(PcdMipiFrameBufferHeight);
  mStride = FixedPcdGet32(PcdMipiFrameBufferPixelsPerScanLine);
  if (mStride == 0)
    mStride = mWidth;

  if (mWidth == 0 || mHeight == 0 || mWidth > 4096 ||
      mHeight > 4096 || mStride < mWidth || mStride > 4096 ||
      (Bpp != 24 && Bpp != 32))
    return EFI_UNSUPPORTED;

  Bytes = Bpp / 8;
  mFramebufferBytes = mStride * mHeight * Bytes;
  if (Address < J250F_FB_REGION_BASE ||
      Address - J250F_FB_REGION_BASE > J250F_FB_REGION_SIZE ||
      mFramebufferBytes > J250F_FB_REGION_SIZE ||
      Address - J250F_FB_REGION_BASE >
        J250F_FB_REGION_SIZE - mFramebufferBytes)
    return EFI_DEVICE_ERROR;

  /* Native 32-bit scanout must have a word-aligned physical buffer. */
  if (Bpp == 32 && ((Address & 3u) != 0))
    return EFI_UNSUPPORTED;

  /* Samsung starts in known-good packed RGB888; attempt an isolated
     native BGRX scanout only after verifying the inherited MDP registers.
     Failed probes leave the original GOP untouched and working. */
  if (Bpp == 24) {
    EFI_STATUS NativeStatus = J250fTryNativeBgrx(
      Address, (UINT32)mWidth, (UINT32)mHeight, (UINT32)mStride, &NativeFb);
    if (!EFI_ERROR(NativeStatus)) {
      Address = NativeFb;
      Bpp = 32;
      Bytes = 4;
      mFramebufferBytes = mStride * mHeight * Bytes;
      mNativeBgrx = TRUE;
    } else {
      DEBUG((DEBUG_INFO,
        "J250F GOP: retaining working RGB888 BLT scanout (%r)\n",
        NativeStatus));
    }
  }

  mPixels = (UINT8 *)(UINTN)Address;
  mBytesPerPixel = Bytes;
  ZeroMem(&mInfo, sizeof(mInfo));
  mInfo.Version = 0;
  mInfo.HorizontalResolution = (UINT32)mWidth;
  mInfo.VerticalResolution = (UINT32)mHeight;
  mInfo.PixelsPerScanLine = (UINT32)mStride;

  if (Bpp == 24) {
    mInfo.PixelFormat = PixelBltOnly;
    mMode.FrameBufferBase = 0;
    mMode.FrameBufferSize = 0;
  } else {
    mInfo.PixelFormat = mNativeBgrx
      ? PixelBlueGreenRedReserved8BitPerColor
      : PixelRedGreenBlueReserved8BitPerColor;
    mMode.FrameBufferBase = Address;
    mMode.FrameBufferSize = mFramebufferBytes;
  }

  mMode.MaxMode = 1;
  mMode.Mode = 0;
  mMode.Info = &mInfo;
  mMode.SizeOfInfo = sizeof(mInfo);

  Status = gBS->InstallMultipleProtocolInterfaces(
    &mHandle,
    &gEfiDevicePathProtocolGuid, &mDevicePath,
    &gEfiGraphicsOutputProtocolGuid, &mGop,
    NULL);
  DEBUG((DEBUG_INFO,
    "J250F GOP: %ux%u stride=%u BPP=%u fb=0x%Lx fmt=%u status=%r\n",
    (UINT32)mWidth, (UINT32)mHeight, (UINT32)mStride, Bpp,
    Address, (UINT32)mInfo.PixelFormat, Status));
  return Status;
}
