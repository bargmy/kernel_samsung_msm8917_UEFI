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

VOID InitializeSharedUartBuffers(VOID)
{
  ARM_MEMORY_REGION_DESCRIPTOR_EX DisplayMemoryRegion;
  LocateMemoryMapAreaByName("Display Reserved", &DisplayMemoryRegion);

  UINT32 Width  = FixedPcdGet32 (PcdMipiFrameBufferWidth);
  UINT32 Height = FixedPcdGet32 (PcdMipiFrameBufferHeight);
  UINT32 Bpp    = FixedPcdGet32 (PcdMipiFrameBufferPixelBpp);

  // Clear entire display buffer cleanly
  ZeroMem((VOID *)DisplayMemoryRegion.Address, DisplayMemoryRegion.Length);
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
