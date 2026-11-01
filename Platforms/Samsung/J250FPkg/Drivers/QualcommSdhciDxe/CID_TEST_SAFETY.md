# J250F SDCC1 eMMC CID identification — experimental source only

This changes the **SDCC1 controller** from register-read-only probing to one explicitly
allowed **non-data MMC identification sequence**:

- CMD0 (GO_IDLE_STATE; resets **volatile** eMMC state)
- CMD1 (SEND_OP_COND; reads OCR, negotiates access mode)
- CMD2 (ALL_SEND_CID; reads the chip ID)

## Flash safety boundaries

The driver **does** write to SDHCI controller registers, because commands cannot
be sent otherwise. These writes change volatile host/card state; they **do not
target eMMC data sectors**.

Only these host registers are written:
- `0x0782492f` SDHCI_SOFTWARE_RESET, command-line reset bit only, once before CMD0 if CMD inhibit persists without DAT inhibit. No card reset pin is asserted.
- `0x0782492c` SDHCI_CLOCK_CONTROL (slow clock)
- `0x07824930` SDHCI_INT_STATUS (W1C)
- `0x07824934` SDHCI_INT_ENABLE (command/error status latching; restored on exit)
- `0x07824908` SDHCI_ARGUMENT
- `0x0782490e` SDHCI_COMMAND

The only allowed card commands are CMD0, CMD1, and CMD2. In particular, there
is **no CMD6** (SWITCH/partition changes), **no CMD17/18/24/25** (sector I/O),
**no erase commands**, no DMA, no RPMB, no boot-partition selection, and no
BlockIo / PassThru protocols.

This is **software-enforced**, not a hardware guarantee. A faulty register
address, unusual Qualcomm controller behavior, power/clock failure, or firmware
bug can still cause a hang or other fault. The Windows driver will not inherit
this protection.

## Fail-closed prerequisites

- Host version and Qualcomm core-version registers must look valid.
- Device must already be powered by Samsung aboot.
- SDHCI host spec version must be 3.0 or newer, and advertised base-clock MHz
  must be nonzero/plausible.
- Internal clock must be enabled/stable and the card clock running.
- The host clock is lowered to <=400 kHz using the standard SDHCI clock divider.
- No host reset, voltage changes, vendor-specific MMIO writes, or regulator/PMIC
  writes are performed.
- Any command error/timeout stops further card commands.
- No eMMC data-sector reads/writes; returned CID is only identification data.

## This PR is NOT cleared for device flashing

An EDK2 ARM32 build has **not** been performed by this assistant. Do not replace
the known-good image until another builder has reproduced the DEBUG ARM build,
verified a boot-image binary and SHA-256, and reviewed the C implementation.

Build only from branch `j250f/sdcc1-identify-readonly`. Keep a verified TWRP
boot rollback and recovery route. The firmware will display OCR and raw CID
(or a STOP reason) for 90 seconds for one photograph. Fully power-cycle after
the test because CMD0 leaves the eMMC in its volatile initialization state.

## Source references

- Samsung MSM8917 J250F revision-04 DTS: host base, core base, regulators.
- Samsung `drivers/mmc/host/sdhci-msm.c`: Qualcomm register behavior.
- youheng7185/u-boot `drivers/mmc/msm_sdhci.c` and `include/sdhci.h`.
- U-Boot `drivers/mmc/mmc.c`: MMC identification protocol, used as a reference
  only; no GPL code copied.

This experiment is not a finished SDHCI, filesystem, or Windows Phone driver.

## Legacy identification timing update

Before identification the driver stops SDCLK, selects Qualcomm default MCLK,
clears HS400 input override and clock power-save in host +0x10c, clears
host-control2 (+0x3e) UHS/tuning/preset bits, and clears host-control (+0x28)
high-speed and bus-width bits. Voltage/pad-power and PMIC settings are preserved.
The maximum v3 divider (1023, divide-by-2046) replaces the broken CAPS base-clock
calculation. The Samsung J250F DTS maximum source rate is 384 MHz. These timing
changes are not restored; power-cycle after testing. Hardware verification pending.

## Qualcomm host frontend enable

Before identification V6 sets core HC_MODE (+0x78) HC_MODE_EN (bit 0) and
FF_CLK_SW_RST_DIS (bit 13), preserving other bits, following Samsung probe.
A readback must contain both bits or the test stops before card commands.
This is an additional volatile core-register write; power-cycle after test.
