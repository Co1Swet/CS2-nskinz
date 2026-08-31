"""Verify the v0.4.4 stock-weapon fix without launching/controlling CS2."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--tests', type=Path, required=True)
parser.add_argument('--cs2-root', type=Path, required=True)
parser.add_argument('--items-game', type=Path, required=True)
parser.add_argument('--localization', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
client = args.cs2_root / 'game/csgo/bin/win64/client.dll'
inputs = {'tests': args.tests, 'client': client, 'items_game': args.items_game, 'localization': args.localization}
report = {'scope': 'Test-process fixtures and read-only PE/catalog validation; no CS2 visual acceptance',
          'inputs': {key: {'path': str(path.resolve()), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
                     for key, path in inputs.items()}, 'runs': []}
environment = {key.upper(): value for key, value in os.environ.items()}
environment.update(NSKINZCS2_KNIFE_CLIENT_DLL=str(client.resolve()),
                   NSKINZCS2_REAL_ITEMS_GAME=str(args.items_game.resolve()),
                   NSKINZCS2_REAL_LOCALIZATION=str(args.localization.resolve()))
args.output.mkdir(parents=True, exist_ok=True)
for name, flags in [('weapon-paints', ['--weapon-paint-only']), ('full', [])]:
    print(f'Starting {name}', flush=True)
    start = time.monotonic()
    result = subprocess.run([str(args.tests.resolve()), *flags], env=environment,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=600,
                            creationflags=subprocess.CREATE_NO_WINDOW)
    output = result.stdout.decode('utf-8', errors='strict')
    (args.output / f'{name}.log').write_text(output, encoding='utf-8')
    entry = {'name': name, 'exit': result.returncode, 'seconds': round(time.monotonic() - start, 3),
             'test_entries': output.count('[TEST]'),
             'passed': result.returncode == 0 and 'standard_weapon_material_native_evidence=PASS' in output}
    report['runs'].append(entry)
    print(json.dumps(entry), flush=True)
report['passed'] = all(entry['passed'] for entry in report['runs'])
(args.output / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
raise SystemExit(0 if report['passed'] else 1)
