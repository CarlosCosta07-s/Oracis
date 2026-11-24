#!/usr/bin/env python3
"""Validate locally generated Oracis user-BIOS static banks before CMake linking."""
from __future__ import annotations
import argparse, hashlib, json, re, sys
from pathlib import Path

EXPECTED_CODEGEN_IDENTITY = "nds-codegen-v3"
EXPECTED_ADAPTER_IDENTITY = "oracis-memory-direction-v1"
BANK_PREFIX = "oracis_user_bios"
CPUS = ("arm9", "arm7")
BIOS_LAYOUT = {
    "arm9": {"size": 0x1000, "load_address": 0xFFFF0000},
    "arm7": {"size": 0x4000, "load_address": 0x00000000},
}
VECTOR_NAMES = ("reset", "undefined", "swi", "prefetch_abort", "data_abort", "irq", "fiq")
VECTOR_OFFSETS = (0x00, 0x04, 0x08, 0x0C, 0x10, 0x18, 0x1C)
_SHA1 = re.compile(r"[0-9a-f]{40}")
_SHARD_NAME = re.compile(r"bank_part_[0-9]{4}\.c")

def sha1_file(path: Path) -> str:
    digest = hashlib.sha1()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()

def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)

def regular_file(root: Path, filename: str) -> Path:
    path = root / filename
    require(not path.is_symlink(), f"symlink is not allowed: {path}")
    require(path.is_file(), f"required regular file is missing: {path}")
    return path

def validate_hash(path: Path, expected: object, label: str) -> None:
    require(isinstance(expected, str) and _SHA1.fullmatch(expected) is not None,
            f"{label} must be a lowercase SHA-1")
    actual = sha1_file(path)
    require(actual == expected, f"{label} hash mismatch: expected {expected}, got {actual}")

def expected_vectors(cpu: str) -> list[dict]:
    base = BIOS_LAYOUT[cpu]["load_address"]
    return [{"address": base + off, "name": name, "mode": "arm"}
            for off, name in zip(VECTOR_OFFSETS, VECTOR_NAMES)]

def validate_bank_set(bank_dir: Path, *, allow_synthetic_fixture: bool = False) -> dict:
    require(not bank_dir.is_symlink(), f"bank directory must not be a symlink: {bank_dir}")
    if not bank_dir.is_dir():
        raise FileNotFoundError(f"user BIOS bank directory does not exist: {bank_dir}")
    manifest = json.loads(regular_file(bank_dir, "oracis_bios_banks.json").read_text(encoding="utf-8"))
    require(manifest.get("schema_version") == 1, "unsupported BIOS bank manifest schema")
    source_kind = manifest.get("source_kind")
    require(source_kind in ("user-bios", "synthetic-fixture"),
            f"unexpected BIOS bank source_kind: {source_kind!r}")
    if source_kind == "synthetic-fixture":
        require(allow_synthetic_fixture,
                "synthetic BIOS fixtures require explicit --allow-synthetic-fixture")
        require(manifest.get("codegen_identity") == "synthetic-fixture",
                "synthetic fixtures cannot claim a retail codegen identity")
    else:
        require(manifest.get("codegen_identity") == EXPECTED_CODEGEN_IDENTITY,
                "BIOS bank codegen identity mismatch")
    banks = manifest.get("banks")
    require(isinstance(banks, dict) and set(banks) == set(CPUS),
            "manifest must contain exactly ARM9 and ARM7 BIOS banks")

    expected_root_files = {"oracis_bios_banks.json"}
    expected_root_dirs = set()
    for cpu in CPUS:
        record = banks[cpu]
        require(isinstance(record, dict), f"{cpu.upper()} bank metadata is malformed")
        bank = f"{BANK_PREFIX}_{cpu}"
        layout = BIOS_LAYOUT[cpu]
        require(record.get("bank") == bank, f"{cpu.upper()} bank name mismatch")
        require(record.get("load_address") == layout["load_address"],
                f"{cpu.upper()} load address mismatch")
        require(record.get("entry_pc") == layout["load_address"],
                f"{cpu.upper()} reset PC mismatch")
        require(record.get("entry_mode") == "arm", f"{cpu.upper()} reset mode must be ARM")
        require(record.get("size") == layout["size"], f"{cpu.upper()} BIOS size metadata mismatch")
        digest = record.get("bios_sha1")
        require(isinstance(digest, str) and _SHA1.fullmatch(digest) is not None,
                f"{cpu.upper()} BIOS identity must be a lowercase SHA-1")
        vectors = record.get("exception_vectors")
        require(vectors == expected_vectors(cpu), f"{cpu.upper()} exception vector inventory mismatch")
        adapter = "synthetic-fixture" if source_kind == "synthetic-fixture" else EXPECTED_ADAPTER_IDENTITY
        require(record.get("memory_direction_adapter") == adapter,
                f"{cpu.upper()} memory-direction adapter identity mismatch")
        direction_counts = record.get("memory_direction_calls")
        require(isinstance(direction_counts, dict), f"{cpu.upper()} memory-direction counts are missing")
        for field in ("reads", "writes"):
            require(isinstance(direction_counts.get(field), int) and direction_counts[field] >= 0,
                    f"{cpu.upper()} memory-direction {field} count is invalid")

        files = record.get("files")
        expected_names = {f"{bank}.c", f"{bank}.h", f"{bank}_dispatch.c"}
        require(isinstance(files, dict) and set(files) == expected_names,
                f"{cpu.upper()} file inventory mismatch")
        for filename, file_hash in files.items():
            validate_hash(regular_file(bank_dir, filename), file_hash, f"{cpu.upper()} {filename}")

        dispatch_text = (bank_dir / f"{bank}_dispatch.c").read_text(encoding="utf-8")
        table_symbol = f"g_dispatch_{bank}"
        require(f"const NdsDispatchEntry {table_symbol}[]" in dispatch_text
                and f"const unsigned {table_symbol}_len" in dispatch_text,
                f"{cpu.upper()} dispatch table declaration is missing")
        for vector in vectors:
            address = vector["address"]
            require(re.search(rf"0[xX]{address:08X}", dispatch_text) is not None,
                    f"{cpu.upper()} {vector['name']} vector 0x{address:08X} is missing")

        shards = record.get("shards")
        require(isinstance(shards, dict), f"{cpu.upper()} shard metadata is missing")
        shard_dir_name = f"{bank}_shards"
        require(shards.get("directory") == shard_dir_name,
                f"{cpu.upper()} shard directory mismatch")
        max_functions = shards.get("functions_per_shard")
        function_count = shards.get("function_count")
        shard_files = shards.get("files")
        require(isinstance(max_functions, int) and max_functions > 0,
                f"{cpu.upper()} shard function limit is invalid")
        require(isinstance(function_count, int) and function_count > 0,
                f"{cpu.upper()} shard function count is invalid")
        require(isinstance(shard_files, dict) and shard_files,
                f"{cpu.upper()} shard hash inventory is empty")
        shard_dir = bank_dir / shard_dir_name
        require(not shard_dir.is_symlink() and shard_dir.is_dir(),
                f"{cpu.upper()} shard directory is missing or unsafe")
        for filename, file_hash in shard_files.items():
            require(_SHARD_NAME.fullmatch(filename) is not None,
                    f"{cpu.upper()} shard filename is invalid: {filename}")
            validate_hash(regular_file(shard_dir, filename), file_hash, f"{cpu.upper()} {filename}")
        actual_shards = set()
        for child in shard_dir.iterdir():
            require(not child.is_symlink() and child.is_file(),
                    f"{cpu.upper()} shard directory contains unsafe content: {child}")
            actual_shards.add(child.name)
        require(actual_shards == set(shard_files),
                f"{cpu.upper()} shard directory contains unlisted files")
        require(function_count <= len(shard_files) * max_functions,
                f"{cpu.upper()} shard function count exceeds declared capacity")

        expected_root_files.update(expected_names)
        expected_root_dirs.add(shard_dir_name)

    actual_files, actual_dirs = set(), set()
    for child in bank_dir.iterdir():
        require(not child.is_symlink(), f"bank directory contains a symlink: {child}")
        if child.is_file():
            actual_files.add(child.name)
        elif child.is_dir():
            actual_dirs.add(child.name)
        else:
            raise ValueError(f"bank directory contains an unsupported entry: {child}")
    require(actual_files == expected_root_files, "bank directory contains missing or unrelated files")
    require(actual_dirs == expected_root_dirs, "bank directory contains missing or unrelated directories")
    return {"source_kind": source_kind, "codegen_identity": manifest["codegen_identity"], "banks": banks}

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bank-dir", type=Path, required=True)
    parser.add_argument("--allow-synthetic-fixture", action="store_true")
    args = parser.parse_args()
    try:
        result = validate_bank_set(args.bank_dir.expanduser().resolve(),
                                   allow_synthetic_fixture=args.allow_synthetic_fixture)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(f"Validated {result['source_kind']} BIOS bank set: "
          f"ARM9={result['banks']['arm9']['bank']} ARM7={result['banks']['arm7']['bank']}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
