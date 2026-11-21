#!/usr/bin/env python3
"""Emit local, identity-validated Pokémon Platinum USA Rev 1 static banks.

The source ROM, extracted binaries, TOML configuration and generated C sources
stay outside Git. This tool only consumes files created by the local extractor.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Callable

from patch_memory_access_direction import (
    ADAPTER_IDENTITY,
    patch_generated_source,
)

ROOT = Path(__file__).resolve().parents[2]
EXPECTED_ROM_SHA1 = "0862ec35b24de5c7e2dcb88c9eea0873110d755c"
EXPECTED_CODEGEN_IDENTITY = "nds-codegen-v3"
BANK_PREFIX = "pokemon_platinum_usa_rev1"
CPUS = ("arm9", "arm7")
EXPECTED_SEGMENTS = {
    "arm9": {
        "rom_offset": 0x00004000,
        "entry_pc_raw": 0x02000800,
        "load_address": 0x02000000,
        "size": 0x001023F8,
        "entry_pc": 0x02000800,
        "entry_mode": "arm",
        "sha1": "7ec963e737f0f61efcbbf5c7a3d33cbf378143f8",
    },
    "arm7": {
        "rom_offset": 0x00409800,
        "entry_pc_raw": 0x02380000,
        "load_address": 0x02380000,
        "size": 0x000277FC,
        "entry_pc": 0x02380000,
        "entry_mode": "arm",
        "sha1": "082bb349ae7d057edbbd91a07316b1402b18b803",
    },
}


def sha1_file(path: Path) -> str:
    digest = hashlib.sha1()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def run_command(command: list[str]) -> str:
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
    return completed.stdout


def validate_extraction(extracted_dir: Path) -> tuple[dict, dict[str, dict]]:
    manifest_path = extracted_dir / "manifest.json"
    if not manifest_path.is_file():
        raise FileNotFoundError(f"Extraction manifest not found: {manifest_path}")

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("schema_version") != 1:
        raise ValueError("Unsupported extraction manifest schema version")
    if manifest.get("rom_sha1") != EXPECTED_ROM_SHA1:
        raise ValueError(
            "ROM identity mismatch in extraction manifest: "
            f"expected {EXPECTED_ROM_SHA1}, got {manifest.get('rom_sha1')!r}"
        )

    records = manifest.get("segments")
    if not isinstance(records, list) or len(records) != 2:
        raise ValueError("Manifest must contain exactly ARM9 and ARM7 segments")

    by_cpu: dict[str, dict] = {}
    for record in records:
        if not isinstance(record, dict):
            raise ValueError("Malformed segment record in extraction manifest")
        cpu = record.get("cpu")
        if cpu not in CPUS or cpu in by_cpu:
            raise ValueError(f"Unexpected or duplicate CPU segment: {cpu!r}")

        size = record.get("size")
        load = record.get("load_address")
        entry = record.get("entry_pc")
        mode = record.get("entry_mode")
        digest = record.get("sha1")
        if (not isinstance(size, int) or size <= 0 or
                not isinstance(load, int) or not 0 <= load <= 0xFFFFFFFF or
                not isinstance(entry, int) or not 0 <= entry <= 0xFFFFFFFF or
                mode not in ("arm", "thumb") or
                not isinstance(digest, str) or
                re.fullmatch(r"[0-9a-f]{40}", digest) is None):
            raise ValueError(f"Invalid load metadata for {cpu.upper()}")

        alignment = 2 if mode == "thumb" else 4
        if (load + size > 0x100000000 or entry % alignment != 0 or
                entry < load or entry + alignment > load + size):
            raise ValueError(f"Invalid entry point/range for {cpu.upper()}")

        expected_segment = EXPECTED_SEGMENTS[cpu]
        actual_segment = {
            "rom_offset": record.get("rom_offset"),
            "entry_pc_raw": record.get("entry_pc_raw"),
            "load_address": load,
            "size": size,
            "entry_pc": entry,
            "entry_mode": mode,
            "sha1": digest,
        }
        if actual_segment != expected_segment:
            raise ValueError(
                f"{cpu.upper()} does not match the verified Platinum USA Rev 1 segment"
            )

        binary = extracted_dir / f"{cpu}.bin"
        config = extracted_dir / f"{cpu}.toml"
        if not binary.is_file() or not config.is_file():
            raise FileNotFoundError(
                f"Missing local {cpu.upper()} binary or TOML: {binary}, {config}"
            )
        if binary.stat().st_size != size:
            raise ValueError(
                f"{cpu.upper()} size mismatch: manifest={size}, "
                f"file={binary.stat().st_size}"
            )
        actual_sha1 = sha1_file(binary)
        if actual_sha1 != digest:
            raise ValueError(
                f"{cpu.upper()} SHA-1 mismatch: expected {digest}, "
                f"received {actual_sha1}"
            )

        config_text = config.read_text(encoding="utf-8")
        expected_id = f"{BANK_PREFIX}_{cpu}"
        expected_toml_fields = (
            f'id = "{expected_id}"',
            f"load_address = 0x{load:08X}",
            f"size = 0x{size:08X}",
            f"entry_pc = 0x{entry:08X}",
            f"addr = 0x{entry:08X}",
            f'sha1 = "{digest}"',
            f'mode = "{mode}"',
        )
        if any(field not in config_text for field in expected_toml_fields):
            raise ValueError(
                f"{cpu.upper()} TOML metadata does not match its binary manifest"
            )

        by_cpu[cpu] = {
            "cpu": cpu,
            "binary": binary,
            "config": config,
            "size": size,
            "load_address": load,
            "entry_pc": entry,
            "entry_mode": mode,
            "sha1": digest,
        }

    if set(by_cpu) != set(CPUS):
        raise ValueError("Manifest must contain both ARM9 and ARM7")
    return manifest, by_cpu


def validate_output_destination(extracted_dir: Path, output_dir: Path) -> None:
    """Reject unsafe or overlapping destinations before running the compiler."""
    if output_dir.is_symlink():
        raise ValueError(f"Generated output directory must not be a symlink: {output_dir}")

    extracted_resolved = extracted_dir.resolve()
    output_resolved = output_dir.resolve()
    if (output_resolved == extracted_resolved or
            output_resolved in extracted_resolved.parents or
            extracted_resolved in output_resolved.parents):
        raise ValueError(
            "Generated output directory must be separate from the extraction directory"
        )

    if output_resolved.exists():
        if not output_resolved.is_dir():
            raise ValueError(
                f"Generated output destination is not a directory: {output_resolved}"
            )

        # Replacing the directory is necessary to remove stale output files.
        # Never remove unrelated user data or descend through a nested/symlinked
        # entry merely because the caller selected an unsafe destination.
        allowed_names = {"oracis_banks.json"}
        for cpu in CPUS:
            bank = f"{BANK_PREFIX}_{cpu}"
            allowed_names.update({
                f"{bank}.c",
                f"{bank}.h",
                f"{bank}_dispatch.c",
            })
        for entry in output_resolved.iterdir():
            if entry.is_symlink() or not entry.is_file() or entry.name not in allowed_names:
                raise ValueError(
                    "Generated output directory contains unrelated or unsafe content: "
                    f"{entry}. Choose a dedicated empty Oracis output directory."
                )


def publish_output_directory(staging_dir: Path, output_dir: Path) -> None:
    """Replace the generated bank set as a unit, restoring the old set on failure."""
    backup_dir: Path | None = None
    if output_dir.exists():
        # Reserve a unique sibling name, then move the previous generated set
        # there. A sibling keeps the rename on the same filesystem.
        backup_dir = Path(tempfile.mkdtemp(
            prefix=f".{output_dir.name}.backup-",
            dir=output_dir.parent,
        ))
        backup_dir.rmdir()
        output_dir.replace(backup_dir)

    try:
        staging_dir.replace(output_dir)
    except OSError:
        if backup_dir is not None and backup_dir.exists():
            # Roll back the previous complete set if publication did not finish.
            backup_dir.replace(output_dir)
        raise
    else:
        if backup_dir is not None:
            shutil.rmtree(backup_dir)


def emit_banks(
    recompiler: Path,
    extracted_dir: Path,
    output_dir: Path,
    runner: Callable[[list[str]], str] = run_command,
) -> dict:
    if not recompiler.is_file():
        raise FileNotFoundError(f"NDSRecomp executable not found: {recompiler}")

    manifest, segments = validate_extraction(extracted_dir)
    validate_output_destination(extracted_dir, output_dir)
    output_dir.parent.mkdir(parents=True, exist_ok=True)
    identity_output = runner([str(recompiler), "--codegen-identity"])
    actual_identity = identity_output.strip()
    if actual_identity != EXPECTED_CODEGEN_IDENTITY:
        raise RuntimeError(
            "NDSRecomp codegen identity mismatch: "
            f"expected {EXPECTED_CODEGEN_IDENTITY}, received {actual_identity or '<empty>'}"
        )

    emitted: dict[str, dict] = {}
    # Generate both CPUs in a sibling staging directory. A failed ARM7 run
    # must not leave an ARM9-only output looking like a complete bank set.
    with tempfile.TemporaryDirectory(
        prefix=f".{output_dir.name}.staging-",
        dir=output_dir.parent,
    ) as staging_name:
        staging_root = Path(staging_name)
        staging_dir = staging_root / "complete"
        staging_dir.mkdir()
        for cpu in CPUS:
            segment = segments[cpu]
            bank_name = f"{BANK_PREFIX}_{cpu}"
            command = [
                str(recompiler),
                "--config", str(segment["config"]),
                "--bin", str(segment["binary"]),
                "--out", str(staging_dir),
                "--bank", bank_name,
                "--validate-live-bytes",
            ]
            runner(command)

            expected_files = (
                f"{bank_name}.c",
                f"{bank_name}.h",
                f"{bank_name}_dispatch.c",
            )
            file_hashes: dict[str, str] = {}
            for filename in expected_files:
                generated = staging_dir / filename
                if not generated.is_file() or generated.stat().st_size == 0:
                    if filename.endswith("_dispatch.c"):
                        raise RuntimeError(
                            f"NDSRecomp did not emit the {cpu.upper()} dispatch file: "
                            f"{generated}"
                        )
                    raise RuntimeError(
                        f"NDSRecomp did not emit a non-empty {cpu.upper()} file: "
                        f"{generated}"
                    )

            # The pinned codegen emits a three-argument timing helper for both
            # loads and stores. Adapt its C output, not the external compiler,
            # so ARM946E-S TCM load mode gets the actual operation direction.
            bank_source = staging_dir / f"{bank_name}.c"
            source_text = bank_source.read_text(encoding="utf-8")
            adapted_source, read_calls, write_calls = patch_generated_source(
                source_text
            )
            bank_source.write_text(adapted_source, encoding="utf-8")
            emitted_direction_counts = {
                "reads": read_calls,
                "writes": write_calls,
            }

            for filename in expected_files:
                generated = staging_dir / filename
                file_hashes[filename] = sha1_file(generated)

            dispatch_text = (staging_dir / f"{bank_name}_dispatch.c").read_text(
                encoding="utf-8"
            )
            table_symbol = f"g_dispatch_{bank_name}"
            if f"const NdsDispatchEntry {table_symbol}[]" not in dispatch_text:
                raise RuntimeError(
                    f"{cpu.upper()} dispatch table {table_symbol} is missing"
                )
            if f"const unsigned {table_symbol}_len" not in dispatch_text:
                raise RuntimeError(
                    f"{cpu.upper()} dispatch table length {table_symbol}_len is missing"
                )

            emitted[cpu] = {
                "bank": bank_name,
                "load_address": segment["load_address"],
                "entry_pc": segment["entry_pc"],
                "entry_mode": segment["entry_mode"],
                "size": segment["size"],
                "binary_sha1": segment["sha1"],
                "memory_direction_adapter": ADAPTER_IDENTITY,
                "memory_direction_calls": emitted_direction_counts,
                "files": file_hashes,
            }

        result = {
            "schema_version": 1,
            "rom_sha1": manifest["rom_sha1"],
            "banks": emitted,
            "notice": "Local generated C code. Do not commit or distribute game-derived output.",
        }
        (staging_dir / "oracis_banks.json").write_text(
            json.dumps(result, indent=2) + "\n", encoding="utf-8"
        )

        publish_output_directory(staging_dir, output_dir)

    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--recompiler",
        type=Path,
        default=ROOT / ".oracis" / "ndsrecomp-build" / (
            "nds_recompile.exe" if sys.platform == "win32" else "nds_recompile"
        ),
        help="Path to the pinned nds_recompile executable",
    )
    parser.add_argument(
        "--extracted-dir",
        type=Path,
        default=ROOT / ".oracis" / "local" / "pokemon-platinum-usa-rev1",
        help="Local extractor output directory",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=ROOT / ".oracis" / "generated" / "pokemon-platinum-usa-rev1",
        help="Local C-bank output directory",
    )
    args = parser.parse_args()

    try:
        result = emit_banks(
            args.recompiler.expanduser().resolve(),
            args.extracted_dir.expanduser().resolve(),
            args.output_dir.expanduser().resolve(),
        )
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    for cpu in CPUS:
        bank = result["banks"][cpu]
        print(
            f"{cpu.upper()}: {bank['bank']} entry=0x{bank['entry_pc']:08X} "
            f"({bank['entry_mode']}) SHA-1={bank['binary_sha1']}"
        )
    print(f"Local generated banks: {args.output_dir.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
