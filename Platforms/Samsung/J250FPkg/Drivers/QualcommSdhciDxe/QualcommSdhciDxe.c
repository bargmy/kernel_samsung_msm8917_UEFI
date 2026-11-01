// SPDX-License-Identifier: BSD-2-Clause-Patent
// J250F SDCC1 register probe only. No host writes or card commands.
#include <Uefi.h>
#include <Library/IoLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>

#define SDCC1_HOST  0x07824900U
#define SDCC1_CORE  0x07824000U
#define SDHCI_CAPABILITIES  0x40U
#define SDHCI_CAPABILITIES_1  0x44U
#define SDHCI_HOST_VERSION  0xFEU

EFI_STATUS
EFIAPI
QualcommSdhciDiagnosticEntry (
  IN EFI_HANDLE ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  CHAR8 Buffer[512];
  UINTN Length;
  UINT16 Version;
  UINT32 Caps;
  UINT32 Caps1;

  // The platform already maps this range NS_DEVICE. Inherit aboot's
  // clocks/power; do not reset, enable, or otherwise change the controller.
  // An inaccessible/gated peripheral may still cause a synchronous abort.
  Length = AsciiSPrint (Buffer, sizeof (Buffer),
    "\r\nJ250F eMMC DIAGNOSTIC (register probe only)\r\n"
    "Host MMIO: 0x%08x Core MMIO: 0x%08x\r\n"
    "Probe starting; clocks/power inherited from aboot.\r\n",
    SDCC1_HOST, SDCC1_CORE);
  SerialPortWrite ((UINT8 *)Buffer, Length);

  Version = MmioRead16 (SDCC1_HOST + SDHCI_HOST_VERSION);
  Caps = MmioRead32 (SDCC1_HOST + SDHCI_CAPABILITIES);
  Caps1 = MmioRead32 (SDCC1_HOST + SDHCI_CAPABILITIES_1);
  Length = AsciiSPrint (Buffer, sizeof (Buffer),
    "SDHCI version (raw): 0x%04x\r\n"
    "Capabilities: 0x%08x 0x%08x\r\n"
    "NO EMMC COMMANDS OR CONTROLLER WRITES ISSUED\r\n"
    "Register reads alone do not confirm eMMC access.\r\n",
    (UINT32)Version, Caps, Caps1);
  SerialPortWrite ((UINT8 *)Buffer, Length);
  return EFI_SUCCESS;
}
