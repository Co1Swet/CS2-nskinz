"""Verify model-owned AG2 skeleton refs in all 22 local knife VMDL resources.

Uses VRF only as a development decoder. No game writes, launches or injection;
no extracted Valve resource is included in the product or report.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

from inspect_vpk_entries import parse_directory, extract_entry


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vpk', required=True, type=Path)
    parser.add_argument('--vrf', required=True, type=Path)
    parser.add_argument('--assets', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    header, tree, entries = parse_directory(args.vpk)
    entries = {entry.path: entry for entry in entries}
    assets = json.loads(args.assets.read_text(encoding='utf-8'))['rows']
    if len(assets) != 22:
        raise ValueError('expected verified 22-knife asset table')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    rows = []
    with tempfile.TemporaryDirectory(prefix='model-skeleton-', dir=args.output.parent) as directory:
        for asset in assets:
            payload = extract_entry(args.vpk, header, tree, entries[asset['model'] + '_c'])
            source = Path(directory) / 'knife.vmdl_c'
            source.write_bytes(payload)
            decoded = subprocess.run([str(args.vrf.resolve()), '-i', str(source.resolve()), '-b', 'DATA'],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True, timeout=30,
                creationflags=subprocess.CREATE_NO_WINDOW)
            text = decoded.stdout.decode('utf-8', errors='strict')
            match = re.search(r'm_vecNmSkeletonRefs\s*=\s*\[(.*?)\]', text, re.S)
            if not match:
                raise ValueError(f"{asset['definition']}: missing model-owned skeleton refs")
            refs = re.findall(r'resource:\s*"([^"]+)"', match[1])
            if refs != [asset['skeleton']]:
                raise ValueError(f"{asset['definition']}: VMDL skeleton refs {refs} != VData {asset['skeleton']}")
            rows.append(dict(definition=asset['definition'], model=asset['model'], skeleton=refs[0],
                model_sha256=hashlib.sha256(payload).hexdigest()))
    args.output.write_text(json.dumps(dict(verified=len(rows), scope='local VPK/VRF model-owned skeleton refs',
        vrf_sha256=hashlib.sha256(args.vrf.read_bytes()).hexdigest(), rows=rows), indent=2), encoding='utf-8')
    print(f'PASS: {len(rows)} knife VMDLs each own the expected matching AG2 skeleton resource')


if __name__ == '__main__':
    main()
