"""Run background, isolated-copy native loader tests; never start/control CS2.

This is not a substitute for an independent clean Windows machine. It verifies
loading without developer PATH entries, adjacent runtime DLLs or config files.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from audit_portable_dll import audit


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--english-dll", required=True, type=Path)
    parser.add_argument("--chinese-dll", required=True, type=Path)
    parser.add_argument("--baseline-dll", type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    tests = root / "build/portability-tests"
    probe = tests / "Loader/nSkinzCS2.LoadCheck.exe"
    renderer = tests / "Renderer/nSkinzCS2.RenderCheck.exe"
    stub = tests / "CompilerStub/D3DCOMPILER_47.dll"
    for path in (probe, renderer, stub):
        if not path.is_file():
            raise FileNotFoundError(f"Build tools/PortabilityTests.vcxproj first: {path}")
    report = {
        "scope": "Local isolated loader + offscreen WARP tests; not a clean VM or HLAE gameplay test",
        "audit": [audit(args.english_dll), audit(args.chinese_dll)], "tests": [],
    }
    # Preserve necessary Windows/user environment, but exclude every developer
    # directory from executable/DLL PATH lookup (deduplicate case-insensitively).
    environment = {key.upper(): value for key, value in os.environ.items()}
    environment["PATH"] = str(Path(environment["SYSTEMROOT"]) / "System32")
    environment["NoDefaultCurrentDirectoryInExePath"] = "1"
    with tempfile.TemporaryDirectory(prefix="loadcheck-", dir=tests) as temporary:
        staging = Path(temporary)

        def run(name, source, *, system_only=False, compiler_stub=False, expected=0):
            directory = staging / name / "中文目录 with spaces"
            directory.mkdir(parents=True)
            target = directory / source.name
            shutil.copy2(source, target)
            executable = directory / probe.name
            shutil.copy2(probe, executable)
            if compiler_stub:
                shutil.copy2(stub, directory / stub.name)
            command = [str(executable), str(target)]
            if system_only:
                command.append("--system-only")
            completed = subprocess.run(command, cwd=directory, env=environment,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30,
                creationflags=subprocess.CREATE_NO_WINDOW)
            output = completed.stdout.decode("utf-8", errors="replace")
            passed = completed.returncode == expected
            if passed and expected == 0:
                # No config/catalog/log sidecars should be needed just to load.
                allowed = {target.name, executable.name}
                if compiler_stub:
                    allowed.add(stub.name)
                passed = {p.name for p in directory.iterdir()} == allowed
                if "d3dcompiler_" in output.lower() and source != args.baseline_dll:
                    passed = False
            report["tests"].append({"name": name, "passed": passed,
                "expected_exit": expected, "exit": completed.returncode,
                "input_sha256": hashlib.sha256(source.read_bytes()).hexdigest(), "output": output})
            print(f"{'PASS' if passed else 'FAIL'}: {name} (exit {completed.returncode}, expected {expected})")

        for language, dll in (("en", args.english_dll), ("zh", args.chinese_dll)):
            run(f"{language}-hlae-loadlibrary", dll)
            run(f"{language}-system32-only", dll, system_only=True)
            run(f"{language}-incompatible-compiler", dll, compiler_stub=True)
        if args.baseline_dll:
            run("v041-baseline", args.baseline_dll)
            run("v041-incompatible-compiler", args.baseline_dll, compiler_stub=True, expected=127)
        render_dir = staging / "headless-renderer"
        render_dir.mkdir()
        shutil.copy2(renderer, render_dir / renderer.name)
        shutil.copy2(stub, render_dir / stub.name)
        completed = subprocess.run([str(render_dir / renderer.name)], cwd=render_dir,
            env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30,
            creationflags=subprocess.CREATE_NO_WINDOW)
        report["tests"].append({"name": "headless-renderer-with-incompatible-compiler",
            "passed": completed.returncode == 0, "exit": completed.returncode,
            "output": completed.stdout.decode("utf-8", errors="replace")})
        print(report["tests"][-1]["output"].strip())
    report["passed"] = all(test["passed"] for test in report["tests"])
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
