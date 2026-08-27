#!/usr/bin/env python3
"""Read-only Windows process scan for exact pointer references."""

from __future__ import annotations

import argparse
import ctypes
import re
import struct
import sys
from ctypes import wintypes


PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
MEM_COMMIT = 0x1000
PAGE_GUARD = 0x100
PAGE_NOACCESS = 0x01
PAGE_READABLE_MASK = 0x02 | 0x04 | 0x08 | 0x20 | 0x40 | 0x80


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


def parse_pointer(value: str) -> int:
    return int(value, 0)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--pointer", type=parse_pointer, required=True, action="append")
    parser.add_argument("--max-matches", type=int, default=256)
    parser.add_argument("--chunk-mib", type=int, default=2)
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
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
        False,
        args.pid,
    )
    if not process:
        print(f"OpenProcess failed: winerror={ctypes.get_last_error()}", file=sys.stderr)
        return 2

    needles = {pointer: struct.pack("<Q", pointer) for pointer in args.pointer}
    combined_needle = re.compile(
        b"|".join(re.escape(needle) for needle in needles.values())
    )
    pointers_by_needle = {needle: pointer for pointer, needle in needles.items()}
    match_counts = {pointer: 0 for pointer in args.pointer}
    chunk_size = max(64 * 1024, args.chunk_mib * 1024 * 1024)
    address = 0
    maximum_user_address = 0x00007FFFFFFFFFFF
    match_count = 0
    scanned_bytes = 0
    scanned_regions = 0

    try:
        while address < maximum_user_address and match_count < args.max_matches:
            mbi = MEMORY_BASIC_INFORMATION()
            queried = kernel32.VirtualQueryEx(
                process,
                ctypes.c_void_p(address),
                ctypes.byref(mbi),
                ctypes.sizeof(mbi),
            )
            if not queried:
                break

            region_base = int(mbi.BaseAddress or 0)
            region_size = int(mbi.RegionSize)
            next_address = region_base + max(region_size, 0x1000)
            readable = (
                mbi.State == MEM_COMMIT
                and (mbi.Protect & PAGE_GUARD) == 0
                and (mbi.Protect & PAGE_NOACCESS) == 0
                and (mbi.Protect & PAGE_READABLE_MASK) != 0
            )
            if readable and region_size > 0:
                scanned_regions += 1
                offset = 0
                carry = b""
                while offset < region_size and match_count < args.max_matches:
                    requested = min(chunk_size, region_size - offset)
                    buffer = ctypes.create_string_buffer(requested)
                    bytes_read = ctypes.c_size_t(0)
                    ok = kernel32.ReadProcessMemory(
                        process,
                        ctypes.c_void_p(region_base + offset),
                        buffer,
                        requested,
                        ctypes.byref(bytes_read),
                    )
                    data = buffer.raw[: bytes_read.value] if ok or bytes_read.value else b""
                    if data:
                        scanned_bytes += len(data)
                        searchable = carry + data
                        searchable_base = region_base + offset - len(carry)
                        for match in combined_needle.finditer(searchable):
                            if match_count >= args.max_matches:
                                break
                            found = match.start()
                            needle = match.group(0)
                            pointer = pointers_by_needle[needle]
                            absolute = searchable_base + found
                            if absolute >= region_base:
                                context_start = max(0, found - 32)
                                context_end = min(len(searchable), found + 40)
                                context = searchable[context_start:context_end]
                                aligned = context[: len(context) - (len(context) % 8)]
                                qwords = [
                                    f"0x{value:X}"
                                    for (value,) in struct.iter_unpack("<Q", aligned)
                                ]
                                print(
                                    f"pointer=0x{pointer:X} match="
                                    f"0x{absolute:X} region=0x{region_base:X}+0x{region_size:X} "
                                    f"alloc=0x{int(mbi.AllocationBase or 0):X} "
                                    f"protect=0x{mbi.Protect:X} type=0x{mbi.Type:X} "
                                    f"qwords=[{','.join(qwords)}]"
                                )
                                match_count += 1
                                match_counts[pointer] += 1
                        carry = data[-7:]
                    else:
                        carry = b""
                    offset += requested
            address = next_address

        pointer_summary = ",".join(
            f"0x{pointer:X}:{match_counts[pointer]}" for pointer in args.pointer
        )
        print(
            f"summary pid={args.pid} pointers=[{pointer_summary}] "
            f"matches={match_count} regions={scanned_regions} bytes={scanned_bytes}"
        )
        return 0
    finally:
        kernel32.CloseHandle(process)


if __name__ == "__main__":
    raise SystemExit(main())
