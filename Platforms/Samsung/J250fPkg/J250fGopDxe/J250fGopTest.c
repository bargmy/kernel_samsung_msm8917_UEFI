/** @file
  Opt-in EFI application to verify the real J250F GOP before Windows tests.
  Saves the bottom 16 scanlines, tests BLT color roundtrips, and restores them.
  Never accesses storage, chipset registers, or the eMMC controller.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/GraphicsOutput.h>

STATIC EFI_STATUS
CheckPixel(CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL *P, UINTN Band)
{
  if (Band == 0 && P->Red == 255 && P->Green == 0 && P->Blue == 0)
    return EFI_SUCCESS;
  if (Band == 1 && P->Red == 0 && P->Green == 255 && P->Blue == 0)
    return EFI_SUCCESS;
  if (Band == 2 && P->Red == 0 && P->Green == 0 && P->Blue == 255)
    return EFI_SUCCESS;
  return EFI_DEVICE_ERROR;
}

EFI_STATUS EFIAPI
UefiMain(IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Colors[3];
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Saved = NULL;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Observed = NULL;
  UINTN Width = 192;
  UINTN Height = 16;
  UINTN Bottom;
  UINTN Bytes;
  UINTN Band;
  UINTN Pixel;
  EFI_STATUS Status, RestoreStatus;

  (VOID)ImageHandle;
  (VOID)SystemTable;

  Status = gBS->LocateProtocol(
    &gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  if (EFI_ERROR(Status)) {
    Print(L"GOP: not found (%r)\n", Status);
    return Status;
  }
  if (Gop->Mode == NULL || Gop->Mode->Info == NULL ||
      Gop->Mode->Info->HorizontalResolution < Width ||
      Gop->Mode->Info->VerticalResolution < Height) {
    Print(L"GOP: invalid resolution\n");
    return EFI_UNSUPPORTED;
  }

  Print(L"GOP mode: %ux%u stride %u format %u framebuffer 0x%lx bytes %lu\n",
    Gop->Mode->Info->HorizontalResolution,
    Gop->Mode->Info->VerticalResolution,
    Gop->Mode->Info->PixelsPerScanLine,
    Gop->Mode->Info->PixelFormat,
    Gop->Mode->FrameBufferBase, Gop->Mode->FrameBufferSize);
  if (Gop->Mode->Info->PixelFormat == PixelBltOnly)
    Print(L"24-bit inherited GOP: usable for EFI BLT, not Windows-native BGRX\n");

  Bottom = Gop->Mode->Info->VerticalResolution - Height;
  Bytes = Width * Height * sizeof(*Saved);
  Saved = AllocateZeroPool(Bytes);
  Observed = AllocateZeroPool(Bytes);
  if (Saved == NULL || Observed == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Exit;
  }

  Status = Gop->Blt(Gop, Saved, EfiBltVideoToBltBuffer,
                    0, Bottom, 0, 0, Width, Height, 0);
  if (EFI_ERROR(Status))
    goto Exit;

  ZeroMem(Colors, sizeof(Colors));
  Colors[0].Red = 255;
  Colors[1].Green = 255;
  Colors[2].Blue = 255;

  for (Band = 0; Band < 3; ++Band) {
    Status = Gop->Blt(Gop, &Colors[Band], EfiBltVideoFill,
                      0, 0, Band * 64, Bottom, 64, Height, 0);
    if (EFI_ERROR(Status))
      break;
  }
  if (!EFI_ERROR(Status)) {
    Status = Gop->Blt(Gop, Observed, EfiBltVideoToBltBuffer,
                      0, Bottom, 0, 0, Width, Height, 0);
  }
  if (!EFI_ERROR(Status)) {
    for (Pixel = 0; Pixel < Width * Height; ++Pixel) {
      if (EFI_ERROR(CheckPixel(&Observed[Pixel], (Pixel % Width) / 64))) {
        Status = EFI_DEVICE_ERROR;
        break;
      }
    }
  }

  /* Restore the display even if one of the tests fails. */
  RestoreStatus = Gop->Blt(Gop, Saved, EfiBltBufferToVideo,
                           0, 0, 0, Bottom, Width, Height, 0);
  if (EFI_ERROR(RestoreStatus))
    Status = RestoreStatus;

Exit:
  if (Saved != NULL)
    FreePool(Saved);
  if (Observed != NULL)
    FreePool(Observed);
  Print(L"J250F GOP BLT roundtrip: %r\n", Status);
  /* For a Windows boot attempt, a healthy BLT driver is not enough:
     Windows needs a persistent physical BGRX8888 framebuffer. */
  if (!EFI_ERROR(Status) &&
      (Gop->Mode->Info->PixelFormat !=
          PixelBlueGreenRedReserved8BitPerColor ||
       Gop->Mode->FrameBufferBase == 0 ||
       Gop->Mode->FrameBufferSize < 540u * 960u * 4u)) {
    Print(L"J250F Windows GOP check: unsupported (not native 32-bit BGRX)\n");
    return EFI_UNSUPPORTED;
  }
  if (!EFI_ERROR(Status))
    Print(L"J250F Windows GOP layout check: PASS (MDP scanout still needs phone verification)\n");
  return Status;
}
