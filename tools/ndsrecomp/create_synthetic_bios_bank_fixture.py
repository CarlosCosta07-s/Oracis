#!/usr/bin/env python3
"""Create non-distributable synthetic BIOS banks for CMake/link smoke tests."""
from __future__ import annotations
import argparse, hashlib, json
from pathlib import Path

BANK_PREFIX = "oracis_user_bios"
CPUS = ("arm9", "arm7")
LAYOUT = {"arm9": {"size": 0x1000, "base": 0xFFFF0000},
          "arm7": {"size": 0x4000, "base": 0x00000000}}
VECTORS = ((0x00,"reset"),(0x04,"undefined"),(0x08,"swi"),(0x0C,"prefetch_abort"),
           (0x10,"data_abort"),(0x18,"irq"),(0x1C,"fiq"))

def sha1(data: bytes) -> str:
    return hashlib.sha1(data).hexdigest()

def create_fixture(output_dir: Path) -> dict:
    if output_dir.exists():
        if output_dir.is_symlink() or not output_dir.is_dir():
            raise ValueError(f"unsafe fixture destination: {output_dir}")
        if any(output_dir.iterdir()):
            raise ValueError(f"fixture destination must be empty: {output_dir}")
    output_dir.mkdir(parents=True, exist_ok=True)
    banks = {}
    for cpu in CPUS:
        bank = f"{BANK_PREFIX}_{cpu}"
        base = LAYOUT[cpu]["base"]
        prototypes, bodies, rows, vectors = [], [], [], []
        for off, name in VECTORS:
            address, fn = base + off, f"{bank}_vector_{name}"
            prototypes.append(f"void {fn}(void);")
            bodies.append(f"void {fn}(void) {{ (void)0; }}")
            rows.append(f"    {{ UINT32_C(0x{address:08X}), 0u, &{fn}, NULL }}")
            vectors.append({"address": address, "name": name, "mode": "arm"})
        header = '#include "oracis/recomp/runtime_arm.h"\n' + "\n".join(prototypes) + "\n"
        body = f'#include "{bank}.h"\n' + "\n".join(bodies) + "\n"
        dispatch = (f'#include "{bank}.h"\n'
                    f"const NdsDispatchEntry g_dispatch_{bank}[] = {{\n"
                    + ",\n".join(rows) + "\n};\n"
                    f"const unsigned g_dispatch_{bank}_len = {len(rows)}u;\n")
        (output_dir / f"{bank}.h").write_text(header, encoding="utf-8")
        (output_dir / f"{bank}.c").write_text(body, encoding="utf-8")
        (output_dir / f"{bank}_dispatch.c").write_text(dispatch, encoding="utf-8")
        shard_dir = output_dir / f"{bank}_shards"
        shard_dir.mkdir()
        shard_name = "bank_part_0000.c"
        (shard_dir / shard_name).write_text(body, encoding="utf-8")
        file_names = (f"{bank}.c", f"{bank}.h", f"{bank}_dispatch.c")
        banks[cpu] = {
            "bank": bank, "load_address": base, "entry_pc": base,
            "entry_mode": "arm", "size": LAYOUT[cpu]["size"], "bios_sha1": "0"*40,
            "exception_vectors": vectors,
            "memory_direction_adapter": "synthetic-fixture",
            "memory_direction_calls": {"reads": 0, "writes": 0},
            "files": {name: sha1((output_dir / name).read_bytes()) for name in file_names},
            "shards": {"directory": shard_dir.name, "functions_per_shard": 512,
                       "function_count": len(bodies),
                       "files": {shard_name: sha1((shard_dir / shard_name).read_bytes())}},
        }
    manifest = {"schema_version": 1, "source_kind": "synthetic-fixture",
                "codegen_identity": "synthetic-fixture", "banks": banks,
                "notice": "Synthetic C-only fixtures. No BIOS data is present."}
    (output_dir / "oracis_bios_banks.json").write_text(json.dumps(manifest, indent=2)+"\n",
                                                        encoding="utf-8")
    return manifest

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = create_fixture(args.output_dir.expanduser().resolve())
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    print(f"Synthetic BIOS banks: {args.output_dir.resolve()}")
    print("CPUs: " + ", ".join(result["banks"]))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
