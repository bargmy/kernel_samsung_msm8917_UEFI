# J250F Project Mu eMMC development checkpoint

## Device status

The latest phone result supplied in chat was CMD1 timeout with `IRQ=00000000`,
`R1=00000000`, `VENDOR=0001801C`, `DLL=6000642C`, and `CLK=0007`. The vendor
MCLK selector bits `[9:8]` were zero while the 1.8V pad bits were set. Earlier
builds reached CMD2 and reported command CRC failure `IRQ=00028000`; neither
CID nor capacity nor a successful sector read has been demonstrated. Block I/O
has not been published.

The current source restores Qualcomm's legacy MCLK selector (`2 << 8`) after
clock setup and verifies it again while the host is idle before low-speed MMC
commands. If readback fails, no command is issued. This is a concrete response
to the latest log, but it has not yet been tested on the phone and is not a
claim that the card now initializes.

## Current build artifact

The latest ARM32 DEBUG Project Mu firmware was built and packaged as:

- `/workspaces/web-k/Mu-J250F-EMMC-DEBUG.img`
- SHA-256: `3762279c1afd9ea08b23a496616aae6f59b6e0d624af4bcd447e19f487f67c4e`

The Android boot header, 256-byte ARM BootShim, 3MiB firmware payload, QCDT
length, Samsung footer, and 32MiB partition size were checked. Packaging checks
do not test controller behavior on the phone.

## Active modules

- `Platforms/Samsung/J250fPkg/PlatformIntegration/J250fPowerDxe.inf`
- `Platforms/Samsung/J250fPkg/J250fEmmcDxe/J250fLkEmmcDxe.inf`

The driver uses RPM-SMD power requests, SMEM target-info mapping, source-backed
GCC/TLMM setup, and one-bit 400kHz read-only eMMC initialization. It does not
change hardware partitions or issue writes. CID, EXT_CSD capacity, and a
successful user-area sector-0 read are required before Block I/O publication.

## Reproducing the local build

The Mu_Basecore submodule remains pinned to
`022c680837e9bdd70ef221c78a8c985bc1e730a9`. Its local modifications are saved
in `MuPatches/Local_Basecore_Checkpoint.patch`. On a clean checkout of that
submodule, apply the combined patch once:

    git -C Mu_Basecore apply ../MuPatches/Local_Basecore_Checkpoint.patch

From Mu-Qcom:

    CLANG38_BIN=/usr/bin/ CLANG38_ARM_PREFIX=arm-linux-gnueabihf- GCC5_ARM_PREFIX=arm-linux-gnueabihf- stuart_build -c Platforms/Samsung/J250FPkg/PlatformBuild.py TOOL_CHAIN_TAG=CLANG38 TARGET=DEBUG RAM_SIZE=1.5 FD_BASE=0x99000000 FD_SIZE=0x00300000 FD_BLOCKS=0x300 TARGET_ARCH=ARM

The latest full ARM32 DEBUG build succeeded. Host regression tests were not
rerun for the MCLK edit. Earlier simulated checks cover the pad correction and
other transport paths; simulated responses cannot prove physical eMMC
communication. No successful J250F storage recognition is claimed.
