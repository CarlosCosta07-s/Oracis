#!/usr/bin/env python3
"""Verify a complete local NDSRecomp bank set before CMake compiles it.

Retail banks must match the pinned Platinum USA Rev 1 segment identities and
the exact file hashes recorded by the local emitter. Synthetic fixtures are
accepted only when the caller explicitly opts in (CI test builds).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

from emit_platinum_banks import (
    ADAPTER_IDENTITY,
    BANK_PREFIX,
    CPUS,
    EXPECTED_ROM_SHA1,
    EXPECTED_SEGMENTS,
)

SHA1_PATTERN = re.compile(r"[0-9a-f]{40}")
MANIFEST_NAME = "oracis_banks.json"


class BankSetError(ValueError):
    """The local bank directory is incomplete, stale, unsafe or inconsistent."""


def sha1_file(path: Path) -> str:
    digest = hashlib.sha1()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise BankSetError(message)


def _valid_sha1(value: object) -> bool:
    return isinstance(value, str) and SHA1_PATTERN.fullmatch(value) is not None


def _require_int(value: object, label: str, minimum: int = 0) -> int:
    _require(type(value) is int and value >= minimum, f"{label} must be an integer >= {minimum}")
    return value


def _expected_files(cpu: str) -> set[str]:
    bank = f"{BANK_PREFIX}_{cpu}"
    return {f"{bank}.c", f"{bank}.h", f"{bank}_dispatch.c"}


def validate_bank_set(
    bank_dir: Path,
    *,
    allow_synthetic_fixture: bool = False,
) -> dict:
    """Validate the manifest, exact directory contents and every emitted file hash."""
    bank_dir = Path(bank_dir).expanduser()
    _require(not bank_dir.is_symlink(), f"Bank directory must not be a symlink: {bank_dir}")
    _require(bank_dir.is_dir(), f"Bank directory does not exist or is not a directory: {bank_dir}")

    entries = list(bank_dir.iterdir())
    symlinks = [entry.name for entry in entries if entry.is_symlink()]
    _require(not symlinks, f"Bank directory contains symlinks: {', '.join(sorted(symlinks))}")
    _require(
        all(entry.is_file() for entry in entries),
        "Bank directory may contain only regular generated files",
    )

    expected_names = {MANIFEST_NAME}
    for cpu in CPUS:
        expected_names.update(_expected_files(cpu))
    actual_names = {entry.name for entry in entries}
    missing = sorted(expected_names - actual_names)
    unexpected = sorted(actual_names - expected_names)
    _require(not missing, f"Bank set is incomplete; missing: {', '.join(missing)}")
    _require(not unexpected, f"Bank directory contains unexpected files: {', '.join(unexpected)}")

    manifest_path = bank_dir / MANIFEST_NAME
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise BankSetError(f"Cannot read bank manifest {manifest_path}: {exc}") from exc

    _require(isinstance(manifest, dict), "Bank manifest must be a JSON object")
    _require(type(manifest.get("schema_version")) is int and manifest["schema_version"] == 1,
             "Unsupported bank manifest schema_version")
    source_kind = manifest.get("source_kind")
    _require(source_kind in ("retail-rom", "synthetic-fixture"),
             "Bank manifest source_kind must be retail-rom or synthetic-fixture")
    if source_kind == "synthetic-fixture":
        _require(allow_synthetic_fixture,
                 "Synthetic bank fixtures are disabled; set ORACIS_ALLOW_SYNTHETIC_BANKS=ON only for tests")
        _require(manifest.get("rom_sha1") is None,
                 "Synthetic fixture manifest must not claim a retail ROM identity")
    else:
        _require(manifest.get("rom_sha1") == EXPECTED_ROM_SHA1,
                 "Bank manifest ROM SHA-1 does not match Pokémon Platinum USA Rev 1")

    banks = manifest.get("banks")
    _require(isinstance(banks, dict) and set(banks) == set(CPUS),
             "Bank manifest must contain exactly ARM9 and ARM7")

    for cpu in CPUS:
        record = banks[cpu]
        _require(isinstance(record, dict), f"{cpu.upper()} bank metadata must be an object")
        bank_name = f"{BANK_PREFIX}_{cpu}"
        _require(record.get("bank") == bank_name, f"{cpu.upper()} bank name mismatch")

        files = record.get("files")
        expected_files = _expected_files(cpu)
        _require(isinstance(files, dict) and set(files) == expected_files,
                 f"{cpu.upper()} manifest must hash exactly its C, header and dispatch files")
        for filename, recorded_hash in files.items():
            _require(_valid_sha1(recorded_hash),
                     f"{cpu.upper()} manifest has an invalid SHA-1 for {filename}")
            actual_hash = sha1_file(bank_dir / filename)
            _require(actual_hash == recorded_hash,
                     f"Generated bank file hash mismatch: {filename}")

        calls = record.get("memory_direction_calls")
        _require(isinstance(calls, dict) and set(calls) == {"reads", "writes"},
                 f"{cpu.upper()} memory-direction call counts are malformed")
        _require_int(calls.get("reads"), f"{cpu.upper()} read call count")
        _require_int(calls.get("writes"), f"{cpu.upper()} write call count")

        if source_kind == "retail-rom":
            expected = EXPECTED_SEGMENTS[cpu]
            _require(record.get("load_address") == expected["load_address"],
                     f"{cpu.upper()} load address does not match the pinned segment")
            _require(record.get("entry_pc") == expected["entry_pc"],
                     f"{cpu.upper()} entry point does not match the pinned segment")
            _require(record.get("entry_mode") == expected["entry_mode"],
                     f"{cpu.upper()} entry mode does not match the pinned segment")
            _require(record.get("size") == expected["size"],
                     f"{cpu.upper()} segment size does not match the pinned segment")
            _require(record.get("binary_sha1") == expected["sha1"],
                     f"{cpu.upper()} segment SHA-1 does not match the pinned segment")
            _require(record.get("memory_direction_adapter") == ADAPTER_IDENTITY,
                     f"{cpu.upper()} bank was not processed by {ADAPTER_IDENTITY}")
        else:
            _require(record.get("entry_mode") in ("arm", "thumb"),
                     f"{cpu.upper()} synthetic fixture entry mode is invalid")
            _require_int(record.get("load_address"), f"{cpu.upper()} synthetic load address")
            _require_int(record.get("entry_pc"), f"{cpu.upper()} synthetic entry point")
            _require_int(record.get("size"), f"{cpu.upper()} synthetic segment size", 1)
            _require(record.get("binary_sha1") is None,
                     f"{cpu.upper()} synthetic fixture must not claim a retail segment hash")
            _require(record.get("memory_direction_adapter") == "synthetic-fixture",
                     f"{cpu.upper()} synthetic fixture adapter marker is invalid")

    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bank-dir", type=Path, required=True)
    parser.add_argument(
        "--allow-synthetic-fixture",
        action="store_true",
        help="Allow explicitly marked test fixtures; never enable for a retail build",
    )
    args = parser.parse_args()

    try:
        manifest = validate_bank_set(
            args.bank_dir,
            allow_synthetic_fixture=args.allow_synthetic_fixture,
        )
    except (BankSetError, OSError) as exc:
        print(f"invalid local bank set: {exc}", file=sys.stderr)
        return 1

    kind = manifest["source_kind"]
    print(f"Validated {kind} ARM9/ARM7 bank set: {args.bank_dir.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
