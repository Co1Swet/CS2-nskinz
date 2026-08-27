#!/usr/bin/env python3
"""Find model material groups whose records reference one resource instance.

The scanner is read-only.  It expands a resource instance to all of its strong
handle wrappers, then validates the native Source 2 chain used by CS2:

    material group -> layout -> 0x118-byte records -> handle at +0x110

Only candidates that can be traced back through that structure are reported.
"""

from __future__ import annotations

import argparse
import ctypes
import struct
import sys
from ctypes import wintypes

import numpy as np


PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
MEM_COMMIT = 0x1000
PAGE_GUARD = 0x100
PAGE_NOACCESS = 0x01
PAGE_READABLE_MASK = 0x02 | 0x04 | 0x08 | 0x20 | 0x40 | 0x80
PAGE_EXECUTABLE_MASK = 0x10 | 0x20 | 0x40 | 0x80
RESOURCE_HANDLE_STATE_MASK = 0xFFFF0000FFFF0000
RESOURCE_HANDLE_STATE_VALUE = 0xFFFF0000FFFF0000
RECORD_STRIDE = 0x118
RECORD_HANDLE_OFFSET = 0x110


class MEMORY_BASIC_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("BaseAddress", ctypes.c_void_p),
        ("AllocationBase", ctypes.c_void_p),
        ("AllocationProtect", wintypes.DWORD),
        ("PartitionId", wintypes.WORD),
        ("RegionSize", ctypes.c_size_t),
        ("State", wintypes.DWORD),
        ("Protect", wintypes.DWORD),
        ("Type", wintypes.DWORD),
    ]


def parse_int(value: str) -> int:
    return int(value, 0)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--instance", type=parse_int, required=True)
    parser.add_argument("--max-materials", type=int, default=32)
    parser.add_argument("--chunk-mib", type=int, default=4)
    args = parser.parse_args()

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.VirtualQueryEx.argtypes = [
        wintypes.HANDLE,
        ctypes.c_void_p,
        ctypes.POINTER(MEMORY_BASIC_INFORMATION),
        ctypes.c_size_t,
    ]
    kernel32.VirtualQueryEx.restype = ctypes.c_size_t
    kernel32.ReadProcessMemory.argtypes = [
        wintypes.HANDLE,
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_size_t),
    ]
    kernel32.ReadProcessMemory.restype = wintypes.BOOL

    process = kernel32.OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, args.pid
    )
    if not process:
        print(f"OpenProcess failed: winerror={ctypes.get_last_error()}", file=sys.stderr)
        return 2

    chunk_size = max(64 * 1024, args.chunk_mib * 1024 * 1024)
    maximum_user_address = 0x00007FFFFFFFFFFF
    max_materials = max(1, min(args.max_materials, 128))

    def query(address: int) -> MEMORY_BASIC_INFORMATION | None:
        mbi = MEMORY_BASIC_INFORMATION()
        if not kernel32.VirtualQueryEx(
            process,
            ctypes.c_void_p(address),
            ctypes.byref(mbi),
            ctypes.sizeof(mbi),
        ):
            return None
        return mbi

    def readable_regions():
        address = 0
        while address < maximum_user_address:
            mbi = query(address)
            if mbi is None:
                break
            base = int(mbi.BaseAddress or 0)
            size = int(mbi.RegionSize)
            readable = (
                mbi.State == MEM_COMMIT
                and (mbi.Protect & PAGE_GUARD) == 0
                and (mbi.Protect & PAGE_NOACCESS) == 0
                and (mbi.Protect & PAGE_READABLE_MASK) != 0
            )
            if readable and size > 0:
                yield base, size
            address = base + max(size, 0x1000)

    def read(address: int, size: int) -> bytes:
        buffer = ctypes.create_string_buffer(size)
        bytes_read = ctypes.c_size_t(0)
        ok = kernel32.ReadProcessMemory(
            process,
            ctypes.c_void_p(address),
            buffer,
            size,
            ctypes.byref(bytes_read),
        )
        return buffer.raw[: bytes_read.value] if ok or bytes_read.value else b""

    def read_qword(address: int) -> int | None:
        data = read(address, 8)
        return struct.unpack("<Q", data)[0] if len(data) == 8 else None

    def executable(address: int) -> bool:
        mbi = query(address)
        return bool(
            mbi is not None
            and mbi.State == MEM_COMMIT
            and (mbi.Protect & PAGE_GUARD) == 0
            and (mbi.Protect & PAGE_EXECUTABLE_MASK) != 0
        )

    def scan_values(targets: set[int]):
        if not targets:
            return
        target_array = np.array(sorted(targets), dtype="<u8")
        for region_base, region_size in readable_regions():
            for offset in range(0, region_size, chunk_size):
                data = read(region_base + offset, min(chunk_size, region_size - offset))
                aligned_size = len(data) - len(data) % 8
                if aligned_size == 0:
                    continue
                values = np.frombuffer(data[:aligned_size], dtype="<u8")
                for index in np.flatnonzero(np.isin(values, target_array)):
                    yield (
                        region_base + offset + int(index) * 8,
                        int(values[index]),
                    )

    try:
        # Pass 1: expand the resource instance to valid strong-handle wrappers.
        candidate_handles: set[int] = set()
        for match, _ in scan_values({args.instance}):
            header = read(match, 0x28)
            if len(header) < 0x20:
                continue
            qwords = struct.unpack_from("<4Q", header)
            if (
                qwords[0] == args.instance
                and (qwords[3] & RESOURCE_HANDLE_STATE_MASK)
                == RESOURCE_HANDLE_STATE_VALUE
            ):
                candidate_handles.add(match)

        # Pass 2: every reference to one of those wrappers could be record+0x110.
        handle_consumers = list(scan_values(candidate_handles))
        possible_record_bases: set[int] = set()
        consumer_for_record_base: dict[int, list[tuple[int, int, int]]] = {}
        for match, handle in handle_consumers:
            for material_index in range(max_materials):
                record_base = match - RECORD_HANDLE_OFFSET - material_index * RECORD_STRIDE
                if record_base <= 0:
                    continue
                possible_record_bases.add(record_base)
                consumer_for_record_base.setdefault(record_base, []).append(
                    (match, handle, material_index)
                )

        # Pass 3: a validated layout points at the first material record at +0x20.
        layouts: dict[int, tuple[int, int, int, int]] = {}
        for pointer_ref, record_base in scan_values(possible_record_bases):
            layout = pointer_ref - 0x20
            header = read(layout + 0x18, 0x10)
            if len(header) != 0x10:
                continue
            material_count, flags, records = struct.unpack("<IIQ", header)
            if (
                records != record_base
                or material_count <= 0
                or material_count > max_materials
                or (flags & 0x7FFFFFFF) == 0
            ):
                continue
            for match, handle, material_index in consumer_for_record_base[record_base]:
                if material_index < material_count:
                    layouts[layout] = (
                        record_base,
                        material_count,
                        flags,
                        material_index,
                    )

        # Pass 4: groups point to the layout at +0x20 (or the +0x18 fallback).
        groups: dict[int, tuple[int, int, str]] = {}
        for pointer_ref, layout in scan_values(set(layouts)):
            for offset, source in ((0x20, "primary"), (0x18, "fallback")):
                group = pointer_ref - offset
                vtable = read_qword(group)
                if vtable is None or not executable(vtable):
                    continue
                linked_layout = read_qword(group + offset)
                if linked_layout != layout:
                    continue
                next_group = read_qword(group + 0x40) or 0
                groups[group] = (layout, next_group, source)

        # Pass 5: a model object points to the group at +0x10 and has flag bit 1.
        model_objects: dict[int, tuple[int, int]] = {}
        for pointer_ref, group in scan_values(set(groups)):
            model_object = pointer_ref - 0x10
            flags = read_qword(model_object + 0x78)
            if flags is not None and (flags & 0x2) != 0:
                model_objects[model_object] = (group, flags)

        print(
            f"summary pid={args.pid} instance=0x{args.instance:X} "
            f"handles={len(candidate_handles)} consumers={len(handle_consumers)} "
            f"layouts={len(layouts)} groups={len(groups)} "
            f"model_objects={len(model_objects)}"
        )
        for layout, (records, count, flags, material_index) in sorted(layouts.items()):
            handle = read_qword(
                records + material_index * RECORD_STRIDE + RECORD_HANDLE_OFFSET
            ) or 0
            linked_groups = [
                group for group, value in groups.items() if value[0] == layout
            ]
            print(
                f"layout=0x{layout:X} records=0x{records:X} count={count} "
                f"flags=0x{flags:X} matched_index={material_index} "
                f"handle=0x{handle:X} groups="
                + ",".join(f"0x{group:X}" for group in sorted(linked_groups))
            )
        for group, (layout, next_group, source) in sorted(groups.items()):
            linked_objects = [
                model_object
                for model_object, value in model_objects.items()
                if value[0] == group
            ]
            print(
                f"group=0x{group:X} layout=0x{layout:X} next=0x{next_group:X} "
                f"source={source} model_objects="
                + ",".join(f"0x{value:X}" for value in sorted(linked_objects))
            )
        for model_object, (group, flags) in sorted(model_objects.items()):
            print(
                f"model_object=0x{model_object:X} group=0x{group:X} "
                f"flags=0x{flags:X}"
            )
        return 0
    finally:
        kernel32.CloseHandle(process)


if __name__ == "__main__":
    raise SystemExit(main())
