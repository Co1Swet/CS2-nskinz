"""Read own development DLL state using its matching local PDB; never attaches a debugger or writes memory."""
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
from pathlib import Path
from knife_live_probe import Memory


class Symbol(c.Structure):
    _fields_ = [('SizeOfStruct', w.ULONG), ('TypeIndex', w.ULONG), ('Reserved', c.c_ulonglong * 2),
                ('Index', w.ULONG), ('Size', w.ULONG), ('ModBase', c.c_ulonglong), ('Flags', w.ULONG),
                ('Value', c.c_ulonglong), ('Address', c.c_ulonglong), ('Register', w.ULONG),
                ('Scope', w.ULONG), ('Tag', w.ULONG), ('NameLen', w.ULONG), ('MaxNameLen', w.ULONG),
                ('Name', c.c_char * 1)]


class Symbols:
    def __init__(self, mem, module):
        self.mem, self.base = mem, module['base']
        self.d = c.WinDLL('dbghelp', use_last_error=True)
        self.d.SymInitializeW.argtypes = [w.HANDLE, w.LPCWSTR, w.BOOL]
        self.d.SymInitializeW.restype = w.BOOL
        self.d.SymSetOptions.argtypes = [w.DWORD]
        self.d.SymSetOptions(0x80000 | 0x200 | 0x400 | 2)
        self.d.SymLoadModuleExW.argtypes = [w.HANDLE, w.HANDLE, w.LPCWSTR, w.LPCWSTR, c.c_ulonglong, w.DWORD, c.c_void_p, w.DWORD]
        self.d.SymLoadModuleExW.restype = c.c_ulonglong
        self.d.SymGetTypeInfo.argtypes = [w.HANDLE, c.c_ulonglong, w.ULONG, c.c_int, c.c_void_p]
        self.d.SymGetTypeInfo.restype = w.BOOL
        self.d.SymCleanup.argtypes = [w.HANDLE]
        if not self.d.SymInitializeW(mem.handle, str(Path(module['path']).parent), False):
            raise OSError(c.get_last_error(), 'SymInitializeW')
        if not self.d.SymLoadModuleExW(mem.handle, None, module['path'], None, self.base, module['size'], None, 0):
            raise OSError(c.get_last_error(), 'SymLoadModuleExW local image/PDB')

    def get(self, type_id, kind, result_type=w.ULONG):
        value = result_type()
        if not self.d.SymGetTypeInfo(self.mem.handle, self.base, type_id, kind, c.byref(value)):
            raise OSError(c.get_last_error(), f'SymGetTypeInfo({type_id}, {kind})')
        return value.value

    def fields(self, type_id):
        count = self.get(type_id, 13)
        if count > 4096:
            raise ValueError('unbounded symbol children')
        class Children(c.Structure):
            _fields_ = [('Count', w.ULONG), ('Start', w.ULONG), ('ChildId', w.ULONG * count)]
        children = Children(count, 0)
        if not self.d.SymGetTypeInfo(self.mem.handle, self.base, type_id, 7, c.byref(children)):
            raise OSError(c.get_last_error(), 'TI_FINDCHILDREN')
        result = {}
        self.mem.k.LocalFree.argtypes = [c.c_void_p]
        for child in children.ChildId:
            pointer = self.get(child, 1, c.c_void_p)
            try:
                name = c.wstring_at(pointer)
            finally:
                self.mem.k.LocalFree(pointer)
            try:
                result[name] = dict(offset=self.get(child, 10), type=self.get(child, 4))
            except OSError:  # static members and methods have no instance offset
                continue
        return result

    def find(self, pattern):
        results = []
        callback_type = c.WINFUNCTYPE(w.BOOL, c.POINTER(Symbol), w.ULONG, c.c_void_p)
        @callback_type
        def callback(symbol, _size, _context):
            info = symbol.contents
            name = c.string_at(c.addressof(info) + Symbol.Name.offset, info.NameLen).decode('utf-8')
            results.append(dict(name=name, address=info.Address, type=info.TypeIndex))
            return True
        self.d.SymEnumSymbols.argtypes = [w.HANDLE, c.c_ulonglong, c.c_char_p, callback_type, c.c_void_p]
        self.d.SymEnumSymbols.restype = w.BOOL
        if not self.d.SymEnumSymbols(self.mem.handle, self.base, pattern.encode(), callback, None):
            raise OSError(c.get_last_error(), 'SymEnumSymbols')
        return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', required=True, type=int)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    mem = Memory(args.pid)
    symbols = None
    try:
        modules = [m for name, m in mem.modules.items() if 'nskinzcs2' in name and name.endswith('.dll')]
        if len(modules) != 1:
            raise ValueError('exactly one nSkinz development module required')
        symbols = Symbols(mem, modules[0])
        matches = symbols.find('*g_hook_state*')
        matches = [m for m in matches if m['type'] and
                   (m['name'] == 'g_hook_state' or m['name'].endswith('::g_hook_state'))]
        if len(matches) != 1:
            raise ValueError(f'global symbol ambiguous: {matches}')
        root = matches[0]
        fields = symbols.fields(root['type'])
        result = dict(pid=args.pid, read_only=True, module=modules[0], symbol=root, fields={})
        selected = ['schema_offsets', 'knife_model_last_probe_binding', 'knife_update_probe']
        for name in selected:
            field = fields[name]
            address = root['address'] + field['offset']
            children = symbols.fields(field['type'])
            values = {}
            for key, child in children.items():
                if name == 'schema_offsets' and not (key.startswith('knife_') or key in ['pawn_hud_model_arms', 'base_owner_entity']):
                    continue
                size = symbols.get(child['type'], 2, c.c_ulonglong)
                if 0 < size <= 32:
                    values[key] = dict(offset=child['offset'], size=size,
                        bytes=mem.read(address + child['offset'], size).hex(),
                        value=mem.value(address + child['offset']) if size == 8 else None)
            result['fields'][name] = values
        print(json.dumps(result, ensure_ascii=False, indent=2))
        if args.output:
            args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    finally:
        if symbols:
            symbols.d.SymCleanup(mem.handle)
        mem.close()


if __name__ == '__main__':
    main()
