"""Read-only x64 unwind metadata walker for captured stack memory.

No live attach, target execution, internet symbols, or instruction emulation.
Stops rather than guessing on unsupported metadata or unavailable/mismatched images.
"""
import bisect
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'build/tools/knife-audit-deps'))
import pefile

REGS = ('rax', 'rcx', 'rdx', 'rbx', 'rsp', 'rbp', 'rsi', 'rdi',
        'r8', 'r9', 'r10', 'r11', 'r12', 'r13', 'r14', 'r15')


def unwind(memory, modules, initial):
    registers = dict(initial)
    images = {}

    def read64(address):
        raw = memory.read(address, 8)
        if raw is None:
            raise ValueError(f'stack not captured at 0x{address:X}')
        return struct.unpack('<Q', raw)[0]

    def apply_codes(pe, unwind_rva, prolog_offset, nesting=0):
        if nesting > 8:
            raise ValueError('chained unwind depth limit')
        header = pe.get_data(unwind_rva, 4)
        version, prolog, count, frame = header
        flags, version = version >> 3, version & 7
        if version != 1:
            raise ValueError(f'unsupported unwind version {version}')
        codes = pe.get_data(unwind_rva + 4, 2 * count)
        slot = 0
        while slot < count:
            offset, operation = codes[slot * 2: slot * 2 + 2]
            op, info = operation & 15, operation >> 4
            extra = {0: 0, 1: 1 if info == 0 else 2, 2: 0, 3: 0,
                     4: 1, 5: 2, 8: 1, 9: 2, 10: 0}.get(op)
            if extra is None or slot + extra >= count:
                raise ValueError(f'unsupported/truncated unwind op {op}')
            operand = int.from_bytes(codes[(slot+1)*2:(slot+1+extra)*2], 'little')
            slot += 1 + extra
            if prolog_offset < offset:
                continue
            rsp = registers['rsp']
            if op == 0:
                registers[REGS[info]] = read64(rsp)
                registers['rsp'] += 8
            elif op == 1:
                registers['rsp'] += operand * (8 if info == 0 else 1)
            elif op == 2:
                registers['rsp'] += info * 8 + 8
            elif op == 3:
                registers['rsp'] = registers[REGS[frame & 15]] - (frame >> 4) * 16
            elif op in (4, 5):
                registers[REGS[info]] = read64(rsp + operand * (8 if op == 4 else 1))
            elif op == 10:
                raise ValueError('machine frame not supported')
            # XMM saves do not affect integer register or stack unwinding.
        if flags & 4:
            chained = pe.get_data(unwind_rva + 4 + ((count + 1) & ~1) * 2, 12)
            _begin, _end, next_rva = struct.unpack('<III', chained)
            apply_codes(pe, next_rva, 0xFFFFFFFF, nesting + 1)

    print('metadata-unwound frames (integer nonvolatile registers):')
    try:
        for number in range(32):
            pc = registers['rip']
            module = next((m for m in modules if m.contains(pc)), None)
            if module is None:
                raise ValueError(f'no module for 0x{pc:X}')
            rva = pc - module.base
            print(f'  #{number} {module.name}+0x{rva:X} rsp=0x{registers["rsp"]:X} ' +
                  ' '.join(f'{r}=0x{registers[r]:X}' for r in ('rbp', 'rbx', 'rsi', 'rdi', 'r12', 'r13')))
            if module.base not in images:
                pe = pefile.PE(str(Path(module.path)), fast_load=True)
                if pe.FILE_HEADER.TimeDateStamp != module.timestamp or pe.OPTIONAL_HEADER.SizeOfImage != module.size:
                    raise ValueError(f'local image identity mismatch: {module.path}')
                entry = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
                table = list(struct.iter_unpack('<III', pe.get_data(entry.VirtualAddress, entry.Size)))
                images[module.base] = (pe, table, [t[0] for t in table])
            pe, table, starts = images[module.base]
            index = bisect.bisect_right(starts, rva) - 1
            if index >= 0 and table[index][0] <= rva < table[index][1]:
                begin, end, unwind_rva = table[index]
                # Do not pretend to decode a partially executed epilogue.
                if pe.get_data(rva, 1) in (b'\xc3', b'\xc2'):
                    raise ValueError('epilogue PC needs instruction-level unwinding')
                apply_codes(pe, unwind_rva, rva - begin)
            pc = read64(registers['rsp'])
            registers['rsp'] += 8
            if pc == 0:
                return
            registers['rip'] = pc
    except (ValueError, OSError, pefile.PEFormatError) as error:
        print(f'  unwind stopped: {error}')
