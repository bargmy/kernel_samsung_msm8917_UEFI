# J250F eMMC recognition and read-only Block I/O

Current milestone: recognize the internal eMMC, capture CID, and obtain user-area
capacity from EXT_CSD. On success the driver installs read-only Block I/O.
The framebuffer displays the result for 90 seconds. Hardware recognition is
not confirmed yet; prior on-device versions timed out or rejected startup power
status. Host simulation is not evidence of physical device operation.

## Source-backed platform integration

The linked GPL-2.0-only J250fSdccPlatformLib adapts Samsung kernel commit
`d8e6e0d79bf4e26a11d5f69c843a6ad096b2b181` clock and pinctrl code:
- GCC root: XO 19.2MHz, predivide 12, M/N=1/4 gives 400kHz.
- SDHCI divider is /1 on this explicit source, not inferred from CAPS.
- J250F SDCC1 active pads use the exact revision-04 DTS pull/drive settings.
- Startup power-status cleanup follows the Samsung probe sequence. Stale
  BUS_OFF/IO_LOW status is cleared/acknowledged at startup; it is not treated
  as an active request to change regulators. Runtime requests for those
  transitions remain unsupported. Only HC_MODE enable bit0 must latch.

## Hardware dependencies still inherited

PMIC rails, XO/RPM parent voting and interconnect state must already be enabled
by aboot. This is not a complete cold-start RPM-SMD regulator/power driver.
Failure to recognize the card cannot be ruled out by compilation or simulation.
No new regulator transition is performed or silently acknowledged as completed.

## Storage behavior

Initialization uses CMD0/1/2/3/9/7/8. The CID raw response and sector count are
reported. Hardware partition selection must be user-area (EXT_CSD[179]&7==0).
Reads use CMD17, one-bit, low-speed PIO. No DMA, HS200/HS400, CMD6, writes,
erase, RPMB or hardware boot-partition support. WriteBlocks returns
EFI_WRITE_PROTECTED; the low-level command allowlist excludes write/erase.
An I/O error disables further reads until reboot. This is software protection.

Host CMD/DAT reset, timing, GCC and pad settings are changed. Power-cycle after
testing. The module is UEFI boot-services code, not a WP8.1 kernel driver.

## Validation

Full J250F ARM DEBUG CLANG38 firmware build and Android boot packaging checks.
`tests/j250f_emmc_host_test.c` runs the real platform library with a simulated
controller and checks initialization, stale-power cleanup, runtime-power rejection,
PIO data, bounds, media ID, command/write rejection, partition selection, failure
handling, and unsupported HC_MODE bit13. `tests/j250f_sdcc_platform_test.c` checks
exact clock/pad register values, preservation, and stuck-update/branch timeouts.
