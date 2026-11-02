#!/usr/bin/env bash
set -euo pipefail

mode=${1:?Usage: build-j250f.sh DEBUG|RELEASE version}
version=${2:?A release version is required}
[[ "$mode" == DEBUG || "$mode" == RELEASE ]] || exit 1
[[ "$version" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]] || exit 1
cd "$(dirname "$0")/../.."

# Only production firmware dependencies: an unused upstream unit-test submodule
# references a deleted repository, so do not recurse through every test library.
git -C Mu_Basecore submodule update --init -- \
  CryptoPkg/Library/OpensslLib/openssl \
  MdeModulePkg/Universal/RegularExpressionDxe/oniguruma \
  MdeModulePkg/Library/BrotliCustomDecompressLib/brotli \
  BaseTools/Source/C/BrotliCompress/brotli
git -C Silicon/Arm/Mu_Tiano submodule update --init

# Fail on dependency drift instead of silently ignoring a required patch.
apply_patch_checked() {
  local repo=$1 patch=$2
  if git -C "$repo" apply --reverse --check "$patch" 2>/dev/null; then
    return
  fi
  git -C "$repo" apply --check "$patch"
  git -C "$repo" apply "$patch"
}
apply_patch_checked Mu_Basecore ../MuPatches/Compile.patch
apply_patch_checked Mu_Basecore ../MuPatches/GenFw_ARM32.patch
apply_patch_checked Mu_Basecore ../MuPatches/GraphicsConsole_NarrowScreen.patch
apply_patch_checked Common/Mu_Tiano_Plus ../../MuPatches/J250F_ADB_RAM_Execution.patch

build_firmware() {
  make -C BootShim/ARM UEFI_BASE=0x99000000 UEFI_SIZE=0x00300000
  # Submodules are prepared above; stuart_setup would recurse into unused tests.
  stuart_update -c Platforms/Samsung/J250FPkg/PlatformBuild.py TOOL_CHAIN_TAG=CLANG38
  # Stuart downloads precompiled BaseTools. Rebuild GenFw so our ARM relocation
  # patch is used by the binary that actually converts the linked firmware.
  make -C Mu_Basecore/BaseTools/Source/C/Common
  make -C Mu_Basecore/BaseTools/Source/C/GenFw
  cp Mu_Basecore/BaseTools/Source/C/bin/GenFw \
    Mu_Basecore/BaseTools/Bin/Mu-Basetools_extdep/Linux-x86/GenFw
  stuart_build -c Platforms/Samsung/J250FPkg/PlatformBuild.py \
    TOOL_CHAIN_TAG=CLANG38 "TARGET=$mode" RAM_SIZE=1.5 \
    FD_BASE=0x99000000 FD_SIZE=0x00300000 FD_BLOCKS=0x300 TARGET_ARCH=ARM
  local payload="Build/J250FPkg-ARM/${mode}_CLANG38/FV/J250F_UEFI.fd-bootshim"
  cat BootShim/ARM/BootShim.bin "Build/J250FPkg-ARM/${mode}_CLANG38/FV/J250F_UEFI.fd" > "$payload"
  python ImageResources/make_j250f_bootimg.py "$payload" ImageResources/DTBs/J250F.qcdt Mu-J250F.img
}
build_firmware 2>&1 | tee j250f-build.log
mkdir -p dist
python scripts/ci/validate-j250f-image.py Mu-J250F.img "$mode"
image="Mu-J250F-${version}-${mode}.img"
cp Mu-J250F.img "dist/$image"
(cd dist && sha256sum "$image" > "$image.sha256")
