# J250F firmware ADB session

Experimental branch `experimental/j250f-adb-shell`. This adds ADB bulk endpoints
and a legacy ADB shell/SYNC-v1 daemon to the ChipIdea USB controller. It uses
AOSP's wire formats described in [protocol.md](https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/docs/dev/protocol.md),
[services.md](https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/docs/dev/services.md)
and file_sync_protocol.h. It advertises no Android shell-v2 or sync-v2 features.

## PC connection

Install Android SDK Platform Tools. Connect the data cable and boot this
experimental image using your established recovery procedure. The screen
prints `ADB DEBUG start`. The USB development ID is **1209:8918**, with serial
`J250F-UEFI`, and interface class/subclass/protocol FF/42/01. This differs from
the previous endpoint-0 gadget ID 1209:8917 to avoid stale Windows descriptors.
The VID/PID is development-only and requires an authorized assignment before
redistribution.

    adb devices -l
    adb -s J250F-UEFI shell help
    adb -s J250F-UEFI shell

The firmware includes Microsoft OS descriptors requesting WinUSB with the ADB
interface GUID. Their actual Windows binding remains unverified. If Windows
shows the device but adb does not, inspect its driver: it needs WinUSB and ADB
interface GUID {F72FE0D4-CBCB-407D-8814-9ED673D0DD6B}. An existing custom PyUSB
binding for 1209:8917 does not configure this new device. On Linux, configure
USB permissions for 1209:8918 if adb reports insufficient permissions.

If the device never enumerates, the daemon cannot be reached. This build still
relies on inherited USB PHY/clock/rail state. A previous EP0 enumeration result
does not establish bulk endpoint operation.

## Commands

    adb shell emmc status
    adb shell emmc registers > registers.txt
    adb shell emmc init
    adb shell emmc dump-cid
    adb shell emmc set-clock 400000

`emmc init` retries the complete identification sequence from CMD0, at 400kHz.
`emmc cmd2` is an alias for that sequence, not an isolated CMD2 issued in an
unknown card state. Attempts remain bounded at 32 per boot. CID is reported
only after successful identification. A successful interactive retry does not
publish a new Block I/O disk. The 144kHz rate is unsupported by the current
clock implementation. Existing built-in storage diagnostics reject writes.

In the interactive prompt, `exit` closes that shell connection; the firmware
continues serving ADB. `firmware exit` disconnects USB and returns from this
DXE debug session so normal firmware dispatch can continue. `reboot` resets
the phone. There is no 90-second cutoff. USB polling stops during a blocking
command, while an EFI image is running, and when firmware exits or an OS takes
over. The DXE's GOP/CPU/power dependencies must first be satisfied. This debug
mode holds the DXE entry point until explicit exit; other drivers may not yet
have dispatched, which can limit protocols available to uploaded applications.

## Built-in GOP diagnostics (no external EFI app required)

These commands use EDK II's real Graphics Output Protocol GUID, UEFI service
definitions, and the firmware's own `gBS` table, avoiding hand-written UEFI
ABI structs in an ARM32 test binary.

```sh
adb -s J250F-UEFI shell gop status
adb -s J250F-UEFI shell gop test
```

`gop status` reports `LocateProtocol`, `LocateHandleBuffer`, handle count,
screen resolution, stride, pixel format, framebuffer address and size, and
whether a physical 32-bit BGRX GOP layout is advertised. GOP format 3 is
`PixelBltOnly` (24-bit fallback); format 1 is `PixelBlueGreenRedReserved8BitPerColor`
(native BGRX). Neither alone proves actual panel scanout.

`gop test` saves a **192x16** rectangle on the bottom edge of the screen,
draws red/green/blue bars, performs a GOP readback, displays the bars for
750ms, then **restores the original pixels**. It does not write to storage
or raw display-controller MMIO. `RGB readback=PASS` verifies software readback,
but physically visible bars must be checked separately.

If the standalone `J250fGopTest.efi` reports `StartImage returned: Not Found`
while `gop status` finds the GOP, the standalone app's hand-written UEFI
ABI/GUID, build options, or its PE/COFF entry point are suspect. These built-in
commands avoid that ambiguity. If `gop status` also reports Not Found, inspect
the flashed firmware build, DXE GOP installation and debug logs; don't blindly
write to `0xA8300000`.

## Upload and run compiled firmware code

    adb push mytool.efi /ram/mytool.efi
    adb shell efi list
    adb shell efi run /ram/mytool.efi
    adb shell efi run /ram/mytool.efi argument1 argument2
    adb shell efi unload 0

The release also includes J250fAdbHello.efi, built from AdbDebug/Hello. Upload
it to /ram/J250fAdbHello.efi and run that path as a first load/return test. It
returns EFI_SUCCESS and prints to the EFI console if one is available.

Uploads use **one RAM slot**, up to 16MiB; a new SEND replaces the previous
upload. No eMMC filesystem is required. Files disappear on reboot. Firmware
uses LoadImage and StartImage, so the executable must be a compatible ARM32
UEFI PE/COFF application or driver. Source C/C++, Linux ELF executables and
Windows EXE/SYS files cannot be launched this way. Compile your new driver or
diagnostic with EDK II for ARM before uploading it. Image loading can still
fail due to image format, firmware authentication, missing protocols or the
program's own assumptions.

Resident drivers are listed by slot (up to 16). Unload requires the driver's
own unload support. EFI application console output is not automatically routed
into ADB; the shell returns the StartImage result after the image returns.
Programs which expect interactive EFI console input are not remote terminals.
A non-returning or crashing image can interrupt the session and require a
power cycle.

This is an **unauthenticated development ADB daemon**. It accepts a connected
PC without Android RSA key approval. Uploaded EFI code runs with firmware
privileges and can write storage or MMIO; the built-in diagnostic restrictions
do not constrain that code. It is intended for your physical debugging setup.

The shell is a firmware command interpreter. `help` lists its implemented
commands; Android/Linux commands such as logcat, mount, sh or apt are not
provided. Legacy shell transport does not report command exit codes to adb;
inspect the response text. Logs can be captured with host redirection as above.

## Verification and limits

The protocol engine is tested with Google's real adb host over a TCP harness:
CNXN, one-shot/interactive shell, fragmented packets and multi-frame SYNC push.
The harness uses fixture command/upload callbacks and does not simulate USB
hardware. Separate tests use the actual EFI upload/launch callbacks against a
mocked UEFI loader. A USB state-machine fixture checks separate header/payload
transfers, descriptors and DMA teardown with mocked registers/cache operations.
The Mu integration tests verify persistence and explicit
exit, and retain disk-publication/write-protection checks. ARM compilation,
sanitisers and boot-image packaging are checked locally. Physical USB bulk
transfers, Windows driver binding, uploaded ARM EFI execution and eMMC
recognition remain unverified until tested on the phone.

    make -C Platforms/Samsung/J250fPkg test arm syntax sanitizer
    make -C Platforms/Samsung/J250fPkg adb-test ADB_BIN=/path/to/adb
