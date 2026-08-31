"""Verify the curated knife table against a local VRF VData dump and game VPK.

Read-only game inspection. VRF and Python are development tools, never DLL
runtime requirements. No Valve resource payloads are copied into the product.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re

from inspect_vpk_entries import parse_directory


def verify(source: Path, vdata: Path, vpk: Path) -> dict:
    rows = re.findall(r'KNIFE_ASSET\((\d+),\s*"([^"]+)",\s*"([^"]+)"\)', source.read_text(encoding='utf-8'))
    blocks = dict(re.findall(r'^\t"(\d+)" =\s*\n\t\{\n(.*?)^\t\}', vdata.read_text(encoding='utf-8'), re.M | re.S))
    if len(rows) != 22 or len({row[0] for row in rows}) != len(rows):
        raise ValueError('Expected 22 unique released knife definitions')
    _, _, entries = parse_directory(vpk)
    paths = {entry.path for entry in entries}
    verified = []
    for definition, animation_class, stem in rows:
        body = blocks[definition]
        def field(name: str) -> str:
            match = re.search(r'\b' + re.escape(name) + r' = (?:resource_name:)?"([^"]*)"', body)
            return match[1] if match else ''
        model = f'weapons/models/knife/{stem}/weapon_{stem}.vmdl'
        skeleton = f'animation/skeletons/weapons/{stem}.vnmskel'
        actual_class = field('m_szAnimClass') or field('m_szName')
        if field('m_WeaponType') != 'WEAPONTYPE_KNIFE':
            raise ValueError(f'{definition}: not a knife')
        if (model, skeleton, animation_class) != (field('m_szWorldModel'), field('m_szAnimSkeleton'), actual_class):
            raise ValueError(f'{definition}: model / skeleton / animation class mismatch')
        for resource in (model, skeleton):
            if resource + '_c' not in paths:
                raise ValueError(f'Missing compiled VPK resource: {resource}')
        verified.append({'definition': int(definition), 'animation_class': animation_class,
                         'model': model, 'skeleton': skeleton})
    return {'verified_knives': len(verified), 'verified_resources': 2 * len(verified),
            'vdata_sha256': hashlib.sha256(vdata.read_bytes()).hexdigest(), 'rows': verified}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1] / 'src/Runtime/KnifeModel.cpp')
    parser.add_argument('--vdata', type=Path, required=True)
    parser.add_argument('--vpk', type=Path, required=True)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    report = verify(args.source, args.vdata, args.vpk)
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    print(f"PASS: {report['verified_knives']} knife mappings, {report['verified_resources']} compiled resources")


if __name__ == '__main__':
    main()
