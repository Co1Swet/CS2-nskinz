#!/usr/bin/env python3
"""Small dependency-free CS2 minidump triage helper.

Prints the exception, x64 instruction/stack pointers, loaded-module mapping and
module-looking return addresses from the exception thread's stack.  It is not a
replacement for WinDbg; it is intentionally sufficient for comparing local
HLAE/nSkinz crash sites when the Windows debugger package is unavailable.
"""

from __future__ import annotations

import argparse
import struct
from dataclasses import dataclass
from pathlib import Path


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def u64(data: bytes, offset: int) -> int:
    return struct.unpack_from("<Q", data, offset)[0]


@dataclass
class Module:
    base: int
    size: int
    name: str
    path: str = ""
    timestamp: int = 0

    def contains(self, address: int) -> bool:
        return self.base <= address < self.base + self.size


class DumpMemory:
    def __init__(self, data: bytes) -> None:
        self.data = data
        self.ranges: list[tuple[int, int, int]] = []

    def add(self, virtual_address: int, size: int, file_offset: int) -> None:
        self.ranges.append((virtual_address, size, file_offset))

    def read(self, virtual_address: int, size: int) -> bytes | None:
        for start, length, file_offset in self.ranges:
            if start <= virtual_address and virtual_address + size <= start + length:
                relative = virtual_address - start
                return self.data[file_offset + relative : file_offset + relative + size]
        return None


def read_minidump_string(data: bytes, rva: int) -> str:
    if rva <= 0 or rva + 4 > len(data):
        return "<unknown>"
    length = u32(data, rva)
    raw = data[rva + 4 : rva + 4 + length]
    return raw.decode("utf-16-le", errors="replace")


def module_for(modules: list[Module], address: int) -> Module | None:
    return next((module for module in modules if module.contains(address)), None)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("dump", type=Path)
    parser.add_argument("--stack-bytes", type=lambda value: int(value, 0), default=0x800)
    parser.add_argument("--inspect", action="append", default=[],
                        help="Read a captured address or register, optionally +offset (never live memory)")
    parser.add_argument("--unwind", action="store_true", help="Unwind with timestamp-matched local x64 PE images")
    args = parser.parse_args()

    data = args.dump.read_bytes()
    if data[:4] != b"MDMP":
        raise SystemExit("not a minidump")

    stream_count = u32(data, 8)
    directory_rva = u32(data, 12)
    streams: dict[int, tuple[int, int]] = {}
    for index in range(stream_count):
        entry = directory_rva + index * 12
        stream_type, size, rva = struct.unpack_from("<III", data, entry)
        streams[stream_type] = (rva, size)

    modules: list[Module] = []
    if 4 in streams:
        rva, _ = streams[4]
        count = u32(data, rva)
        for index in range(count):
            entry = rva + 4 + index * 108
            base = u64(data, entry)
            size = u32(data, entry + 8)
            name = read_minidump_string(data, u32(data, entry + 20))
            modules.append(Module(base, size, Path(name).name, name, u32(data, entry + 16)))
        modules.sort(key=lambda item: item.base)

    memory = DumpMemory(data)
    if 9 in streams:
        rva, _ = streams[9]
        count = u64(data, rva)
        file_offset = u64(data, rva + 8)
        for index in range(count):
            entry = rva + 16 + index * 16
            start = u64(data, entry)
            size = u64(data, entry + 8)
            memory.add(start, size, file_offset)
            file_offset += size
    if 5 in streams:
        rva, _ = streams[5]
        count = u32(data, rva)
        for index in range(count):
            entry = rva + 4 + index * 16
            start = u64(data, entry)
            size = u32(data, entry + 8)
            file_offset = u32(data, entry + 12)
            memory.add(start, size, file_offset)

    if 6 not in streams:
        raise SystemExit("exception stream missing")
    exception_rva, _ = streams[6]
    thread_id = u32(data, exception_rva)
    exception_code = u32(data, exception_rva + 8)
    exception_address = u64(data, exception_rva + 24)
    context_size = u32(data, exception_rva + 160)
    context_rva = u32(data, exception_rva + 164)
    context = data[context_rva : context_rva + context_size]
    if len(context) < 0x100:
        raise SystemExit("x64 context missing or truncated")
    register_offsets = {
        "rax": 0x78,
        "rcx": 0x80,
        "rdx": 0x88,
        "rbx": 0x90,
        "rsp": 0x98,
        "rbp": 0xA0,
        "rsi": 0xA8,
        "rdi": 0xB0,
        "r8": 0xB8,
        "r9": 0xC0,
        "r10": 0xC8,
        "r11": 0xD0,
        "r12": 0xD8,
        "r13": 0xE0,
        "r14": 0xE8,
        "r15": 0xF0,
        "rip": 0xF8,
    }
    registers = {name: u64(context, offset) for name, offset in register_offsets.items()}
    rsp = registers["rsp"]
    rip = registers["rip"]

    def describe(address: int) -> str:
        module = module_for(modules, address)
        if module is None:
            return f"0x{address:X}"
        return f"{module.name}+0x{address - module.base:X} (0x{address:X})"

    print(f"dump={args.dump}")
    print(f"thread={thread_id}")
    print(f"exception=0x{exception_code:08X} at {describe(exception_address)}")
    print(f"rip={describe(rip)}")
    print(f"rsp=0x{rsp:X}")
    print("registers:")
    for row_start in range(0, len(register_offsets), 4):
        names = list(register_offsets)[row_start : row_start + 4]
        print("  " + "  ".join(f"{name}=0x{registers[name]:016X}" for name in names))

    def inspect(address: int, depth: int = 0) -> None:
        raw = next((memory.read(address, size) for size in (128, 64, 32, 16, 8)
                    if memory.read(address, size) is not None), None)
        indent = "  " * depth
        print(f"{indent}memory {describe(address)}: {raw.hex() if raw else '<not captured>'}")
        if raw is None:
            return
        print(f"{indent}ascii: {''.join(chr(b) if 32 <= b < 127 else '.' for b in raw)}")
        if depth >= 2:
            return
        for offset in range(0, min(len(raw), 32), 8):
            pointer = u64(raw, offset)
            print(f"{indent}+0x{offset:X} -> {describe(pointer)}")
            if memory.read(pointer, 8) is not None and pointer != address:
                inspect(pointer, depth + 1)

    for expression in args.inspect:
        parts = expression.split("+", 1)
        address = registers[parts[0]] if parts[0] in registers else int(parts[0], 0)
        if len(parts) > 1:
            address += int(parts[1], 0)
        inspect(address)

    if args.unwind:
        from minidump_unwind import unwind
        unwind(memory, modules, registers)

    stack = memory.read(rsp, args.stack_bytes)
    if stack is None:
        print("stack memory unavailable")
        return 0
    print("module-looking stack qwords:")
    for offset in range(0, len(stack) - 7, 8):
        value = u64(stack, offset)
        module = module_for(modules, value)
        if module is not None:
            print(f"  rsp+0x{offset:03X}: {describe(value)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
