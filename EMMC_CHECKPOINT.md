# J250F Project Mu eMMC development checkpoint

Current device result: CMD0/CMD1 complete; CMD2 fails command CRC with
IRQ=0x00028000, R1/OCR=0xC0FF8080, pre-reset state=0x01F80001.
The low-speed DLL change did not resolve it. No valid CID/capacity or Block I/O
publication has been demonstrated. CRC checking remains enabled.

Active modules:
- Platforms/Samsung/J250fPkg/PlatformIntegration/J250fPowerDxe.inf
- Platforms/Samsung/J250fPkg/J250fEmmcDxe/J250fLkEmmcDxe.inf

Includes RPM-SMD power requests, SMEM target-info mapping, source-backed GCC/TLMM,
one-bit 400kHz identification/read-only I/O, and 90-second status display.
Previous diagnostics/driver attempts are retained as source references but are
not active platform components.

## Reproducing the local build

The Mu_Basecore submodule remains pinned to `022c680837e9bdd70ef221c78a8c985bc1e730a9`.
Its local modifications are saved in MuPatches/Local_Basecore_Checkpoint.patch.
On a clean checkout of that submodule, apply the combined patch once:

    git -C Mu_Basecore apply ../MuPatches/Local_Basecore_Checkpoint.patch

The patch includes the ARM toolchain, GenFw relocation and narrow-screen console
changes. Do not apply the older individual patches first. Other configured
submodules/toolchain dependencies must be present as in the existing build setup.

From Mu-Qcom:

    CLANG38_BIN=/usr/bin/ CLANG38_ARM_PREFIX=arm-linux-gnueabihf- GCC5_ARM_PREFIX=arm-linux-gnueabihf- stuart_build -c Platforms/Samsung/J250FPkg/PlatformBuild.py TOOL_CHAIN_TAG=CLANG38 TARGET=DEBUG RAM_SIZE=1.5 FD_BASE=0x99000000 FD_SIZE=0x00300000 FD_BLOCKS=0x300 TARGET_ARCH=ARM

From Platforms/Samsung/J250fPkg, run `make test arm syntax sanitizer`.
Those checks passed locally; their hardware responses are simulated. The actual
ARM EFI modules and Android image packaging were also verified.

The corresponding image SHA-256 is `5014b606a510409d8d873492715e131998a3f2fb332d925b76d453e5d6e2a4f1`.
Boot images and source input archives are preserved as development release
assets rather than committed as firmware binaries in the source tree.
