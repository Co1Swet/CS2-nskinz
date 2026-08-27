#!/usr/bin/env python3
"""Expand one resource instance into its strong handles and find consumers."""

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
RESOURCE_HANDLE_STATE_MASK = 0xFFFF0000FFFF0000
RESOURCE_HANDLE_STATE_VALUE = 0xFFFF0000FFFF0000


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
    parser.add_argument("--max-matches", type=int, default=512)
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
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
        False,
        args.pid,
    )
    if not process:
        print(f"OpenProcess failed: winerror={ctypes.get_last_error()}", file=sys.stderr)
        return 2

    chunk_size = max(64 * 1024, args.chunk_mib * 1024 * 1024)
    maximum_user_address = 0x00007FFFFFFFFFFF

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

    def regions():
        address = 0
        while address < maximum_user_address:
            mbi = MEMORY_BASIC_INFORMATION()
            queried = kernel32.VirtualQueryEx(
                process,
                ctypes.c_void_p(address),
                ctypes.byref(mbi),
                ctypes.sizeof(mbi),
            )
            if not queried:
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
                yield base, size, int(mbi.AllocationBase or 0), mbi.Protect, mbi.Type
            address = base + max(size, 0x1000)

    try:
        candidate_handles: set[int] = set()
        scanned_bytes = 0
        for region_base, region_size, _, _, _ in regions():
            for offset in range(0, region_size, chunk_size):
                data = read(region_base + offset, min(chunk_size, region_size - offset))
                aligned_size = len(data) - len(data) % 8
                if aligned_size == 0:
                    continue
                scanned_bytes += aligned_size
                values = np.frombuffer(data[:aligned_size], dtype="<u8")
                for index in np.flatnonzero(values == args.instance):
                    candidate = region_base + offset + int(index) * 8
                    header = read(candidate, 0x28)
                    if len(header) < 0x20:
                        continue
                    qwords = struct.unpack_from("<4Q", header)
                    if (
                        qwords[0] == args.instance
                        and (qwords[3] & RESOURCE_HANDLE_STATE_MASK)
                        == RESOURCE_HANDLE_STATE_VALUE
                    ):
                        candidate_handles.add(candidate)

        if not candidate_handles:
            print(
                f"summary pid={args.pid} instance=0x{args.instance:X} "
                f"handles=0 matches=0 bytes={scanned_bytes}"
            )
            return 0

        handle_values = np.array(sorted(candidate_handles), dtype="<u8")
        print(
            "handles=" + ",".join(f"0x{value:X}" for value in handle_values.tolist())
        )

        match_count = 0
        second_pass_bytes = 0
        for region_base, region_size, allocation_base, protect, region_type in regions():
            for offset in range(0, region_size, chunk_size):
                if match_count >= args.max_matches:
                    break
                data = read(region_base + offset, min(chunk_size, region_size - offset))
                aligned_size = len(data) - len(data) % 8
                if aligned_size == 0:
                    continue
                second_pass_bytes += aligned_size
                values = np.frombuffer(data[:aligned_size], dtype="<u8")
                matching_indices = np.flatnonzero(np.isin(values, handle_values))
                for index in matching_indices:
                    absolute = region_base + offset + int(index) * 8
                    pointer = int(values[index])
                    context_address = max(region_base, absolute - 0x20)
                    context = read(context_address, 0x48)
                    context_aligned = context[: len(context) - len(context) % 8]
                    qwords = [
                        f"0x{value:X}"
                        for (value,) in struct.iter_unpack("<Q", context_aligned)
                    ]
                    print(
                        f"handle=0x{pointer:X} match=0x{absolute:X} "
                        f"region=0x{region_base:X}+0x{region_size:X} "
                        f"alloc=0x{allocation_base:X} protect=0x{protect:X} "
                        f"type=0x{region_type:X} qwords=[{','.join(qwords)}]"
                    )
                    match_count += 1
                    if match_count >= args.max_matches:
                        break
            if match_count >= args.max_matches:
                break

        print(
            f"summary pid={args.pid} instance=0x{args.instance:X} "
            f"handles={len(candidate_handles)} matches={match_count} "
            f"bytes={scanned_bytes + second_pass_bytes}"
        )
        return 0
    finally:
        kernel32.CloseHandle(process)


if __name__ == "__main__":
    raise SystemExit(main())
