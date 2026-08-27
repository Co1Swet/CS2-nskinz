#!/usr/bin/env python3
"""Read and format a small process-memory range without modifying it."""

from __future__ import annotations

import argparse
import ctypes
import struct
import sys
from ctypes import wintypes


PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010


def parse_int(value: str) -> int:
    return int(value, 0)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--address", type=parse_int, required=True)
    parser.add_argument("--size", type=parse_int, default=0x400)
    args = parser.parse_args()

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
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

    try:
        buffer = ctypes.create_string_buffer(args.size)
        bytes_read = ctypes.c_size_t(0)
        ok = kernel32.ReadProcessMemory(
            process,
            ctypes.c_void_p(args.address),
            buffer,
            args.size,
            ctypes.byref(bytes_read),
        )
        data = buffer.raw[: bytes_read.value]
        if not ok and not data:
            print(f"ReadProcessMemory failed: winerror={ctypes.get_last_error()}", file=sys.stderr)
            return 3

        for offset in range(0, len(data), 0x20):
            row = data[offset : offset + 0x20]
            padded = row + b"\0" * ((8 - len(row) % 8) % 8)
            qwords = [
                f"{value:016X}" for (value,) in struct.iter_unpack("<Q", padded)
            ]
            ascii_text = "".join(chr(value) if 32 <= value < 127 else "." for value in row)
            print(
                f"{args.address + offset:016X}  {' '.join(qwords):<67}  {ascii_text}"
            )
        return 0
    finally:
        kernel32.CloseHandle(process)


if __name__ == "__main__":
    raise SystemExit(main())
