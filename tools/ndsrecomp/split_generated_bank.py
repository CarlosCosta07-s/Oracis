#!/usr/bin/env python3
"""Split an Oracis-adapted NDSRecomp C bank into bounded C translation units.

The original monolithic C file and generated function header are retained.
Shards contain only function bodies and include the authoritative header, which
provides all cross-shard prototypes. Game-derived outputs remain local.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import re
import shutil
import sys
import tempfile
from pathlib import Path

PATCHER_PATH = Path(__file__).with_name("patch_memory_access_direction.py")
PATCHER_SPEC = importlib.util.spec_from_file_location(
    "oracis_splitter_c_lexer", PATCHER_PATH
)
if PATCHER_SPEC is None or PATCHER_SPEC.loader is None:
    raise RuntimeError(f"Cannot load C source masker: {PATCHER_PATH}")
PATCHER = importlib.util.module_from_spec(PATCHER_SPEC)
PATCHER_SPEC.loader.exec_module(PATCHER)

FUNCTION_DEFINITION = re.compile(
    r"(?m)^void\s+([A-Za-z_]\w*)\s*\(\s*void\s*\)\s*\{"
)
FUNCTION_DECLARATION = re.compile(
    r"(?m)^\s*void\s+([A-Za-z_]\w*)\s*\(\s*void\s*\)\s*;"
)
FUNCTION_PROTOTYPE = re.compile(
    r"(?m)^\s*void\s+[A-Za-z_]\w*\s*\(\s*void\s*\)\s*;[^\r\n]*$"
)
INCLUDE_LINE = re.compile(r"(?m)^\s*#\s*include\s*[<\"][^>\"]+[>\"]\s*$")
SHARD_NAME = re.compile(r"bank_part_[0-9]{4}\.c")


def sha1_file(path: Path) -> str:
    digest = hashlib.sha1()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _function_bodies(source: str, header: str) -> list[tuple[int, int, str]]:
    masked = PATCHER._mask_non_code(source)
    definitions = list(FUNCTION_DEFINITION.finditer(masked))
    if not definitions:
        raise ValueError("generated C bank contains no function definitions")

    declared_names = set(FUNCTION_DECLARATION.findall(header))
    missing = [match.group(1) for match in definitions
               if match.group(1) not in declared_names]
    if missing:
        raise ValueError(
            "generated header is missing function prototypes: "
            + ", ".join(missing[:8])
        )

    # Do not silently discard file-scope state or helper definitions when
    # splitting. The generated bank format is includes + prototypes + bodies.
    prefix = source[:definitions[0].start()]
    prefix = INCLUDE_LINE.sub("", prefix)
    prefix_masked = PATCHER._mask_non_code(prefix)
    prefix_masked = FUNCTION_PROTOTYPE.sub("", prefix_masked)
    if prefix_masked.strip():
        raise ValueError(
            "generated C bank has unsupported file-scope content before its functions"
        )

    brace_iter = iter(re.finditer(r"[{}]", masked))
    next_brace = next(brace_iter, None)
    bodies: list[tuple[int, int, str]] = []
    previous_end = definitions[0].start()
    for definition in definitions:
        gap = PATCHER._mask_non_code(source[previous_end:definition.start()])
        if gap.strip():
            raise ValueError(
                "generated C bank has unsupported file-scope content between functions"
            )
        opening = definition.end() - 1
        while next_brace is not None and next_brace.start() < opening:
            next_brace = next(brace_iter, None)
        if (next_brace is None or next_brace.start() != opening
                or next_brace.group(0) != "{"):
            raise ValueError(
                f"function body opener missing for {definition.group(1)}"
            )

        depth = 1
        closing_end: int | None = None
        next_brace = next(brace_iter, None)
        while next_brace is not None:
            if next_brace.group(0) == "{":
                depth += 1
            else:
                depth -= 1
                if depth == 0:
                    closing_end = next_brace.end()
                    next_brace = next(brace_iter, None)
                    break
            next_brace = next(brace_iter, None)
        if closing_end is None:
            raise ValueError(f"unterminated function {definition.group(1)}")
        bodies.append((definition.start(), closing_end, definition.group(1)))
        previous_end = closing_end

    suffix = PATCHER._mask_non_code(source[bodies[-1][1]:])
    if suffix.strip():
        raise ValueError(
            "generated C bank has unsupported file-scope content after its functions"
        )
    return bodies


def split_generated_bank(
    source_path: Path,
    header_path: Path,
    output_dir: Path,
    bank_name: str,
    functions_per_shard: int = 512,
) -> dict:
    """Split a generated bank and return manifest metadata for the shard set."""
    if not re.fullmatch(r"[A-Za-z_]\w*", bank_name):
        raise ValueError(f"invalid bank name: {bank_name!r}")
    if type(functions_per_shard) is not int or functions_per_shard < 1:
        raise ValueError("functions_per_shard must be a positive integer")
    for path, label in ((source_path, "source"), (header_path, "header")):
        if path.is_symlink() or not path.is_file():
            raise FileNotFoundError(
                f"generated bank {label} is missing or unsafe: {path}"
            )
    if output_dir.is_symlink():
        raise ValueError(
            f"shard output directory must not be a symlink: {output_dir}"
        )
    output_dir.mkdir(parents=True, exist_ok=True)
    if not output_dir.is_dir():
        raise ValueError(f"shard output path is not a directory: {output_dir}")

    source = source_path.read_text(encoding="utf-8")
    header = header_path.read_text(encoding="utf-8")
    bodies = _function_bodies(source, header)
    shard_dir_name = f"{bank_name}_shards"
    target_dir = output_dir / shard_dir_name

    with tempfile.TemporaryDirectory(
        prefix=f".{shard_dir_name}.staging-", dir=output_dir
    ) as staging_name:
        staging_dir = Path(staging_name)
        file_hashes: dict[str, str] = {}
        preamble = (
            "/* Generated by Oracis split_generated_bank.py. Do not edit. */\n"
            '#include "runtime_arm.h"\n'
            f'#include "{header_path.name}"\n\n'
        )
        for shard_index, first in enumerate(
            range(0, len(bodies), functions_per_shard)
        ):
            group = bodies[first:first + functions_per_shard]
            filename = f"bank_part_{shard_index:04d}.c"
            destination = staging_dir / filename
            with destination.open("w", encoding="utf-8", newline="\n") as stream:
                stream.write(preamble)
                for start, end, _name in group:
                    stream.write(source[start:end])
                    stream.write("\n\n")
            file_hashes[filename] = sha1_file(destination)

        if target_dir.exists() or target_dir.is_symlink():
            if target_dir.is_symlink() or not target_dir.is_dir():
                raise ValueError(
                    f"existing shard path is not a safe directory: {target_dir}"
                )
            for entry in target_dir.iterdir():
                if (entry.is_symlink() or not entry.is_file()
                        or not SHARD_NAME.fullmatch(entry.name)):
                    raise ValueError(
                        "existing shard directory contains unrelated or unsafe "
                        f"content: {entry}"
                    )

        backup_dir: Path | None = None
        if target_dir.exists():
            backup_name = tempfile.mkdtemp(
                prefix=f".{shard_dir_name}.backup-", dir=output_dir
            )
            backup_dir = Path(backup_name)
            backup_dir.rmdir()
            target_dir.replace(backup_dir)
        try:
            staging_dir.replace(target_dir)
        except OSError:
            if backup_dir is not None and backup_dir.exists():
                backup_dir.replace(target_dir)
            raise
        else:
            if backup_dir is not None:
                shutil.rmtree(backup_dir)

    return {
        "directory": shard_dir_name,
        "functions_per_shard": functions_per_shard,
        "function_count": len(bodies),
        "files": file_hashes,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--header", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--bank", required=True)
    parser.add_argument("--functions-per-shard", type=int, default=512)
    args = parser.parse_args()
    try:
        result = split_generated_bank(
            args.source, args.header, args.output_dir,
            args.bank, args.functions_per_shard,
        )
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(
        f"{args.bank}: split {result['function_count']} functions into "
        f"{len(result['files'])} translation units under "
        f"{args.output_dir / result['directory']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
