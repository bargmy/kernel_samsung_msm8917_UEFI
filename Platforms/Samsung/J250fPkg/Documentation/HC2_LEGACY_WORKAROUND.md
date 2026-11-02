# J250F explicit legacy timing workaround

Device evidence supplied on 2026-11-02: at 400 kHz, `emmc config set hc2 0x7 0`
followed by `emmc init` produced CID_valid=1/card_ready=1. Removing the override
was followed by failure; reapplying it was followed by full-init success.
Reported CRC-valid CID: 15010051 4536364D 4200B0D7 D88C3500.
These are user-supplied phone results, not measurements from the build host.

The core now waits for command and data lines to be idle before initialization
commands. After the existing MCLK check it gates the SDHCI card clock, explicitly
writes HC2 timing bits [2:0] to zero even when already zero, fences and verifies
the timing readback, then restores the original clock and fences before issuing
the command. A failed readback restores the clock and aborts without sending the
command. Other HC2 fields are preserved. Diagnostic profiles still run afterward
so controlled comparisons remain possible. After card_ready, normal reads do not
perform this additional clock gating.

This reproduces the HC2 write/gating part of the successful diagnostic sequence.
The diagnostic profile also rewrites baseline bits in vendor/DLL/TLMM registers;
those extra writes are deliberately not made permanent without evidence that
they are needed. The precise silicon dependency and cold-boot reliability remain
unverified. CRC checking and read-only storage policy remain enabled.

Validation: host recognition, power, SMEM, integrated backend/frontend, EFI and
USB tests; ARM syntax checks; ASan/UBSan with leak detection disabled under
ptrace. A timing-sensitive emulator checks a fresh gated HC2 write before every
initialization command, preserved unrelated HC2 bits, and clock restoration and
no command submission after failed readback. Existing CRC-error tests remain.
Simulation validates ordering and failure handling, not physical recognition.

Phone verification: boot the new Mu-J250F-HC2-FIX.img without setting a profile.
Save `emmc status`, `emmc config show`, `emmc snapshot`, and `emmc dump-cid`.
All profile masks should be zero. Startup must identify the card and read LBA0
before publishing Block I/O. Preserve startup output and confirm partition/disk
discovery. Repeat from a fresh boot to assess reliability. If startup fails,
save traces before trying the old HC2 override for a controlled comparison.
