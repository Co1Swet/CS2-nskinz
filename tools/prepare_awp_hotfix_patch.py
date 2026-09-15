"""Prepare an apply_patch delta containing only the post-checkpoint AWP fix.

Reads the immutable published source + knife checkpoint. Does not edit files.
The caller applies the resulting patch to an isolated v0.4.3 source copy.
"""
import difflib
from pathlib import Path
import sys
import zipfile

root = Path(__file__).resolve().parents[1]
published = root / '.tmp/publish-v0.4.3/source'
destination = root / 'build/awp-stock-fix/source'
files = ['src/Cs2/Runtime.cpp', 'src/Cs2/Runtime.hpp', 'src/Tests/Main.cpp',
         'src/Runtime/WeaponMaterialInit.hpp', 'nSkinzCS2.vcxproj', 'nSkinzCS2.Tests.vcxproj']
sys.stdout.reconfigure(encoding='utf-8')
print('*** Begin Patch')
with zipfile.ZipFile(root / 'build/checkpoints/knife-paused-before-awp-20260831/source-checkpoint.zip') as checkpoint:
    entries = {name.replace('\\', '/'): name for name in checkpoint.namelist()}
    for name in files:
        new = (root / name).read_text(encoding='utf-8-sig')
        target = destination / name
        if name not in entries:
            if target.exists():
                old = target.read_text(encoding='utf-8-sig')
            else:
                print(f'*** Add File: {target.as_posix()}')
                print('\n'.join('+' + line for line in new.splitlines()))
                continue
            desired = new
        else:
            old_checkpoint = checkpoint.read(entries[name]).decode('utf-8-sig').splitlines(keepends=True)
            old_checkpoint = [line.replace('\r\n', '\n') for line in old_checkpoint]
            current = new.splitlines(keepends=True)
            desired = (published / name).read_text(encoding='utf-8-sig')
            for group in difflib.SequenceMatcher(None, old_checkpoint, current, autojunk=False).get_grouped_opcodes(3):
                left = group[0][2] - group[0][1] if group[0][0] == 'equal' else 0
                right = group[-1][2] - group[-1][1] if group[-1][0] == 'equal' else 0
                match = None
                for trim_left in range(left + 1):
                    for trim_right in range(right + 1):
                        before = ''.join(old_checkpoint[group[0][1] + trim_left:group[-1][2] - trim_right])
                        after = ''.join(current[group[0][3] + trim_left:group[-1][4] - trim_right])
                        if before and desired.count(before) == 1:
                            match = before, after
                            break
                    if match: break
                if match is None:
                    raise RuntimeError(f'AWP delta does not uniquely match v0.4.3: {name} {group[0]}')
                before, after = match
                desired = desired.replace(before, after, 1)
            old = target.read_text(encoding='utf-8-sig')
        diff = list(difflib.unified_diff(old.splitlines(), desired.splitlines(), n=3, lineterm=''))
        if diff:
            print(f'*** Update File: {target.as_posix()}')
            for line in diff[2:]:
                print('@@' if line.startswith('@@') else line)
print('*** End Patch')
