#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/SerialPortLib.h>
#include <Library/TimerLib.h>
#include <Library/CacheMaintenanceLib.h>

#include <Library/MemoryMapHelperLib.h>
#include <Library/PlatformPrePiLib.h>

#include "InitializationUtils.h"

// Color Definitions (ARGB8888 in memory: little endian [BB, GG, RR, AA])
#define DIAG_WHITE    0xFFFFFFFF
#define DIAG_YELLOW   0xFFFFFF00
#define DIAG_CYAN     0xFF00FFFF
#define DIAG_GREEN    0xFF00FF00
#define DIAG_MAGENTA  0xFFFF00FF
#define DIAG_RED      0xFFFF0000
#define DIAG_BLUE     0xFF0000FF
#define DIAG_BLACK    0xFF000000
#define DIAG_DARK     0xFF181818

STATIC
VOID
FillSolidScreen (
  UINT32 *Fb,
  UINT32 Width,
  UINT32 Height,
  UINT32 Stride,
  UINT32 Color
  )
{
  for (UINT32 y = 0; y < Height; y++) {
    UINT32 *Row = Fb + (y * Stride);
    for (UINT32 x = 0; x < Width; x++) {
      Row[x] = Color;
    }
  }
  WriteBackInvalidateDataCacheRange ((VOID *)Fb, Stride * Height * sizeof (UINT32));
}

STATIC
VOID
DrawRect (
  UINT32 *Fb,
  UINT32 Stride,
  UINT32 x0,
  UINT32 y0,
  UINT32 w,
  UINT32 h,
  UINT32 Color
  )
{
  for (UINT32 y = y0; y < (y0 + h); y++) {
    UINT32 *Row = Fb + (y * Stride);
    for (UINT32 x = x0; x < (x0 + w); x++) {
      Row[x] = Color;
    }
  }
}

STATIC
VOID
DrawLetter (
  UINT32 *Fb,
  UINT32 Stride,
  CHAR8  Char,
  UINT32 x,
  UINT32 y,
  UINT32 Color
  )
{
  switch (Char) {
  case 'J':
    DrawRect (Fb, Stride, x, y, 60, 12, Color);
    DrawRect (Fb, Stride, x + 48, y, 12, 100, Color);
    DrawRect (Fb, Stride, x, y + 88, 60, 12, Color);
    DrawRect (Fb, Stride, x, y + 56, 12, 44, Color);
    break;

  case '2':
    DrawRect (Fb, Stride, x, y, 60, 12, Color);
    DrawRect (Fb, Stride, x + 48, y, 12, 50, Color);
    DrawRect (Fb, Stride, x, y + 44, 60, 12, Color);
    DrawRect (Fb, Stride, x, y + 50, 12, 50, Color);
    DrawRect (Fb, Stride, x, y + 88, 60, 12, Color);
    break;

  case '5':
    DrawRect (Fb, Stride, x, y, 60, 12, Color);
    DrawRect (Fb, Stride, x, y, 12, 50, Color);
    DrawRect (Fb, Stride, x, y + 44, 60, 12, Color);
    DrawRect (Fb, Stride, x + 48, y + 50, 12, 50, Color);
    DrawRect (Fb, Stride, x, y + 88, 60, 12, Color);
    break;

  case '0':
    DrawRect (Fb, Stride, x, y, 60, 12, Color);
    DrawRect (Fb, Stride, x, y + 88, 60, 12, Color);
    DrawRect (Fb, Stride, x, y, 12, 100, Color);
    DrawRect (Fb, Stride, x + 48, y, 12, 100, Color);
    break;

  case 'F':
    DrawRect (Fb, Stride, x, y, 12, 100, Color);
    DrawRect (Fb, Stride, x, y, 60, 12, Color);
    DrawRect (Fb, Stride, x, y + 44, 45, 12, Color);
    break;

  default:
    break;
  }
}

STATIC
VOID
RunEarlyFrameBufferDiagnostic (
  VOID
  )
{
  ARM_MEMORY_REGION_DESCRIPTOR_EX DisplayMemoryRegion;
  LocateMemoryMapAreaByName ("Display Reserved", &DisplayMemoryRegion);

  UINT32 *FbBase = (UINT32 *)DisplayMemoryRegion.Address;
  UINT32  Width  = FixedPcdGet32 (PcdMipiFrameBufferWidth);
  UINT32  Height = FixedPcdGet32 (PcdMipiFrameBufferHeight);
  UINT32  Stride = FixedPcdGet32 (PcdMipiFrameBufferPixelsPerScanLine);
  if (Stride == 0) {
    Stride = Width;
  }

  // -------------------------------------------------------------
  // Test 1: Full-Screen Solid Color Fills (Red, Green, Blue, White, Black)
  // -------------------------------------------------------------
  FillSolidScreen (FbBase, Width, Height, Stride, DIAG_RED);
  MicroSecondDelay (400000);

  FillSolidScreen (FbBase, Width, Height, Stride, DIAG_GREEN);
  MicroSecondDelay (400000);

  FillSolidScreen (FbBase, Width, Height, Stride, DIAG_BLUE);
  MicroSecondDelay (400000);

  FillSolidScreen (FbBase, Width, Height, Stride, DIAG_WHITE);
  MicroSecondDelay (400000);

  FillSolidScreen (FbBase, Width, Height, Stride, DIAG_BLACK);
  MicroSecondDelay (400000);

  // -------------------------------------------------------------
  // Test 2: 8 Vertical Color Bars (Y: 0 to 199)
  // White, Yellow, Cyan, Green, Magenta, Red, Blue, Black
  // -------------------------------------------------------------
  STATIC CONST UINT32 ColorBars[8] = {
    DIAG_WHITE, DIAG_YELLOW, DIAG_CYAN, DIAG_GREEN,
    DIAG_MAGENTA, DIAG_RED, DIAG_BLUE, DIAG_BLACK
  };

  for (UINT32 bar = 0; bar < 8; bar++) {
    UINT32 x0 = (bar * Width) / 8;
    UINT32 x1 = ((bar + 1) * Width) / 8;
    DrawRect (FbBase, Stride, x0, 0, (x1 - x0), 198, ColorBars[bar]);
  }
  // 2-pixel white divider underneath color bars
  DrawRect (FbBase, Stride, 0, 198, Width, 2, DIAG_WHITE);

  // -------------------------------------------------------------
  // Test 3: 32x32 Checkerboard Pattern (Y: 200 to 519)
  // Validates row pitch: with stride=544, tiles are clean squares.
  // With incorrect stride, tiles become tilted/sheared parallelograms.
  // -------------------------------------------------------------
  for (UINT32 y = 200; y < 520; y++) {
    UINT32 *Row = FbBase + (y * Stride);
    UINT32 cy = (y - 200) / 32;
    for (UINT32 x = 0; x < Width; x++) {
      UINT32 cx = x / 32;
      Row[x] = ((cx + cy) & 1) ? DIAG_WHITE : DIAG_BLACK;
    }
  }

  // -------------------------------------------------------------
  // Test 4: Giant Centered Text "J250F" (Y: 520 to 739)
  // Dark charcoal banner with huge 60x100 block letters.
  // -------------------------------------------------------------
  DrawRect (FbBase, Stride, 0, 520, Width, 220, DIAG_DARK);

  // Characters J, 2, 5, 0, F at X = 80, 160, 240, 320, 400
  STATIC CONST CHAR8 Word[5] = { 'J', '2', '5', '0', 'F' };
  for (UINT32 i = 0; i < 5; i++) {
    UINT32 x = 80 + i * 80;
    // Red 2px shadow
    DrawLetter (FbBase, Stride, Word[i], x + 2, 560 + 2, DIAG_RED);
    // Yellow main letter
    DrawLetter (FbBase, Stride, Word[i], x, 560, DIAG_YELLOW);
  }

  // Subtitle indicator blocks underneath J250F (Y: 690 to 706)
  DrawRect (FbBase, Stride, 150, 690, 40, 16, DIAG_CYAN);
  DrawRect (FbBase, Stride, 210, 690, 40, 16, DIAG_MAGENTA);
  DrawRect (FbBase, Stride, 270, 690, 40, 16, DIAG_YELLOW);
  DrawRect (FbBase, Stride, 330, 690, 40, 16, DIAG_WHITE);

  // -------------------------------------------------------------
  // Test 5: Dual Stride Calibration Strip (Y: 740 to 959)
  // Left: rendered with Stride=540
  // Right: rendered with Stride=544
  // The side matching hardware will display vertical stripes!
  // -------------------------------------------------------------
  DrawRect (FbBase, Stride, 0, 740, Width, 220, DIAG_BLACK);

  // Center vertical green divider
  DrawRect (FbBase, Stride, 269, 740, 2, 220, DIAG_GREEN);

  // Left zone header marker: Red (Stride 540)
  DrawRect (FbBase, Stride, 20, 742, 230, 8, DIAG_RED);
  // Left zone: 4-pixel vertical stripes drawn with STRIDE = 540
  for (UINT32 y = 752; y < 958; y++) {
    UINT32 *Row540 = FbBase + (y * 540);
    for (UINT32 x = 4; x < 266; x++) {
      Row540[x] = ((x / 4) & 1) ? DIAG_WHITE : DIAG_BLACK;
    }
  }

  // Right zone header marker: Green (Stride 544)
  DrawRect (FbBase, Stride, 290, 742, 230, 8, DIAG_GREEN);
  // Right zone: 4-pixel vertical stripes drawn with STRIDE = 544
  for (UINT32 y = 752; y < 958; y++) {
    UINT32 *Row544 = FbBase + (y * 544);
    for (UINT32 x = 274; x < 536; x++) {
      Row544[x] = ((x / 4) & 1) ? DIAG_WHITE : DIAG_BLACK;
    }
  }

  // 2-pixel Neon Green border around active display perimeter
  DrawRect (FbBase, Stride, 0, 0, Width, 2, DIAG_GREEN);
  DrawRect (FbBase, Stride, 0, Height - 2, Width, 2, DIAG_GREEN);
  DrawRect (FbBase, Stride, 0, 0, 2, Height, DIAG_GREEN);
  DrawRect (FbBase, Stride, Width - 2, 0, 2, Height, DIAG_GREEN);

  // Flush entire screen to PoC memory
  WriteBackInvalidateDataCacheRange ((VOID *)FbBase, Stride * Height * sizeof (UINT32));

  // Hold diagnostic test card for 10 seconds for user inspection & photography
  MicroSecondDelay (10000000);
}

VOID InitializeSharedUartBuffers(VOID)
{
  ARM_MEMORY_REGION_DESCRIPTOR_EX DisplayMemoryRegion;
  LocateMemoryMapAreaByName("Display Reserved", &DisplayMemoryRegion);

  UINT32 Width  = FixedPcdGet32 (PcdMipiFrameBufferWidth);
  UINT32 Height = FixedPcdGet32 (PcdMipiFrameBufferHeight);
  UINT32 Bpp    = FixedPcdGet32 (PcdMipiFrameBufferPixelBpp);
  UINT32 Stride = FixedPcdGet32 (PcdMipiFrameBufferPixelsPerScanLine);
  if (Stride == 0) {
    Stride = Width;
  }

  // Run comprehensive early visual graphics test sequence
  RunEarlyFrameBufferDiagnostic ();

  // Clear Screen row-by-row respecting stride
  UINT32 *Line = (UINT32 *)DisplayMemoryRegion.Address;
  for (UINT32 y = 0; y < Height; y++) {
    for (UINT32 x = 0; x < Width; x++) {
      Line[x] = DIAG_BLACK;
    }
    Line += Stride;
  }
  WriteBackInvalidateDataCacheRange ((VOID *)DisplayMemoryRegion.Address, Stride * Height * sizeof (UINT32));

  // Setup Position counter past active framebuffer
  INTN *pFbConPosition =
      (INTN *)(DisplayMemoryRegion.Address + (Stride * Height * Bpp / 8));

  *(pFbConPosition + 0) = 0;
  *(pFbConPosition + 1) = 0;
}

VOID UartInit(VOID)
{
  SerialPortInitialize();
  InitializeSharedUartBuffers();

  DEBUG((EFI_D_WARN, "\nProject Mu on %a\n", (VOID *)FixedPcdGetPtr(PcdSmbiosSystemModel)));
  DEBUG((EFI_D_WARN, "Firmware version %s built on %a at %a\n\n", (CHAR16 *)PcdGetPtr(PcdFirmwareVersionString), __TIME__, __DATE__));
}

VOID EarlyInitialization(VOID)
{
  // Initialize UART Serial
  UartInit();

  // Do platform specific initialization here
  PlatformInitialize();
}
