#!/usr/bin/env python3
"""List or extract Source VPK entries without modifying the game install."""

from __future__ import annotations

import argparse
import dataclasses
import pathlib
import struct
import sys


VPK_SIGNATURE = 0x55AA1234
DIR_ARCHIVE_INDEX = 0x7FFF


@dataclasses.dataclass(frozen=True)
class Entry:
    path: str
    crc32: int
    preload: bytes
    archive_index: int
    offset: int
    length: int


def read_cstring(stream) -> str:
    data = bytearray()
    while True:
        byte = stream.read(1)
        if not byte:
            raise EOFError("unexpected end of VPK directory tree")
        if byte == b"\0":
            return data.decode("utf-8", errors="replace")
        data.extend(byte)


def parse_directory(directory_path: pathlib.Path) -> tuple[int, int, list[Entry]]:
    with directory_path.open("rb") as stream:
        signature, version, tree_size = struct.unpack("<III", stream.read(12))
        if signature != VPK_SIGNATURE:
            raise ValueError(f"unexpected VPK signature 0x{signature:08X}")
        if version == 1:
            header_size = 12
        elif version == 2:
            stream.read(16)
            header_size = 28
        else:
            raise ValueError(f"unsupported VPK version {version}")

        entries: list[Entry] = []
        while True:
            extension = read_cstring(stream)
            if not extension:
                break
            while True:
                directory = read_cstring(stream)
                if not directory:
                    break
                while True:
                    filename = read_cstring(stream)
                    if not filename:
                        break
                    raw = stream.read(18)
                    if len(raw) != 18:
                        raise EOFError("truncated VPK directory entry")
                    crc32, preload_size, archive_index, offset, length, terminator = (
                        struct.unpack("<IHHIIH", raw)
                    )
                    if terminator != 0xFFFF:
                        raise ValueError(
                            f"invalid entry terminator 0x{terminator:04X} for {filename}"
                        )
                    preload = stream.read(preload_size)
                    normalized_directory = "" if directory == " " else directory
                    normalized_extension = "" if extension == " " else f".{extension}"
                    path = "/".join(
                        part for part in (normalized_directory, filename) if part
                    ) + normalized_extension
                    entries.append(
                        Entry(path, crc32, preload, archive_index, offset, length)
                    )
        return header_size, tree_size, entries


def archive_path_for(directory_path: pathlib.Path, archive_index: int) -> pathlib.Path:
    name = directory_path.name
    if not name.endswith("_dir.vpk"):
        raise ValueError("directory VPK name must end in _dir.vpk")
    return directory_path.with_name(f"{name[:-8]}_{archive_index:03d}.vpk")


def extract_entry(
    directory_path: pathlib.Path,
    header_size: int,
    tree_size: int,
    entry: Entry,
) -> bytes:
    if entry.archive_index == DIR_ARCHIVE_INDEX:
        archive_path = directory_path
        absolute_offset = header_size + tree_size + entry.offset
    else:
        archive_path = archive_path_for(directory_path, entry.archive_index)
        absolute_offset = entry.offset
    with archive_path.open("rb") as stream:
        stream.seek(absolute_offset)
        payload = stream.read(entry.length)
    if len(payload) != entry.length:
        raise EOFError(
            f"truncated payload for {entry.path}: expected {entry.length}, got {len(payload)}"
        )
    return entry.preload + payload


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("directory_vpk", type=pathlib.Path)
    parser.add_argument("--match", default="")
    parser.add_argument("--extract")
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()

    header_size, tree_size, entries = parse_directory(args.directory_vpk)
    if args.extract:
        target = args.extract.replace("\\", "/").casefold()
        entry = next((item for item in entries if item.path.casefold() == target), None)
        if entry is None:
            print(f"entry not found: {args.extract}", file=sys.stderr)
            return 2
        if args.output is None:
            print("--output is required with --extract", file=sys.stderr)
            return 2
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(
            extract_entry(args.directory_vpk, header_size, tree_size, entry)
        )
        print(
            f"extracted {entry.path} archive={entry.archive_index} "
            f"offset={entry.offset} length={entry.length} preload={len(entry.preload)} "
            f"to {args.output}"
        )
        return 0

    needle = args.match.casefold()
    for entry in entries:
        if not needle or needle in entry.path.casefold():
            print(
                f"{entry.path}\tarchive={entry.archive_index}\toffset={entry.offset}"
                f"\tlength={entry.length}\tpreload={len(entry.preload)}"
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
