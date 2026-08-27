"""Locate MSVC x64 RTTI complete-object locators and their vtables in a PE image."""

from __future__ import annotations

import argparse
from pathlib import Path
import struct


def u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def u64(data: bytes, offset: int) -> int:
    return struct.unpack_from("<Q", data, offset)[0]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    parser.add_argument("type_name", help="Exact MSVC RTTI name, e.g. .?AVCSceneObject@@")
    parser.add_argument("--slots", type=int, default=64)
    args = parser.parse_args()

    data = args.image.read_bytes()
    pe = u32(data, 0x3C)
    if data[pe:pe + 4] != b"PE\0\0":
        raise SystemExit("not a PE image")
    section_count = u16(data, pe + 6)
    optional_size = u16(data, pe + 20)
    optional = pe + 24
    if u16(data, optional) != 0x20B:
        raise SystemExit("only PE32+ is supported")
    image_base = u64(data, optional + 24)
    section_table = optional + optional_size
    sections: list[tuple[str, int, int, int, int]] = []
    for index in range(section_count):
        entry = section_table + index * 40
        name = data[entry:entry + 8].split(b"\0", 1)[0].decode("ascii", "replace")
        virtual_size = u32(data, entry + 8)
        virtual_address = u32(data, entry + 12)
        raw_size = u32(data, entry + 16)
        raw_address = u32(data, entry + 20)
        sections.append((name, virtual_address, virtual_size, raw_address, raw_size))

    def file_to_rva(offset: int) -> int | None:
        for _, va, _, raw, raw_size in sections:
            if raw <= offset < raw + raw_size:
                return va + offset - raw
        return None

    def rva_to_file(rva: int) -> int | None:
        for _, va, virtual_size, raw, raw_size in sections:
            if va <= rva < va + max(virtual_size, raw_size):
                delta = rva - va
                if delta < raw_size:
                    return raw + delta
        return None

    def executable_rva(rva: int) -> bool:
        for name, va, virtual_size, _, raw_size in sections:
            if name == ".text" and va <= rva < va + max(virtual_size, raw_size):
                return True
        return False

    needle = args.type_name.encode("ascii") + b"\0"
    start = 0
    found_any = False
    while True:
        name_file = data.find(needle, start)
        if name_file < 0:
            break
        start = name_file + 1
        type_file = name_file - 16
        type_rva = file_to_rva(type_file)
        if type_rva is None:
            continue
        print(f"type name file=0x{name_file:X} type_descriptor_rva=0x{type_rva:X}")
        type_ref = struct.pack("<I", type_rva)
        cursor = 0
        while True:
            ref_file = data.find(type_ref, cursor)
            if ref_file < 0:
                break
            cursor = ref_file + 1
            col_file = ref_file - 12
            col_rva = file_to_rva(col_file) if col_file >= 0 else None
            if col_rva is None or col_file + 24 > len(data):
                continue
            signature, offset, cd_offset, td, hierarchy, self_rva = struct.unpack_from(
                "<IIIIII", data, col_file
            )
            if signature != 1 or td != type_rva or self_rva != col_rva:
                continue
            found_any = True
            print(
                f"  COL rva=0x{col_rva:X} offset=0x{offset:X} cd=0x{cd_offset:X} "
                f"hierarchy=0x{hierarchy:X}"
            )
            locator_va = image_base + col_rva
            locator_ref = struct.pack("<Q", locator_va)
            vcursor = 0
            while True:
                locator_file = data.find(locator_ref, vcursor)
                if locator_file < 0:
                    break
                vcursor = locator_file + 1
                vtable_file = locator_file + 8
                vtable_rva = file_to_rva(vtable_file)
                if vtable_rva is None:
                    continue
                slots: list[int] = []
                for slot in range(args.slots):
                    entry_file = vtable_file + slot * 8
                    if entry_file + 8 > len(data):
                        break
                    target_va = u64(data, entry_file)
                    if target_va < image_base:
                        break
                    target_rva = target_va - image_base
                    if not executable_rva(target_rva):
                        break
                    slots.append(target_rva)
                print(
                    f"    vtable rva=0x{vtable_rva:X} slots={len(slots)} "
                    + " ".join(f"[{i}]=0x{target:X}" for i, target in enumerate(slots))
                )
    if not found_any:
        print("no complete-object locator found")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
