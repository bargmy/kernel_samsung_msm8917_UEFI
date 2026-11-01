// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Samsung J250F SDCC1 host-controller discovery (Milestone A+).
// No SD/MMC commands, controller writes, eMMC block reads, or storage protocols.
//
// Hardware addresses: Samsung msm8917-sec-j2y18lte-mea-open-r04.dts
// SDHCI offsets: U-Boot include/sdhci.h
// SDCC_MCI_VERSION offset: Qualcomm U-Boot drivers/mmc/msm_sdhci.c
//
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/IoLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>

#define SDCC1_HOST             0x07824900U
#define SDCC1_CORE             0x07824000U

#define SDHCI_PRESENT_STATE    0x24U
#define SDHCI_POWER_CONTROL    0x29U
#define SDHCI_CLOCK_CONTROL    0x2CU
#define SDHCI_CAPABILITIES     0x40U
#define SDHCI_CAPABILITIES_1   0x44U
#define SDHCI_HOST_VERSION     0xFEU
#define SDCC_MCI_VERSION       0x50U

STATIC
VOID
ProbePrint (
  IN CONST CHAR8 *Message
  )
{
  UINTN Length = AsciiStrLen (Message);
  SerialPortWrite ((CONST UINT8 *)Message, Length);
}

EFI_STATUS
EFIAPI
QualcommSdhciDiagnosticEntry (
  IN EFI_HANDLE ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  CHAR8  Buffer[512];
  UINTN  Length;
  UINT16 Version;
  UINT32 Caps;
  UINT32 Caps1;
  UINT32 CoreVersion;
  UINT32 PresentState;
  UINT16 ClockControl;
  UINT8  PowerControl;

  // Device MMIO is mapped by the existing J250F platform memory map.
  // We do not reset, clock, configure, or send any command to the controller.
  ProbePrint (
    "\r\nJ250F SDCC1 CONTROLLER PROBE V2\r\n"
    "READ-ONLY MMIO; eMMC card will NOT be accessed.\r\n"
    "HOST=0x07824900 CORE=0x07824000\r\n"
    "Reading host state...\r\n"
    );

  // Deliberately use ONLY MmioRead*. Changing these to writes is forbidden
  // without a separate safety review and explicit user approval.
  Version      = MmioRead16 (SDCC1_HOST + SDHCI_HOST_VERSION);
  Caps         = MmioRead32 (SDCC1_HOST + SDHCI_CAPABILITIES);
  Caps1        = MmioRead32 (SDCC1_HOST + SDHCI_CAPABILITIES_1);
  CoreVersion  = MmioRead32 (SDCC1_CORE + SDCC_MCI_VERSION);
  PresentState = MmioRead32 (SDCC1_HOST + SDHCI_PRESENT_STATE);
  ClockControl = MmioRead16 (SDCC1_HOST + SDHCI_CLOCK_CONTROL);
  PowerControl = MmioRead8  (SDCC1_HOST + SDHCI_POWER_CONTROL);

  Length = AsciiSPrint (
             Buffer,
             sizeof (Buffer),
             "HOST_VERSION       0x%04x\r\n"
             "CAPABILITIES       0x%08x\r\n"
             "CAPABILITIES_1     0x%08x\r\n"
             "CORE_MCI_VERSION   0x%08x (major=%u minor=%u)\r\n"
             "PRESENT_STATE      0x%08x\r\n"
             "CLOCK_CONTROL      0x%04x\r\n"
             "POWER_CONTROL      0x%02x\r\n",
             (UINT32)Version,
             Caps,
             Caps1,
             CoreVersion,
             (CoreVersion >> 28) & 0xFU,
             CoreVersion & 0xFFU,
             PresentState,
             (UINT32)ClockControl,
             (UINT32)PowerControl
             );
  SerialPortWrite ((UINT8 *)Buffer, Length);

  if (Version == 0x0000 || Version == 0xFFFF ||
      CoreVersion == 0x00000000 || CoreVersion == 0xFFFFFFFF) {
    ProbePrint ("WARNING: suspicious version; NO card init attempted.\r\n");
  }

  ProbePrint (
    "MMC COMMANDS: NONE | HOST WRITES: NONE | BLOCK IO: NONE\r\n"
    "NOTE: controller state != proof of eMMC access.\r\n"
    "PHOTO MODE: Results held on-screen for 90 seconds.\r\n"
    "No video recording required. Boot resumes afterward.\r\n"
    );

  // Keep this DXE diagnostic screen visible for a single photo instead of
  // letting the normal DXE/BDS console immediately scroll it out of view.
  // Chunk the delay so firmware Stall implementations need only handle 1s.
  // No MMIO, controller or eMMC access occurs while waiting.
  if (SystemTable != NULL && SystemTable->BootServices != NULL) {
    for (UINTN Second = 0; Second < 90; Second++) {
      SystemTable->BootServices->Stall (1000000);
    }
  }

  return EFI_SUCCESS;
}
