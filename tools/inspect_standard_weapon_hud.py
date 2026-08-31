"""Bounded, read-only HUD binding snapshot for the 2000899 local Demo baseline.

Does not inject, call game functions, write process memory, or control windows.
Offsets below were checked against this baseline's on-disk Schema descriptors
and GetEntityFromIndex disassembly. Supply the current client module address.
"""
import argparse
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--client-base', type=lambda s: int(s, 0), required=True)
    parser.add_argument('--weapon-index', type=int, required=True)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                        ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    process = kernel.OpenProcess(0x410, False, args.pid)
    if not process:
        raise ctypes.WinError(ctypes.get_last_error())

    def read(address, size):
        if not address or size > 4096:
            return b''
        buf = ctypes.create_string_buffer(size)
        count = ctypes.c_size_t()
        kernel.ReadProcessMemory(process, address, buf, size, ctypes.byref(count))
        return buf.raw[:count.value]

    def number(address, fmt='Q'):
        data = read(address, struct.calcsize(fmt))
        return struct.unpack('<' + fmt, data)[0] if len(data) == struct.calcsize(fmt) else 0

    def string(address, size=200):
        return read(address, size).split(b'\0', 1)[0].decode('utf-8', errors='replace')

    def classname(entity):
        table = number(entity)
        locator = number(table - 8) if table else 0
        if not locator or number(locator, 'I') != 1:
            return ''
        base = locator - number(locator + 20, 'I')
        return string(base + number(locator + 12, 'I') + 16)

    def describe(entity):
        identity = number(entity + 0x10) if entity else 0
        node = number(entity + 0x330) if entity else 0
        return dict(address=hex(entity), identity=hex(identity),
                    handle=hex(number(identity + 0x10, 'I')) if identity else None,
                    identity_roundtrip=bool(identity and number(identity) == entity),
                    owner=hex(number(entity + 0x520, 'I')) if entity else None,
                    class_name=classname(entity) if entity else '', node=hex(node),
                    scene_roundtrip=bool(node and number(node + 0x30) == entity),
                    parent_node=hex(number(node + 0x38)) if node else None,
                    model=string(number(node + 0x140 + 0xA8)) if node else '',
                    mesh_mask=number(node + 0x140 + 0x208) if node else None)

    def tree(entity):
        root = number(entity + 0x330) if entity else 0
        pending = [number(root + 0x40)] if root else []
        seen, result = set(), []
        while pending and len(seen) < 64:
            node = pending.pop()
            if not node or node in seen:
                continue
            seen.add(node)
            owner = number(node + 0x30)
            result.append(dict(scene=hex(node), entity=describe(owner)))
            pending.extend([number(node + 0x48), number(node + 0x40)])
        return dict(nodes=result, truncated=bool(pending))

    try:
        # RIP-relative storage at the unique kEntitySystemRootPattern + 18.
        instruction = args.client_base + 0xAD6C32
        if read(instruction, 3) != bytes.fromhex('48 8b 0d'):
            raise RuntimeError('Baseline entity-root instruction does not match; refusing snapshot')
        storage = instruction + 7 + number(instruction + 3, 'i')
        system = number(storage)

        def entity_from_handle(handle):
            index = handle & 0x7FFF
            if index >= 0x7FFF:
                return 0
            chunk = number(system + 0x10 + (index >> 9) * 8)
            identity = chunk + (index & 0x1FF) * 0x70 if chunk else 0
            if not identity or number(identity + 0x10, 'I') & 0x7FFF != index:
                return 0
            return number(identity)

        world = entity_from_handle(args.weapon_index)
        pawn = entity_from_handle(number(world + 0x520, 'I')) if world else 0
        arms_handle = number(pawn + 0x1B84, 'I') if pawn else 0xFFFFFFFF
        arms = entity_from_handle(arms_handle)
        services = number(pawn + 0x1208) if pawn else 0
        report = dict(pid=args.pid, scope='read-only snapshot; not a visual verification',
                      entity_system=hex(system), world=describe(world), pawn=describe(pawn),
                      pawn_active_handle=hex(number(services + 0x60, 'I')) if services else None,
                      pawn_arms_handle=hex(arms_handle), arms=describe(arms),
                      arms_tree=tree(arms), world_tree=tree(world))
        text = json.dumps(report, ensure_ascii=False, indent=2)
        print(text)
        if args.report:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(text, encoding='utf-8')
    finally:
        kernel.CloseHandle(process)


if __name__ == '__main__':
    main()
