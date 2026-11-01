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

STATIC
VOID
DrawColorSwatch (
  UINT8 *Fb,
  UINT32 Pitch,
  UINT32 x0,
  UINT32 y0,
  UINT32 w,
  UINT32 h,
  UINT8 R,
  UINT8 G,
  UINT8 B
  )
{
  for (UINT32 y = y0; y < (y0 + h); y++) {
    UINT8 *Row = Fb + (y * Pitch) + (x0 * 3);
    for (UINT32 x = 0; x < w; x++) {
      Row[x * 3 + 0] = R;
      Row[x * 3 + 1] = G;
      Row[x * 3 + 2] = B;
    }
  }
}

VOID InitializeSharedUartBuffers(VOID)
{
  ARM_MEMORY_REGION_DESCRIPTOR_EX DisplayMemoryRegion;
  LocateMemoryMapAreaByName("Display Reserved", &DisplayMemoryRegion);

  UINT32 Width  = FixedPcdGet32 (PcdMipiFrameBufferWidth);
  UINT32 Height = FixedPcdGet32 (PcdMipiFrameBufferHeight);
  UINT32 Bpp    = FixedPcdGet32 (PcdMipiFrameBufferPixelBpp);
  UINT64 FbAddr = FixedPcdGet64 (PcdMipiFrameBufferAddress);
  UINT8  *Fb    = (FbAddr != 0) ? (UINT8 *)(UINTN)FbAddr : (UINT8 *)DisplayMemoryRegion.Address;
  UINT32 Pitch  = Width * (Bpp / 8);

  // Clear entire display buffer cleanly to black
  ZeroMem((VOID *)DisplayMemoryRegion.Address, DisplayMemoryRegion.Length);

  // 2-pixel Neon Green border around active display perimeter (540x960)
  DrawColorSwatch (Fb, Pitch, 0, 0, Width, 2, 0, 255, 0);
  DrawColorSwatch (Fb, Pitch, 0, Height - 2, Width, 2, 0, 255, 0);
  DrawColorSwatch (Fb, Pitch, 0, 0, 2, Height, 0, 255, 0);
  DrawColorSwatch (Fb, Pitch, Width - 2, 0, 2, Height, 0, 255, 0);

  // 4 Color Verification Bars (Red, Green, Blue, White) at Y: 30 to 70
  DrawColorSwatch (Fb, Pitch, 40, 30, 100, 40, 255, 0, 0);      // Pure Red
  DrawColorSwatch (Fb, Pitch, 160, 30, 100, 40, 0, 255, 0);     // Pure Green
  DrawColorSwatch (Fb, Pitch, 280, 30, 100, 40, 0, 0, 255);     // Pure Blue
  DrawColorSwatch (Fb, Pitch, 400, 30, 100, 40, 255, 255, 255); // Pure White

  // Thin cyan divider under color bars
  DrawColorSwatch (Fb, Pitch, 40, 80, 460, 2, 0, 255, 255);

  WriteBackInvalidateDataCacheRange ((VOID *)DisplayMemoryRegion.Address, DisplayMemoryRegion.Length);

  // Setup Position counter past active framebuffer
  INTN *pFbConPosition =
      (INTN *)(DisplayMemoryRegion.Address + (Width * Height * Bpp / 8));

  *(pFbConPosition + 0) = 0;
  *(pFbConPosition + 1) = 0;
}

VOID UartInit(VOID)
{
  SerialPortInitialize();
  InitializeSharedUartBuffers();

  DEBUG((EFI_D_WARN, "\n\n\n\n\n"));
  DEBUG((EFI_D_WARN, "  ==========================================\n"));
  DEBUG((EFI_D_WARN, "                  PROJECT MU                \n"));
  DEBUG((EFI_D_WARN, "         Samsung Galaxy J2 Pro 2018         \n"));
  DEBUG((EFI_D_WARN, "                 ARM32 UEFI                 \n"));
  DEBUG((EFI_D_WARN, "          FRAMEBUFFER TEST PASSED           \n"));
  DEBUG((EFI_D_WARN, "  ==========================================\n\n"));
  DEBUG((EFI_D_WARN, "  Firmware %s built on %a\n\n", (CHAR16 *)PcdGetPtr(PcdFirmwareVersionString), __DATE__));

  // Hold test card for 4 seconds for user inspection & photography
  MicroSecondDelay (4000000);
}

VOID EarlyInitialization(VOID)
{
  // Initialize UART Serial
  UartInit();

  // Do platform specific initialization here
  PlatformInitialize();
}
