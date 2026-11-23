#!/usr/bin/env python3
"""Synthetic, copyright-free tests for the NDS overlay extractor."""
from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "ndsrecomp" / "extract_nds_overlays.py"
SPEC = importlib.util.spec_from_file_location(
    "oracis_extract_nds_overlays_test_target", SCRIPT
)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"Cannot load overlay extractor module: {SCRIPT}")
extractor = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = extractor
SPEC.loader.exec_module(extractor)


def make_blz_repeat_fixture(
    prefix: bytes = b"", padding_size: int = 0
) -> tuple[bytes, bytes]:
    """Encode a tiny backwards-LZ fixture: prefix + twenty A bytes."""
    expanded = prefix + (b"A" * 20)
    # Decoding from right to left: three literals, then a length-17
    # reference at displacement 3. The flag bit is the fourth item.
    compressed_body = b"\xE0\x00AAA\x10"
    padding = b"\xFF" * padding_size
    header_size = 8 + padding_size
    extra_size = len(expanded) - (len(prefix) + len(compressed_body) + header_size)
    footer = struct.pack(
        "<II", (header_size << 24) | len(compressed_body), extra_size
    )
    return prefix + compressed_body + padding + footer, expanded


class ExtractNdsOverlaysTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="oracis-overlay-extract-test-")
        self.root = Path(self.temp.name)
        self.rom_path = self.root / "synthetic-homebrew-fixture.nds"
        self.original_expected_sha1 = extractor.EXPECTED_ROM_SHA1

    def tearDown(self) -> None:
        extractor.EXPECTED_ROM_SHA1 = self.original_expected_sha1
        self.temp.cleanup()

    def make_rom(self, *, bad_ram_size: bool = False) -> bytes:
        # Layout: header, ARM9 table, ARM7 table, FAT, then two overlay files.
        arm9_table_offset = 0x200
        arm7_table_offset = 0x220
        fat_offset = 0x240
        arm9_file_offset = 0x260
        arm7_file_offset = 0x280

        arm9_image = bytes(range(1, 13))
        arm7_payload, arm7_image = make_blz_repeat_fixture(b"XYZ")
        image = bytearray(arm7_file_offset + len(arm7_payload))

        struct.pack_into("<II", image, 0x48, fat_offset, 16)
        struct.pack_into("<II", image, 0x50, arm9_table_offset, 32)
        struct.pack_into("<II", image, 0x58, arm7_table_offset, 32)

        # ARM9 overlay 3: raw, with a bounded static-init function range.
        struct.pack_into(
            "<8I", image, arm9_table_offset,
            3, 0x02200000, len(arm9_image), 4,
            0x02200004, 0x0220000C,
            0,
            0,
        )
        # ARM7 overlay 1: BLZ-compressed, with a three-byte passthrough prefix.
        struct.pack_into(
            "<8I", image, arm7_table_offset,
            1, 0x03800000, len(arm7_image) + (1 if bad_ram_size else 0), 8,
            0, 0,
            1,
            (len(arm7_payload) & 0x00FFFFFF) | (1 << 24),
        )
        struct.pack_into(
            "<4I", image, fat_offset,
            arm9_file_offset, arm9_file_offset + len(arm9_image),
            arm7_file_offset, arm7_file_offset + len(arm7_payload),
        )
        image[arm9_file_offset:arm9_file_offset + len(arm9_image)] = arm9_image
        image[arm7_file_offset:arm7_file_offset + len(arm7_payload)] = arm7_payload
        self.rom_path.write_bytes(image)
        extractor.EXPECTED_ROM_SHA1 = hashlib.sha1(image).hexdigest()
        return bytes(image)

    def test_decodes_blz_suffix_and_preserves_uncompressed_prefix(self) -> None:
        compressed, expected = make_blz_repeat_fixture(b"XYZ")
        self.assertEqual(extractor.decompress_blz(compressed, len(expected)), expected)

    def test_decodes_ff_padding_in_extended_blz_footer(self) -> None:
        compressed, expected = make_blz_repeat_fixture(b"XYZ", padding_size=4)
        self.assertEqual(extractor.decompress_blz(compressed, len(expected)), expected)

    def test_extracts_uncompressed_and_compressed_overlays(self) -> None:
        self.make_rom()
        output = self.root / "overlays"
        manifest = extractor.extract_overlays(self.rom_path, output)

        self.assertEqual(manifest["overlay_count"], 2)
        arm9 = output / "arm9_overlay_0003.bin"
        arm7 = output / "arm7_overlay_0001.bin"
        self.assertEqual(arm9.read_bytes(), bytes(range(1, 13)))
        self.assertEqual(arm7.read_bytes(), b"XYZ" + (b"A" * 20))
        self.assertEqual(
            json.loads((output / "manifest.json").read_text(encoding="utf-8")),
            manifest,
        )
        self.assertEqual(
            {entry["cpu"] for entry in manifest["overlays"]}, {"arm9", "arm7"}
        )
        compressed_record = next(
            entry for entry in manifest["overlays"] if entry["cpu"] == "arm7"
        )
        self.assertTrue(compressed_record["compressed"])
        self.assertEqual(compressed_record["ram_size"], 23)
        self.assertEqual(
            compressed_record["image_sha256"],
            hashlib.sha256(arm7.read_bytes()).hexdigest(),
        )

    def test_replaces_only_a_previous_extractor_owned_output(self) -> None:
        self.make_rom()
        output = self.root / "overlays"
        extractor.extract_overlays(self.rom_path, output)
        extractor.extract_overlays(self.rom_path, output)
        self.assertTrue((output / "manifest.json").is_file())

    def test_rejects_rom_identity_mismatch_before_writing(self) -> None:
        self.make_rom()
        extractor.EXPECTED_ROM_SHA1 = "0" * 40
        output = self.root / "no-output"
        with self.assertRaisesRegex(ValueError, "identity mismatch"):
            extractor.extract_overlays(self.rom_path, output)
        self.assertFalse(output.exists())

    def test_rejects_overlay_file_id_outside_fat(self) -> None:
        self.make_rom()
        image = bytearray(self.rom_path.read_bytes())
        struct.pack_into("<I", image, 0x200 + 24, 99)
        self.rom_path.write_bytes(image)
        extractor.EXPECTED_ROM_SHA1 = hashlib.sha1(image).hexdigest()
        output = self.root / "bad-file-id"
        with self.assertRaisesRegex(ValueError, "missing FAT file"):
            extractor.extract_overlays(self.rom_path, output)
        self.assertFalse(output.exists())

    def test_rejects_decompressed_size_that_disagrees_with_ram_size(self) -> None:
        self.make_rom(bad_ram_size=True)
        output = self.root / "bad-size"
        with self.assertRaisesRegex(ValueError, "output size does not match"):
            extractor.extract_overlays(self.rom_path, output)
        self.assertFalse(output.exists())

    def test_rejects_corrupt_back_reference(self) -> None:
        compressed, _ = make_blz_repeat_fixture()
        corrupted = bytearray(compressed)
        # Force a back-reference beyond the already-produced output region.
        corrupted[1] = 0xFF
        with self.assertRaises(ValueError):
            extractor.decompress_blz(bytes(corrupted), 20)

    def test_refuses_to_replace_directory_with_unowned_file(self) -> None:
        self.make_rom()
        output = self.root / "overlays"
        output.mkdir()
        (output / "keep-me.txt").write_text("user data", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "without an Oracis manifest"):
            extractor.extract_overlays(self.rom_path, output)
        self.assertEqual((output / "keep-me.txt").read_text(), "user data")


if __name__ == "__main__":
    unittest.main(verbosity=2)
