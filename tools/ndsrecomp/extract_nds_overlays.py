#!/usr/bin/env python3
"""Extract and validate Pokémon Platinum USA Rev 1 ARM9/ARM7 overlays.

The ROM and all extracted overlay images stay in a local, Git-ignored folder.
This tool inventories executable modules; it does not claim that they have
been statically recompiled or that their call targets are fully resolved.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import struct
import sys
import tempfile
from dataclasses import asdict, dataclass
from pathlib import Path

EXPECTED_ROM_SHA1 = "0862ec35b24de5c7e2dcb88c9eea0873110d755c"
NDS_HEADER_MIN_SIZE = 0x200
FAT_OFFSET_FIELD = 0x48
OVERLAY_TABLE_FIELDS = {"arm9": 0x50, "arm7": 0x58}
OVERLAY_ENTRY_SIZE = 32
OVERLAY_COMPRESSED_FLAG = 1 << 0
OVERLAY_VERIFY_HASH_FLAG = 1 << 1
OVERLAY_KNOWN_FLAGS = OVERLAY_COMPRESSED_FLAG | OVERLAY_VERIFY_HASH_FLAG
MAX_GUEST_SIZE = 0x1_0000_0000


@dataclass(frozen=True)
class Overlay:
    cpu: str
    overlay_id: int
    ram_address: int
    ram_size: int
    bss_size: int
    static_init_start: int
    static_init_end: int
    file_id: int
    table_compressed_size: int
    flags: int
    compressed: bool
    verify_hash: bool
    source_offset: int
    source_size: int
    source_sha1: str
    image_file: str
    image_size: int
    image_sha1: str
    image_sha256: str


def sha1_bytes(data: bytes) -> str:
    return hashlib.sha1(data).hexdigest()


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def checked_range(offset: int, size: int, file_size: int, label: str) -> None:
    if offset < 0 or size < 0 or offset > file_size or size > file_size - offset:
        raise ValueError(
            f"{label} exceeds ROM bounds: offset={offset:#x}, size={size:#x}, "
            f"file_size={file_size:#x}"
        )


def decompress_blz(data: bytes, expected_size: int) -> bytes:
    """Decode a Nintendo DS backwards-LZ (BLZ) module with strict bounds.

    The eight-byte footer stores (header length, compressed-region length) and
    the extra output size. The compressed suffix is decoded from end to start.
    A prefix before the compressed region is copied through unchanged.
    """
    if len(data) < 8:
        raise ValueError("BLZ overlay is shorter than its eight-byte footer")
    if expected_size <= 0 or expected_size > MAX_GUEST_SIZE:
        raise ValueError(f"invalid BLZ output size: {expected_size:#x}")

    packed, extra_size = struct.unpack_from("<II", data, len(data) - 8)
    header_length = packed >> 24
    compressed_length = packed & 0x00FFFFFF
    if header_length < 8:
        raise ValueError(f"invalid BLZ footer length: {header_length}")
    if header_length > len(data):
        raise ValueError("BLZ footer length exceeds overlay file size")
    if compressed_length == 0:
        raise ValueError("BLZ footer declares an empty compressed region")
    if extra_size == 0:
        raise ValueError("BLZ footer does not declare any decompressed growth")

    output_size = len(data) + extra_size
    if output_size != expected_size:
        raise ValueError(
            "BLZ output size does not match overlay RAM size: "
            f"footer={output_size:#x}, table={expected_size:#x}"
        )

    compressed_end = len(data) - header_length
    compressed_start = compressed_end - compressed_length
    if compressed_start < 0 or compressed_end < compressed_start:
        raise ValueError("BLZ compressed region is outside the overlay file")

    padding = data[compressed_end:len(data) - 8]
    if any(value != 0xFF for value in padding):
        raise ValueError("BLZ footer padding contains bytes other than 0xFF")

    output = bytearray(output_size)
    output[:compressed_start] = data[:compressed_start]
    source = compressed_end
    destination = output_size

    while destination > compressed_start:
        if source <= compressed_start:
            raise ValueError("BLZ input ended before the output was complete")
        flags = data[source - 1]
        source -= 1

        for bit in range(7, -1, -1):
            if destination <= compressed_start:
                break

            if flags & (1 << bit):
                if source - compressed_start < 2:
                    raise ValueError("BLZ back-reference is truncated")
                second = data[source - 1]
                first = data[source - 2]
                source -= 2
                length = (first >> 4) + 3
                displacement = (((first & 0x0F) << 8) | second) + 3
                if length > destination - compressed_start:
                    raise ValueError("BLZ back-reference crosses the output prefix")

                for _ in range(length):
                    destination -= 1
                    copy_from = destination + displacement
                    if copy_from >= output_size:
                        raise ValueError("BLZ back-reference points beyond output bounds")
                    output[destination] = output[copy_from]
            else:
                if source <= compressed_start:
                    raise ValueError("BLZ literal is truncated")
                destination -= 1
                source -= 1
                output[destination] = data[source]

    if destination != compressed_start:
        raise ValueError("BLZ decoder did not reach the output boundary")
    if source != compressed_start:
        raise ValueError(
            "BLZ decoder did not consume the compressed region exactly: "
            f"consumed_end={source:#x}, expected={compressed_start:#x}"
        )
    return bytes(output)


def read_fat_entries(rom: bytes) -> list[tuple[int, int]]:
    if len(rom) < NDS_HEADER_MIN_SIZE:
        raise ValueError("File is too small to contain a Nintendo DS header")
    fat_offset, fat_size = struct.unpack_from("<II", rom, FAT_OFFSET_FIELD)
    if fat_size == 0 or fat_size % 8 != 0:
        raise ValueError("NDS FAT size must be a nonzero multiple of eight bytes")
    checked_range(fat_offset, fat_size, len(rom), "NDS FAT")

    entries: list[tuple[int, int]] = []
    for index in range(fat_size // 8):
        start, end = struct.unpack_from("<II", rom, fat_offset + index * 8)
        if start > end:
            raise ValueError(f"FAT file {index} has an inverted extent")
        checked_range(start, end - start, len(rom), f"FAT file {index}")
        entries.append((start, end))
    return entries


def read_overlay_table(
    rom: bytes,
    cpu: str,
    header_field: int,
    fat_entries: list[tuple[int, int]],
) -> list[tuple[dict, bytes]]:
    table_offset, table_size = struct.unpack_from("<II", rom, header_field)
    if table_size == 0:
        return []
    if table_size % OVERLAY_ENTRY_SIZE != 0:
        raise ValueError(
            f"{cpu.upper()} overlay table size is not a multiple of {OVERLAY_ENTRY_SIZE}"
        )
    checked_range(table_offset, table_size, len(rom), f"{cpu.upper()} overlay table")

    overlays: list[tuple[dict, bytes]] = []
    seen_ids: set[int] = set()
    for table_index in range(table_size // OVERLAY_ENTRY_SIZE):
        fields = struct.unpack_from(
            "<8I", rom, table_offset + table_index * OVERLAY_ENTRY_SIZE
        )
        (overlay_id, ram_address, ram_size, bss_size, init_start, init_end,
         file_id, packed_size_flags) = fields

        if overlay_id in seen_ids:
            raise ValueError(f"{cpu.upper()} overlay table repeats ID {overlay_id}")
        seen_ids.add(overlay_id)
        if ram_size == 0:
            raise ValueError(f"{cpu.upper()} overlay {overlay_id} has zero RAM size")
        if ram_address + ram_size > MAX_GUEST_SIZE:
            raise ValueError(f"{cpu.upper()} overlay {overlay_id} code range overflows")
        if ram_address + ram_size + bss_size > MAX_GUEST_SIZE:
            raise ValueError(f"{cpu.upper()} overlay {overlay_id} RAM+BSS range overflows")
        if (init_start == 0) != (init_end == 0):
            raise ValueError(
                f"{cpu.upper()} overlay {overlay_id} has a partial static-init range"
            )
        if init_start:
            if (init_start < ram_address or init_end < init_start or
                    init_end > ram_address + ram_size or
                    (init_start & 3) != 0 or (init_end & 3) != 0):
                raise ValueError(
                    f"{cpu.upper()} overlay {overlay_id} static-init range is invalid"
                )

        flags = packed_size_flags >> 24
        if flags & ~OVERLAY_KNOWN_FLAGS:
            raise ValueError(
                f"{cpu.upper()} overlay {overlay_id} uses unknown flags {flags:#x}"
            )
        table_compressed_size = packed_size_flags & 0x00FFFFFF
        if file_id >= len(fat_entries):
            raise ValueError(
                f"{cpu.upper()} overlay {overlay_id} references missing FAT file {file_id}"
            )
        source_start, source_end = fat_entries[file_id]
        source = rom[source_start:source_end]
        if not source:
            raise ValueError(f"{cpu.upper()} overlay {overlay_id} references an empty FAT file")

        compressed = bool(flags & OVERLAY_COMPRESSED_FLAG)
        image = decompress_blz(source, ram_size) if compressed else source
        if len(image) != ram_size:
            raise ValueError(
                f"{cpu.upper()} overlay {overlay_id} image size mismatch: "
                f"table={ram_size:#x}, file={len(image):#x}"
            )

        metadata = {
            "cpu": cpu,
            "overlay_id": overlay_id,
            "ram_address": ram_address,
            "ram_size": ram_size,
            "bss_size": bss_size,
            "static_init_start": init_start,
            "static_init_end": init_end,
            "file_id": file_id,
            "table_compressed_size": table_compressed_size,
            "flags": flags,
            "compressed": compressed,
            "verify_hash": bool(flags & OVERLAY_VERIFY_HASH_FLAG),
            "source_offset": source_start,
            "source_size": len(source),
            "source_sha1": sha1_bytes(source),
            "image": image,
        }
        overlays.append((metadata, image))
    return overlays


def validate_previous_output(output_dir: Path) -> None:
    """Only replace an earlier output created by this exact extractor."""
    if output_dir.is_symlink() or not output_dir.is_dir():
        raise ValueError(f"Overlay output destination is not a safe directory: {output_dir}")
    manifest_path = output_dir / "manifest.json"
    if not manifest_path.is_file() or manifest_path.is_symlink():
        raise ValueError(
            "Overlay output directory already exists without an Oracis manifest; "
            "choose another --output-dir"
        )
    try:
        previous = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError("Existing overlay manifest is not valid JSON") from exc
    if previous.get("schema") != "oracis-nds-overlays-v1":
        raise ValueError("Existing directory was not generated by the Oracis overlay extractor")
    declared = previous.get("generated_files")
    if (not isinstance(declared, list) or any(
        not isinstance(name, str) or Path(name).name != name or name == "manifest.json"
        for name in declared
    ) or len(set(declared)) != len(declared)):
        raise ValueError("Existing overlay manifest has an invalid generated-file list")
    expected = set(declared) | {"manifest.json"}
    actual: set[str] = set()
    for entry in output_dir.iterdir():
        if entry.is_symlink() or not entry.is_file():
            raise ValueError(f"Existing overlay directory contains unsafe entry: {entry}")
        actual.add(entry.name)
    if actual != expected:
        raise ValueError(
            "Existing overlay directory contains files not owned by the previous "
            "Oracis extraction; refusing to replace it"
        )


def publish_directory(staging: Path, output_dir: Path) -> None:
    backup: Path | None = None
    if output_dir.exists():
        validate_previous_output(output_dir)
        backup = Path(tempfile.mkdtemp(
            prefix=f".{output_dir.name}.backup-", dir=output_dir.parent
        ))
        backup.rmdir()
        output_dir.replace(backup)
    try:
        staging.replace(output_dir)
    except OSError:
        if backup is not None and backup.exists():
            backup.replace(output_dir)
        raise
    if backup is not None:
        shutil.rmtree(backup)


def extract_overlays(rom_path: Path, output_dir: Path) -> dict:
    if not rom_path.is_file():
        raise FileNotFoundError(f"ROM file not found: {rom_path}")
    rom = rom_path.read_bytes()
    if len(rom) < NDS_HEADER_MIN_SIZE:
        raise ValueError("File is too small to contain a Nintendo DS header")
    rom_sha1 = sha1_bytes(rom)
    if rom_sha1 != EXPECTED_ROM_SHA1:
        raise ValueError(
            "ROM identity mismatch. Oracis currently accepts only Pokémon Platinum "
            f"USA Rev 1 ({EXPECTED_ROM_SHA1}); received {rom_sha1}."
        )

    fat_entries = read_fat_entries(rom)
    extracted: list[tuple[dict, bytes]] = []
    for cpu, field in OVERLAY_TABLE_FIELDS.items():
        extracted.extend(read_overlay_table(rom, cpu, field, fat_entries))

    output_dir = output_dir.expanduser()
    if output_dir.is_symlink():
        raise ValueError(f"Overlay output destination must not be a symlink: {output_dir}")
    output_dir = output_dir.resolve()
    output_dir.parent.mkdir(parents=True, exist_ok=True)
    if output_dir.exists():
        validate_previous_output(output_dir)

    staging = Path(tempfile.mkdtemp(
        prefix=f".{output_dir.name}.staging-", dir=output_dir.parent
    ))
    try:
        records: list[Overlay] = []
        generated_files: list[str] = []
        for metadata, image in extracted:
            file_name = f"{metadata['cpu']}_overlay_{metadata['overlay_id']:04d}.bin"
            target = staging / file_name
            target.write_bytes(image)
            written = target.read_bytes()
            if len(written) != metadata["ram_size"] or sha1_bytes(written) != sha1_bytes(image):
                raise ValueError(
                    f"Post-write verification failed for {metadata['cpu']} overlay "
                    f"{metadata['overlay_id']}"
                )
            generated_files.append(file_name)
            records.append(Overlay(
                cpu=metadata["cpu"],
                overlay_id=metadata["overlay_id"],
                ram_address=metadata["ram_address"],
                ram_size=metadata["ram_size"],
                bss_size=metadata["bss_size"],
                static_init_start=metadata["static_init_start"],
                static_init_end=metadata["static_init_end"],
                file_id=metadata["file_id"],
                table_compressed_size=metadata["table_compressed_size"],
                flags=metadata["flags"],
                compressed=metadata["compressed"],
                verify_hash=metadata["verify_hash"],
                source_offset=metadata["source_offset"],
                source_size=metadata["source_size"],
                source_sha1=metadata["source_sha1"],
                image_file=file_name,
                image_size=len(written),
                image_sha1=sha1_bytes(written),
                image_sha256=sha256_bytes(written),
            ))

        manifest = {
            "schema": "oracis-nds-overlays-v1",
            "rom_sha1": rom_sha1,
            "rom_size": len(rom),
            "source_file_name": rom_path.name,
            "overlay_count": len(records),
            "overlays": [asdict(record) for record in records],
            "generated_files": generated_files,
            "notice": (
                "Local extracted code. Not statically recompiled, not runtime-ready, "
                "and never to be committed or distributed."
            ),
        }
        (staging / "manifest.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
            newline="\n",
        )
        publish_directory(staging, output_dir)
        return manifest
    except Exception:
        if staging.exists():
            shutil.rmtree(staging)
        raise


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "rom", type=Path,
        help="User-provided Pokémon Platinum USA Rev 1 .nds file",
    )
    parser.add_argument(
        "--output-dir", type=Path,
        default=Path(".oracis/local/pokemon-platinum-usa-rev1/overlays"),
        help="Local output directory (default: .oracis/local/pokemon-platinum-usa-rev1/overlays)",
    )
    args = parser.parse_args()
    try:
        manifest = extract_overlays(args.rom.expanduser().resolve(), args.output_dir)
    except (OSError, ValueError, struct.error) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    print(f"ROM SHA-1 verified: {manifest['rom_sha1']}")
    print(f"Validated overlay modules: {manifest['overlay_count']}")
    for item in manifest["overlays"]:
        state = "BLZ decompressed" if item["flags"] & OVERLAY_COMPRESSED_FLAG else "uncompressed"
        print(
            f"{item['cpu'].upper()} overlay {item['overlay_id']:04d}: "
            f"RAM=0x{item['ram_address']:08X}+0x{item['ram_size']:X} "
            f"BSS=0x{item['bss_size']:X} file_id={item['file_id']} "
            f"({state}) SHA-1={item['image_sha1']}"
        )
    print(f"Local overlay images and manifest written to: {args.output_dir.resolve()}")
    print("These modules still need per-overlay static code discovery, recompilation and dispatch integration.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
