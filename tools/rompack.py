#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Pack the reset code and ROM modules into a 4 MiB OpenPS2BIOS image."""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path


ROM_SIZE = 4 * 1024 * 1024
ALIGNMENT = 16
ROMVER = b"0003AD20261021"
ENTRY = struct.Struct("<10sHI")
RESERVED_NAMES = {"RESET", "ROMDIR", "EXTINFO", "ROMVER"}
REGION_OFFSET = 0x7FF52


def _align(value: int) -> int:
    return (value + ALIGNMENT - 1) & ~(ALIGNMENT - 1)


def _validate_name(name: str) -> bytes:
    try:
        encoded = name.encode("ascii")
    except UnicodeEncodeError as exc:
        raise ValueError(f"file name is not ASCII: {name!r}") from exc
    if not 1 <= len(encoded) <= 9 or b"\0" in encoded:
        raise ValueError(f"file names must be 1 to 9 non-NUL ASCII bytes: {name!r}")
    return encoded


def _directory_entry(name: str, size: int) -> bytes:
    encoded = _validate_name(name)
    if not 0 <= size <= 0xFFFFFFFF:
        raise ValueError(f"file is too large for ROMDIR: {name}")
    return ENTRY.pack(encoded + b"\0" * (10 - len(encoded)), 0, size)


def build_rom(reset: bytes, modules: list[tuple[str, bytes]], romver: bytes = ROMVER) -> bytes:
    """Return a 4 MiB ROM image; the unused area is erased (0xFF)."""
    if not reset:
        raise ValueError("RESET must not be empty")
    if len(romver) != 14:
        raise ValueError("ROMVER must be exactly 14 bytes")

    reset_data = reset + b"\xff" * (_align(len(reset)) - len(reset))
    files: list[tuple[str, bytes]] = [("RESET", reset_data), ("EXTINFO", b""), ("ROMVER", romver)]
    seen = set(RESERVED_NAMES)
    for name, data in modules:
        _validate_name(name)
        if name in seen:
            raise ValueError(f"duplicate or reserved file name: {name}")
        seen.add(name)
        files.append((name, data))

    # ROMDIR is the second file. Its size includes one all-zero terminator.
    records = [_directory_entry("RESET", len(reset_data)),
               _directory_entry("ROMDIR", (len(files) + 2) * ENTRY.size)]
    records.extend(_directory_entry(name, len(data)) for name, data in files[1:])
    directory = b"".join(records) + bytes(ENTRY.size)

    rom = bytearray(b"\xff" * ROM_SIZE)
    rom[:len(reset_data)] = reset_data
    directory_end = len(reset_data) + len(directory)
    rom[len(reset_data):directory_end] = directory

    cursor = _align(directory_end)
    for name, data in files[1:]:
        end = cursor + len(data)
        if end > ROM_SIZE:
            raise ValueError(f"ROM contents exceed the 4 MiB image at {name}")
        # Programs read the console region from this fixed ROM byte; it
        # repeats ROMVER's region letter, so no file may cover it.
        if cursor <= REGION_OFFSET < end:
            raise ValueError(f"{name} overlaps the region byte at {REGION_OFFSET:#x}")
        rom[cursor:end] = data
        cursor = _align(end)
    rom[REGION_OFFSET] = romver[4]
    return bytes(rom)


def _module_argument(value: str) -> tuple[str, Path]:
    name, sep, path = value.partition("=")
    if not sep or not path:
        raise argparse.ArgumentTypeError("module must be NAME=PATH")
    try:
        _validate_name(name)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc
    return name, Path(path)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reset", required=True, type=Path)
    parser.add_argument("--module", action="append", default=[], type=_module_argument,
                        metavar="NAME=PATH")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        modules = [(name, path.read_bytes()) for name, path in args.module]
        args.output.write_bytes(build_rom(args.reset.read_bytes(), modules))
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    return 0


if __name__ == "__main__":
    sys.exit(main())
