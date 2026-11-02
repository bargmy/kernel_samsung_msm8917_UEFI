# J250F ADB diagnostics version 2

Experimental ARM32 DEBUG image: Mu-J250F-ADB-DIAG2.img.

Completed 144/400 kHz selection, bounded named MMIO reads, command traces captured
before recovery, raw long responses, and clock-gated mask/value profiles.
CRC checks remain enabled. All storage access remains read-only.

Start with the serial actually shown by `adb devices -l`. Examples assume J250F-UEFI:

```sh
adb -s J250F-UEFI shell emmc capabilities
adb -s J250F-UEFI shell emmc snapshot
adb -s J250F-UEFI shell emmc clock
adb -s J250F-UEFI shell emmc identify
adb -s J250F-UEFI shell emmc trace
adb -s J250F-UEFI shell emmc set-clock 144000
adb -s J250F-UEFI shell emmc clock
adb -s J250F-UEFI shell emmc identify
adb -s J250F-UEFI shell emmc snapshot
```

`emmc trace <seq>` prints one complete transaction snapshot from the last 16.
`emmc map` lists named register addresses and widths; `emmc read HC2` and
`emmc read hc 0x3e 16` are equivalent bounded reads. Live reads are labeled;
PRE-RECOVERY records capture command completion before W1C/reset.
The decoded clock assumes a 19.2 MHz XO; it is not a physical measurement.
Raw response words after CRC failure are untrusted and may be stale.

`identify` and `cmd2` run CMD0/CMD1/CMD2 only. A valid CID can be dumped without
capacity discovery. `init` continues through EXT_CSD, preserving the selected
clock. Interactive attempts share a 32-per-boot limit, and do not publish Block
I/O. Startup still requires a successful LBA0 read before publishing a disk.

`emmc config show` lists allowed masks and baseline/live values.
`emmc config set <vendor|dll|hc2|tlmm> <mask> <value>` applies only allowed bits,
with the card clock gated and readback verified. Profiles reapply before
identification commands; `emmc config reset` restores captured baseline bits.
Change one understood field per experiment; these controls do not establish a
working eMMC fix. Commands reject unavailable/busy/already-initialized hosts.
Profiles live in RAM and are not saved across reboot.

Validation: full Mu ARM32 DEBUG build; host controller/power/SMEM/USB/EFI tests;
ARM syntax checks; ASan/UBSan (leak detection disabled because this environment
uses ptrace); Google adb interoperability over a TCP fixture; Android header,
BootShim entry/size, complete FD payload, QCDT and Samsung footer verified.
Physical USB operation and eMMC identification with this image remain unverified.

SHA-256: 2a6d350f3ce48cd52c420c93b0f1e0e71393a4e3f4ffedc2ec5c715b32d0e1d4
