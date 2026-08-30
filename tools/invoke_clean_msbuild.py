"""Launch MSBuild with a case-insensitively deduplicated Windows environment."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys


def clean_environment(temp_directory: Path) -> dict[str, str]:
    environment: dict[str, str] = {}
    seen: set[str] = set()
    for key, value in os.environ.items():
        folded = key.casefold()
        if folded in seen:
            continue
        seen.add(folded)
        environment["PATH" if folded == "path" else key] = value
    environment["TEMP"] = str(temp_directory)
    environment["TMP"] = str(temp_directory)
    return environment


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--msbuild", required=True)
    parser.add_argument("--solution", required=True)
    parser.add_argument("--configuration", default="Release")
    parser.add_argument("--platform", default="x64")
    parser.add_argument("--stdout-log", required=True)
    parser.add_argument("--stderr-log", required=True)
    parser.add_argument(
        "--property",
        action="append",
        default=[],
        help="Additional MSBuild property in NAME=VALUE form (repeatable).",
    )
    arguments = parser.parse_args()

    solution = Path(arguments.solution).resolve()
    temp_directory = solution.parent / "build" / "temp"
    temp_directory.mkdir(parents=True, exist_ok=True)
    command = [
        arguments.msbuild,
        str(solution),
        "/t:Rebuild",
        f"/p:Configuration={arguments.configuration}",
        f"/p:Platform={arguments.platform}",
        "/m:1",
        "/nr:false",
        "/nologo",
        "/v:minimal",
    ]
    command.extend(f"/p:{value}" for value in arguments.property)
    with open(arguments.stdout_log, "w", encoding="utf-8", errors="replace") as stdout_log, open(
        arguments.stderr_log, "w", encoding="utf-8", errors="replace"
    ) as stderr_log:
        completed = subprocess.run(
            command,
            cwd=solution.parent,
            env=clean_environment(temp_directory),
            stdout=stdout_log,
            stderr=stderr_log,
            check=False,
        )
    return completed.returncode


if __name__ == "__main__":
    sys.exit(main())
