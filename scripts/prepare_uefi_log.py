#!/usr/bin/env python3
"""Create and inspect the local UEFI log template (no device operations)."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

SECTOR_SIZE = 512
SECTOR_COUNT = 2048
TEMPLATE_SIZE = SECTOR_SIZE * SECTOR_COUNT
HEADER_SIZE = 32
PAYLOAD_SIZE = SECTOR_SIZE - HEADER_SIZE
HEADER_PREFIX = b"J250FLOG2 "
STATE_OFFSET = 19
READY = b"READY"
USED = b"USED "


class TemplateError(ValueError):
    """The template does not match the defined on-disk format."""


def crc16(data: bytes, value: int = 0xffff) -> int:
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = (value >> 1) ^ (0xa001 if value & 1 else 0)
    return value


def sector_header(index: int, state: bytes = READY, length: int = 0,
                  payload: bytes = b"") -> bytes:
    """V2 marker: prefix, CRC16+index, state, exact length, LF."""
    if not 0 <= index < SECTOR_COUNT:
        raise ValueError(f"sector index out of range: {index}")
    if state not in (READY, USED):
        raise ValueError("state must be READY or USED-space")
    if state == READY and length != 0 or state == USED and not 1 <= length <= PAYLOAD_SIZE:
        raise ValueError("READY length must be zero; USED length must be 1..480")
    if state == USED and len(payload) != length:
        raise ValueError("USED records require the exact payload for their checksum")
    marker = HEADER_PREFIX + f"0000{index:04x} ".encode("ascii") + state
    suffix = b" " * 7 if state == READY else f" {length:06x}".encode("ascii")
    header = marker + suffix + b"\n"
    if state == USED:
        checksum = crc16(header[14:] + payload.ljust(PAYLOAD_SIZE, b" "))
        header = header[:10] + f"{checksum:04x}".encode("ascii") + header[14:]
    return header


def create_template(path: Path) -> None:
    """Write an exactly 1 MiB template, refusing to replace an existing path."""
    path = Path(path)
    # Exclusive creation prevents accidental overwrite and symlink following.
    with path.open("xb") as output:
        for index in range(SECTOR_COUNT):
            output.write(sector_header(index))
            output.write(b" " * PAYLOAD_SIZE)


def record_payload(index: int, sector: bytes) -> bytes | None:
    header = sector[:HEADER_SIZE]
    if header == sector_header(index, READY):
        if sector[HEADER_SIZE:] != b" " * PAYLOAD_SIZE:
            raise TemplateError(f"nonblank READY payload in sector {index}")
        return None
    try:
        length = int(header[25:31], 16)
        payload = sector[HEADER_SIZE:HEADER_SIZE+length]
        expected = sector_header(index, USED, length, payload)
    except ValueError as error:
        raise TemplateError(f"invalid marker in sector {index}") from error
    if header != expected or sector[HEADER_SIZE+length:] != b" " * (PAYLOAD_SIZE-length):
        raise TemplateError(f"invalid marker/checksum in sector {index}")
    return payload


def extract_used(template_path: Path, output_path: Path, *, salvage: bool = False) -> int:
    """Validate a template and extract USED payloads without losing whitespace."""
    template_path = Path(template_path)
    output_path = Path(output_path)
    data = template_path.read_bytes()
    if len(data) != TEMPLATE_SIZE:
        raise TemplateError(f"expected {TEMPLATE_SIZE} bytes, found {len(data)}")

    extracted: list[bytes] = []
    ready_seen = False
    for index in range(SECTOR_COUNT):
        start = index * SECTOR_SIZE
        sector = data[start : start + SECTOR_SIZE]
        try:
            payload = record_payload(index, sector)
            if payload is None:
                ready_seen = True
                continue
            if ready_seen:
                raise TemplateError(f"USED record after READY in sector {index}")
        except TemplateError as error:
            if not salvage:
                raise
            print(f"warning: {error}; salvaging only the verified prefix", file=sys.stderr)
            break
        extracted.append(payload)

    # Validate the complete input before creating output; also refuse overwrites.
    with output_path.open("xb") as output:
        for payload in extracted:
            output.write(payload)
    return len(extracted)


EPILOG = """Android installation note (manual, only when the full firmware is ready):
Use recovery to install this template as /data/wp81/uefi.log. Fully write the
template (do not use fallocate or truncate), then sync and cleanly unmount before
starting the firmware. The firmware requires a single-link 1 MiB regular file,
at most four initialized inline-root extents, clean 4 KiB ext4, incompat=0x42,
and ro_compat restricted to 0x13; unsupported layouts disable logging. Do not
format userdata to meet these restrictions. See
Platforms/Samsung/J250fPkg/Documentation/PERSISTENT_TEXT_LOG.md.
This CLI only operates on local files; it never uploads firmware or writes to a
device."""


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, epilog=EPILOG,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    subparsers = parser.add_subparsers(dest="command", required=True)
    create_parser = subparsers.add_parser("create", help="create a local 1 MiB template")
    create_parser.add_argument("template", type=Path)
    extract_parser = subparsers.add_parser("extract", help="validate and extract USED records")
    extract_parser.add_argument("template", type=Path)
    extract_parser.add_argument("output", type=Path)
    extract_parser.add_argument("--salvage", action="store_true",
                                help="recover only the checksum-verified prefix before a torn record")
    args = parser.parse_args(argv)

    try:
        if args.command == "create":
            create_template(args.template)
            print(f"Created {args.template} ({TEMPLATE_SIZE} bytes)")
        else:
            count = extract_used(args.template, args.output, salvage=args.salvage)
            print(f"Extracted {count} USED sector(s) to {args.output}")
    except (OSError, TemplateError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
