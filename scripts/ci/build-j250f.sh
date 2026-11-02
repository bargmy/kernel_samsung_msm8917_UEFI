#!/usr/bin/env bash
set -euo pipefail

mode=${1:?Usage: build-j250f.sh DEBUG|RELEASE version}
version=${2:?A release version is required}
[[ "$mode" == DEBUG || "$mode" == RELEASE ]] || exit 1
[[ "$version" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]] || exit 1
cd "$(dirname "$0")/../.."

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

bash build_uefi.sh -d J250F -a ARM -r "$mode" -t CLANG38 2>&1 | tee j250f-build.log
mkdir -p dist
python scripts/ci/validate-j250f-image.py Mu-J250F.img "$mode"
image="Mu-J250F-${version}-${mode}.img"
cp Mu-J250F.img "dist/$image"
(cd dist && sha256sum "$image" > "$image.sha256")
