#!/usr/bin/env python3
"""Collect a deep, read-only J250F UEFI diagnostic snapshot over ADB.

Run: python3 scripts/j250f_adb_inspect.py --serial J250F-UEFI
Requires current DEBUG firmware with "dbg help". No device writes.
"""
import argparse
import datetime as dt
import pathlib
import re
import subprocess
import sys


def adb_shell(adb: str, serial: str, command: str, timeout: int = 25) -> str:
    try:
        p = subprocess.run(
            [adb, "-s", serial, "shell", command],
            text=True, encoding="utf-8", errors="replace",
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=timeout, check=False,
        )
    except subprocess.TimeoutExpired:
        return "(timeout - device may be busy or MMIO inaccessible)\n"
    except OSError as exc:
        return f"(failed to execute ADB: {exc})\n"
    return (p.stdout or "") + (f"\n[stderr: {p.stderr.strip()}]\n" if p.stderr.strip() else "")


def snapshot(adb: str, serial: str, outfile: pathlib.Path,
             max_handles: int, max_vars: int) -> None:
    lines = [
        f"J250F ADB deep firmware snapshot UTC {dt.datetime.now(dt.timezone.utc).isoformat()}",
        f"Serial: {serial}",
        "Read-only diagnostic snapshot; no MMIO or eMMC writes.",
        "",
    ]

    def collect(command: str) -> str:
        output = adb_shell(adb, serial, command)
        lines.extend([f"===== {command} =====", output, ""])
        print(f"{command}: {len(output)} chars", file=sys.stderr)
        return output

    for cmd in (
        "help", "dbg help", "dbg system", "gop status",
        "dbg disks", "emmc status", "emmc capabilities",
        "emmc clock", "emmc snapshot", "emmc trace",
    ):
        collect(cmd)

    # Enumerate all UEFI handles, with each handle's dynamic protocol GUIDs.
    first = collect("dbg handles 0")
    match = re.search(r"UEFI handles:\s*(\d+) total", first)
    count = min(int(match.group(1)), max_handles) if match else 0
    for page in range(8, count, 8):
        collect(f"dbg handles {page}")
    if count:
        for handle in range(count):
            first_proto = collect(f"dbg protocols {handle} 0")
            pm = re.search(r"ProtocolsPerHandle=.*?count=(\d+)", first_proto)
            protocol_count = min(int(pm.group(1)), 512) if pm else 0
            for page in range(8, protocol_count, 8):
                collect(f"dbg protocols {handle} {page}")

    for subject, limit in (("tables", 256), ("memory", 256),
                           ("images", 256), ("vars", max_vars)):
        for page in range(0, limit, 8):
            output = collect(f"dbg {subject} {page}")
            # EFI variable enumeration may simply stop at end of database.
            if subject == "vars" and (
                "Not Found" in output or "No entries on this page" in output
            ):
                break
            if subject == "tables":
                m = re.search(r"EFI config tables count=(\d+)", output)
                if m and page + 8 >= int(m.group(1)):
                    break
            if subject == "memory":
                m = re.search(r"UEFI memory map:\s*(\d+) records", output)
                if m and page + 8 >= int(m.group(1)):
                    break
            if subject == "images":
                m = re.search(r"LoadedImages=.*?count=(\d+)", output)
                if m and page + 8 >= int(m.group(1)):
                    break

    outfile.parent.mkdir(parents=True, exist_ok=True)
    outfile.write_text("\n".join(lines), encoding="utf-8")
    print(f"Saved: {outfile}", file=sys.stderr)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", default="J250F-UEFI")
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--out", type=pathlib.Path, default=None)
    parser.add_argument("--max-handles", type=int, default=256)
    parser.add_argument("--max-vars", type=int, default=128)
    args = parser.parse_args()
    now = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    out = args.out or pathlib.Path(f"j250f-diagnostic-{now}.txt")
    snapshot(args.adb, args.serial, out,
             max(0, min(args.max_handles, 1024)),
             max(0, min(args.max_vars, 256)))


if __name__ == "__main__":
    main()
