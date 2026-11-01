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
