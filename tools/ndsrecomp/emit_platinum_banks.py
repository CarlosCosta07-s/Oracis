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
from pathlib import Path
from typing import Callable

ROOT = Path(__file__).resolve().parents[2]
EXPECTED_ROM_SHA1 = "0862ec35b24de5c7e2dcb88c9eea0873110d755c"
BANK_PREFIX = "pokemon_platinum_usa_rev1"
CPUS = ("arm9", "arm7")


def sha1_file(path: Path) -> str:
    digest = hashlib.sha1()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def run_command(command: list[str]) -> None:
    print("+", " ".join(str(part) for part in command), flush=True)
    completed = subprocess.run(command, check=False)
    if completed.returncode != 0:
        raise RuntimeError(
            f"Command failed with exit code {completed.returncode}: "
            + " ".join(str(part) for part in command)
        )


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
        if f'id = "{expected_id}"' not in config_text:
            raise ValueError(f"{cpu.upper()} TOML program id does not match")
        if f'sha1 = "{digest}"' not in config_text:
            raise ValueError(f"{cpu.upper()} TOML identity does not match its binary")

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


def emit_banks(
    recompiler: Path,
    extracted_dir: Path,
    output_dir: Path,
    runner: Callable[[list[str]], None] = run_command,
) -> dict:
    if not recompiler.is_file():
        raise FileNotFoundError(f"NDSRecomp executable not found: {recompiler}")

    manifest, segments = validate_extraction(extracted_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    emitted: dict[str, dict] = {}
    for cpu in CPUS:
        segment = segments[cpu]
        bank_name = f"{BANK_PREFIX}_{cpu}"
        command = [
            str(recompiler),
            "--config", str(segment["config"]),
            "--bin", str(segment["binary"]),
            "--out", str(output_dir),
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
            generated = output_dir / filename
            if not generated.is_file() or generated.stat().st_size == 0:
                raise RuntimeError(
                    f"NDSRecomp did not emit a non-empty {cpu.upper()} file: "
                    f"{generated}"
                )
            file_hashes[filename] = sha1_file(generated)

        dispatch_text = (output_dir / f"{bank_name}_dispatch.c").read_text(
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
            "files": file_hashes,
        }

    result = {
        "schema_version": 1,
        "rom_sha1": manifest["rom_sha1"],
        "banks": emitted,
        "notice": "Local generated C code. Do not commit or distribute game-derived output.",
    }
    (output_dir / "oracis_banks.json").write_text(
        json.dumps(result, indent=2) + "\n", encoding="utf-8"
    )
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
