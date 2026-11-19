#!/usr/bin/env python3
"""Build only the pinned NDSRecomp static compiler for local Oracis work.

The NDSRecomp checkout is kept outside the Oracis repository. This script
never configures or builds the upstream runner, melonDS-derived devices,
oracle, or frontend.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

UPSTREAM_URL = "https://github.com/RetroPortingToolKit/ndsrecomp.git"
PINNED_REVISION = "14b7e15023e9affc9dc375e4e611ac703f5c76ab"
ROOT = Path(__file__).resolve().parents[2]


def run(command: list[str], *, cwd: Path | None = None) -> str:
    print("+", " ".join(str(part) for part in command), flush=True)
    completed = subprocess.run(
        command,
        cwd=cwd,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    if completed.stdout:
        print(completed.stdout, end="", flush=True)
    if completed.returncode != 0:
        raise RuntimeError(
            f"Command failed with exit code {completed.returncode}: "
            + " ".join(str(part) for part in command)
        )
    return completed.stdout.strip()


def git(source: Path, *args: str) -> str:
    return run(["git", "-C", str(source), *args])


def ensure_checkout(source: Path) -> None:
    if not shutil.which("git"):
        raise RuntimeError("Git is required. Install Git and retry.")

    if not source.exists():
        source.parent.mkdir(parents=True, exist_ok=True)
        run([
            "git", "clone", "--filter=blob:none", "--no-checkout",
            UPSTREAM_URL, str(source),
        ])
        run(["git", "-C", str(source), "fetch", "--depth=1", "origin",
             PINNED_REVISION])
        run(["git", "-C", str(source), "checkout", "--detach",
             PINNED_REVISION])
    else:
        if not (source / ".git").exists():
            raise RuntimeError(
                f"{source} exists but is not a Git checkout. "
                "Choose another path with --source."
            )
        remote = git(source, "remote", "get-url", "origin")
        normalized_remote = remote.rstrip("/")
        normalized_expected = UPSTREAM_URL.rstrip("/")
        if normalized_remote != normalized_expected:
            raise RuntimeError(
                f"Unexpected origin in {source}: {remote}. "
                "Use a fresh directory with --source."
            )
        head = git(source, "rev-parse", "HEAD")
        if head != PINNED_REVISION:
            status = git(source, "status", "--porcelain")
            detail = " The checkout has local changes." if status else ""
            raise RuntimeError(
                f"Existing NDSRecomp checkout is at {head}, not the pinned "
                f"revision {PINNED_REVISION}.{detail} "
                "The script will not reset or overwrite an existing checkout. "
                "Use a fresh path with --source."
            )

    head = git(source, "rev-parse", "HEAD")
    if head != PINNED_REVISION:
        raise RuntimeError(f"Pin mismatch: expected {PINNED_REVISION}, got {head}")

    # The compiler uses this MIT-licensed ARM core submodule. Do not initialize
    # freebios or any runtime/oracle dependencies.
    run([
        "git", "-C", str(source), "submodule", "update", "--init",
        "--depth", "1", "--", "external/arm-recomp-core",
    ])

    if not (source / "external/arm-recomp-core" / "CMakeLists.txt").exists():
        raise RuntimeError(
            "The arm-recomp-core submodule is missing or incomplete."
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--source",
        type=Path,
        default=ROOT.parent / "ndsrecomp",
        help="External NDSRecomp source checkout (default: sibling ../ndsrecomp).",
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=ROOT / ".oracis" / "ndsrecomp-build",
        help="Local compiler build directory (default: .oracis/ndsrecomp-build).",
    )
    parser.add_argument(
        "--generator",
        default="Ninja",
        help="CMake generator (default: Ninja).",
    )
    args = parser.parse_args()

    if not shutil.which("cmake"):
        raise RuntimeError("CMake 3.20 or newer is required.")
    if args.generator == "Ninja" and not shutil.which("ninja"):
        raise RuntimeError("Ninja is required for the selected generator.")

    source = args.source.expanduser().resolve()
    build_dir = args.build_dir.expanduser().resolve()

    ensure_checkout(source)
    build_dir.mkdir(parents=True, exist_ok=True)

    # Configure upstream recompiler directory, then build only the
    # nds_recompile target. runner/ is not configured as a build target.
    run([
        "cmake", "-S", str(source / "recompiler"), "-B", str(build_dir),
        "-G", args.generator, "-DCMAKE_BUILD_TYPE=Release",
    ])
    run([
        "cmake", "--build", str(build_dir), "--target", "nds_recompile",
        "--parallel",
    ])

    executable = build_dir / ("nds_recompile.exe" if sys.platform == "win32"
                              else "nds_recompile")
    if not executable.exists():
        raise RuntimeError(
            f"Build returned success but executable was not found: {executable}"
        )

    print(f"Static recompiler ready: {executable}")
    print(f"Upstream revision: {PINNED_REVISION}")
    print("Built target: nds_recompile only (no runner/oracle/melonDS target).")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1)
