# J250F SDCC1 safe probe v2

This is a **controller-state diagnostic only**, not an eMMC driver.

## Sources
* Samsung J250F rev04 DTS: `arch/arm/boot/dts/samsung/msm8917/msm8917-sec-j2y18lte-mea-open-r04.dts` in bargmy/android_kernel_samsung_msm8917_forOneUI (lineage-19.1).
* Qualcomm SDHCI register definitions: `include/sdhci.h` in youheng7185/u-boot.
* Qualcomm SDCC core version: `drivers/mmc/msm_sdhci.c` in youheng7185/u-boot.
* Linux driver reference: `drivers/mmc/host/sdhci-msm.c` in Samsung kernel.

## What it reads
* SDHCI HOST_VERSION, CAPABILITIES, CAPABILITIES_1.
* SDCC core MCI_VERSION.
* SDHCI PRESENT_STATE, CLOCK_CONTROL and POWER_CONTROL.

No calls to MmioWrite, no SD/MMC command, no DMA, no eMMC block access, no BlockIo/PassThru protocols, no controller initialization, no GPT changes. Every register access is a READ to the host or core MMIO windows only.

The CPU can still fault on an inaccessible/gated register and the phone can still hang. This is **not** a fully safe eMMC driver and does not imply Windows storage protection.

## First test
Use this branch to build a fresh ARM32 DEBUG boot image. Verify Android boot packaging and checksum and preserve the known-good boot backup. Only replace the existing boot partition with explicit user approval and established recovery.

Photograph the values printed under `J250F SDCC1 CONTROLLER PROBE V2`. If the screen freezes, report the last visible line. Do not run identification commands, read sectors, or initialize the controller yet.

## Next gate
Review the on-device values and compare SDCC core-version family with the Qualcomm U-Boot v4/v5 register map. For safety, prefer a **separate SDCC2 microSD driver with read-only command allowlist** for EFI-file loading; any SDCC1 card initialization or BlockIo exposure requires a new review. The current test changes no other platform source and is not a validated boot image.
