"""Fail release validation if a PE DLL requires non-inbox runtime DLLs.

Standard library only; this is a build-time tool, not a user dependency.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct


ALLOWED_IMPORTS = {
    "kernel32.dll", "user32.dll", "gdi32.dll", "shell32.dll", "ole32.dll",
    "advapi32.dll", "version.dll", "imm32.dll", "d3d11.dll", "dxgi.dll",
}


def audit(path: Path) -> dict:
    data = path.read_bytes()

    def unpack(fmt: str, offset: int):
        return struct.unpack_from(fmt, data, offset)

    if data[:2] != b"MZ":
        raise ValueError(f"{path}: not a Windows PE file")
    pe = unpack("<I", 0x3C)[0]
    if data[pe:pe+4] != b"PE\0\0":
        raise ValueError(f"{path}: bad PE signature")
    machine, sections = unpack("<HH", pe + 4)
    optional_size, characteristics = unpack("<HH", pe + 20)
    optional = pe + 24
    if machine != 0x8664 or unpack("<H", optional)[0] != 0x20B or not characteristics & 0x2000:
        raise ValueError(f"{path}: expected a native x64 DLL, not x86/EXE")
    if optional_size < 240 or unpack("<I", optional + 108)[0] < 16:
        raise ValueError(f"{path}: truncated optional header")
    section_table = optional + optional_size

    def offset(rva: int) -> int:
        for index in range(sections):
            base = section_table + index * 40
            size, start, raw_size, raw = unpack("<IIII", base + 8)
            if start <= rva < start + max(size, raw_size) and rva-start < raw_size:
                result = raw + rva-start
                if result < len(data):
                    return result
        raise ValueError(f"{path}: unmapped RVA {rva:#x}")

    def string(rva: int) -> str:
        start = offset(rva)
        return data[start:data.index(0, start)].decode("ascii")

    def directory(index: int):
        return unpack("<II", optional + 112 + index * 8)

    if directory(14) != (0, 0):
        raise ValueError(f"{path}: managed/.NET dependency is not portable")
    if directory(13) != (0, 0):
        raise ValueError(f"{path}: delay imports require explicit audit")
    imports_rva, imports_size = directory(1)
    imports = []
    if not imports_rva:
        raise ValueError(f"{path}: no import table")
    for index in range(imports_size // 20):
        descriptor = unpack("<IIIII", offset(imports_rva + index * 20))
        if descriptor == (0, 0, 0, 0, 0):
            break
        imports.append(string(descriptor[3]).lower())
    else:
        raise ValueError(f"{path}: unterminated import table")
    rejected = sorted(set(imports) - ALLOWED_IMPORTS)
    if rejected:
        raise ValueError(f"{path}: non-inbox/unapproved dependencies: {', '.join(rejected)}")

    export_rva, _ = directory(0)
    if not export_rva:
        raise ValueError(f"{path}: no exports")
    export_offset = offset(export_rva)
    name_count = unpack("<I", export_offset + 24)[0]
    names_rva = unpack("<I", export_offset + 32)[0]
    exports = [string(unpack("<I", offset(names_rva + i * 4))[0]) for i in range(name_count)]
    if "nSkinzUiHost" not in exports:
        raise ValueError(f"{path}: isolated UI export missing")
    return {
        "file": path.name, "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data),
        "architecture": "x64", "imports": imports, "exports": exports,
        "scope": "direct imports only; runtime loading tested separately",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dll", nargs="+", type=Path)
    args = parser.parse_args()
    print(json.dumps([audit(path) for path in args.dll], ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
