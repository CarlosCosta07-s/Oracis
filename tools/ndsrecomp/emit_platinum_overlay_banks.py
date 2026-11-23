#!/usr/bin/env python3
"""Audit and emit local, identity-validated Pokémon Platinum overlay banks.

All extracted images, TOMLs and generated C files stay outside Git. The tool
uses only the pinned NDSRecomp static compiler and Oracis-owned source adapters.
It does not copy or compile the supplied Poképlatinum source tree.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Callable

ROOT = Path(__file__).resolve().parents[2]
EXPECTED_ROM_SHA1 = "0862ec35b24de5c7e2dcb88c9eea0873110d755c"
EXPECTED_CODEGEN_IDENTITY = "nds-codegen-v3"
FINDER_SUMMARY = re.compile(
    r"\[finder\] functions=(\d+) \(arm=(\d+) thumb=(\d+)\) "
    r"indirect=(\d+) undefined=(\d+)"
)
DECODE_SUMMARY = re.compile(r"\[audit\] decoded (\d+) instructions across (\d+) functions")
UNDEFINED_SUMMARY = re.compile(
    r"\[audit\] UNDEFINED encodings \(distinct=(\d+) arm, (\d+) thumb\)"
)
INDIRECT_OUTCOMES_SUMMARY = re.compile(
    r"\[finder\] indirect outcomes \(non-return\): "
    r"resolved=(\d+) known-unreachable=(\d+) unknown=(\d+) stored_ptr_seeds=(\d+)"
)
INDIRECT_REGION = re.compile(
    r"(?m)^\s+(0x[0-9A-Fa-f]{8})\.\.(0x[0-9A-Fa-f]{8})\s+(\d+)\s+site\(s\)\s*$"
)


def _load_helper(name: str, filename: str):
    path = Path(__file__).with_name(filename)
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Cannot load Oracis NDSRecomp helper: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


_PATCHER = _load_helper("oracis_overlay_emitter_patcher", "patch_memory_access_direction.py")
_SPLITTER = _load_helper("oracis_overlay_emitter_splitter", "split_generated_bank.py")
patch_generated_source = _PATCHER.patch_generated_source
ADAPTER_IDENTITY = _PATCHER.ADAPTER_IDENTITY
split_generated_bank = _SPLITTER.split_generated_bank


def hash_file(path: Path, algorithm: str = "sha256") -> str:
    digest = hashlib.new(algorithm)
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def run_command(command: list[str], env: dict[str, str]) -> str:
    print("+", " ".join(str(part) for part in command), flush=True)
    completed = subprocess.run(
        command,
        check=False,
        env=env,
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


def load_overlay_manifest(image_dir: Path) -> tuple[dict, dict[int, dict]]:
    if image_dir.is_symlink():
        raise ValueError("overlay image directory must not be a symlink")
    image_dir = image_dir.resolve(strict=True)
    manifest_path = image_dir / "manifest.json"
    if manifest_path.is_symlink() or not manifest_path.is_file():
        raise ValueError("overlay extraction manifest is missing or unsafe")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if not isinstance(manifest, dict) or manifest.get("schema") != "oracis-nds-overlays-v1":
        raise ValueError("unsupported overlay extraction manifest")
    if manifest.get("rom_sha1") != EXPECTED_ROM_SHA1:
        raise ValueError("overlay images do not match Pokémon Platinum USA Rev 1")
    rows = manifest.get("overlays")
    if not isinstance(rows, list) or manifest.get("overlay_count") != len(rows):
        raise ValueError("overlay extraction manifest count is inconsistent")
    records: dict[int, dict] = {}
    for row in rows:
        if not isinstance(row, dict) or row.get("cpu") != "arm9":
            continue
        overlay_id = row.get("overlay_id")
        if type(overlay_id) is not int or not 0 <= overlay_id <= 999:
            raise ValueError("invalid ARM9 overlay ID in extraction manifest")
        if overlay_id in records:
            raise ValueError(f"duplicate ARM9 overlay ID {overlay_id}")
        image_name = row.get("image_file")
        if (not isinstance(image_name, str) or not image_name or
                Path(image_name).name != image_name or image_name == "manifest.json"):
            raise ValueError(f"unsafe image filename for overlay {overlay_id}")
        image = image_dir / image_name
        if image.is_symlink() or not image.is_file():
            raise ValueError(f"missing or unsafe overlay image: {image}")
        size = row.get("image_size")
        if (type(size) is not int or size <= 0 or row.get("ram_size") != size or
                image.stat().st_size != size):
            raise ValueError(f"overlay {overlay_id} image size mismatch")
        if (row.get("image_sha1") != hash_file(image, "sha1") or
                row.get("image_sha256") != hash_file(image, "sha256")):
            raise ValueError(f"overlay {overlay_id} extracted image hash mismatch")
        records[overlay_id] = row
    if not records:
        raise ValueError("overlay manifest contains no ARM9 records")
    return manifest, records


def parse_scalar(section: str, key: str, *, string: bool = False) -> str | int:
    if string:
        match = re.search(rf"(?m)^\s*{re.escape(key)}\s*=\s*\"([^\"]+)\"\s*$", section)
        if match is None:
            raise ValueError(f"TOML field {key!r} is missing")
        return match.group(1)
    match = re.search(rf"(?m)^\s*{re.escape(key)}\s*=\s*(0x[0-9A-Fa-f]+|[0-9]+)\s*$", section)
    if match is None:
        raise ValueError(f"TOML numeric field {key!r} is missing")
    return int(match.group(1), 0)


def validate_overlay_config(config_path: Path, overlay_id: int, image_record: dict) -> dict:
    if config_path.is_symlink() or not config_path.is_file():
        raise ValueError(f"overlay config is missing or unsafe: {config_path}")
    source = config_path.read_text(encoding="utf-8")
    program_match = re.search(r"(?ms)^\[program\]\s*\n(.*?)(?=^\[|\Z)", source)
    identity_match = re.search(r"(?ms)^\[identity\]\s*\n(.*?)(?=^\[|\Z)", source)
    if program_match is None or identity_match is None:
        raise ValueError(f"overlay config lacks [program]/[identity]: {config_path}")
    program = program_match.group(1)
    identity = identity_match.group(1)
    bank = f"pokemon_platinum_usa_rev1_arm9_overlay_{overlay_id:04d}"
    config_id = parse_scalar(program, "id", string=True)
    load_address = parse_scalar(program, "load_address")
    size = parse_scalar(program, "size")
    entry_pc = parse_scalar(program, "entry_pc")
    image_sha1 = parse_scalar(identity, "sha1", string=True)
    if config_id != bank:
        raise ValueError(f"overlay config ID mismatch for overlay {overlay_id}")
    if load_address != image_record.get("ram_address") or size != image_record.get("ram_size"):
        raise ValueError(f"overlay config RAM range mismatch for overlay {overlay_id}")
    if image_sha1 != image_record.get("image_sha1"):
        raise ValueError(f"overlay config image identity mismatch for overlay {overlay_id}")
    entries = []
    for block in re.findall(r"(?ms)^\[\[entry_point\]\]\s*\n(.*?)(?=^\[|\Z)", source):
        address = parse_scalar(block, "addr")
        mode = parse_scalar(block, "mode", string=True)
        if mode not in {"arm", "thumb"}:
            raise ValueError(f"invalid entry mode in overlay {overlay_id}")
        alignment = 2 if mode == "thumb" else 4
        if (address < load_address or address >= load_address + size or
                address % alignment != 0):
            raise ValueError(f"entry point 0x{address:08X} outside/alignment-invalid in overlay {overlay_id}")
        entries.append({"address": address, "mode": mode})
    if not entries:
        raise ValueError(f"overlay {overlay_id} has no entry points")
    if entry_pc != entries[0]["address"]:
        raise ValueError(f"overlay {overlay_id} entry_pc differs from first entry_point")
    return {
        "bank": bank,
        "load_address": load_address,
        "size": size,
        "entry_pc": entry_pc,
        "entry_count": len(entries),
        "image_sha1": image_sha1,
    }


def parse_audit(log: str) -> dict:
    finder = FINDER_SUMMARY.search(log)
    if finder is None:
        raise ValueError("NDSRecomp audit did not emit the function discovery summary")
    decoded = DECODE_SUMMARY.search(log)
    undefined = UNDEFINED_SUMMARY.search(log)
    if decoded is None or undefined is None:
        raise ValueError("NDSRecomp audit omitted decoded-instruction or undefined-encoding summary")
    not_implemented = re.search(
        r"(?ms)^\[audit\] codegen NOT-IMPLEMENTED ops.*?:\s*\n(.*?)(?=^\[audit\]|\Z)",
        log,
    )
    not_implemented_text = not_implemented.group(1).strip() if not_implemented else "unknown"
    indirect_outcomes = INDIRECT_OUTCOMES_SUMMARY.search(log)
    region_section = re.search(
        r"(?ms)^\[finder\] unreachable indirect targets by 64K region[^\n]*\n(.*?)(?=^\[|^\[emit\]|\Z)",
        log,
    )
    regions = []
    if region_section is not None:
        for match in INDIRECT_REGION.finditer(region_section.group(1)):
            start = int(match.group(1), 16)
            end = int(match.group(2), 16)
            sites = int(match.group(3))
            if end <= start or sites <= 0:
                raise ValueError("NDSRecomp audit emitted an invalid indirect-target region")
            regions.append({
                "start": match.group(1).lower(),
                "end": match.group(2).lower(),
                "sites": sites,
            })
    if indirect_outcomes is None:
        raise ValueError("NDSRecomp audit omitted indirect-call outcome summary")
    return {
        "functions_total": int(finder.group(1)),
        "functions_arm": int(finder.group(2)),
        "functions_thumb": int(finder.group(3)),
        "indirect": int(finder.group(4)),
        "finder_undefined": int(finder.group(5)),
        "indirect_resolved": int(indirect_outcomes.group(1)),
        "indirect_known_unreachable": int(indirect_outcomes.group(2)),
        "indirect_unknown": int(indirect_outcomes.group(3)),
        "stored_pointer_seeds": int(indirect_outcomes.group(4)),
        "unreachable_indirect_regions": regions,
        "unreachable_indirect_region_sites": sum(row["sites"] for row in regions),
        "decoded_instructions": int(decoded.group(1)),
        "distinct_undefined_arm_encodings": int(undefined.group(1)),
        "distinct_undefined_thumb_encodings": int(undefined.group(2)),
        "codegen_not_implemented_empty": not_implemented_text == "(none)",
        "codegen_not_implemented_summary": not_implemented_text[:2000],
    }


def validate_output_destination(
    image_dir: Path,
    config_dir: Path,
    output_dir: Path,
    existing_bank_dir: Path | None = None,
) -> None:
    if output_dir.is_symlink():
        raise ValueError(f"overlay output directory must not be a symlink: {output_dir}")
    if config_dir.is_symlink():
        raise ValueError("overlay config directory must not be a symlink")
    output_resolved = output_dir.resolve()
    source_dirs = [image_dir.resolve(), config_dir.resolve()]
    if existing_bank_dir is not None:
        if existing_bank_dir.is_symlink():
            raise ValueError("existing overlay bank directory must not be a symlink")
        source_dirs.append(existing_bank_dir.resolve(strict=True))
    for source_dir in source_dirs:
        if (output_resolved == source_dir or
                output_resolved in source_dir.parents or
                source_dir in output_resolved.parents):
            raise ValueError("overlay output directory must be separate from images/configs")
    if output_resolved.exists():
        if not output_resolved.is_dir() or any(output_resolved.iterdir()):
            raise ValueError(
                f"overlay output directory already exists and is not empty: {output_resolved}; "
                "choose a fresh dedicated directory"
            )


def load_reusable_banks(
    existing_dir: Path,
    image_records: dict[int, dict],
    excluded_ids: set[int],
) -> dict[int, dict]:
    """Verify and copy metadata for previously emitted, identity-matching banks."""
    if existing_dir.is_symlink():
        raise ValueError(f"existing overlay bank directory must not be a symlink: {existing_dir}")
    existing_dir = existing_dir.resolve(strict=True)
    if not existing_dir.is_dir():
        raise ValueError(f"existing overlay bank directory is missing or unsafe: {existing_dir}")
    report_path = existing_dir / "emission_report.json"
    if report_path.is_symlink() or not report_path.is_file():
        raise ValueError("existing overlay bank directory has no safe emission_report.json")
    previous = json.loads(report_path.read_text(encoding="utf-8"))
    if (not isinstance(previous, dict) or
            previous.get("schema") != "oracis-overlay-bank-emission-v1" or
            previous.get("rom_sha1") != EXPECTED_ROM_SHA1 or
            previous.get("codegen") != EXPECTED_CODEGEN_IDENTITY or
            not isinstance(previous.get("direction_adapter"), dict) or
            previous["direction_adapter"].get("status") != "passed"):
        raise ValueError("existing overlay emission report is incompatible or unverified")
    rows = previous.get("results")
    if not isinstance(rows, list):
        raise ValueError("existing overlay emission report has no result list")

    reusable: dict[int, dict] = {}
    for row in rows:
        if not isinstance(row, dict) or row.get("status") != "emitted":
            continue
        overlay_id = row.get("overlay_id")
        if type(overlay_id) is not int or overlay_id not in image_records:
            raise ValueError("existing emission report contains an invalid/unknown overlay ID")
        if overlay_id in reusable:
            raise ValueError(f"existing emission report repeats overlay ID {overlay_id}")
        if overlay_id in excluded_ids:
            continue
        if row.get("image_sha1") != image_records[overlay_id].get("image_sha1"):
            raise ValueError(f"existing bank image identity changed for overlay {overlay_id}")
        bank = f"pokemon_platinum_usa_rev1_arm9_overlay_{overlay_id:04d}"
        if row.get("bank") != bank:
            raise ValueError(f"existing bank name mismatch for overlay {overlay_id}")
        folder = existing_dir / f"overlay_{overlay_id:04d}"
        if folder.is_symlink() or not folder.is_dir():
            raise ValueError(f"existing bank folder is missing or unsafe: {folder}")
        hashes = row.get("sha256")
        if not isinstance(hashes, dict):
            raise ValueError(f"existing bank {overlay_id} has no SHA-256 inventory")
        for filename in (f"{bank}.c", f"{bank}.h", f"{bank}_dispatch.c"):
            source = folder / filename
            if (source.is_symlink() or not source.is_file() or source.stat().st_size == 0 or
                    hashes.get(filename) != hash_file(source)):
                raise ValueError(f"existing bank file failed SHA-256 validation: {source}")
        shards = row.get("shards")
        if shards is not None:
            if not isinstance(shards, dict) or shards.get("directory") != f"{bank}_shards":
                raise ValueError(f"existing bank {overlay_id} has malformed shard metadata")
            shard_files = shards.get("files")
            if (not isinstance(shard_files, dict) or not shard_files or
                    shards.get("function_count") != row.get("functions_total")):
                raise ValueError(f"existing bank {overlay_id} has invalid shard inventory")
            shard_dir = folder / shards["directory"]
            if shard_dir.is_symlink() or not shard_dir.is_dir():
                raise ValueError(f"existing bank {overlay_id} shard directory is unsafe")
            if {entry.name for entry in shard_dir.iterdir()} != set(shard_files):
                raise ValueError(f"existing bank {overlay_id} shard set differs from its manifest")
            for filename, expected_sha1 in shard_files.items():
                shard = shard_dir / filename
                if (not re.fullmatch(r"bank_part_[0-9]{4}\.c", filename) or
                        shard.is_symlink() or not shard.is_file() or
                        hash_file(shard, "sha1") != expected_sha1):
                    raise ValueError(f"existing bank {overlay_id} shard failed SHA-1 validation: {shard}")
        reusable[overlay_id] = dict(row)
    return reusable


def emit_overlay_banks(
    recompiler: Path,
    image_dir: Path,
    config_dir: Path,
    output_dir: Path,
    *,
    functions_per_shard: int = 512,
    runner: Callable[[list[str], dict[str, str]], str] = run_command,
    overlay_ids: set[int] | None = None,
    existing_bank_dir: Path | None = None,
) -> dict:
    if not recompiler.is_file() or recompiler.is_symlink():
        raise FileNotFoundError(f"NDSRecomp executable not found or unsafe: {recompiler}")
    manifest, image_records = load_overlay_manifest(image_dir)
    discovery_path = config_dir / "anchor_discovery_report.json"
    if discovery_path.is_symlink() or not discovery_path.is_file():
        raise ValueError("overlay anchor_discovery_report.json is missing or unsafe")
    discovery = json.loads(discovery_path.read_text(encoding="utf-8"))
    if (not isinstance(discovery, dict) or
            discovery.get("schema") != "oracis-overlay-anchor-discovery-v1" or
            discovery.get("rom_sha1") != EXPECTED_ROM_SHA1):
        raise ValueError("overlay anchor discovery report is incompatible")
    if type(functions_per_shard) is not int or functions_per_shard < 1:
        raise ValueError("functions_per_shard must be a positive integer")
    validate_output_destination(image_dir, config_dir, output_dir, existing_bank_dir)
    output_dir.parent.mkdir(parents=True, exist_ok=True)

    environment = os.environ.copy()
    environment["NDSRECOMP_NO_RELOC_SCAN"] = "1"
    identity = runner([str(recompiler), "--codegen-identity"], environment).strip()
    if identity != EXPECTED_CODEGEN_IDENTITY:
        raise RuntimeError(
            f"NDSRecomp codegen identity mismatch: expected {EXPECTED_CODEGEN_IDENTITY}, "
            f"received {identity or '<empty>'}"
        )

    ready_rows = [
        row for row in discovery.get("results", [])
        if isinstance(row, dict) and row.get("status") == "config-ready"
    ]
    if not ready_rows:
        raise ValueError("overlay discovery report contains no config-ready overlays")
    ready_ids: set[int] = set()
    for row in ready_rows:
        overlay_id = row.get("overlay_id")
        if type(overlay_id) is not int or overlay_id not in image_records or overlay_id in ready_ids:
            raise ValueError("overlay discovery report contains a missing/duplicate ID")
        ready_ids.add(overlay_id)

    selected_ids = set(ready_ids) if overlay_ids is None else set(overlay_ids)
    if not selected_ids or not selected_ids.issubset(ready_ids):
        raise ValueError("requested overlay IDs must be a non-empty subset of config-ready IDs")
    for overlay_id in selected_ids:
        if type(overlay_id) is not int or not 0 <= overlay_id <= 999:
            raise ValueError(f"invalid requested overlay ID: {overlay_id!r}")

    reusable_rows = (
        load_reusable_banks(existing_bank_dir, image_records, selected_ids)
        if existing_bank_dir is not None else {}
    )
    results: list[dict] = []
    with tempfile.TemporaryDirectory(
        prefix=f".{output_dir.name}.staging-",
        dir=output_dir.parent,
    ) as staging_name:
        staging_dir = Path(staging_name) / "complete"
        staging_dir.mkdir()
        for overlay_id, previous_row in sorted(reusable_rows.items()):
            folder = staging_dir / f"overlay_{overlay_id:04d}"
            folder.mkdir()
            old_folder = existing_bank_dir / f"overlay_{overlay_id:04d}"
            for suffix in (".c", ".h", "_dispatch.c"):
                filename = f"{previous_row['bank']}{suffix}"
                shutil.copy2(old_folder / filename, folder / filename)
            shards = previous_row.get("shards")
            if shards is not None:
                shard_dir = old_folder / shards["directory"]
                target_shard_dir = folder / shards["directory"]
                target_shard_dir.mkdir()
                for filename in sorted(shards["files"]):
                    shutil.copy2(shard_dir / filename, target_shard_dir / filename)
            reused = dict(previous_row)
            reused["reused"] = True
            reused["audit_status"] = "previously-audited-and-hash-verified"
            reused["output_dir"] = str(output_dir / f"overlay_{overlay_id:04d}")
            results.append(reused)

        for row in sorted(ready_rows, key=lambda item: item["overlay_id"]):
            overlay_id = row["overlay_id"]
            if overlay_id not in selected_ids:
                continue
            image_record = image_records[overlay_id]
            config_name = row.get("config")
            if (not isinstance(config_name, str) or not config_name or
                    Path(config_name).name != config_name or not config_name.endswith(".toml")):
                raise ValueError(f"unsafe config filename for overlay {overlay_id}")
            config_path = config_dir / config_name
            config = validate_overlay_config(config_path, overlay_id, image_record)
            image_path = image_dir / image_record["image_file"]
            output_folder = staging_dir / f"overlay_{overlay_id:04d}"
            output_folder.mkdir()
            bank = config["bank"]

            audit_log = runner([
                str(recompiler), "--config", str(config_path),
                "--bin", str(image_path), "--audit",
            ], environment)
            audit = parse_audit(audit_log)
            if not audit["codegen_not_implemented_empty"]:
                raise RuntimeError(
                    f"overlay {overlay_id} contains NDSRecomp codegen gaps; "
                    "refusing to publish this static bank"
                )
            emit_log = runner([
                str(recompiler), "--config", str(config_path),
                "--bin", str(image_path), "--out", str(output_folder),
                "--bank", bank, "--validate-live-bytes",
            ], environment)
            generated = [output_folder / f"{bank}.c",
                         output_folder / f"{bank}.h",
                         output_folder / f"{bank}_dispatch.c"]
            for path in generated:
                if path.is_symlink() or not path.is_file() or path.stat().st_size == 0:
                    raise RuntimeError(f"NDSRecomp failed to emit a non-empty file: {path}")

            body = generated[0]
            adapted, read_calls, write_calls = patch_generated_source(
                body.read_text(encoding="utf-8")
            )
            body.write_text(adapted, encoding="utf-8", newline="\n")
            shards = split_generated_bank(
                body, generated[1], output_folder, bank,
                functions_per_shard=functions_per_shard,
            )
            dispatch_text = generated[2].read_text(encoding="utf-8")
            if (f"const NdsDispatchEntry g_dispatch_{bank}[]" not in dispatch_text or
                    f"const unsigned g_dispatch_{bank}_len" not in dispatch_text):
                raise RuntimeError(f"generated dispatch table symbols are missing for overlay {overlay_id}")

            results.append({
                "overlay_id": overlay_id,
                "bank": bank,
                "status": "emitted",
                "returncode": 0,
                **audit,
                "audit_status": "passed",
                "reused": False,
                "direction_adapter_status": "passed",
                "direction_reads": read_calls,
                "direction_writes": write_calls,
                "direction_timing_calls": read_calls + write_calls,
                "generated_bytes": {path.name: path.stat().st_size for path in generated},
                "sha256": {path.name: hash_file(path) for path in generated},
                "image_sha1": image_record["image_sha1"],
                "output_dir": str(output_dir / f"overlay_{overlay_id:04d}"),
                "config": str(config_path),
                "entry_count": config["entry_count"],
                "shards": shards,
            })
            print(
                f"overlay {overlay_id:04d}: {audit['functions_total']} functions, "
                f"indirect unknown={audit['indirect_unknown']} "
                f"undefined={audit['finder_undefined']} "
                f"unmapped-sites={audit['unreachable_indirect_region_sites']}, "
                f"{read_calls} reads/{write_calls} writes, "
                f"{len(shards['files'])} C shards",
                flush=True,
            )

        compiled_ids = {row["overlay_id"] for row in results}
        uncompiled_ids = sorted(set(image_records) - compiled_ids)
        total_reads = sum(int(row.get("direction_reads", 0)) for row in results)
        total_writes = sum(int(row.get("direction_writes", 0)) for row in results)
        audit_metric_keys = (
            "finder_undefined",
            "indirect_resolved",
            "indirect_known_unreachable",
            "indirect_unknown",
            "stored_pointer_seeds",
            "unreachable_indirect_region_sites",
        )
        audited_rows = [
            row for row in results
            if all(type(row.get(key)) is int for key in audit_metric_keys)
        ]
        missing_audit_metrics = sorted(
            row["overlay_id"] for row in results if row not in audited_rows
        )
        report = {
            "schema": "oracis-overlay-bank-emission-v1",
            "rom_sha1": manifest["rom_sha1"],
            "codegen": identity,
            "source_overlay_count": len(image_records),
            "compiled_overlay_count": len(results),
            "emitted_this_run_count": sum(not row.get("reused", False) for row in results),
            "reused_overlay_count": sum(bool(row.get("reused", False)) for row in results),
            "uncompiled_overlay_count": len(uncompiled_ids),
            "uncompiled_overlay_ids": uncompiled_ids,
            "audit_metrics_overlay_count": len(audited_rows),
            "audit_metrics_missing_overlay_ids": missing_audit_metrics,
            "audit_totals": {
                key: sum(row[key] for row in audited_rows)
                for key in audit_metric_keys
            },
            "direction_adapter": {
                "identity": ADAPTER_IDENTITY,
                "status": "passed",
                "total_reads": total_reads,
                "total_writes": total_writes,
            },
            "results": results,
            "notice": (
                "Local ROM-derived C sources and objects. Do not commit or distribute. "
                "Auditing/emission does not prove that all overlay references are resolved."
            ),
        }
        (staging_dir / "emission_report.json").write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        if output_dir.exists():
            output_dir.rmdir()
        staging_dir.replace(output_dir)
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--recompiler", type=Path,
        default=ROOT / ".oracis" / "ndsrecomp-build" / (
            "nds_recompile.exe" if sys.platform == "win32" else "nds_recompile"
        ),
    )
    parser.add_argument(
        "--overlay-image-dir", type=Path,
        default=ROOT / ".oracis" / "local" / "pokemon-platinum-usa-rev1" / "overlays",
    )
    parser.add_argument(
        "--config-dir", type=Path,
        default=ROOT / ".oracis" / "local" / "pokemon-platinum-usa-rev1" / "overlays",
    )
    parser.add_argument(
        "--output-dir", type=Path,
        default=ROOT / ".oracis" / "generated" / "pokemon-platinum-usa-rev1-overlays",
    )
    parser.add_argument("--functions-per-shard", type=int, default=512)
    parser.add_argument("--overlay-ids",
                        help="Comma-separated overlay IDs to re-audit/re-emit; other matching banks may be reused.")
    parser.add_argument("--reuse-existing-bank-dir", type=Path,
                        help="Previously emitted bank set whose non-selected overlays are SHA-256 verified and copied.")
    args = parser.parse_args()
    try:
        selected_ids = None
        if args.overlay_ids:
            parts = [part.strip() for part in args.overlay_ids.split(",") if part.strip()]
            selected_ids = {int(part, 16) if part.lower().startswith("0x") else int(part, 10) for part in parts}
            if len(selected_ids) != len(parts):
                raise ValueError("--overlay-ids contains duplicate values")
        report = emit_overlay_banks(
            args.recompiler.expanduser(),
            args.overlay_image_dir.expanduser(),
            args.config_dir.expanduser(),
            args.output_dir.expanduser(),
            functions_per_shard=args.functions_per_shard,
            overlay_ids=selected_ids,
            existing_bank_dir=args.reuse_existing_bank_dir.expanduser() if args.reuse_existing_bank_dir else None,
        )
    except (OSError, ValueError, RuntimeError, KeyError, TypeError, json.JSONDecodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(
        f"overlay banks: {report['compiled_overlay_count']}/{report['source_overlay_count']} emitted; "
        f"{report['uncompiled_overlay_count']} without static banks; "
        f"fresh={report['emitted_this_run_count']} reused={report['reused_overlay_count']}; "
        f"functions={sum(row['functions_total'] for row in report['results'])}"
    )
    print(f"Local overlay banks: {args.output_dir.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
