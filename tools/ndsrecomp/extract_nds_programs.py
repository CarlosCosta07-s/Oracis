#!/usr/bin/env python3
"""Safely extract Platinum USA Rev 1 ARM9/ARM7 program images for NDSRecomp.

All outputs are local generated data. The default output path is ignored by Git
and must never be committed or uploaded.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from dataclasses import asdict, dataclass
from pathlib import Path

EXPECTED_ROM_SHA1 = "0862ec35b24de5c7e2dcb88c9eea0873110d755c"
NDS_HEADER_MIN_SIZE = 0x200
NDS_HEADER_STORED_SIZE = 0x170
SEGMENT_FIELDS = {"arm9": 0x20, "arm7": 0x30}
EXPECTED_ARM9_SEGMENT_SHA1 = "7ec963e737f0f61efcbbf5c7a3d33cbf378143f8"

# Extra static entry points observed in the retail direct-entry probe. These
# are metadata seeds only; no game bytes are copied into the repository.
VERIFIED_STATIC_ENTRY_POINTS = {
    "arm9": {
        EXPECTED_ARM9_SEGMENT_SHA1: (
            (0x020C1978, "arm", "observed_slot2_restore_arm7_entry_020C1978"),
            (0x020C2EBC, "arm", "observed_dispatch_thunk_020C2EBC"),
            (0x020C2ECC, "arm", "observed_branch_table_entry_020C2ECC"),
            (0x020C2F18, "arm", "observed_indirect_function_020C2F18"),
        ),
    },
    "arm7": {},
}


@dataclass(frozen=True)
class Segment:
    cpu: str
    rom_offset: int
    entry_pc_raw: int
    load_address: int
    size: int
    entry_pc: int
    entry_mode: str
    sha1: str


def sha1_file(path: Path) -> str:
    digest = hashlib.sha1()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def read_segment_header(stream, file_size: int, cpu: str, header_offset: int) -> Segment:
    stream.seek(header_offset)
    raw = stream.read(16)
    if len(raw) != 16:
        raise ValueError(f"Nintendo DS header is truncated while reading {cpu.upper()} fields")

    rom_offset, entry_pc_raw, load_address, size = struct.unpack("<IIII", raw)
    if size == 0:
        raise ValueError(f"{cpu.upper()} program segment has zero size")
    if rom_offset < NDS_HEADER_MIN_SIZE:
        raise ValueError(f"{cpu.upper()} ROM offset {rom_offset:#x} overlaps the NDS header")
    if rom_offset + size > file_size:
        raise ValueError(
            f"{cpu.upper()} segment exceeds file bounds: "
            f"{rom_offset:#x} + {size:#x} > {file_size:#x}"
        )
    if load_address + size > 0x1_0000_0000:
        raise ValueError(f"{cpu.upper()} load range overflows the 32-bit guest address space")

    thumb = bool(entry_pc_raw & 1)
    entry_pc = entry_pc_raw & ~1
    entry_mode = "thumb" if thumb else "arm"
    alignment = 2 if thumb else 4
    if entry_pc % alignment:
        raise ValueError(f"{cpu.upper()} entry PC {entry_pc:#x} is not {alignment}-byte aligned")
    if not (load_address <= entry_pc < load_address + size):
        raise ValueError(
            f"{cpu.upper()} entry PC {entry_pc:#x} is outside loaded image "
            f"[{load_address:#x}, {load_address + size:#x})"
        )

    stream.seek(rom_offset)
    remaining = size
    digest = hashlib.sha1()
    while remaining:
        chunk = stream.read(min(1024 * 1024, remaining))
        if not chunk:
            raise ValueError(f"Unexpected end of file while hashing {cpu.upper()} segment")
        digest.update(chunk)
        remaining -= len(chunk)

    return Segment(
        cpu=cpu,
        rom_offset=rom_offset,
        entry_pc_raw=entry_pc_raw,
        load_address=load_address,
        size=size,
        entry_pc=entry_pc,
        entry_mode=entry_mode,
        sha1=digest.hexdigest(),
    )


def entry_points_for_segment(segment: Segment) -> tuple[tuple[int, str, str], ...]:
    """Return the header entry plus only SHA-1-bound, verified static seeds."""
    entries = [(segment.entry_pc, segment.entry_mode, "rom_entry")]
    entries.extend(
        VERIFIED_STATIC_ENTRY_POINTS.get(segment.cpu, {}).get(segment.sha1, ())
    )

    seen: set[int] = set()
    for address, mode, name in entries:
        if mode not in ("arm", "thumb") or not name:
            raise ValueError(f"Invalid {segment.cpu.upper()} entry-point metadata")
        alignment = 2 if mode == "thumb" else 4
        if address % alignment:
            raise ValueError(
                f"{segment.cpu.upper()} entry point {address:#x} is not {alignment}-byte aligned"
            )
        if not (segment.load_address <= address < segment.load_address + segment.size):
            raise ValueError(
                f"{segment.cpu.upper()} entry point {address:#x} is outside the verified image"
            )
        if address in seen:
            raise ValueError(
                f"Duplicate {segment.cpu.upper()} entry point address {address:#x}"
            )
        seen.add(address)
    return tuple(entries)


def toml_for_segment(segment: Segment) -> str:
    program_name = f"Pokemon Platinum USA Rev 1 {segment.cpu.upper()}"
    program_id = f"pokemon_platinum_usa_rev1_{segment.cpu}"
    lines = [
        "# Generated locally by Oracis extract_nds_programs.py. Do not commit.",
        "# The identity hash is for this extracted ARM program image, not the ROM.",
        "",
        "[program]",
        f'name = "{program_name}"',
        f'id = "{program_id}"',
        f"load_address = 0x{segment.load_address:08X}",
        f"size = 0x{segment.size:08X}",
        f"entry_pc = 0x{segment.entry_pc:08X}",
        "",
        "[identity]",
        f'sha1 = "{segment.sha1}"',
        "",
    ]
    for address, mode, name in entry_points_for_segment(segment):
        lines.extend([
            "[[entry_point]]",
            f"addr = 0x{address:08X}",
            f'mode = "{mode}"',
            f'name = "{name}"',
            "",
        ])
    return "\n".join(lines)


def extract(rom_path: Path, output_dir: Path) -> dict:
    if not rom_path.is_file():
        raise FileNotFoundError(f"ROM file not found: {rom_path}")
    file_size = rom_path.stat().st_size
    if file_size < NDS_HEADER_MIN_SIZE:
        raise ValueError("File is too small to contain a Nintendo DS header")

    rom_sha1 = sha1_file(rom_path)
    if rom_sha1 != EXPECTED_ROM_SHA1:
        raise ValueError(
            "ROM identity mismatch. Oracis currently accepts only Pokémon Platinum "
            f"USA Rev 1 ({EXPECTED_ROM_SHA1}); received {rom_sha1}."
        )

    with rom_path.open("rb") as stream:
        boot_header = stream.read(NDS_HEADER_STORED_SIZE)
        if len(boot_header) != NDS_HEADER_STORED_SIZE:
            raise ValueError("Nintendo DS boot header is truncated")

        segments = [
            read_segment_header(stream, file_size, cpu, offset)
            for cpu, offset in SEGMENT_FIELDS.items()
        ]

        arm9, arm7 = segments
        arm9_start, arm9_end = arm9.rom_offset, arm9.rom_offset + arm9.size
        arm7_start, arm7_end = arm7.rom_offset, arm7.rom_offset + arm7.size
        if max(arm9_start, arm7_start) < min(arm9_end, arm7_end):
            raise ValueError("ARM9 and ARM7 ROM segments overlap")

        output_dir.mkdir(parents=True, exist_ok=True)
        boot_header_path = output_dir / "header.bin"
        boot_header_path.write_bytes(boot_header)
        if boot_header_path.stat().st_size != NDS_HEADER_STORED_SIZE or \
                boot_header_path.read_bytes() != boot_header:
            raise ValueError("Post-write verification failed for NDS boot header")

        for segment in segments:
            target = output_dir / f"{segment.cpu}.bin"
            stream.seek(segment.rom_offset)
            remaining = segment.size
            with target.open("wb") as out:
                while remaining:
                    chunk = stream.read(min(1024 * 1024, remaining))
                    if not chunk:
                        raise ValueError(
                            f"Unexpected end of file while writing {segment.cpu.upper()}"
                        )
                    out.write(chunk)
                    remaining -= len(chunk)
            if sha1_file(target) != segment.sha1:
                raise ValueError(
                    f"Post-write SHA-1 verification failed for {segment.cpu.upper()}"
                )
            (output_dir / f"{segment.cpu}.toml").write_text(
                toml_for_segment(segment), encoding="utf-8", newline="\n"
            )

    manifest = {
        "schema_version": 1,
        "rom_sha1": rom_sha1,
        "rom_size": file_size,
        "source_file_name": rom_path.name,
        "boot_header": {
            "file": "header.bin",
            "size": NDS_HEADER_STORED_SIZE,
            "sha1": hashlib.sha1(boot_header).hexdigest(),
        },
        "segments": [asdict(segment) for segment in segments],
        "notice": "Local generated content. Never commit or distribute these files.",
    }
    (output_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "rom",
        type=Path,
        help="User-provided Pokémon Platinum USA Rev 1 .nds file",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path(".oracis/local/pokemon-platinum-usa-rev1"),
        help="Local output directory (default: .oracis/local/pokemon-platinum-usa-rev1)",
    )
    args = parser.parse_args()

    try:
        output_dir = args.output_dir.expanduser().resolve()
        manifest = extract(args.rom.expanduser().resolve(), output_dir)
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    print(f"ROM SHA-1 verified: {manifest['rom_sha1']}")
    for segment in manifest["segments"]:
        print(
            f"{segment['cpu'].upper()}: load=0x{segment['load_address']:08X} "
            f"entry=0x{segment['entry_pc']:08X} ({segment['entry_mode']}) "
            f"size=0x{segment['size']:X} SHA-1={segment['sha1']}"
        )
    print(f"Local outputs written to: {output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
