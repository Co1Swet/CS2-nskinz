"""Background knife-graph regression against on-disk PE files; never starts CS2."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tests", required=True, type=Path)
    parser.add_argument("--cs2-root", required=True, type=Path)
    parser.add_argument("--items-game", required=True, type=Path)
    parser.add_argument("--localization", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    client = args.cs2_root / "game/csgo/bin/win64/client.dll"
    animation = args.cs2_root / "game/bin/win64/animationsystem.dll"
    inputs = {"tests": args.tests, "client": client, "animation": animation,
              "items_game": args.items_game, "localization": args.localization}
    for path in inputs.values():
        if not path.is_file():
            raise FileNotFoundError(path)
    environment = {key.upper(): value for key, value in os.environ.items()}
    environment.update({
        "NSKINZCS2_KNIFE_CLIENT_DLL": str(client.resolve()),
        "NSKINZCS2_KNIFE_ANIMATION_DLL": str(animation.resolve()),
        "NSKINZCS2_REAL_ITEMS_GAME": str(args.items_game.resolve()),
        "NSKINZCS2_REAL_LOCALIZATION": str(args.localization.resolve()),
    })
    args.output.mkdir(parents=True, exist_ok=True)
    report = {"scope": "Test process + on-disk game files; no CS2 launch/injection/desktop control",
              "inputs": {name: {"path": str(path.resolve()),
                                 "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                         for name, path in inputs.items()}, "runs": []}
    for name, flags in (("knife-graph", ["--knife-model-only"]),
                        ("weapon-paints", ["--weapon-paint-only"]), ("full", [])):
        start = time.monotonic()
        print(f"Starting {name}", flush=True)
        completed = subprocess.run([str(args.tests.resolve()), *flags], cwd=root, env=environment,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=600,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        output = completed.stdout.decode("utf-8", errors="replace")
        (args.output / f"{name}.log").write_text(output, encoding="utf-8")
        passed = completed.returncode == 0
        if name != "weapon-paints":
            passed = passed and "knife_graph_installed_evidence=PASS" in output
        entry = {"name": name, "passed": passed, "exit": completed.returncode,
                 "seconds": round(time.monotonic() - start, 3), "test_entries": output.count("[TEST]")}
        report["runs"].append(entry)
        print(json.dumps(entry), flush=True)
    report["passed"] = all(run["passed"] for run in report["runs"])
    (args.output / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
