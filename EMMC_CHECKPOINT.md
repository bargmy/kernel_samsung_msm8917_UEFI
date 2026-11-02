# J250F persistent ADB experimental checkpoint

Branch: experimental/j250f-adb-shell, based on the USB EP0 experiment. Main
has not been merged or changed. See [ADB setup and commands](Platforms/Samsung/J250fPkg/AdbDebug/README.md)
and [current build evidence](Platforms/Samsung/J250fPkg/AdbDebug/BUILD_STATUS.md).

The driver serves a development ADB session until firmware exit or reboot.
It supports diagnostic shell commands and SYNC uploads to one 16MiB RAM slot.
Uploaded ARM32 EFI applications/drivers use LoadImage/StartImage with firmware
privileges. There is no Android RSA authorization in this development build.
The session pauses during blocking commands/EFI execution and ends when an OS
takes over. CPU/GOP/power dependencies and inherited USB PHY/clocks are required.

The latest physical eMMC evidence remains CMD2 CRC failure (IRQ=00028000,
R1/OCR=C0FF8080, VENDOR=00018A1C). No successful physical CID, capacity, LBA0
read or Block I/O publication is claimed. This build has no phone result yet.

Image: /workspaces/web-k/Mu-J250F-EMMC-DEBUG.img
SHA-256: `39b75497bf5a5eb7a048f2aa652e3331cf81160e46ae76fb968d0dd397ac67d9`

The full ARM32 DEBUG build, boot packaging, host simulation/sanitizer tests,
and real adb protocol tests over TCP passed. USB hardware and uploaded ARM EFI
execution remain unverified. The real adb harness uses fixture callbacks;
separate tests exercise actual USB/EFI adapters with mocked hardware/services.

## Reproducing the build

Mu_Basecore stays pinned to 022c680837e9bdd70ef221c78a8c985bc1e730a9. Apply
MuPatches/Local_Basecore_Checkpoint.patch once to a clean submodule checkout.
Existing local submodule changes remain captured in that patch.

    CLANG38_BIN=/usr/bin/ CLANG38_ARM_PREFIX=arm-linux-gnueabihf- GCC5_ARM_PREFIX=arm-linux-gnueabihf- stuart_build -c Platforms/Samsung/J250FPkg/PlatformBuild.py TOOL_CHAIN_TAG=CLANG38 TARGET=DEBUG RAM_SIZE=1.5 FD_BASE=0x99000000 FD_SIZE=0x00300000 FD_BLOCKS=0x300 TARGET_ARCH=ARM

    make -C Platforms/Samsung/J250fPkg test arm syntax sanitizer
    make -C Platforms/Samsung/J250fPkg adb-test ADB_BIN=/path/to/adb
