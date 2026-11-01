# Linux source reassessment and UEFI-driver mapping

The original ZIP provides Linux 3.18.120 `drivers/mmc/host/sdhci-msm.c`
and the J250F rev. 04 DT. That Linux module is a **host-controller** driver:
Linux's MMC core and block layer provide card enumeration, partition block
access, and OS-facing storage. None of those Linux kernel APIs can link into
Project Mu DXE, and `SdMmcPciHcDxe` targets PCI controllers.

## Board observations directly from provided DT

- Qualcomm SDCC1/MMC eMMC `sdhci@7824900`, nonremovable, 8-bit.
- HC `0x07824900`, length `0x500` ; CORE `0x07824000`, length `0x800`;
  CMDQ `0x07824E00`, length `0x200` (unused in read-only firmware driver).
- Interrupts: GIC SPI 0x7b (123) and 0x8a (138); this port deliberately polls.
- Rails: VDD=2,900,000uV; IO=1,800,000uV; DT both declared always-on.
  PMIC/RPM transport cannot be learned from DTS alone.
- Source clock, mux and TLMM drive strengths translated in `Core/j250f_board.c`.
- HS200/HS400 and CQE require far more functionality than just the register
  settings and are intentionally omitted for conservative 20MHz SDR reads.

## Newly checked protocol defects in previous port

1. Previous R1 status mask did not check bit 25 (`CARD_IS_LOCKED`), so a
   locked-card CMD6 could be reported successful. Fixed by extending R1 mask.
2. Previous command function treated all DAT_TIMEOUT errors on R1B commands
   as fatal, while Samsung source uses
   `SDHCI_QUIRK2_IGNORE_DATATOUT_FOR_R1BCMD`. New transport ignores only
   that specific error on no-data busy-response commands; CRC errors remain
   fatal. Both cases are covered by `Tests/test_regressions.c`.
3. Firmware needs a UEFI protocol so the firmware disk stack can **discover**
   the device. This is now provided by `J250fEmmcDxe.c`, which installs the
   EFI_BLOCK_IO_PROTOCOL and a vendor device path only when card init passes.

## Source test summary

| Area | Evidence | What is NOT proved |
|---|---|---|
| Linux board and host register address mapping | Cross-read board DTS | Actual board revision and pre-boot memory map |
| CMD0/1/2/3/7/8/6/17 | Deterministic fake-SDHCI tests | Real eMMC timings, signal integrity |
| GCC, TLMM | Memory-map simulator | Real silicon PLL/clock/pin programming |
| RPM power handling | Missing callback *fails closed* | PM8917 voltage/power success |
| DXE EFI BlockIo publication | Fake UEFI Services + MMIO | Firmware dispatch and true firmware build |
| ARMv7 | Freestanding transport `.o` | PE/COFF DXE executable |

## Integration requirements not solved here

- Platform-specific RPM/PM8917 driver with real regulator votes and GPLL0.
- ARM DXE/early boot Device MMIO mappings, clock ownership and order.
- Build tested in your exact Project Mu source tree, not fake headers.
- Physical J250F logs: PWRCTL, CMD1 OCR, CID, EXT_CSD SEC_COUNT and CMD17.
- Existing eMMC partition table with bootable FAT volume and compatible boot
  environment if you want UEFI applications to read files from it.
- Operating system driver/firmware handoff (DT or ACPI) **after**
  `ExitBootServices` — firmware BlockIo does not make Windows/Linux see eMMC.

## Why this is not yet a device-ready release

It has not been built with genuine EDK II headers/libraries in a user's
actual Mu platform, and no complete executable `.efi`/FD was generated. Its
mock tests cannot validate eMMC timing, pinmux, PMIC votes, memory-region
mapping, or bootloader state on silicon. The supplied source alone is
insufficient to implement the platform-specific RPM protocol accurately.

`make test` can show BlockIo publishing successfully in a simulated DXE
runtime, but is not a substitute for the real board logs. Once the board
provides the power service, start with non-destructive `map -r` and read-only
partition enumeration from the UEFI Shell. Compare selected sectors against
an independently verified offline eMMC backup. Never enable writes until
proven physical recovery and a complete firmware integration are available.
