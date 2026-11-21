#!/usr/bin/env python3
"""End-to-end synthetic test: NDS extractor output -> pinned NDSRecomp C banks."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import os
import shlex
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
PATCHER_PATH = ROOT / "tools" / "ndsrecomp" / "patch_memory_access_direction.py"
PATCHER_SPEC = importlib.util.spec_from_file_location(
    "oracis_memory_direction_for_recompiler_test", PATCHER_PATH
)
if PATCHER_SPEC is None or PATCHER_SPEC.loader is None:
    raise RuntimeError(f"Cannot load memory direction patcher: {PATCHER_PATH}")
patcher = importlib.util.module_from_spec(PATCHER_SPEC)
PATCHER_SPEC.loader.exec_module(patcher)

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
    arm7_offset = 0x300
    arm9_load = 0x02000000
    arm7_load = 0x02380000
    # Exercise explicit read/write timing adaptation in actual compiler
    # output, including single and multiple-transfer ARM/Thumb instructions.
    arm9_code = struct.pack(
        "<IIIII",
        0xE5910000,  # LDR r0, [r1]
        0xE5810000,  # STR r0, [r1]
        0xE8B10005,  # LDMIA r1!, {r0, r2}
        0xE8A10005,  # STMIA r1!, {r0, r2}
        0xE12FFF1E,  # BX lr
    )
    arm7_code = struct.pack(
        "<HHHHH",
        0x6808,      # LDR r0, [r1, #0]
        0x6008,      # STR r0, [r1, #0]
        0xC908,      # LDMIA r1!, {r0}
        0xC108,      # STMIA r1!, {r0}
        0x4770,      # BX lr
    )

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


def compile_bank(recompiler: Path, c_compiler: list[str], config: Path,
                 binary: Path, output: Path, bank: str) -> None:
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

    # Apply the same Oracis-owned codegen adapter used by the local bank
    # publisher. The synthetic instruction stream must exercise both sides.
    bank_source = output / f"{bank}.c"
    original_source = bank_source.read_text(encoding="utf-8")
    adapted_source, read_calls, write_calls = patcher.patch_generated_source(
        original_source
    )
    if read_calls == 0 or write_calls == 0:
        raise AssertionError(
            f"NDSRecomp did not expose both memory directions for {bank}: "
            f"reads={read_calls}, writes={write_calls}"
        )
    bank_source.write_text(adapted_source, encoding="utf-8")

    # Compile both generated C translation units against the Oracis C99 ABI.
    include_dir = ROOT / "include" / "oracis" / "recomp"
    for source_name in (f"{bank}.c", f"{bank}_dispatch.c"):
        source = output / source_name
        object_file = output / f"{source.stem}.o"
        command = [
            *c_compiler,
            "-std=c99",
            "-I", str(ROOT / "include"),
            "-I", str(include_dir),
            "-I", str(output),
            "-c", str(source),
            "-o", str(object_file),
        ]
        result = subprocess.run(
            command, capture_output=True, text=True, timeout=60, check=False
        )
        if result.returncode != 0:
            raise AssertionError(
                f"Generated C failed the Oracis C99 ABI compile ({source_name})\n"
                f"command: {command}\nstdout:\n{result.stdout}\n"
                f"stderr:\n{result.stderr}"
            )
        if not object_file.is_file() or object_file.stat().st_size == 0:
            raise AssertionError(f"C compiler did not produce {object_file}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--recompiler", type=Path, required=True)
    parser.add_argument(
        "--c-compiler",
        default=os.environ.get("CC", "cc"),
        help="C99 compiler command (default: CC environment variable or cc)",
    )
    args = parser.parse_args()
    recompiler = args.recompiler.expanduser().resolve()
    c_compiler = shlex.split(args.c_compiler)
    if not c_compiler:
        parser.error("C compiler command must not be empty")
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
            recompiler, c_compiler, extracted / "arm9.toml",
            extracted / "arm9.bin", root / "arm9-output", "arm9_fixture",
        )
        compile_bank(
            recompiler, c_compiler, extracted / "arm7.toml",
            extracted / "arm7.bin", root / "arm7-output", "arm7_fixture",
        )

    print(
        "synthetic extractor -> NDSRecomp ARM9/ARM7 emission -> "
        "Oracis C99 ABI compilation passed"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
