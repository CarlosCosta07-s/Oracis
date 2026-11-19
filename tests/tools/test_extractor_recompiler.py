#!/usr/bin/env python3
"""End-to-end synthetic test: NDS extractor output -> pinned NDSRecomp C banks."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
EXTRACTOR_PATH = ROOT / "tools" / "ndsrecomp" / "extract_nds_programs.py"
SPEC = importlib.util.spec_from_file_location(
    "oracis_extract_for_recompiler_test", EXTRACTOR_PATH
)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"Cannot load extractor: {EXTRACTOR_PATH}")
extractor = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = extractor
SPEC.loader.exec_module(extractor)


def make_synthetic_nds(path: Path) -> str:
    """Create a minimal NDS header with one ARM BX LR and one Thumb BX LR."""
    arm9_offset = 0x200
    arm7_offset = 0x204
    arm9_load = 0x02000000
    arm7_load = 0x02380000
    arm9_code = bytes.fromhex("1e ff 2f e1")  # ARM: bx lr
    arm7_code = bytes.fromhex("70 47")        # Thumb: bx lr

    image = bytearray(arm7_offset + len(arm7_code))
    struct.pack_into(
        "<IIII", image, 0x20,
        arm9_offset, arm9_load, arm9_load, len(arm9_code),
    )
    struct.pack_into(
        "<IIII", image, 0x30,
        arm7_offset, arm7_load | 1, arm7_load, len(arm7_code),
    )
    image[arm9_offset:arm9_offset + len(arm9_code)] = arm9_code
    image[arm7_offset:arm7_offset + len(arm7_code)] = arm7_code
    path.write_bytes(image)
    return hashlib.sha1(image).hexdigest()


def compile_bank(recompiler: Path, config: Path, binary: Path,
                 output: Path, bank: str) -> None:
    output.mkdir(parents=True, exist_ok=True)
    command = [
        str(recompiler.resolve()),
        "--config", str(config.resolve()),
        "--bin", str(binary.resolve()),
        "--out", str(output.resolve()),
        "--bank", bank,
    ]
    result = subprocess.run(
        command, capture_output=True, text=True, timeout=60, check=False
    )
    if result.returncode != 0:
        raise AssertionError(
            f"NDSRecomp failed for {bank} (exit {result.returncode})\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )

    expected = {
        f"{bank}.c",
        f"{bank}.h",
        f"{bank}_dispatch.c",
    }
    actual = {item.name for item in output.iterdir()}
    if actual != expected:
        raise AssertionError(
            f"Unexpected {bank} output files: expected {sorted(expected)}, "
            f"received {sorted(actual)}"
        )
    empty = [name for name in expected if (output / name).stat().st_size == 0]
    if empty:
        raise AssertionError(f"NDSRecomp emitted empty files for {bank}: {empty}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--recompiler", type=Path, required=True)
    args = parser.parse_args()
    recompiler = args.recompiler.expanduser().resolve()
    if not recompiler.is_file():
        parser.error(f"NDSRecomp executable does not exist: {recompiler}")

    with tempfile.TemporaryDirectory(prefix="oracis-extractor-recomp-") as temp:
        root = Path(temp)
        synthetic_rom = root / "synthetic-homebrew-fixture.nds"
        extractor.EXPECTED_ROM_SHA1 = make_synthetic_nds(synthetic_rom)
        extracted = root / "extracted"
        manifest = extractor.extract(synthetic_rom, extracted)

        if [s["cpu"] for s in manifest["segments"]] != ["arm9", "arm7"]:
            raise AssertionError("Extractor did not return ARM9 then ARM7 images")

        compile_bank(
            recompiler, extracted / "arm9.toml", extracted / "arm9.bin",
            root / "arm9-output", "arm9_fixture",
        )
        compile_bank(
            recompiler, extracted / "arm7.toml", extracted / "arm7.bin",
            root / "arm7-output", "arm7_fixture",
        )

    print("synthetic extractor -> NDSRecomp ARM9/ARM7 bank emission passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
