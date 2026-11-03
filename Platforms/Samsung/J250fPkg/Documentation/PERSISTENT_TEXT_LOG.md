# Bounded persistent firmware text log (development)

## Current status

Build 65 successfully compiled in GitHub Actions and persisted **100 checksum-valid
records (11,044 text bytes)** on the phone after preparation. The logger's physical
write path is now demonstrated; the separate Windows Boot Manager reboot remains
unresolved. New console/handoff changes have host and ARM validation but still
require their own CI/hardware test. See [reboot investigation](BOOTMGFW_REBOOT_CHECKPOINT.md).

The connected recovery reported SM-J250G / j2y18lte; its donor images were
verified under `/data/wp81`, not `/data/media/0/wp81`. The existing firmware
target is still J250F; this change does not establish variant compatibility.
The agent has not flashed or rebooted the phone. The prepared log file was written
and verified, and userdata was cleanly unmounted with explicit user permission.
The user booted the DEBUG image. No donor image was modified.

## What is captured

In a **DEBUG, USE_UART=0** J250F build:

- Framebuffer `SerialPortWrite` and `SerialPortWriteCritical` append to a shared
  48 KiB queue from immediately after PrePi creates its HOB.
- DXE instances rediscover the HOB on each write because DXE relocates the list.
- Once the private userdata backing is staged and its public disk interfaces
  are concealed, the logger drains queued text to `/wp81/uefi.log`.
- The current system `ConOut` and `StdErr` OutputString calls are also captured
  after installation, as UTF-8, preserving whitespace. Return values and actual
  display output are preserved. A timer/ReadyToBoot callback checks the console.
- Flushes use the boot CPU, safe TPL, the existing disk serialization, and a
  recursion guard. Elevated-TPL/IRQ-masked text is queued for a later safe flush.

This is **not literally every pixel/text ever displayed**. Text before HOB
creation, early console calls before the tee is installed, graphical text drawn
directly into GOP/framebuffer, and Windows/native-OS output after firmware
handoff are outside coverage. Capture stops at ExitBootServices without doing
storage I/O in that callback. A reset can lose queued bytes. Queue contention
and overflow are counted and reported when a later flush can run.

## Storage boundaries

General physical Block I/O and ext4 writes remain write-protected. The only new
physical write callsite uses an index into a completely validated file-sector
allowlist. It cannot create, extend, truncate, allocate blocks, update timestamps,
change ext4 metadata, or write the donor images. Records are strictly sequential.

The pre-existing regular file must be exactly 1 MiB, have one link, initialized
allocation, no external ACL block, and no flags except extents. At most four
inline-root extents are accepted; holes/unwritten extents are rejected. The
filesystem must be cleanly unmounted, with 4 KiB blocks, 32-byte descriptors,
`incompat=0x42`, `ro_compat` restricted to `0x13`, and no pending orphan inode.
Legacy GDT checksums and allocation bits are checked; filesystem layouts with
64bit, flex_bg, metadata_csum, encryption, etc. are deliberately rejected.
**Do not format userdata to satisfy these checks.** Unsupported storage simply
disables logging while the existing boot path continues.

The superblock is rechecked before activation. The private CMD24 operation checks
bounds, waits for programming/TRAN readiness, and verifies the written sector by
reading it back. A timeout or uncertain write permanently disables further writes
for that boot; only a busy-before-write result is deferred. The eMMC volatile
cache must already be disabled; firmware does not change cache configuration.

These are software restrictions, not power-loss guarantees or a security sandbox.
UEFI isolation still does not constrain Windows native hardware/MMIO access.

## Prepare a file (only for a tested, recoverable firmware build)

On the PC:

```text
python scripts/prepare_uefi_log.py create uefi.log
```

In recovery, first check that `/data/wp81` is the real directory and that no
`uefi.log` already exists. Preserve any old log elsewhere. Fully copy this
template to **`/data/wp81/uefi.log`**. Do not use `truncate` or `fallocate`: all
data sectors must really contain the initialized template. Verify a byte-exact
copy/hash and sync. Recovery/Android must cleanly unmount *all* aliases of
userdata (this device also mounted it at `/sdcard`) before EFI starts. Merely
running `sync` does not clear ext4's needs-recovery flag. No automatic unmount,
reboot, flashing, or replacement of an existing log is performed by the utility.

In EFI the path is `/wp81/uefi.log`, relative to userdata. It is **not** a file
inside the FAT donor image. A missing, full, corrupt, dirty, unsupported or
unprepared target is refused; inspect the `UEFI LOG` framebuffer diagnostics.

The file contains printable framing plus text and appends across boots until
its 2048 record slots are exhausted. Each record stores up to 480 text bytes;
short flushes consume whole slots. It never wraps or erases old records.

## Read it after a failure

Copy `/data/wp81/uefi.log` back using recovery, then:

```text
python scripts/prepare_uefi_log.py extract uefi.log boot-log.txt
```

If power loss tore a record, normal extraction refuses it. Preserve the raw file
and recover only its verified prefix with:

```text
python scripts/prepare_uefi_log.py extract uefi.log boot-log-prefix.txt --salvage
```

The V2 record has a 32-byte ASCII header: bytes 0..9 `J250FLOG2 `; 10..13
CRC16; 14..17 sector index; 19..23 `READY`/`USED `; 25..30 exact payload length
for USED; byte 31 LF. CRC16 covers header bytes 14..31 and all 480 payload bytes.
READY records have zero CRC and blank length/payload. CRC16 detects torn/corrupt
records but is not cryptographic. A damaged file disables future firmware writes;
preserve it and deliberately prepare a new file, rather than patching its markers.

## Verification / resume checkpoint

```text
python scripts/test_uefi_log.py
```

This runs Python template/extraction tests, the mocked-controller CMD24 tests,
the actual ext4 parser/allowlist fixture, the actual console/flush engine with
mocked UEFI services, a C-to-Python byte-exact format round trip, and ARM syntax
checks/object compilation. It does not replace a full Mu build, power-loss tests
or phone validation.

Remaining work: full ARM DEBUG Mu build with the repository's pinned dependencies
for new changes, then an explicitly authorized flash/boot test using an independent
recovery path. The existing GitHub workflow builds both DEBUG and RELEASE; this
Windows session has clang but no working WSL or
`make` on PATH. Downloaded Basecore/Arm submodules are pinned to their recorded
commits; no toolchain packages were installed. Do not flash an unbuilt artifact.

Latest verification also passed the existing controller recognition fixture
(`Tests/test_recognition.c`): normal initialization/reads still issue no CMD24
and ordinary writes remain rejected. The older
`tests/j250f_lk_mu_integration_test.c` cannot currently link as a standalone
fixture: its GOP/WP helpers and graphics GUID are not supplied by that test.
That broader test is not counted as passing; no unrelated fixture was rewritten.
