# J250F Project Mu eMMC integration

Active components in the J250F platform DSC/FDF:
- `PlatformIntegration/J250fPowerDxe.inf`: actual bounded RPM-SMD backend.
- `J250fEmmcDxe/J250fLkEmmcDxe.inf`: controller backend plus read-only Block I/O.

The active frontend/backend came from `J250F_ProjectMu_eMMC_Actual_Backend_Source.zip`.
This remains a source-level SDHCI implementation cross-checked against Samsung/LK;
it is not a complete verbatim transplant of Qualcomm LK. The previous draft
frontend and its tests are retained as references, but are not active components.

## Implemented platform backend

`RpmSmd.c` adapts the Samsung kernel's SMEM/SMD/RPM message formats. It locates
already allocated APPS/RPM edge-15 `rpm_requests` stream/packet channels in global
or APPS/RPM uncached SMEM, preserves existing allocations, uses bounded polling,
and requires the matching message-ID acknowledgment. Bootloader sessions are
reclaimed through bounded, peer-acknowledged SMD close/open, rather than requiring
empty inherited FIFOs. Only locally owned counters are reset, after the peer
acknowledges closure. NACK, corruption, failed handoff, and timeout remain errors.

`SmemMap.c` follows Samsung's target-info register at 0x0193d000, with the DTS
0x86300000/1MiB fallback when target information is absent/invalid. J250F's kernel
has CONFIG_PHYS_ADDR_T_64BIT=n, so the SIII physical-base field is 32-bit.
GCD/CPU mappings are uncached; allocatable system memory and owned ranges are
rejected. No SMEM object is allocated. The private RPM transport assumes that
UEFI is the sole APPS-side SMD owner after the bootloader handoff.

`J250fPowerDxe.c` obtains ACKed active XO and PM8917 L8/L5 votes: VDD 2.9V,
VQMMC 1.8V, active loads 570/325mA. Both rails are always-on in the J250F DTS;
logical BUS_OFF reduces load rather than disabling the shared supplies. RPM ACK
means the request was accepted; it is not an independent electrical measurement.
No GPLL0 readiness is fabricated: that callback returns EFI_UNSUPPORTED.

## Recognition-first storage path

The local integration stays at one-bit 400kHz using the Samsung GCC XO rate.
It does not switch bus width/partition with CMD6 or depend on 20MHz/GPLL0.
A selected hardware partition is rejected. Card identification, CID, EXT_CSD
capacity, and a successful user-area sector-0 read are required before Block I/O
publication. Writes remain rejected even through the core's old enable API.

The DXE dependency expression waits for CPU, GOP, and power protocol readiness.
The archive's notification path was replaced to avoid leaving a callback into
an image that returns a load error. Initialization results remain visible for
90 seconds. The backend reports the last command, interrupt, R1, and pre-reset
controller state on card initialization failure.

## Evidence and limits

A full ARM32 DEBUG Project Mu build and host checks are performed locally.
`make test arm syntax sanitizer` checks the integrated code against real Mu
headers: SMEM parsing/mapping, RPM framing/ACK/error paths, rail-provider vote
values, recognition, initial sector read, Block I/O and failure propagation.
All remote RPM/card responses in those checks are simulated. They cannot prove
SMEM access permissions, the electrical rails, or eMMC communication on hardware.
No successful physical J250F recognition is claimed for this build.

Only uncached pre-existing SMEM entries and the known edge-15 word-access SMD
layout are supported. Shared-memory bus faults are not recoverable by C bounds
checks. Sleep/suspend, high-speed clocks, DMA, tuning, write/erase operations,
and Windows OS/runtime storage are not implemented. Bootloader DDR, RPM firmware,
and base interconnect availability are prerequisites.

## Build

From Mu-Qcom, use the existing J250F ARM DEBUG CLANG38 platform command.
From this package directory, `make test arm syntax sanitizer` runs the host checks
and standalone ARM compilation. The actual firmware must be built with stuart;
standalone objects and simulations are not installable EFI drivers.

Upstream reference scripts are references, not setup scripts for this integration.
Original license/provenance files are retained. See current BUILD_AND_TEST_STATUS.md.

## Low-speed sampling correction

Samsung `sdhci_msm_set_uhs_signaling()` explicitly asserts CORE_DLL_CONFIG
DLL_RST (bit30) then DLL_PDN (bit29) and selects a low-speed HOST_CONTROL2
mode below 100MHz. This is now done with the card clock stopped, preserving
unrelated bits. CRC checks remain enabled. See reported hardware results.

## Fixed 1.8V pad selection on BUS_ON

The J250F fixed-1.8V pad selector is now applied after a successful BUS_ON as
well as IO_HIGH/IO_LOW, while logical rails are on. Samsung's BUS_ON handler
also sets io_level=REQ_IO_HIGH. Both PAD_SWITCH_EN (bit15) and PAD_SWITCH
(bit16) are ORed into the vendor register; unrelated bits are preserved.
Regulator/pin failures and BUS_OFF cannot enter that update. No RPM/SMEM/DLL
code is replaced by an older source package. Failure snapshots now include
VENDOR, DLL and CLK values captured before command reset.

BUS_ON-only, regulator failure, pin failure, BUS_OFF and CRC rejection tests
pass in simulation. Physical success after this change is not yet confirmed.

## Legacy MCLK readback guard

The latest user-reported CMD1 values included `VENDOR=0x0001801C`, whose
MCLK selector field `[9:8]` is zero, and `CLK=0x0007`. The core now restores
and verifies Qualcomm's default legacy MCLK selection (`2 << 8`) after clock
setup and again while the host is idle immediately before commands at or below
50MHz. It preserves the pad-voltage and DLL fields and aborts without sending
a command if the selector fails readback. This correction has not yet been
tested on the phone; it is not evidence that eMMC recognition now succeeds.
The failure path captures the observed vendor, present-state, DLL and clock
registers before returning without issuing a command.
