# J250F MSM8917 SDCC1 read-only eMMC DXE v1 — EXPERIMENTAL

This is the first **actual UEFI eMMC block driver** for this port, not a register probe.
It contains the Qualcomm v4 host initialization, 400kHz GCC SDCC1 root-clock setup,
polled eMMC identification, PIO-only 512-byte reads, and an EFI_BLOCK_IO_PROTOCOL
that is published **only after reading LBA0 and LBA1 and confirming the protective
MBR and GPT signature**.

## Safety / exact scope
- **No media writes**. BlockIo.WriteBlocks unconditionally returns EFI_WRITE_PROTECTED.
- There is no raw SD/MMC PassThru exposed and no EFI Erase Block or Storage Security.
- The host command dispatcher hardcodes ONLY CMD0, CMD1, CMD2, CMD3, CMD7, CMD8,
  CMD9, CMD13 and CMD17. There is no CMD6 SWITCH, CMD24/25 write, CMD35/36/38
  erase, RPMB, trim, boot-configuration change, multi-block write or CQE DMA.
- Every data transfer is one **PIO read** of 512 bytes; a GPT signature is checked
  before publishing the disk.
- No code modifies the stock bootloader, GPT, recovery or Android partitions.
- The driver **does write volatile host registers and GCC clock registers** and
  issues read/identification commands that affect volatile card state. Bugs could
  still cause crashes or unexpected behavior. No software filter is absolute.
- **Windows after ExitBootServices** is not protected by UEFI ReadOnly mode.
  Never expose SDCC1 to Windows via ACPI or a Windows host driver in the current
  research port.

## Qualcomm clock-specific fix
The previous diagnostic stopped at "cannot safely establish 400kHz identification
clock" because it inferred source rate from generic SDHCI CAPABILITIES bits and
required an inherited clock state. This driver instead programs the dedicated
GCC SDCC1_APPS_CLK_SRC RCG2 for the documented 400kHz XO frequency:

Source: youheng7185/linux_j2y18lte, drivers/clk/qcom/gcc-msm8917.c,
F(400000, P_XO, 12, 1, 4), RCG command offset 0x42004
with GCC base at 0x01800000. Uses M=1, complement N=~3, D=~4,
HID=23, dual-edge, then polls the RCG UPDATE bit.

This is a hardware-level clock change and **must be reviewed on the exact
J250F board**; no other clocks or PMIC registers are changed.

## Sources
- Samsung J250F rev04 DTS and Linux drivers/mmc/host/sdhci-msm.c.
- youheng7185/linux_j2y18lte drivers/clk/qcom/gcc-msm8917.c,
  drivers/clk/qcom/clk-rcg2.c and MSM8917 DTS.
- youheng7185/u-boot drivers/mmc/msm_sdhci.c, drivers/mmc/sdhci.c,
  include/sdhci.h and drivers/mmc/mmc.c (reference only; no code copied).
- Project Mu MdePkg Protocol/BlockIo.h API.

## Gate: Not built, not cleared to flash
This source was published for independent review and a fresh ARM32 EDK2 build,
**not flashed or compiled here**. Do not flash or merge just because it exists.
Have an independent builder compile the DEBUG image from this isolated branch,
audit GCC/SDHCI register offsets against the current board, verify PE/COFF and
Android boot header/QCDT/signature, report SHA256, and preserve the known-good
boot backup/TWRP recovery route.

## ONE hardware-result request after independent clearance
Take ONE photo of the held 90-second result screen:
- `SUCCESS: GPT verified; read-only BlockIo published` with eMMC capacity, OR
- `STOP: ...` with a status/error reason.

It does **not** install Windows or allow internal flash writes.
