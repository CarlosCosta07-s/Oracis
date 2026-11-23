#!/usr/bin/env python3
"""Discover verified per-overlay NDSRecomp configs from address-named source files.

The user-supplied source tree is inspected only for C source filenames of the
form src/overlayNNN/ovNNN_ADDRESS.c, plus optional function-pointer header
filenames. No source file contents are copied or compiled. Extracted ROM-derived
images and generated TOMLs stay local.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import struct
from pathlib import Path
from zipfile import BadZipFile, ZipFile

EXPECTED_ROM_SHA1 = "0862ec35b24de5c7e2dcb88c9eea0873110d755c"
SOURCE_RE = re.compile(
    r"(?:^|/)src/overlay(?P<folder>\d{3})/ov\d+_(?P<address>[0-9A-Fa-f]{8})\.c$"
)
FUNCTION_POINTER_HEADER_RE = re.compile(
    r"(?:^|/)include/overlay(?P<folder>\d{3})/funcptr_ov\d+_(?P<address>[0-9A-Fa-f]{8})\.h$"
)


def file_hash(path: Path, algorithm: str) -> str:
    digest = hashlib.new(algorithm)
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def load_overlay_records(image_dir: Path) -> dict[int, dict]:
    image_dir = image_dir.resolve(strict=True)
    manifest_path = image_dir / "manifest.json"
    if manifest_path.is_symlink() or not manifest_path.is_file():
        raise ValueError("overlay extraction manifest is missing or unsafe")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if not isinstance(manifest, dict) or manifest.get("schema") != "oracis-nds-overlays-v1":
        raise ValueError("unsupported overlay extraction manifest")
    if manifest.get("rom_sha1") != EXPECTED_ROM_SHA1:
        raise ValueError("overlay manifest does not match Pokémon Platinum USA Rev 1")
    rows = manifest.get("overlays")
    if not isinstance(rows, list) or manifest.get("overlay_count") != len(rows):
        raise ValueError("overlay manifest count is inconsistent")

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
        ram_size = row.get("ram_size")
        sha1 = row.get("image_sha1")
        sha256 = row.get("image_sha256")
        if (type(size) is not int or size <= 0 or ram_size != size or
                not isinstance(sha1, str) or not re.fullmatch(r"[0-9a-f]{40}", sha1) or
                not isinstance(sha256, str) or not re.fullmatch(r"[0-9a-f]{64}", sha256)):
            raise ValueError(f"invalid image identity metadata for overlay {overlay_id}")
        if (image.stat().st_size != size or file_hash(image, "sha1") != sha1 or
                file_hash(image, "sha256") != sha256):
            raise ValueError(f"extracted overlay image integrity failure for ID {overlay_id}")
        records[overlay_id] = row
    if not records:
        raise ValueError("overlay manifest contains no ARM9 overlays")
    return records


def source_anchors(
    source: Path,
    include_function_pointer_headers: bool = False,
) -> tuple[dict[int, list[tuple[int, str]]], str, int]:
    anchors: dict[int, set[tuple[int, str]]] = {}
    matched_files = 0
    if source.is_file():
        try:
            with ZipFile(source) as archive:
                names = archive.namelist()
        except BadZipFile as exc:
            raise ValueError(f"source archive is not a valid ZIP: {source}") from exc
        source_digest = file_hash(source, "sha1")
        for name in names:
            match = SOURCE_RE.search(name)
            is_function_pointer_header = False
            if match is None and include_function_pointer_headers:
                match = FUNCTION_POINTER_HEADER_RE.search(name)
                is_function_pointer_header = match is not None
            if not match:
                continue
            matched_files += 1
            overlay_id = int(match.group("folder"))
            address = int(match.group("address"), 16)
            stem = Path(name).stem
            if is_function_pointer_header:
                stem = stem.removeprefix("funcptr_")
            anchors.setdefault(overlay_id, set()).add((address, stem))
    elif source.is_dir():
        if source.is_symlink():
            raise ValueError("source directory must not be a symlink")
        source_digest = "directory-input-no-archive-hash"
        for path in source.rglob("*.c"):
            if path.is_symlink() or not path.is_file():
                continue
            relative = path.relative_to(source).as_posix()
            match = SOURCE_RE.search("/" + relative)
            is_function_pointer_header = False
            if match is None and include_function_pointer_headers:
                match = FUNCTION_POINTER_HEADER_RE.search("/" + relative)
                is_function_pointer_header = match is not None
            if not match:
                continue
            matched_files += 1
            overlay_id = int(match.group("folder"))
            address = int(match.group("address"), 16)
            stem = path.stem
            if is_function_pointer_header:
                stem = stem.removeprefix("funcptr_")
            anchors.setdefault(overlay_id, set()).add((address, stem))
    else:
        raise FileNotFoundError(f"source ZIP or directory not found: {source}")
    return {key: sorted(value) for key, value in anchors.items()}, source_digest, matched_files


def static_init_seeds(
    image_dir: Path,
    overlay_id: int,
    row: dict,
) -> list[tuple[int, str, str, str]]:
    """Read exact ARM/Thumb function pointers from the NDS overlay static-init table."""
    start = row.get("static_init_start")
    end = row.get("static_init_end")
    if start == 0 and end == 0:
        return []
    load = row.get("ram_address")
    size = row.get("ram_size")
    if (type(start) is not int or type(end) is not int or
            type(load) is not int or type(size) is not int or
            size <= 0 or start < load or end < start or
            end > load + size or (start & 3) != 0 or
            (end & 3) != 0 or ((end - start) % 4) != 0):
        raise ValueError(f"invalid static-init range in overlay {overlay_id}")

    image_name = row.get("image_file")
    if not isinstance(image_name, str) or Path(image_name).name != image_name:
        raise ValueError(f"unsafe image filename for static-init overlay {overlay_id}")
    image_path = image_dir / image_name
    image = image_path.read_bytes()
    if len(image) != size:
        raise ValueError(f"static-init image size mismatch for overlay {overlay_id}")

    offset = start - load
    table_size = end - start
    if offset < 0 or table_size > len(image) - offset:
        raise ValueError(f"static-init table exceeds overlay image for ID {overlay_id}")

    seeds: dict[tuple[int, str], tuple[int, str, str, str]] = {}
    for index in range(table_size // 4):
        pointer = struct.unpack_from("<I", image, offset + index * 4)[0]
        if pointer == 0:
            continue
        thumb = (pointer & 1) != 0
        address = pointer & ~1
        mode = "thumb" if thumb else "arm"
        alignment = 2 if thumb else 4
        instruction_size = 2 if thumb else 4
        if (address < load or address % alignment != 0 or
                address + instruction_size > load + size):
            raise ValueError(
                f"static-init pointer {pointer:#010x} is outside/misaligned "
                f"for overlay {overlay_id}"
            )
        name = f"ov{overlay_id}_{address:08X}"
        seeds[(address, mode)] = (
            address, name, mode, "overlay static-init pointer"
        )
    return [seeds[key] for key in sorted(seeds)]


def render_config(
    overlay_id: int,
    row: dict,
    entries: list[tuple[int, str, str, str]],
) -> str:
    load = row["ram_address"]
    size = row["ram_size"]
    if (type(load) is not int or type(size) is not int or size <= 0 or
            load < 0 or load + size > 0x100000000):
        raise ValueError(f"invalid RAM range in overlay {overlay_id}")
    image_sha1 = row["image_sha1"]
    lines = [
        "[program]",
        f'name = "Pokémon Platinum USA Rev 1 ARM9 overlay {overlay_id}"',
        f'id = "pokemon_platinum_usa_rev1_arm9_overlay_{overlay_id:04d}"',
        f"load_address = 0x{load:08X}",
        f"size = 0x{size:08X}",
        f"entry_pc = 0x{entries[0][0]:08X}",
        "authoritative_entry_points = false",
        "",
        "[identity]",
        f'sha1 = "{image_sha1}"',
        "",
    ]
    for address, name, mode, seed_source in entries:
        if (not re.fullmatch(r"ov\d+_[0-9A-Fa-f]{8}", name) or
                mode not in {"arm", "thumb"} or
                seed_source not in {
                    "source filename (provisional Thumb)",
                    "overlay static-init pointer",
                }):
            raise ValueError(f"unsafe entry-point seed metadata: {name!r}")
        lines.extend([
            f"# Seed source: {seed_source}",
            "[[entry_point]]",
            f"addr = 0x{address:08X}",
            f'mode = "{mode}"',
            f'name = "{name}"',
            "",
        ])
    return "\n".join(lines)


def discover(
    image_dir: Path,
    source: Path,
    output_dir: Path,
    include_function_pointer_headers: bool = False,
    include_static_init_pointers: bool = False,
) -> dict:
    image_dir = image_dir.expanduser().resolve(strict=True)
    records = load_overlay_records(image_dir)
    candidates, source_sha1, matched_files = source_anchors(
        source, include_function_pointer_headers
    )
    output_dir = output_dir.expanduser()
    if output_dir.is_symlink():
        raise ValueError("config output directory must not be a symlink")
    output_dir.mkdir(parents=True, exist_ok=True)
    results = []
    for overlay_id, row in sorted(records.items()):
        anchors = candidates.get(overlay_id, [])
        load = row["ram_address"]
        end = load + row["ram_size"]
        invalid = [
            (address, name) for address, name in anchors
            if not load <= address < end or (address & 1) != 0
        ]
        if invalid:
            results.append({
                "overlay_id": overlay_id,
                "status": "invalid-source-anchor",
                "anchor_count": len(anchors),
                "source_anchor_count": len(anchors),
                "static_init_seed_count": 0,
                "invalid_anchors": [
                    {"address": f"0x{address:08X}", "name": name}
                    for address, name in invalid
                ],
            })
            continue

        static_seeds = (
            static_init_seeds(image_dir, overlay_id, row)
            if include_static_init_pointers else []
        )
        # Exact mode encoded by the NDS metadata takes precedence over the
        # provisional Thumb assumption of address-named decomp source files.
        unique_entries: dict[tuple[int, str], tuple[int, str, str, str]] = {}
        for seed in static_seeds:
            unique_entries[(seed[0], seed[2])] = seed
        for address, name in anchors:
            unique_entries.setdefault(
                (address, "thumb"),
                (address, name, "thumb", "source filename (provisional Thumb)"),
            )
        entries = sorted(
            unique_entries.values(),
            key=lambda item: (item[0], item[2], item[1]),
        )
        if not entries:
            results.append({
                "overlay_id": overlay_id,
                "status": "no-entry-seeds",
                "anchor_count": 0,
                "source_anchor_count": 0,
                "static_init_seed_count": 0,
            })
            continue

        config = output_dir / f"arm9_overlay_{overlay_id:04d}.toml"
        content = render_config(overlay_id, row, entries)
        if config.exists():
            if config.is_symlink() or not config.is_file():
                raise ValueError(f"existing config path is unsafe: {config}")
            if config.read_text(encoding="utf-8") != content:
                raise ValueError(
                    f"existing config differs from discovered metadata: {config}; "
                    "move it aside before regenerating"
                )
        else:
            config.write_text(content, encoding="utf-8", newline="\n")
        results.append({
            "overlay_id": overlay_id,
            "status": "config-ready",
            "anchor_count": len(entries),
            "source_anchor_count": len(anchors),
            "static_init_seed_count": len(static_seeds),
            "static_init_only": len(anchors) == 0 and len(static_seeds) != 0,
            "entry_point_seeds": [
                {
                    "address": f"0x{address:08X}",
                    "mode": mode,
                    "source": seed_source,
                }
                for address, _name, mode, seed_source in entries
            ],
            "entry_pc": f"0x{entries[0][0]:08X}",
            "config": config.name,
            "image_sha1": row["image_sha1"],
        })

    source_only_ids = sorted(set(candidates) - set(records))
    report = {
        "schema": "oracis-overlay-anchor-discovery-v1",
        "rom_sha1": EXPECTED_ROM_SHA1,
        "source_sha1": source_sha1,
        "source_matched_candidate_files": matched_files,
        "includes_function_pointer_headers": include_function_pointer_headers,
        "includes_static_init_pointers": include_static_init_pointers,
        "source_only_overlay_ids": source_only_ids,
        "source_anchor_overlay_count": sum(bool(candidates.get(oid)) for oid in records),
        "static_init_seed_overlay_count": sum(
            item.get("static_init_seed_count", 0) > 0 for item in results
        ),
        "static_init_seed_count": sum(
            item.get("static_init_seed_count", 0) for item in results
        ),
        "without_source_anchors": sum(
            item.get("source_anchor_count", 0) == 0 for item in results
        ),
        "without_any_candidates": sum(
            item["status"] == "no-entry-seeds" for item in results
        ),
        "mode_policy": (
            "Source filenames use provisional Thumb; overlay static-init pointers "
            "use the exact ARM/Thumb bit encoded in the pointer. All entry seeds "
            "remain non-authoritative."
        ),
        "overlay_count": len(records),
        "config_ready_count": sum(item["status"] == "config-ready" for item in results),
        "invalid_anchor_overlays": sum(item["status"] == "invalid-source-anchor" for item in results),
        "results": results,
    }
    (output_dir / "anchor_discovery_report.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--overlay-image-dir", type=Path, required=True)
    parser.add_argument(
        "--source",
        type=Path,
        required=True,
        help="User-supplied Poképlatinum ZIP or source directory",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path(".oracis/local/pokemon-platinum-usa-rev1/overlays"),
    )
    parser.add_argument(
        "--include-function-pointer-headers",
        action="store_true",
        help="Also use funcptr_ovNNN_ADDRESS.h names as provisional indirect-call candidates.",
    )
    parser.add_argument(
        "--include-static-init-pointers",
        action="store_true",
        help=(
            "Also seed exact ARM/Thumb entry candidates from the overlay table's "
            "static_init_start..static_init_end function-pointer array."
        ),
    )
    args = parser.parse_args()
    try:
        report = discover(
            args.overlay_image_dir,
            args.source,
            args.output_dir,
            include_function_pointer_headers=args.include_function_pointer_headers,
            include_static_init_pointers=args.include_static_init_pointers,
        )
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(
        f"overlay configs: {report['config_ready_count']}/{report['overlay_count']} ready; "
        f"{report['without_source_anchors']} have no source anchors; "
        f"{report['static_init_seed_count']} static-init seeds across "
        f"{report['static_init_seed_overlay_count']} overlays; "
        f"{report['without_any_candidates']} without any entry candidates; "
        f"{report['invalid_anchor_overlays']} contain invalid source anchors"
    )
    print(f"Local anchor discovery report: {(args.output_dir / 'anchor_discovery_report.json').resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
