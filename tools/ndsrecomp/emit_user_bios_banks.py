#!/usr/bin/env python3
"""Compile user-owned Nintendo DS BIOS dumps into local Oracis static banks.

The BIOS bytes, temporary TOML files, compiler output and generated C sources
stay outside Git. This tool never downloads, embeds, or distributes BIOS data.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Callable

ROOT = Path(__file__).resolve().parents[2]
EXPECTED_CODEGEN_IDENTITY = "nds-codegen-v3"
BANK_PREFIX = "oracis_user_bios"
CPUS = ("arm9", "arm7")
BIOS_LAYOUT = {
    "arm9": {
        "size": 0x1000,
        "load_address": 0xFFFF0000,
    },
    "arm7": {
        "size": 0x4000,
        "load_address": 0x00000000,
    },
}
EXCEPTION_VECTORS = (
    (0x00, "reset"),
    (0x04, "undefined"),
    (0x08, "swi"),
    (0x0C, "prefetch_abort"),
    (0x10, "data_abort"),
    (0x18, "irq"),
    (0x1C, "fiq"),
)

_PATCHER_PATH = Path(__file__).with_name("patch_memory_access_direction.py")
_PATCHER_SPEC = importlib.util.spec_from_file_location(
    "oracis_bios_memory_direction_adapter", _PATCHER_PATH
)
if _PATCHER_SPEC is None or _PATCHER_SPEC.loader is None:
    raise RuntimeError(f"Cannot load memory direction adapter: {_PATCHER_PATH}")
_PATCHER = importlib.util.module_from_spec(_PATCHER_SPEC)
_PATCHER_SPEC.loader.exec_module(_PATCHER)
ADAPTER_IDENTITY = _PATCHER.ADAPTER_IDENTITY
patch_generated_source = _PATCHER.patch_generated_source

_SPLITTER_PATH = Path(__file__).with_name("split_generated_bank.py")
_SPLITTER_SPEC = importlib.util.spec_from_file_location(
    "oracis_bios_bank_splitter", _SPLITTER_PATH
)
if _SPLITTER_SPEC is None or _SPLITTER_SPEC.loader is None:
    raise RuntimeError(f"Cannot load generated-bank splitter: {_SPLITTER_PATH}")
_SPLITTER = importlib.util.module_from_spec(_SPLITTER_SPEC)
_SPLITTER_SPEC.loader.exec_module(_SPLITTER)
split_generated_bank = _SPLITTER.split_generated_bank


def sha1_bytes(data: bytes) -> str:
    return hashlib.sha1(data).hexdigest()


def sha1_file(path: Path) -> str:
    digest = hashlib.sha1()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
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
        timeout=300,
    )
    if completed.stdout:
        print(completed.stdout, end="", flush=True)
    if completed.returncode != 0:
        raise RuntimeError(
            f"Command failed with exit code {completed.returncode}: "
            + " ".join(str(part) for part in command)
        )
    return completed.stdout


def entry_points(cpu: str) -> tuple[tuple[int, str], ...]:
    if cpu not in BIOS_LAYOUT:
        raise ValueError(f"Unsupported BIOS CPU: {cpu!r}")
    base = BIOS_LAYOUT[cpu]["load_address"]
    size = BIOS_LAYOUT[cpu]["size"]
    points = tuple((base + offset, name) for offset, name in EXCEPTION_VECTORS)
    for address, _ in points:
        if not base <= address <= base + size - 4 or address % 4:
            raise ValueError(f"{cpu.upper()} exception vector is outside its BIOS image")
    return points


def toml_for_bios(cpu: str, image_sha1: str) -> str:
    if cpu not in BIOS_LAYOUT:
        raise ValueError(f"Unsupported BIOS CPU: {cpu!r}")
    layout = BIOS_LAYOUT[cpu]
    bank_name = f"{BANK_PREFIX}_{cpu}"
    lines = [
        "# Generated locally by emit_user_bios_banks.py. Do not commit.",
        "# Source BIOS is user-provided. The identity hash is not an authenticity check.",
        "",
        "[program]",
        f'name = "Oracis user BIOS {cpu.upper()}"',
        f'id = "{bank_name}"',
        f"load_address = 0x{layout['load_address']:08X}",
        f"size = 0x{layout['size']:08X}",
        f"entry_pc = 0x{layout['load_address']:08X}",
        "",
        "[identity]",
        f'sha1 = "{image_sha1}"',
        "",
    ]
    for address, name in entry_points(cpu):
        lines.extend([
            "[[entry_point]]",
            f"addr = 0x{address:08X}",
            'mode = "arm"',
            f'name = "vector_{name}"',
            "",
        ])
    return "\n".join(lines)


def validate_bios_inputs(
    arm9_bios: Path, arm7_bios: Path
) -> dict[str, dict]:
    result: dict[str, dict] = {}
    for cpu, path in (("arm9", arm9_bios), ("arm7", arm7_bios)):
        if path.is_symlink() or not path.is_file():
            raise FileNotFoundError(f"User {cpu.upper()} BIOS is not a regular file: {path}")
        expected_size = BIOS_LAYOUT[cpu]["size"]
        actual_size = path.stat().st_size
        if actual_size != expected_size:
            raise ValueError(
                f"{cpu.upper()} BIOS must be exactly {expected_size} bytes; "
                f"received {actual_size}"
            )
        digest = sha1_file(path)
        result[cpu] = {
            "source": path,
            "size": expected_size,
            "load_address": BIOS_LAYOUT[cpu]["load_address"],
            "entry_pc": BIOS_LAYOUT[cpu]["load_address"],
            "entry_points": entry_points(cpu),
            "sha1": digest,
        }
    return result


def validate_output_destination(
    bioses: dict[str, dict], output_dir: Path
) -> None:
    if output_dir.is_symlink():
        raise ValueError(f"Generated output directory must not be a symlink: {output_dir}")
    output_resolved = output_dir.resolve()
    for record in bioses.values():
        source = record["source"].resolve()
        if output_resolved == source or output_resolved in source.parents or source in output_resolved.parents:
            raise ValueError(
                "Generated output directory must be separate from user BIOS files"
            )

    if not output_resolved.exists():
        return
    if not output_resolved.is_dir():
        raise ValueError(f"Generated output destination is not a directory: {output_resolved}")

    allowed_files = {"oracis_bios_banks.json"}
    allowed_directories = {f"{BANK_PREFIX}_{cpu}_shards" for cpu in CPUS}
    for cpu in CPUS:
        bank = f"{BANK_PREFIX}_{cpu}"
        allowed_files.update({f"{bank}.c", f"{bank}.h", f"{bank}_dispatch.c"})
    for item in output_resolved.iterdir():
        if item.is_symlink():
            raise ValueError(f"Generated output contains a symlink: {item}")
        if item.is_file() and item.name in allowed_files:
            continue
        if item.is_dir() and item.name in allowed_directories:
            for shard in item.iterdir():
                if (
                    shard.is_symlink()
                    or not shard.is_file()
                    or re.fullmatch(r"bank_part_[0-9]{4}\.c", shard.name) is None
                ):
                    raise ValueError(f"Generated output contains unrelated content: {shard}")
            continue
        raise ValueError(f"Generated output contains unrelated or unsafe content: {item}")


def publish_output_directory(staging_dir: Path, output_dir: Path) -> None:
    backup_dir: Path | None = None
    if output_dir.exists():
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
            backup_dir.replace(output_dir)
        raise
    else:
        if backup_dir is not None:
            shutil.rmtree(backup_dir)


def emit_banks(
    recompiler: Path,
    arm9_bios: Path,
    arm7_bios: Path,
    output_dir: Path,
    runner: Callable[[list[str]], str] = run_command,
    functions_per_shard: int = 512,
) -> dict:
    if not recompiler.is_file():
        raise FileNotFoundError(f"NDSRecomp executable not found: {recompiler}")
    if functions_per_shard < 1:
        raise ValueError("functions_per_shard must be at least one")

    bioses = validate_bios_inputs(arm9_bios, arm7_bios)
    validate_output_destination(bioses, output_dir)
    output_dir.parent.mkdir(parents=True, exist_ok=True)

    actual_identity = runner([str(recompiler), "--codegen-identity"]).strip()
    if actual_identity != EXPECTED_CODEGEN_IDENTITY:
        raise RuntimeError(
            "NDSRecomp codegen identity mismatch: "
            f"expected {EXPECTED_CODEGEN_IDENTITY}, "
            f"received {actual_identity or '<empty>'}"
        )

    emitted: dict[str, dict] = {}
    with tempfile.TemporaryDirectory(
        prefix=f".{output_dir.name}.staging-",
        dir=output_dir.parent,
    ) as temporary_name:
        temporary_root = Path(temporary_name)
        input_dir = temporary_root / "input"
        staging_dir = temporary_root / "complete"
        input_dir.mkdir()
        staging_dir.mkdir()

        for cpu in CPUS:
            record = bioses[cpu]
            binary = input_dir / f"{cpu}.bin"
            config = input_dir / f"{cpu}.toml"
            shutil.copyfile(record["source"], binary)
            if sha1_file(binary) != record["sha1"]:
                raise RuntimeError(f"{cpu.upper()} BIOS changed while it was staged")
            config.write_text(
                toml_for_bios(cpu, record["sha1"]),
                encoding="utf-8",
                newline="\n",
            )

            bank_name = f"{BANK_PREFIX}_{cpu}"
            runner([
                str(recompiler),
                "--config", str(config),
                "--bin", str(binary),
                "--out", str(staging_dir),
                "--bank", bank_name,
                "--validate-live-bytes",
            ])

            expected_files = (
                f"{bank_name}.c",
                f"{bank_name}.h",
                f"{bank_name}_dispatch.c",
            )
            for filename in expected_files:
                generated = staging_dir / filename
                if not generated.is_file() or generated.stat().st_size == 0:
                    raise RuntimeError(
                        f"NDSRecomp did not emit a non-empty BIOS bank file: {generated}"
                    )

            bank_source = staging_dir / f"{bank_name}.c"
            source_text = bank_source.read_text(encoding="utf-8")
            adapted_source, read_calls, write_calls = patch_generated_source(source_text)
            bank_source.write_text(adapted_source, encoding="utf-8")

            shard_metadata = split_generated_bank(
                bank_source,
                staging_dir / f"{bank_name}.h",
                staging_dir,
                bank_name,
                functions_per_shard=functions_per_shard,
            )

            dispatch_path = staging_dir / f"{bank_name}_dispatch.c"
            dispatch_text = dispatch_path.read_text(encoding="utf-8")
            table_symbol = f"g_dispatch_{bank_name}"
            if (
                f"const NdsDispatchEntry {table_symbol}[]" not in dispatch_text
                or f"const unsigned {table_symbol}_len" not in dispatch_text
            ):
                raise RuntimeError(f"{cpu.upper()} BIOS dispatch table is missing")
            for address, vector_name in record["entry_points"]:
                literal = rf"0[xX]{address:08X}"
                if re.search(literal, dispatch_text, re.IGNORECASE) is None:
                    raise RuntimeError(
                        f"{cpu.upper()} BIOS {vector_name} vector "
                        f"0x{address:08X} is absent from the emitted dispatch table"
                    )

            file_hashes = {
                filename: sha1_file(staging_dir / filename)
                for filename in expected_files
            }
            emitted[cpu] = {
                "bank": bank_name,
                "load_address": record["load_address"],
                "entry_pc": record["entry_pc"],
                "entry_mode": "arm",
                "size": record["size"],
                "bios_sha1": record["sha1"],
                "exception_vectors": [
                    {"address": address, "name": name, "mode": "arm"}
                    for address, name in record["entry_points"]
                ],
                "memory_direction_adapter": ADAPTER_IDENTITY,
                "memory_direction_calls": {"reads": read_calls, "writes": write_calls},
                "files": file_hashes,
                "shards": shard_metadata,
            }

        manifest = {
            "schema_version": 1,
            "source_kind": "user-bios",
            "codegen_identity": EXPECTED_CODEGEN_IDENTITY,
            "banks": emitted,
            "notice": (
                "User-derived BIOS C code. This output is local; do not commit "
                "or redistribute it."
            ),
        }
        (staging_dir / "oracis_bios_banks.json").write_text(
            json.dumps(manifest, indent=2) + "\n",
            encoding="utf-8",
        )
        publish_output_directory(staging_dir, output_dir)
    return manifest


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
    parser.add_argument("--arm9-bios", type=Path, required=True)
    parser.add_argument("--arm7-bios", type=Path, required=True)
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=ROOT / ".oracis" / "generated" / "nds-user-bios",
    )
    parser.add_argument("--functions-per-shard", type=int, default=512)
    args = parser.parse_args()

    try:
        result = emit_banks(
            args.recompiler.expanduser().resolve(),
            args.arm9_bios.expanduser().absolute(),
            args.arm7_bios.expanduser().absolute(),
            args.output_dir.expanduser().absolute(),
            functions_per_shard=args.functions_per_shard,
        )
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    for cpu in CPUS:
        bank = result["banks"][cpu]
        print(
            f"{cpu.upper()} BIOS: {bank['bank']} entry=0x{bank['entry_pc']:08X} "
            f"SHA-1={bank['bios_sha1']}"
        )
    print(f"Local generated BIOS banks: {args.output_dir.expanduser().absolute()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
