"""Extract one valid default mount per model variant from compiled KeychainMarkup.

Only derived point coordinates and resource hashes are saved, not Valve assets.
The center of the model's explicitly marked DefaultLocation quad is used.
"""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import struct
import tempfile


def generate(model_dir, vrf_cli):
    result = {}
    for path in sorted(model_dir.glob("*.vmdl_c"), key=lambda p: int(p.stem)):
        resource = path.read_bytes()
        table_offset, block_count = struct.unpack_from("<II", resource, 8)
        data = None
        for index in range(block_count):
            entry_offset = 8 + table_offset + index * 12
            name, relative_offset, size = struct.unpack_from("<4sII", resource, entry_offset)
            if name == b"DATA":
                start = entry_offset + 4 + relative_offset
                data = resource[start:start + size]
                if len(data) != size:
                    raise ValueError(f"{path}: truncated DATA block")
                break
        if data is None:
            raise ValueError(f"{path}: no DATA block")
        # Isolate metadata from unrelated RED2 editor information. Some installed
        # models use newer RED2 types that the bundled CLI cannot decode, even
        # though their DATA placement metadata is fully supported.
        header = struct.pack("<IHHII4sII", 28 + len(data), 12, 1, 8, 1, b"DATA", 8, len(data))
        with tempfile.TemporaryDirectory(prefix="keychain-model-", dir=model_dir.parent) as temporary:
            assert pathlib.Path(temporary).resolve().is_relative_to(model_dir.parent.resolve())
            isolated = pathlib.Path(temporary) / path.name
            isolated.write_bytes(header + data)
            text = subprocess.run(
                [str(vrf_cli.resolve()), "-i", str(isolated), "-b", "DATA"], check=True,
                capture_output=True, text=True, encoding="utf-8", errors="strict",
            ).stdout
        section = re.search(r"\bKeychainMarkup\s*=\s*\[", text)
        if not section:
            raise ValueError(f"{path}: no KeychainMarkup")
        depth, end = 1, section.end()
        while depth and end < len(text):
            depth += (text[end] == "[") - (text[end] == "]")
            end += 1
        if depth:
            raise ValueError(f"{path}: unterminated KeychainMarkup")
        variants = {}
        for match in re.finditer(r"\{([^{}]*)\}", text[section.end():end-1], re.S):
            entry = match[1]
            if not re.search(r"DefaultLocation\s*=\s*true", entry):
                continue
            corners = re.search(r"Corners\s*=\s*\[(.*?)\]", entry, re.S)[1]
            values = [float(v) for v in re.findall(r"[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?", corners)]
            if len(values) != 12:
                raise ValueError(f"{path}: expected four XYZ corners, got {len(values)}")
            variant = "legacy" if re.search(r"LegacyModel\s*=\s*true", entry) else "modern"
            variants.setdefault(variant, {
                "position": [sum(values[axis::3]) / 4 for axis in range(3)],
                "bone": re.search(r'BoneName\s*=\s*"([^"]+)"', entry)[1],
            })
        if not variants:
            raise ValueError(f"{path}: no valid placement quad")
        result[path.stem] = {"sha256": hashlib.sha256(path.read_bytes()).hexdigest(), **variants}
    if not result:
        raise ValueError("no compiled weapon models found")
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model-dir", type=pathlib.Path, required=True)
    parser.add_argument("--vrf-cli", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    mounts = generate(args.model_dir, args.vrf_cli)
    args.output.write_text(json.dumps(mounts, indent=2) + "\n", encoding="utf-8")
    print(f"{len(mounts)} weapon models -> {args.output}")
