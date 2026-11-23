#!/usr/bin/env python3
"""Generate a local-only C++ registrar for validated retail overlay banks.

The ROM-derived C/H files and the generated registrar stay outside Git. This
script emits only a build-tree C++ source and a hash inventory.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path


BANK_RE = re.compile(r"pokemon_platinum_usa_rev1_arm9_overlay_(\d{4})\Z")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()



def sha1(path: Path) -> str:
    digest = hashlib.sha1()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def load_source_overlay_manifest(image_dir: Path) -> dict[int, dict]:
    image_dir = image_dir.resolve(strict=True)
    source_manifest_path = image_dir / "manifest.json"
    if source_manifest_path.is_symlink() or not source_manifest_path.is_file():
        raise ValueError("extracted overlay manifest is missing or unsafe")
    manifest = json.loads(source_manifest_path.read_text(encoding="utf-8"))
    if not isinstance(manifest, dict):
        raise ValueError("extracted overlay manifest root must be an object")
    if manifest.get("schema") != "oracis-nds-overlays-v1":
        raise ValueError("unsupported or missing extracted overlay manifest schema")
    if manifest.get("rom_sha1") != "0862ec35b24de5c7e2dcb88c9eea0873110d755c":
        raise ValueError("extracted overlays do not match Pokémon Platinum USA Rev 1")
    rows = manifest.get("overlays")
    if not isinstance(rows, list) or manifest.get("overlay_count") != len(rows):
        raise ValueError("extracted overlay manifest count is inconsistent")

    records: dict[int, dict] = {}
    for row in rows:
        if not isinstance(row, dict) or row.get("cpu") != "arm9":
            raise ValueError("the current compiled overlay registry accepts ARM9 records only")
        overlay_id = row.get("overlay_id")
        if type(overlay_id) is not int or not 0 <= overlay_id <= 0xFFFFFFFF:
            raise ValueError("extracted overlay manifest contains an invalid overlay ID")
        if overlay_id in records:
            raise ValueError(f"duplicate extracted ARM9 overlay ID {overlay_id}")

        image_name = row.get("image_file")
        if (not isinstance(image_name, str) or not image_name or
                Path(image_name).name != image_name or image_name == "manifest.json"):
            raise ValueError(f"overlay {overlay_id} has an unsafe image filename")
        image_path = image_dir / image_name
        if image_path.is_symlink() or not image_path.is_file():
            raise ValueError(f"missing or unsafe extracted overlay image: {image_path}")
        image_size = row.get("image_size")
        image_sha1 = row.get("image_sha1")
        image_sha256 = row.get("image_sha256")
        if (type(image_size) is not int or image_size <= 0 or
                not isinstance(image_sha1, str) or
                not re.fullmatch(r"[0-9a-f]{40}", image_sha1) or
                not isinstance(image_sha256, str) or
                not re.fullmatch(r"[0-9a-f]{64}", image_sha256)):
            raise ValueError(f"overlay {overlay_id} has invalid image identity metadata")
        if image_path.stat().st_size != image_size:
            raise ValueError(f"extracted overlay image size mismatch for ID {overlay_id}")
        if sha1(image_path) != image_sha1 or sha256(image_path) != image_sha256:
            raise ValueError(f"extracted overlay image hash mismatch for ID {overlay_id}")
        if row.get("ram_size") != image_size:
            raise ValueError(f"overlay {overlay_id} RAM size does not match its image size")
        records[overlay_id] = row
    return records


def generate(
    bank_dir: Path,
    overlay_image_dir: Path,
    output_cpp: Path,
    manifest_path: Path,
) -> dict:
    bank_dir = bank_dir.resolve(strict=True)
    source_overlays = load_source_overlay_manifest(overlay_image_dir)
    report_path = bank_dir / "emission_report.json"
    report = json.loads(report_path.read_text(encoding="utf-8"))
    if not isinstance(report, dict):
        raise ValueError("overlay emission report root must be an object")
    if report.get("schema") != "oracis-overlay-bank-emission-v1":
        raise ValueError("unsupported or missing overlay emission report schema")
    if report.get("codegen") != "nds-codegen-v3":
        raise ValueError("overlay banks were not emitted by the pinned nds-codegen-v3")
    if report.get("rom_sha1") != "0862ec35b24de5c7e2dcb88c9eea0873110d755c":
        raise ValueError("overlay banks do not match Pokémon Platinum USA Rev 1")
    adapter = report.get("direction_adapter")
    if not isinstance(adapter, dict) or adapter.get("status") != "passed":
        raise ValueError("overlay memory-direction adapter did not pass")
    results = report.get("results")
    if not isinstance(results, list):
        raise ValueError("overlay emission report results must be a list")
    records = []
    seen_ids: set[int] = set()
    for row in results:
        if not isinstance(row, dict):
            raise ValueError("overlay emission report contains a non-object result")
        if row.get("status") != "emitted":
            continue
        bank = row.get("bank", "")
        match = BANK_RE.fullmatch(bank)
        if not match:
            raise ValueError(f"unexpected overlay bank identifier: {bank!r}")
        overlay_id = int(match.group(1))
        if overlay_id in seen_ids:
            raise ValueError(f"duplicate overlay ID {overlay_id}")
        seen_ids.add(overlay_id)
        if row.get("overlay_id") != overlay_id:
            raise ValueError(f"overlay ID disagrees with bank name: {bank}")
        source = source_overlays.get(overlay_id)
        if source is None:
            raise ValueError(f"compiled overlay ID {overlay_id} is absent from extracted manifest")
        if row.get("image_sha1") != source.get("image_sha1"):
            raise ValueError(
                f"compiled overlay image hash does not match extracted image for ID {overlay_id}"
            )
        folder = bank_dir / f"overlay_{overlay_id:04d}"
        body = folder / f"{bank}.c"
        header = folder / f"{bank}.h"
        dispatch = folder / f"{bank}_dispatch.c"
        for path in (body, header, dispatch):
            if path.is_symlink() or not path.is_file() or path.stat().st_size == 0:
                raise ValueError(f"missing, empty, or unsafe overlay bank file: {path}")
        records.append({
            "overlay_id": overlay_id,
            "bank": bank,
            "body": str(body),
            "header": str(header),
            "dispatch": str(dispatch),
            "body_sha256": sha256(body),
            "header_sha256": sha256(header),
            "dispatch_sha256": sha256(dispatch),
            "function_count": row.get("functions_total"),
        })

    if not records:
        raise ValueError("no emitted overlay banks found")
    records.sort(key=lambda item: item["overlay_id"])

    lines = [
        "/* Generated locally by generate_overlay_bank_registry.py. */",
        '#include "oracis/recomp/static_dispatch_abi.h"',
        '#include "oracis/recomp/static_dispatch.hpp"',
        "#include <cstddef>",
        "",
    ]
    for item in records:
        bank = item["bank"]
        lines.extend([
            f'extern "C" const NdsDispatchEntry g_dispatch_{bank}[];',
            f'extern "C" const unsigned g_dispatch_{bank}_len;',
        ])
    lines.extend([
        "",
        'extern "C" int oracis_register_compiled_overlay_banks(void)',
        "{",
        "    auto &registry = oracis::recomp::global_static_dispatch_registry();",
        "    struct Added { const NdsDispatchEntry *table; unsigned length; };",
        f"    Added added[{len(records)}]{{}};",
        "    std::size_t added_count = 0u;",
    ])
    for item in records:
        bank = item["bank"]
        overlay_id = item["overlay_id"]
        lines.extend([
            f"    if (!registry.contains_bank(NDS_ARM9, g_dispatch_{bank}, g_dispatch_{bank}_len)) {{",
            "        const int status = oracis_static_dispatch_register_bank(",
            f'            NDS_ARM9, g_dispatch_{bank}, g_dispatch_{bank}_len,',
            f'            "pokemon-platinum-usa-rev1-overlay-{overlay_id:04d}");',
            "        if (status != ORACIS_DISPATCH_REGISTER_OK) {",
            "            while (added_count != 0u) {",
            "                --added_count;",
            "                (void)oracis_static_dispatch_unregister_bank(",
            "                    NDS_ARM9, added[added_count].table, added[added_count].length);",
            "            }",
            "            return status;",
            "        }",
            f"        added[added_count++] = Added{{g_dispatch_{bank}, g_dispatch_{bank}_len}};",
            "    }",
        ])
    lines.extend(["    return ORACIS_DISPATCH_REGISTER_OK;", "}", ""])
    output_cpp.parent.mkdir(parents=True, exist_ok=True)
    output_cpp.write_text("\n".join(lines), encoding="utf-8")
    emitted_ids = {item["overlay_id"] for item in records}
    source_ids = set(source_overlays)
    uncompiled_ids = sorted(source_ids - emitted_ids)
    manifest = {
        "schema": "oracis-overlay-registry-v1",
        "codegen": report["codegen"],
        "rom_sha1": report.get("rom_sha1"),
        "source_overlay_count": len(source_overlays),
        "compiled_overlay_count": len(records),
        "uncompiled_overlay_count": len(uncompiled_ids),
        "uncompiled_overlay_ids": uncompiled_ids,
        "bank_count": len(records),
        "function_count": sum(x.get("function_count") or 0 for x in records),
        "banks": records,
        "registrar_sha256": sha256(output_cpp),
    }
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bank-dir", type=Path, required=True)
    parser.add_argument("--overlay-image-dir", type=Path, required=True)
    parser.add_argument("--output-cpp", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = generate(
            args.bank_dir,
            args.overlay_image_dir,
            args.output_cpp,
            args.manifest,
        )
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as exc:
        parser.error(str(exc))
    print(
        f"overlay registry: {result['bank_count']} ARM9 banks, "
        f"{result['function_count']} functions; coverage "
        f"{result['compiled_overlay_count']}/{result['source_overlay_count']} "
        f"(uncompiled: {result['uncompiled_overlay_count']}); registrar SHA-256 "
        f"{result['registrar_sha256']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
