# J250F SDCC1 diagnostic (Milestone A)

This DXE driver samples only SDHCI HOST_VERSION (16-bit, +0xfe),
CAPABILITIES (+0x40) and CAPABILITIES_1 (+0x44), once during DXE.
The platform already maps peripheral MMIO as NS_DEVICE.

Addresses come from the extracted Samsung revision-04 DTS:
`j250f-uefi/analysis/extracted_dtbs/j2y18lte_rev04.dts`,
node `/soc/sdhci@7824900`: host 0x07824900, core 0x07824000.
Core and command queue registers are not accessed.

Output uses the platform SerialPortLib (framebuffer on this target),
including a message before the first MMIO access. No console wait or
key press is introduced; later boot output may scroll the results away.
There are no controller writes, clock/power changes, card commands,
interrupt handlers, DMA, or installed storage protocols.

Clock/power state is inherited from aboot. Reads of a gated or protected
peripheral may hang or abort the CPU; this driver cannot recover from
that. Zero/all-ones values do not establish a valid controller, and
plausible register values do not establish communication with the eMMC.
Hardware results are still required before Milestone B.

Build from Mu-Qcom using the existing J250F ARM DEBUG CLANG38 settings.
The isolated test artifact is Mu-J250F-SDCC1-PROBE-DEBUG.img at workspace
root; no hardware boot or flash has been performed by the coding agent.
