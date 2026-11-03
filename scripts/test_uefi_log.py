#!/usr/bin/env python3
"""Host regressions and ARM object checks; not a firmware build or phone test."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PKG = Path("Platforms/Samsung/J250fPkg")
OUT = Path(os.environ.get("LOCALAPPDATA", tempfile.gettempdir())) / "Temp/opencode/j250f-log-tests"


def run(args):
    result = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, timeout=120)
    if result.stdout:
        print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    if result.returncode:
        raise RuntimeError(f"exit {result.returncode}: {' '.join(map(str, args))}")


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    cc = shutil.which("clang")
    if not cc:
        raise RuntimeError("clang is required")
    run([sys.executable, "-m", "unittest", "discover", "-s", "tests", "-p", "test_prepare_uefi_log.py", "-v"])
    common = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter"]
    includes = ["-IMu_Basecore/MdePkg/Include", "-ISilicon/Qualcomm/QcomPkg/Include",
                f"-I{PKG}/J250fEmmcDxe/Include", f"-I{PKG}/J250fGopDxe",
                "-ISilicon/Arm/Mu_Tiano/ArmPkg/Include"]
    ext = ".exe" if os.name == "nt" else ""
    for name, sources in [
        ("writer", ["tests/j250f_log_write_test.c", f"{PKG}/J250fEmmcDxe/Core/j250f_emmc.c"]),
        ("storage", ["tests/j250f_log_storage_test.c"]),
        ("text", ["tests/j250f_text_log_test.c"]),
    ]:
        output = OUT / (name + ext)
        run([cc, *common, *includes, "-IMu_Basecore/MdePkg/Include/X64", "-DMDEPKG_NDEBUG",
             "-O1", "-fshort-wchar", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
             *sources, "-o", str(output)])
        if name == "text":
            fixture = OUT / "firmware-log-fixture.bin"
            extracted = OUT / "firmware-log-fixture.txt"
            run([str(output), str(fixture)])
            extracted.unlink(missing_ok=True)  # only this runner's previous output
            run([sys.executable, "scripts/prepare_uefi_log.py", "extract", str(fixture), str(extracted)])
            if not extracted.read_bytes().endswith("A€😀  \n".encode("utf-8")):
                raise RuntimeError("C/Python record-format round trip changed console bytes")
            print("C firmware -> Python extractor byte-exact round trip: PASS")
        else:
            run([str(output)])
    arm = [cc, *common, *includes, "-IMu_Basecore/MdePkg/Include/Arm", "--target=armv7a-none-eabi",
           "-mthumb", "-ffreestanding", "-fshort-wchar", "-DMDEPKG_NDEBUG"]
    for name in ["J250fWpPrivate.c", "J250fTextLogDxe.c", "J250fLkBackend.c", "J250fLkEmmcDxe.c", "Core/j250f_emmc.c"]:
        run([*arm, "-c", str(PKG / "J250fEmmcDxe" / name), "-o", str(OUT / (Path(name).stem + "-arm.o"))])
        print(f"ARM object compilation: {name}: PASS")
    pcd = {"PcdMipiFrameBufferWidth": 540, "PcdMipiFrameBufferHeight": 960,
           "PcdMipiFrameBufferPixelBpp": 24, "PcdMipiFrameBufferPixelsPerScanLine": 540,
           "PcdMipiFrameBufferDelay": 0, "PcdMipiFrameBufferAddress": "0xA800035AULL"}
    defines = [f"-D_PCD_VALUE_{key}={value}" for key, value in pcd.items()]
    source = "Silicon/Qualcomm/QcomPkg/Library/FrameBufferSerialPortLib/FrameBufferSerialPortLib.c"
    for dxe in [False, True]:
        run([*arm, "-fsyntax-only", "-Wno-sign-compare", *defines, *(["-DJ250F_TEXT_LOG_DXE=1"] if dxe else []), source])
        print(f"ARM syntax: serial capture ({'DXE' if dxe else 'SEC'}): PASS")
    print("UEFI log checks passed. Hardware writes, full firmware build and boot remain unverified.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        sys.exit(str(error))
