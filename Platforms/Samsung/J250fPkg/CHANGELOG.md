# Revision 0.2 changes

- Linked real SDCC1/MMC backend; removed EFI_UNSUPPORTED stub.
- Power IRQ registers accessed as 32-bit MMIO, consistent with LK `sdhci_clear_power_ctrl_irq`.
- A **successful eMMC LBA0 read** is required before BlockIo publication.
- PM8917 power provider arrives asynchronously via `RegisterProtocolNotify`; missing provider cannot silently block DXE dispatch.
- Read-only user-area BlockIo, IoAlign=1 (PIO), no DMA.

**Source provenance:** Standalone controller transport from original supplied J250fMuPkg, cross-checked against LK, NOT a line-for-line transplant of upstream lk2nd's three MMC files. See README for full disclosure.
- Optional explicitly opt-in `J250F_ALLOW_LK_POWER_HANDOFF` uses LK's bootloader-retained-power assumption rather than depending on a PM8917 provider; this cannot verify rail voltage, and is not enabled by default.
