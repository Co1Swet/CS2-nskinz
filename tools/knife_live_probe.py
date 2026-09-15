"""Bounded external READ-ONLY knife binding inspection; no injection or writes.

Offsets are extracted by name from the installed client Schema. Native identity
layout and entity-table instruction are restricted to the audited binary hash.
This discovers evidence; it does not replace the production C++ graph probe.
"""
from __future__ import annotations

import argparse
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'build/tools/knife-audit-deps'))
import pefile

CLIENT_SHA = 'b8e2c009763e8cefb88d89a2bdcf452db17501553d473db6da060df8e6769eb4'


class Module(c.Structure):
    _fields_ = [('dwSize', w.DWORD), ('th32ModuleID', w.DWORD),
                ('th32ProcessID', w.DWORD), ('GlblcntUsage', w.DWORD),
                ('ProccntUsage', w.DWORD), ('modBaseAddr', c.c_void_p),
                ('modBaseSize', w.DWORD), ('hModule', w.HMODULE),
                ('szModule', w.WCHAR * 256), ('szExePath', w.WCHAR * 260)]


class Memory:
    def __init__(self, pid):
        self.k = c.WinDLL('kernel32', use_last_error=True)
        self.k.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
        self.k.OpenProcess.restype = w.HANDLE
        self.k.CloseHandle.argtypes = [w.HANDLE]
        self.k.ReadProcessMemory.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p,
                                           c.c_size_t, c.POINTER(c.c_size_t)]
        self.k.ReadProcessMemory.restype = w.BOOL
        self.k.CreateToolhelp32Snapshot.argtypes = [w.DWORD, w.DWORD]
        self.k.CreateToolhelp32Snapshot.restype = w.HANDLE
        for name in ['Module32FirstW', 'Module32NextW']:
            fn = getattr(self.k, name)
            fn.argtypes = [w.HANDLE, c.POINTER(Module)]
            fn.restype = w.BOOL
        self.handle = self.k.OpenProcess(0x410, False, pid)  # QUERY_INFORMATION | VM_READ only
        if not self.handle:
            raise OSError(c.get_last_error(), 'OpenProcess (read-only)')
        snap = self.k.CreateToolhelp32Snapshot(0x18, pid)
        if snap == c.c_void_p(-1).value:
            self.k.CloseHandle(self.handle)
            raise OSError(c.get_last_error(), 'module snapshot')
        self.modules = {}
        try:
            entry = Module()
            entry.dwSize = c.sizeof(entry)
            ok = self.k.Module32FirstW(snap, c.byref(entry))
            while ok:
                self.modules[entry.szModule.lower()] = dict(
                    base=entry.modBaseAddr, size=entry.modBaseSize, path=entry.szExePath)
                ok = self.k.Module32NextW(snap, c.byref(entry))
        finally:
            self.k.CloseHandle(snap)

    def close(self):
        self.k.CloseHandle(self.handle)

    def read(self, address, size):
        if not 0x10000 <= address < 0x7fffffffffff or not 0 < size <= 0x10000:
            raise ValueError(f'invalid bounded read {address:#x}/{size}')
        buf = c.create_string_buffer(size)
        read = c.c_size_t()
        if not self.k.ReadProcessMemory(self.handle, address, buf, size, c.byref(read)) or read.value != size:
            raise OSError(c.get_last_error(), f'ReadProcessMemory {address:#x}/{size}')
        return buf.raw

    def value(self, address, fmt='Q'):
        return struct.unpack('<' + fmt, self.read(address, struct.calcsize('<' + fmt)))[0]

    def string(self, address, maximum=256):
        if not address:
            return ''
        data = bytearray()
        for offset in range(0, maximum, 16):
            chunk = self.read(address + offset, min(16, maximum - offset))
            if b'\0' in chunk:
                return (data + chunk.split(b'\0')[0]).decode('utf-8', errors='strict')
            data.extend(chunk)
        raise ValueError('unterminated bounded string')

    def rtti(self, address):
        table = self.value(address)
        owner = next((m for m in self.modules.values() if m['base'] <= table < m['base'] + m['size']), None)
        if not owner:
            return ''
        col = self.value(table - 8)
        signature, suboffset, _, typ, _, self_rva = struct.unpack('<6I', self.read(col, 24))
        if signature != 1 or col - self_rva != owner['base']:
            return ''
        return self.string(owner['base'] + typ + 16)


class Schema:
    def __init__(self, path):
        self.data = Path(path).read_bytes()
        if hashlib.sha256(self.data).hexdigest() != CLIENT_SHA:
            raise ValueError('client binary changed: re-audit native layout first')
        self.pe = pefile.PE(data=self.data, fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.cache = {}

    def occurrences(self, needle):
        start = 0
        while (index := self.data.find(needle, start)) >= 0:
            yield index
            start = index + 1

    def fields(self, name):
        if name in self.cache:
            return self.cache[name]
        matches = []
        for index in self.occurrences(name.encode() + b'\0'):
            ptr = self.base + self.pe.get_rva_from_offset(index)
            for ref in self.occurrences(struct.pack('<Q', ptr)):
                start = ref - 8
                size, count = struct.unpack_from('<IH', self.data, start + 0x20)
                table = struct.unpack_from('<Q', self.data, start + 0x30)[0]
                if not (0 < size < 0x100000 and 0 < count < 1000 and self.base < table < self.base + self.pe.OPTIONAL_HEADER.SizeOfImage):
                    continue
                raw = self.pe.get_data(table - self.base, count * 32)
                fields = {}
                for i in range(count):
                    field, _, offset = struct.unpack_from('<QQI', raw, i * 32)
                    label = self.pe.get_string_at_rva(field - self.base).decode('ascii')
                    fields[label] = offset
                matches.append(fields)
        if len(matches) != 1:
            raise ValueError(f'Schema {name}: expected unique descriptor, found {len(matches)}')
        self.cache[name] = matches[0]
        return matches[0]

    def off(self, name, field):
        return self.fields(name)[field]


def inspect(mem, schema):
    off, val = schema.off, mem.value
    client = mem.modules['client.dll']['base']
    instruction = mem.read(client + 0x104E33B, 7)
    if instruction != bytes.fromhex('4c 8b 05 1e 2f 1a 01'):
        raise ValueError('live native entity table instruction differs from audit')
    table = val(client + 0x104E342 + struct.unpack('<i', instruction[3:])[0])

    def entity(handle):
        if handle == 0xffffffff:
            return 0
        index = handle & 0x7fff
        chunk = val(table + (index >> 9) * 8)
        if not chunk:
            return 0
        identity = chunk + (index & 511) * 0x70
        if val(identity + 0x10, 'I') != handle:
            return 0
        result = val(identity)
        if result and val(result + off('CEntityInstance', 'm_pEntity')) != identity:
            raise ValueError('identity backpointer mismatch')
        return result

    def identity(address):
        ident = val(address + off('CEntityInstance', 'm_pEntity'))
        handle = val(ident + 0x10, 'I')
        if entity(handle) != address:
            raise ValueError('stale entity')
        return dict(address=address, handle=handle,
                    creation_tick=val(address + off('C_BaseEntity', 'm_nCreationTick'), 'i'))

    def model(address):
        scene = val(address + off('C_BaseEntity', 'm_pGameSceneNode'))
        if not scene or val(scene + off('CGameSceneNode', 'm_pOwner')) != address:
            raise ValueError('scene owner mismatch')
        state = scene + off('CSkeletonInstance', 'm_modelState')
        return dict(scene=scene, scene_type=mem.rtti(scene),
                    name=mem.string(val(state + off('CModelState', 'm_ModelName'))),
                    handle=val(state + off('CModelState', 'm_hModel')))

    def resource(address):
        flags = val(address + 4, 'I')
        if flags & 0x40000000:
            return mem.string(address + 8)
        return mem.string(val(address + 8)) if flags & 0x3fffffff else ''

    def body_details(address):
        body = val(address + off('C_BaseEntity', 'm_CBodyComponent'))
        controller = body + off('CBodyComponentBaseAnimGraph', 'm_animationController')
        detail = dict(body=body, controller=controller,
            algorithm=val(controller + off('CBaseAnimGraphController', 'm_nAnimationAlgorithm'), 'B'),
            masters=val(controller + off('CBaseAnimGraphController', 'm_nSecondarySkeletonMasterCount'), 'i'),
            graph_wrapper_owner=val(controller + 0x448), graph=val(controller + 0x450))
        # Native client 935C00/935C0E proves slot symbols are 8-byte and entity
        # handles are 4-byte; the two vectors must not use the same stride.
        detail['secondary_slot_header'] = mem.read(controller + 0x20, 24).hex(' ')
        detail['secondary_skeleton_header'] = mem.read(controller + 0x38, 24).hex(' ')
        slots, skeletons = val(controller + 0x20, 'i'), val(controller + 0x38, 'i')
        if 0 < slots <= 8:
            detail['secondary_slots'] = [dict(symbol=val(val(controller + 0x28) + i * 8),
                name=mem.string(val(val(controller + 0x28) + i * 8))) for i in range(slots)]
        if 0 < skeletons <= 8:
            detail['secondary_skeleton_handles'] = [val(val(controller + 0x40) + i * 4, 'I') for i in range(skeletons)]
        detail['graph_handle'] = val(controller + 0x370)
        return detail

    result = dict(table=table, players=[], errors=[])
    chunk = val(table)
    for slot in range(1, 128):
        address = val(chunk + slot * 0x70)
        if not address:
            continue
        try:
            if mem.rtti(address) != '.?AVCCSPlayerController@@':
                continue
            xuid = val(address + off('CBasePlayerController', 'm_steamID'))
            pawn = entity(val(address + off('CCSPlayerController', 'm_hPlayerPawn'), 'I'))
            if not pawn:
                continue
            services = val(pawn + off('C_BasePlayerPawn', 'm_pWeaponServices'))
            world = entity(val(services + off('CPlayer_WeaponServices', 'm_hActiveWeapon'), 'I')) if services else 0
            row = dict(slot=slot, xuid=xuid, pawn=identity(pawn), active_weapon=world)
            result['players'].append(row)
            if not world:
                continue
            row['world'] = identity(world)
            row['world_type'] = mem.rtti(world)
            row['world_model'] = model(world)
            item = world + off('C_EconEntity', 'm_AttributeManager') + off('C_AttributeContainer', 'm_Item')
            row['definition'] = val(item + off('C_EconItemView', 'm_iItemDefinitionIndex'), 'H')
            if row['definition'] not in [42, 59] and not 500 <= row['definition'] <= 526:
                continue
            # Installed client hash + static vtable audit: world SetModel's
            # notification resolves +0x1760 then queues that HUD for removal.
            notification = val(val(world) + 0x780)
            row['world_model_notification'] = dict(
                entry=notification, rva=notification - mem.modules['client.dll']['base'],
                deferred_hud_removal_verified=notification == mem.modules['client.dll']['base'] + 0x7F6A70,
                native_hud_handle=val(world + 0x1760, 'I'))
            vdata = val(world + off('C_BaseEntity', 'm_nSubclassID') + 8)
            row['vdata'] = dict(address=vdata,
                name=mem.string(val(vdata + off('CCSWeaponBaseVData', 'm_szName'))),
                name_symbol=val(vdata + off('CCSWeaponBaseVData', 'm_szName')),
                anim=mem.string(val(vdata + off('CCSWeaponBaseVData', 'm_szAnimClass'))),
                anim_symbol=val(vdata + off('CCSWeaponBaseVData', 'm_szAnimClass')),
                model=resource(vdata + off('CBasePlayerWeaponVData', 'm_szWorldModel')),
                skeleton=resource(vdata + off('CCSWeaponBaseVData', 'm_szAnimSkeleton')))
            arms = entity(val(pawn + off('C_CSPlayerPawn', 'm_hHudModelArms'), 'I'))
            if not arms:
                continue
            row['arms'] = identity(arms)
            row['arms_model'] = model(arms)
            row['arms_body'] = body_details(arms)
            body = val(arms + off('C_BaseEntity', 'm_CBodyComponent'))
            controller = body + off('CBodyComponentBaseAnimGraph', 'm_animationController')
            row['body_animation_controller'] = controller
            row['body_type'] = mem.rtti(body)
            wrapper = controller + off('CBaseAnimGraphController', 'm_pGraphInstanceAG2')
            row['body_wrapper_owner'] = val(wrapper)
            row['body_graph'] = val(wrapper + 8)
            graphctrl = val(arms + off('CBaseAnimGraph', 'm_pMainGraphController'))
            row['weapon_graph_controller'] = graphctrl
            row['graph_controller_type'] = mem.rtti(graphctrl) if graphctrl else ''
            if graphctrl:
                param = graphctrl + off('CCS2WeaponGraphController', 'm_weaponType')
                name, graph, index = val(param), val(param + 8), val(param + 16, 'h')
                row['parameter'] = dict(name=mem.string(name), name_symbol=name, graph=graph, index=index)
                if graph and 0 <= index < min(val(graph + 8, 'i'), 32768):
                    node = val(val(graph + 16) + index * 8)
                    row['parameter'].update(node=node, node_type=mem.rtti(node), value=mem.string(val(node + 24)), value_symbol=val(node + 24))
            children, seen = [], set()
            child = val(row['arms_model']['scene'] + off('CGameSceneNode', 'm_pChild'))
            while child and len(seen) < 64:
                if child in seen:
                    raise ValueError('scene child cycle')
                seen.add(child)
                if val(child + off('CGameSceneNode', 'm_pParent')) != row['arms_model']['scene']:
                    raise ValueError('scene child parent mismatch')
                owner = val(child + off('CGameSceneNode', 'm_pOwner'))
                entry = dict(entity=identity(owner), type=mem.rtti(owner), model=model(owner),
                    owner_handle=val(owner + off('C_BaseEntity', 'm_hOwnerEntity'), 'I'))
                if entry['owner_handle'] == row['world']['handle']:
                    entry['body'] = body_details(owner)
                children.append(entry)
                child = val(child + off('CGameSceneNode', 'm_pNextSibling'))
            row['children'] = children
            row['identities_still_valid'] = all(identity(i['address']) == i for i in [row['pawn'], row['world'], row['arms']])
        except (OSError, ValueError, KeyError) as error:
            result['errors'].append(dict(slot=slot, error=str(error)))
    result['schema'] = schema.cache
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--verbose', action='store_true')
    args = parser.parse_args()
    mem = Memory(args.pid)
    try:
        schema = Schema(mem.modules['client.dll']['path'])
        report = dict(pid=args.pid, read_only=True, modules={k: v for k, v in mem.modules.items()
            if k in ['client.dll', 'animationsystem.dll', 'afxhooksource2.dll'] or 'nskinz' in k},
            observation=inspect(mem, schema))
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        if args.verbose:
            print(json.dumps({k: v for k, v in report['observation'].items() if k != 'schema'}, ensure_ascii=False, indent=2))
        else:
            print(json.dumps(dict(output=str(args.output), errors=report['observation']['errors'], players=[
                {k: row[k] for k in ['slot', 'definition', 'parameter', 'arms_body'] if k in row}
                for row in report['observation']['players']]), ensure_ascii=False, indent=2))
    finally:
        mem.close()


if __name__ == '__main__':
    main()
