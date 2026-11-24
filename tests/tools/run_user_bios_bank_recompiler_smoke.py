#!/usr/bin/env python3
"""Exercise the pinned NDSRecomp against synthetic ARM9/ARM7 BIOS address maps.

The images are generated in a temporary directory and contain only simple
branch, load/store and return instructions. No real BIOS or ROM is used.
"""
from __future__ import annotations

import argparse
import importlib.util
import os
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EMITTER_PATH = ROOT / "tools" / "ndsrecomp" / "emit_user_bios_banks.py"
VALIDATOR_PATH = ROOT / "tools" / "ndsrecomp" / "validate_user_bios_bank_set.py"


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


emitter = load_module("oracis_emit_bios_smoke", EMITTER_PATH)
validator = load_module("oracis_validate_bios_smoke", VALIDATOR_PATH)

VECTORS = (
    (0x00, "reset"),
    (0x04, "undefined"),
    (0x08, "swi"),
    (0x0C, "prefetch_abort"),
    (0x10, "data_abort"),
    (0x18, "irq"),
    (0x1C, "fiq"),
)
EXECUTION_TARGET_OFFSETS = {"arm9": 0x100, "arm7": 0x1200}
MEMORY_TARGET_OFFSETS = {"arm9": 0x200, "arm7": 0x1300}
ARM_MOV_R0_42 = 0xE3A0002A
ARM_ADD_R0_R0_1 = 0xE2800001
ARM_BRANCH_TO_RESET_STUB = 0xEAFFFFFC
ARM_LDR_R0_R1 = 0xE5910000
ARM_STR_R0_R1 = 0xE5810000
ARM_BX_LR = 0xE12FFF1E


def create_synthetic_bios(path: Path, cpu: str) -> None:
    layout = emitter.BIOS_LAYOUT[cpu]
    base = layout["load_address"]
    data = bytearray(layout["size"])
    if cpu == "arm7":
        # The host bridge also loads the synthetic ARM7 KEY1 table. Keep it
        # deterministic and data-only so this remains a BIOS-free fixture.
        for index in range(0x1048):
            data[0x30 + index] = (index * 13 + 9) & 0xFF
    execution_target = base + EXECUTION_TARGET_OFFSETS[cpu]
    memory_target = base + MEMORY_TARGET_OFFSETS[cpu]

    for offset, _name in VECTORS:
        address = base + offset
        target = execution_target if offset == 0 else memory_target
        delta = target - (address + 8)
        if delta % 4 != 0:
            raise AssertionError("ARM branch target is not word-aligned")
        immediate = (delta // 4) & 0x00FFFFFF
        branch = 0xEA000000 | immediate
        struct.pack_into("<I", data, offset, branch)

    # Reset enters a harmless ARM loop that keeps R0 at 0x2B without leaving BIOS.
    # Other vectors retain reachable LDR/STR bodies for ABI/codegen coverage.
    struct.pack_into(
        "<III",
        data,
        EXECUTION_TARGET_OFFSETS[cpu],
        ARM_MOV_R0_42,
        ARM_ADD_R0_R0_1,
        ARM_BRANCH_TO_RESET_STUB,
    )
    struct.pack_into(
        "<III",
        data,
        MEMORY_TARGET_OFFSETS[cpu],
        ARM_LDR_R0_R1,
        ARM_STR_R0_R1,
        ARM_BX_LR,
    )
    path.write_bytes(data)


def compile_generated_bank(output_dir: Path, cpu: str, compiler: list[str]) -> int:
    bank = f"{emitter.BANK_PREFIX}_{cpu}"
    shard_dir = output_dir / f"{bank}_shards"
    sources = sorted(shard_dir.glob("bank_part_*.c"))
    sources.append(output_dir / f"{bank}_dispatch.c")
    if len(sources) < 2:
        raise RuntimeError(f"{cpu.upper()} BIOS bank has no split C bodies/dispatch")

    object_dir = output_dir.parent / f".{bank}-objects"
    object_dir.mkdir()
    for source in sources:
        object_file = object_dir / f"{source.stem}.o"
        command = [
            *compiler,
            "-std=c99",
            "-I", str(ROOT / "include"),
            "-I", str(ROOT / "include" / "oracis" / "recomp"),
            "-I", str(output_dir),
            "-c", str(source),
            "-o", str(object_file),
        ]
        result = subprocess.run(
            command,
            capture_output=True,
            text=True,
            timeout=120,
            check=False,
        )
        if result.returncode != 0:
            raise RuntimeError(
                f"Generated {cpu.upper()} BIOS C failed C99 compilation: {source.name}\n"
                f"command: {command}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
            )
        if not object_file.is_file() or object_file.stat().st_size == 0:
            raise RuntimeError(f"C compiler did not create object file: {object_file}")
    return len(sources)


def run_checked(command: list[str], description: str, timeout: int = 600) -> None:
    result = subprocess.run(
        command,
        capture_output=True,
        text=True,
        timeout=timeout,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"{description} failed (exit={result.returncode})\n"
            f"command: {command}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )


def link_and_preflight_with_oracis(
    output_dir: Path,
    arm9_bios: Path,
    arm7_bios: Path,
    cmake_c_compiler: str,
    cmake_cxx_compiler: str,
    generator: str,
) -> None:
    cmake = shutil.which("cmake")
    if cmake is None:
        raise RuntimeError("CMake is required for the Oracis runtime integration smoke")
    if generator == "Ninja" and shutil.which("ninja") is None:
        raise RuntimeError("Ninja generator was requested but ninja was not found")

    build_dir = output_dir.parent / "oracis-runtime-build"
    configure = [
        cmake,
        "-S", str(ROOT),
        "-B", str(build_dir),
        "-G", generator,
        "-DORACIS_BUILD_HOST=OFF",
        "-DBUILD_TESTING=ON",
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_C_COMPILER={cmake_c_compiler}",
        f"-DCMAKE_CXX_COMPILER={cmake_cxx_compiler}",
        f"-DORACIS_LOCAL_BIOS_BANK_DIR={output_dir}",
    ]
    run_checked(configure, "Oracis runtime CMake configure")

    build = [
        cmake,
        "--build", str(build_dir),
        "--target", "oracis_bios_bank_link_smoke",
        "--parallel",
    ]
    run_checked(build, "Oracis runtime link with NDSRecomp BIOS banks")

    executable = build_dir / (
        "oracis_bios_bank_link_smoke.exe"
        if os.name == "nt"
        else "oracis_bios_bank_link_smoke"
    )
    if not executable.is_file():
        raise RuntimeError(f"Oracis BIOS integration executable was not created: {executable}")
    run_checked(
        [str(executable), str(arm9_bios), str(arm7_bios)],
        "Oracis live BIOS dispatch preflight",
        timeout=120,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--recompiler", type=Path, required=True)
    parser.add_argument("--c-compiler", default="cc")
    parser.add_argument("--cmake-c-compiler", default=None)
    parser.add_argument("--cmake-cxx-compiler", default=None)
    parser.add_argument("--cmake-generator", default="Ninja")
    args = parser.parse_args()
    recompiler = args.recompiler.expanduser().resolve()
    compiler = shlex.split(args.c_compiler)
    if not compiler:
        parser.error("--c-compiler must not be empty")
    cmake_c_compiler = args.cmake_c_compiler or compiler[0]
    cmake_cxx_compiler = (
        args.cmake_cxx_compiler
        or os.environ.get("CXX")
        or shutil.which("clang++")
        or shutil.which("c++")
        or "c++"
    )
    if not recompiler.is_file():
        parser.error(f"NDSRecomp executable does not exist: {recompiler}")

    with tempfile.TemporaryDirectory(prefix="oracis-real-bios-codegen-") as temporary:
        root = Path(temporary)
        arm9_bios = root / "bios9.synthetic.bin"
        arm7_bios = root / "bios7.synthetic.bin"
        output_dir = root / "generated"
        create_synthetic_bios(arm9_bios, "arm9")
        create_synthetic_bios(arm7_bios, "arm7")

        emitter.emit_banks(recompiler, arm9_bios, arm7_bios, output_dir)
        validator.validate_bank_set(output_dir)
        compiled = {
            cpu: compile_generated_bank(output_dir, cpu, compiler)
            for cpu in ("arm9", "arm7")
        }
        link_and_preflight_with_oracis(
            output_dir,
            arm9_bios,
            arm7_bios,
            cmake_c_compiler,
            cmake_cxx_compiler,
            args.cmake_generator,
        )

    print(
        "Pinned NDSRecomp BIOS integration passed: "
        f"ARM9/ARM7 C99 objects compiled and linked into Oracis ({compiled}); "
        "live-byte mismatch rejected; both native reset stubs executed with register results verified"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1)
