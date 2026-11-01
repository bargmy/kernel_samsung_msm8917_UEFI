#!/bin/bash

# configs/J250F.sh - Device-specific packaging script for Samsung Galaxy J2 Pro 2018 (SM-J250F / j2y18lte)

UEFI_FD="./Build/J250FPkg-${_TARGET_ARCH_TYPE}/${_TARGET_BUILD_MODE}_${TOOL_CHAIN_TAG}/FV/J250F_UEFI.fd"
BOOTSHIM="./BootShim/${_TARGET_ARCH_TYPE}/BootShim.bin"
PAYLOAD="./Build/J250FPkg-${_TARGET_ARCH_TYPE}/${_TARGET_BUILD_MODE}_${TOOL_CHAIN_TAG}/FV/J250F_UEFI.fd-bootshim"
QCDT="./ImageResources/DTBs/J250F.qcdt"
PACKAGER="./ImageResources/make_j250f_bootimg.py"
OUTPUT_IMG="Mu-J250F.img"

if [ ! -f "${UEFI_FD}" ]; then
    echo "ERROR: UEFI FD not found at ${UEFI_FD}"
    exit 1
fi

if [ ! -f "${BOOTSHIM}" ]; then
    echo "ERROR: BootShim binary not found at ${BOOTSHIM}"
    exit 1
fi

echo "Concatenating BootShim + UEFI FD into kernel payload..."
cat "${BOOTSHIM}" "${UEFI_FD}" > "${PAYLOAD}" || exit 1

echo "Packaging Android boot.img using Samsung J250F verified parameters..."
python3 "${PACKAGER}" \
    "${PAYLOAD}" \
    "${QCDT}" \
    "${OUTPUT_IMG}" || exit 1

if [ -d "/workspaces/web-k/j250f-uefi/build_output" ]; then
    cp -f "${OUTPUT_IMG}" /workspaces/web-k/j250f-uefi/build_output/
    sha256sum "${OUTPUT_IMG}" > /workspaces/web-k/j250f-uefi/build_output/Mu-J250F.img.sha256
fi
echo "Saved build artifact to ${OUTPUT_IMG}"
