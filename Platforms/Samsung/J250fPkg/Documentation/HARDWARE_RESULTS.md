# User-reported hardware evidence

The handoff build progressed past CMD0 and CMD1 on the phone. The reported
failure was CMD2 with IRQ=0x00028000, last R1/OCR=0xC0FF8080 and pre-reset
state=0x01F80001. The R1/OCR value is the earlier CMD1 R3 response; it is
not a CID. CMD1 reported ready and sector addressing. IRQ bit17 is command
CRC error, with summary bit15. No Block I/O was published.

This is progress in card communication, not successful CID/capacity discovery.
The next build adds Samsung's missing low-speed DLL/feedback setup and retains
CRC failure checks. Its behavior on the physical phone is not confirmed.

## Latest reported result after the DLL correction

The user reported the same failure again: CMD=2, IRQ=00028000, last R1/OCR
C0FF8080, STATE_BEFORE_RESET=01F80001, Device Error, no Block IO published.
Thus the DLL change did not resolve the on-device CRC error. CMD0/CMD1 complete,
but CMD1/R3 has no CRC check; their completion does not prove response integrity.
The CMD2 response flags match both Samsung SDHCI and the Qualcomm LK reference.
No CRC bypass is enabled and no successful CID/capacity/sector read is claimed.

## Pad-voltage correction awaiting hardware results

The current build selects 1.8V pads after successful BUS_ON even without a
separate IO-voltage interrupt. The prior handler only applied this for explicit
IO_HIGH/IO_LOW. The proposed analysis was supplied as text; the referenced
J250F_CMD2_PadVoltage_Review.zip was not present locally. Only the narrow
condition was adapted to the current source; RPM, SMEM and DLL work is retained.
Failed-command snapshots now include VENDOR/DLL/CLK before recovery reset.
The expected pad bits satisfy (VENDOR & 0x18000)==0x18000. This is not proof
of actual electrical supply voltage or successful card communication.

## Legacy MCLK selection correction awaiting hardware results

The latest CMD1 diagnostic supplied in chat reported IRQ=0, R1=0,
VENDOR=0x0001801C, DLL=0x6000642C and CLK=0x0007. The vendor value has the
1.8V pad bits set but its MCLK selector bits [9:8] are zero. The updated core
restores the Qualcomm legacy default selector (2 << 8), verifies readback,
and checks it again after the command/data inhibit bits report idle before
issuing a command. Its update preserves the pad and DLL fields. This is a
testable explanation for the reported CMD1 timeout, not a confirmed cause;
there is no phone result from a build containing this correction yet.

No command is issued if the MCLK readback does not match. Simulation/build
success cannot establish that the selected clock source is electrically
correct on the J250F. A failed readback is now included in the vendor, present,
DLL and clock diagnostic snapshot before the driver returns.

## Host Control 2 readback and setup log

The latest screenshot supplied in chat reports CMD2 `IRQ=00028000`,
`VENDOR=00018A1C`, `DLL=6000642C`, and `CLK=0007`. The MCLK selector field is
now `2`; the screenshot does not show HC2. The current core already cleared
HC2 timing bits `[2:0]` during low-speed setup, but did not verify the readback.
The updated code makes that operation conditional for rates at or below 25MHz,
checks that `[2:0]` read back as zero, and preserves the 1.8V signaling bit.

The DXE backend now prints `HOST_SETUP HC2=... GCCCFG=... TLMM=...` after host
setup and before it sends CMD0. This identifies the actual timing, GCC clock
configuration, and pin register values for the next device result. HC2 was not
shown in the screenshot, so this check is diagnostic; it does not demonstrate
that a timing-mode mismatch caused the CRC error. The resulting build has not
been tested on the phone.
