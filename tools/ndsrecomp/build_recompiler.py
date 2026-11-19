#!/usr/bin/env python3
"""Build only the pinned NDSRecomp static compiler for local Oracis work.

The checkout is external and uses sparse paths only: recompiler/, cmake/, and
external/arm-recomp-core/. runner/, oracle/, and melonDS-derived trees are not
checked out, configured, compiled, linked, or distributed by this tool.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

UPSTREAM_URL = "https://github.com/RetroPortingToolKit/ndsrecomp.git"
PINNED_REVISION = "14b7e15023e9affc9dc375e4e611ac703f5c76ab"
SPARSE_PATHS = {"recompiler", "cmake", "external/arm-recomp-core"}
ROOT = Path(__file__).resolve().parents[2]


def run(command: list[str]) -> str:
    print("+", " ".join(str(part) for part in command), flush=True)
    completed = subprocess.run(
        command,
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


def ignore_generated_toml_header(source: Path) -> None:
    """Keep NDSRecomp's downloaded toml++ staging header out of Git status."""
    exclude_value = git(source, "rev-parse", "--git-path", "info/exclude")
    exclude_path = Path(exclude_value)
    if not exclude_path.is_absolute():
        exclude_path = source / exclude_path
    exclude_path.parent.mkdir(parents=True, exist_ok=True)
    current = exclude_path.read_text(encoding="utf-8") if exclude_path.exists() else ""
    pattern = "/recompiler/third_party/"
    if pattern not in current.splitlines():
        with exclude_path.open("a", encoding="utf-8", newline="\n") as stream:
            if current and not current.endswith("\n"):
                stream.write("\n")
            stream.write(pattern + "\n")


def configure_sparse_checkout(source: Path) -> None:
    git(source, "sparse-checkout", "init", "--cone")
    git(
        source, "sparse-checkout", "set",
        "recompiler", "cmake", "external/arm-recomp-core",
    )
    sparse_paths = set(git(source, "sparse-checkout", "list").splitlines())
    if sparse_paths != SPARSE_PATHS:
        raise RuntimeError(
            "The NDSRecomp checkout does not match the approved sparse paths: "
            f"{sorted(sparse_paths)}"
        )


def ensure_checkout(source: Path) -> None:
    if not shutil.which("git"):
        raise RuntimeError("Git is required. Install Git and retry.")

    # Git can clone into a pre-created empty directory. Accept that case,
    # but never treat a non-empty directory or symlink as a fresh destination.
    fresh_destination = (
        not source.exists()
        or (
            source.is_dir()
            and not source.is_symlink()
            and not any(source.iterdir())
        )
    )

    if fresh_destination:
        source.parent.mkdir(parents=True, exist_ok=True)
        run([
            "git", "clone", "--filter=blob:none", "--no-checkout",
            UPSTREAM_URL, str(source),
        ])
        configure_sparse_checkout(source)
        git(source, "fetch", "--depth=1", "origin", PINNED_REVISION)
        git(source, "checkout", "--detach", PINNED_REVISION)
    else:
        if not (source / ".git").exists():
            raise RuntimeError(
                f"{source} exists but is not a Git checkout. "
                "Choose another path with --source."
            )
        remote = git(source, "remote", "get-url", "origin").rstrip("/")
        if remote != UPSTREAM_URL.rstrip("/"):
            raise RuntimeError(
                f"Unexpected origin in {source}: {remote}. "
                "Use a fresh empty directory with --source."
            )
        head = git(source, "rev-parse", "HEAD")
        if head != PINNED_REVISION:
            raise RuntimeError(
                f"Existing NDSRecomp checkout is at {head}, not the pinned "
                f"revision {PINNED_REVISION}. The script will not reset or "
                "overwrite it; use a fresh path with --source."
            )
        status = git(source, "status", "--porcelain")
        if status:
            raise RuntimeError(
                "The existing NDSRecomp checkout has local modifications. "
                "The bootstrap will not overwrite it; use a clean checkout."
            )
        # Existing checkouts are only accepted when already sparse and free of
        # the excluded runner/oracle source trees.
        sparse_paths = set(git(source, "sparse-checkout", "list").splitlines())
        if sparse_paths != SPARSE_PATHS:
            raise RuntimeError(
                "The existing checkout is not restricted to the approved "
                "sparse paths. Use a fresh empty directory with --source."
            )

    head = git(source, "rev-parse", "HEAD")
    if head != PINNED_REVISION:
        raise RuntimeError(f"Pin mismatch: expected {PINNED_REVISION}, got {head}")

    if (source / "runner").exists() or (source / "oracle").exists():
        raise RuntimeError(
            "Excluded runner/oracle directories are present. Use a fresh sparse "
            "checkout; Oracis must not include melonDS-derived source trees."
        )

    # Only this MIT-licensed compiler dependency is initialized. FreeBIOS and
    # any runtime/oracle dependencies remain absent.
    git(
        source, "submodule", "update", "--init",
        "--depth", "1", "--", "external/arm-recomp-core",
    )
    if not (source / "external/arm-recomp-core" / "CMakeLists.txt").exists():
        raise RuntimeError(
            "The arm-recomp-core submodule is missing or incomplete."
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source",
        type=Path,
        default=ROOT.parent / "ndsrecomp",
        help="External sparse checkout (default: sibling ../ndsrecomp).",
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
    ignore_generated_toml_header(source)
    build_dir.mkdir(parents=True, exist_ok=True)

    # Configure only recompiler/. The sparse checkout has no runner/, oracle/,
    # frontend, melonDS or FreeBIOS source tree.
    run([
        "cmake", "-S", str(source / "recompiler"), "-B", str(build_dir),
        "-G", args.generator, "-DCMAKE_BUILD_TYPE=Release",
    ])
    compiler_targets = [
        "nds_recompile",
        "armv5te_decode_test",
        "interpreter_cycle_test",
        "reloc_scan_test",
        "hle_manifest_test",
        "interior_entry_fail_closed_test",
        "codegen_golden_test",
        "live_transfer_validation_test",
    ]
    run([
        "cmake", "--build", str(build_dir), "--config", "Release",
        "--target", *compiler_targets, "--parallel",
    ])

    executable_suffix = ".exe" if sys.platform == "win32" else ""
    def locate_executable(target: str) -> Path:
        candidates = list(build_dir.rglob(target + executable_suffix))
        if not candidates:
            raise RuntimeError(
                f"Build returned success but {target}{executable_suffix} "
                f"was not found under {build_dir}"
            )
        return candidates[0]

    recompiler = locate_executable("nds_recompile")
    for test_target in ("armv5te_decode_test", "interpreter_cycle_test"):
        run([str(locate_executable(test_target))])

    # Exercise the real extractor output schema against the pinned compiler,
    # using only a copyright-free synthetic ARM/Thumb image.
    run([
        sys.executable,
        str(ROOT / "tests" / "tools" / "test_extractor_recompiler.py"),
        "--recompiler",
        str(recompiler),
    ])

    # These tests need only the static recompiler and its local ARM core.
    # live_shard_identity_test is intentionally excluded because it requires
    # runner/tests and tools/compile_live_shards.py outside the sparse checkout.
    run([
        "ctest", "--test-dir", str(build_dir), "--output-on-failure",
        "-R",
        "^(reloc_scan_test|hle_manifest_test|interior_entry_fail_closed|"
        "codegen_golden|output_errors|live_transfer_validation)$",
    ])

    print(f"Static recompiler ready: {recompiler}")
    print(f"Upstream revision: {PINNED_REVISION}")
    print("Built compiler/test targets only; runner/oracle/melonDS are excluded.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1)
