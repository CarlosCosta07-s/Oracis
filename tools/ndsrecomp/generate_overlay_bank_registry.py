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


def generate(bank_dir: Path, output_cpp: Path, manifest_path: Path) -> dict:
    bank_dir = bank_dir.resolve(strict=True)
    report_path = bank_dir / "emission_report.json"
    report = json.loads(report_path.read_text(encoding="utf-8"))
    if report.get("schema") != "oracis-overlay-bank-emission-v1":
        raise ValueError("unsupported or missing overlay emission report schema")
    if report.get("codegen") != "nds-codegen-v3":
        raise ValueError("overlay banks were not emitted by the pinned nds-codegen-v3")
    if report.get("rom_sha1") != "0862ec35b24de5c7e2dcb88c9eea0873110d755c":
        raise ValueError("overlay banks do not match Pokémon Platinum USA Rev 1")
    if report.get("direction_adapter", {}).get("status") != "passed":
        raise ValueError("overlay memory-direction adapter did not pass")
    records = []
    seen_ids: set[int] = set()
    for row in report.get("results", []):
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
    manifest = {
        "schema": "oracis-overlay-registry-v1",
        "codegen": report["codegen"],
        "rom_sha1": report.get("rom_sha1"),
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
    parser.add_argument("--output-cpp", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = generate(args.bank_dir, args.output_cpp, args.manifest)
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as exc:
        parser.error(str(exc))
    print(
        f"overlay registry: {result['bank_count']} ARM9 banks, "
        f"{result['function_count']} functions; registrar SHA-256 "
        f"{result['registrar_sha256']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
