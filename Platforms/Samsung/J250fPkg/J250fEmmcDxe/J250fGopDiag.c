/** @file
  J250F firmware ADB graphical diagnostics using the real EDK II GOP types.
  Draws/reads/restores 192x16 pixels; does not touch eMMC, GPT or MDSS MMIO.
  SPDX-License-Identifier: GPL-2.0-only
**/
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/GraphicsOutput.h>
#include <J250fGopSupportLib.h>

/* Saved in the live eMMC/ADB module, not in a temporary EFI application.
   These values allow us to distinguish installation failure from old firmware. */
STATIC EFI_STATUS mInitialGop = EFI_NOT_READY;
STATIC EFI_STATUS mPublishGop = EFI_NOT_READY;
STATIC EFI_STATUS mFinalGop = EFI_NOT_READY;
STATIC BOOLEAN mGopAttempted;
STATIC BOOLEAN mGopRecorded;

VOID EFIAPI
J250fGopDiagRecord(EFI_STATUS PreStatus, EFI_STATUS PublishStatus,
                   EFI_STATUS PostStatus, BOOLEAN Attempted)
{
  mInitialGop = PreStatus;
  mPublishGop = PublishStatus;
  mFinalGop = PostStatus;
  mGopAttempted = Attempted;
  mGopRecorded = TRUE;
}

/* Never changes the Qualcomm display controller. The phone's inherited
   physical RGB888 buffer remains the backing store if GOP publishes. */
UINTN EFIAPI
J250fGopRepairCommand(OUT CHAR8 *Out, IN UINTN Cap)
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop = NULL;
  EFI_STATUS Before, Publish, After;
  if (Out == NULL || Cap == 0)
    return 0;
  Before = gBS->LocateProtocol(
    &gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  Publish = J250fGopPublish(NULL, NULL, FALSE);
  Gop = NULL;
  After = gBS->LocateProtocol(
    &gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  if (!EFI_ERROR(After) && Gop == NULL)
    After = EFI_NOT_FOUND;
  J250fGopDiagRecord(Before, Publish, After, TRUE);
  return AsciiSPrint(Out, Cap,
    "GOPDIAG/4 24-bit-only repair: before=%r publish=%r after=%r GOP=%p\r\n"
    "No MDSS register changes or eMMC access. Now run gop status.\r\n",
    Before, Publish, After, Gop);
}


#define GOP_TEST_WIDTH  192u
#define GOP_TEST_HEIGHT 16u

STATIC BOOLEAN
PixelMatches(IN CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Pixel, IN UINTN Band)
{
  return Pixel->Red   == (Band == 0 ? 255 : 0) &&
         Pixel->Green == (Band == 1 ? 255 : 0) &&
         Pixel->Blue  == (Band == 2 ? 255 : 0);
}

/**
  The ADB command dispatcher calls this directly, avoiding the hand-written
  EFI_SYSTEM_TABLE / EFI_BOOT_SERVICES layout in external test binaries.
**/
UINTN EFIAPI
J250fGopDebugCommand(IN BOOLEAN RunTest, OUT CHAR8 *Out, IN UINTN Cap)
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop = NULL;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Saved = NULL;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Readback;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Colors[3];
  EFI_HANDLE *Handles = NULL;
  EFI_STATUS Status, HandlesStatus, RestoreStatus;
  UINTN HandleCount = 0;
  UINTN Width = GOP_TEST_WIDTH;
  UINTN Height = GOP_TEST_HEIGHT;
  UINTN Size = GOP_TEST_WIDTH * GOP_TEST_HEIGHT *
               sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL);
  UINTN Bottom, Band, X, Y;
  UINTN Used;
  BOOLEAN SavedValid = FALSE;
  BOOLEAN VisuallyChecked = FALSE;

  if (Out == NULL || Cap == 0)
    return 0;

  /* GOP is a real EFI protocol, not a guessed framebuffer address. */
  Status = gBS->LocateProtocol(
    &gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  HandlesStatus = gBS->LocateHandleBuffer(
    ByProtocol, &gEfiGraphicsOutputProtocolGuid, NULL,
    &HandleCount, &Handles);
  if (!EFI_ERROR(HandlesStatus) && Handles != NULL)
    gBS->FreePool(Handles);

  Used = AsciiSPrint(Out, Cap,
    "GOPDIAG/4 startup-recorded=%u fallback-attempted=%u"
    " before=%r publish=%r after=%r\r\n"
    "GOP LocateProtocol=%r; LocateHandleBuffer=%r; handles=%u\r\n",
    (UINT32)mGopRecorded, (UINT32)mGopAttempted,
    mInitialGop, mPublishGop, mFinalGop,
    Status, HandlesStatus, (UINT32)HandleCount);
  if (EFI_ERROR(Status) || Gop == NULL) {
    Used += AsciiSPrint(Out + Used, Cap - Used,
      "No GOP exposed through UEFI. Compare firmware build/boot log.\r\n");
    return Used;
  }

  if (Gop->Mode == NULL || Gop->Mode->Info == NULL ||
      Gop->Blt == NULL) {
    Used += AsciiSPrint(Out + Used, Cap - Used,
      "GOP protocol exists but Mode/Info/Blt is invalid.\r\n");
    return Used;
  }

  Info = Gop->Mode->Info;
  Used += AsciiSPrint(Out + Used, Cap - Used,
    "Mode=%u/%u resolution=%ux%u stride=%u format=%u\r\n"
    "FrameBufferBase=0x%Lx FrameBufferSize=%Lu\r\n",
    Gop->Mode->Mode, Gop->Mode->MaxMode,
    Info->HorizontalResolution, Info->VerticalResolution,
    Info->PixelsPerScanLine, (UINT32)Info->PixelFormat,
    (UINT64)Gop->Mode->FrameBufferBase,
    (UINT64)Gop->Mode->FrameBufferSize);

  if (Info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor &&
      Gop->Mode->FrameBufferBase != 0 &&
      Info->PixelsPerScanLine >= Info->HorizontalResolution &&
      Gop->Mode->FrameBufferSize >=
        (UINT64)Info->PixelsPerScanLine *
        Info->VerticalResolution * 4u) {
    Used += AsciiSPrint(Out + Used, Cap - Used,
      "Windows BGRX GOP memory layout: YES (panel scanout unverified)\r\n");
  } else {
    Used += AsciiSPrint(Out + Used, Cap - Used,
      "Windows BGRX GOP memory layout: NO (24-bit BLT fallback?)\r\n");
  }

  if (!RunTest)
    return Used;

  if (Info->HorizontalResolution < Width ||
      Info->VerticalResolution < Height ||
      Info->HorizontalResolution > 8192u ||
      Info->VerticalResolution > 8192u ||
      Gop->Mode->Mode >= Gop->Mode->MaxMode) {
    Used += AsciiSPrint(Out + Used, Cap - Used,
      "GOP test: SKIPPED (unexpected resolution/mode)\r\n");
    return Used;
  }

  /* Allocate both buffers in a single pool. Never touch physical storage. */
  Status = gBS->AllocatePool(
    EfiBootServicesData, 2u * Size, (VOID **)&Saved);
  if (EFI_ERROR(Status)) {
    Used += AsciiSPrint(Out + Used, Cap - Used,
      "GOP test: AllocatePool failed: %r\r\n", Status);
    return Used;
  }
  Readback = Saved + GOP_TEST_WIDTH * GOP_TEST_HEIGHT;
  Bottom = Info->VerticalResolution - Height;

  Status = Gop->Blt(Gop, Saved, EfiBltVideoToBltBuffer,
                    0, Bottom, 0, 0, Width, Height, Width * sizeof(*Saved));
  if (EFI_ERROR(Status))
    goto Finish;
  SavedValid = TRUE;

  ZeroMem(Colors, sizeof(Colors));
  Colors[0].Red = 255;
  Colors[1].Green = 255;
  Colors[2].Blue = 255;
  for (Band = 0; Band < 3; ++Band) {
    Status = Gop->Blt(Gop, &Colors[Band], EfiBltVideoFill,
                      0, 0, Band * 64u, Bottom, 64u, Height, 0);
    if (EFI_ERROR(Status))
      goto Finish;
  }

  Status = Gop->Blt(Gop, Readback, EfiBltVideoToBltBuffer,
                    0, Bottom, 0, 0, Width, Height, Width * sizeof(*Readback));
  if (EFI_ERROR(Status))
    goto Finish;

  for (Y = 0; Y < Height; ++Y) {
    for (X = 0; X < Width; ++X) {
      if (!PixelMatches(&Readback[Y * Width + X], X / 64u)) {
        Status = EFI_DEVICE_ERROR;
        goto Finish;
      }
    }
  }
  VisuallyChecked = TRUE;

  /* Let bars remain briefly visible, then restore the saved screen area. */
  gBS->Stall(750000);

Finish:
  if (SavedValid) {
    RestoreStatus = Gop->Blt(Gop, Saved, EfiBltBufferToVideo,
                             0, 0, 0, Bottom, Width, Height,
                             Width * sizeof(*Saved));
    if (EFI_ERROR(RestoreStatus))
      Status = RestoreStatus;
  }
  gBS->FreePool(Saved);
  Used += AsciiSPrint(Out + Used, Cap - Used,
    "GOP 192x16 save/draw/read/restore: %r; RGB readback=%a\r\n"
    "A passing readback cannot prove panel scanout; visually check colors.\r\n",
    Status, VisuallyChecked ? "PASS" : "NOT CONFIRMED");
  return Used;
}
