#!/usr/bin/env python3
"""Synthetic, copyright-free regression tests for the NDS program extractor."""

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
SCRIPT = ROOT / "tools" / "ndsrecomp" / "extract_nds_programs.py"
SPEC = importlib.util.spec_from_file_location(
    "oracis_extract_nds_programs_test_target", SCRIPT
)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"Cannot load extractor module: {SCRIPT}")
extractor = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = extractor
SPEC.loader.exec_module(extractor)


class ExtractNdsProgramsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="oracis-nds-extract-test-")
        self.root = Path(self.temp.name)
        self.rom_path = self.root / "synthetic-homebrew-fixture.nds"
        self.original_expected_sha1 = extractor.EXPECTED_ROM_SHA1

    def tearDown(self) -> None:
        extractor.EXPECTED_ROM_SHA1 = self.original_expected_sha1
        self.temp.cleanup()

    def make_rom(
        self,
        *,
        arm9_offset: int = 0x200,
        arm7_offset: int = 0x204,
        arm9_size: int = 4,
        arm7_size: int = 4,
        arm9_load: int = 0x02000000,
        arm7_load: int = 0x02380000,
        arm9_entry_raw: int | None = None,
        arm7_entry_raw: int | None = None,
    ) -> bytes:
        if arm9_entry_raw is None:
            arm9_entry_raw = arm9_load
        if arm7_entry_raw is None:
            arm7_entry_raw = arm7_load | 1  # Exercise Thumb entry metadata.

        image_size = max(
            extractor.NDS_HEADER_MIN_SIZE,
            arm9_offset + arm9_size,
            arm7_offset + arm7_size,
        )
        image = bytearray(image_size)

        # Minimal ARM9/ARM7 header records: ROM offset, entry, load address, size.
        struct.pack_into(
            "<IIII", image, 0x20,
            arm9_offset, arm9_entry_raw, arm9_load, arm9_size,
        )
        struct.pack_into(
            "<IIII", image, 0x30,
            arm7_offset, arm7_entry_raw, arm7_load, arm7_size,
        )

        arm9_bytes = bytes((0x10 + index) & 0xFF for index in range(arm9_size))
        arm7_bytes = bytes((0x80 + index) & 0xFF for index in range(arm7_size))
        image[arm9_offset:arm9_offset + arm9_size] = arm9_bytes
        image[arm7_offset:arm7_offset + arm7_size] = arm7_bytes

        self.rom_path.write_bytes(image)
        extractor.EXPECTED_ROM_SHA1 = hashlib.sha1(image).hexdigest()
        return bytes(image)

    def test_extracts_both_programs_and_verifies_generated_metadata(self) -> None:
        image = self.make_rom()
        output = self.root / "output"

        manifest = extractor.extract(self.rom_path, output)

        arm9_bytes = (output / "arm9.bin").read_bytes()
        arm7_bytes = (output / "arm7.bin").read_bytes()
        self.assertEqual(arm9_bytes, image[0x200:0x204])
        self.assertEqual(arm7_bytes, image[0x204:0x208])
        self.assertEqual(
            hashlib.sha1(arm9_bytes).hexdigest(),
            manifest["segments"][0]["sha1"],
        )
        self.assertEqual(
            hashlib.sha1(arm7_bytes).hexdigest(),
            manifest["segments"][1]["sha1"],
        )

        self.assertEqual(manifest["rom_sha1"], hashlib.sha1(image).hexdigest())
        self.assertEqual(
            (output / "header.bin").read_bytes(),
            image[:extractor.NDS_HEADER_STORED_SIZE],
        )
        self.assertEqual(
            manifest["boot_header"],
            {
                "file": "header.bin",
                "size": extractor.NDS_HEADER_STORED_SIZE,
                "sha1": hashlib.sha1(
                    image[:extractor.NDS_HEADER_STORED_SIZE]
                ).hexdigest(),
            },
        )
        self.assertEqual(
            [segment["cpu"] for segment in manifest["segments"]],
            ["arm9", "arm7"],
        )
        self.assertEqual(manifest["segments"][0]["entry_mode"], "arm")
        self.assertEqual(manifest["segments"][1]["entry_mode"], "thumb")
        self.assertIn('mode = "arm"', (output / "arm9.toml").read_text())
        self.assertIn('mode = "thumb"', (output / "arm7.toml").read_text())
        self.assertEqual(
            json.loads((output / "manifest.json").read_text()),
            manifest,
        )

    def test_emits_verified_arm9_dispatch_entry_seed(self) -> None:
        arm9 = extractor.Segment(
            cpu="arm9",
            rom_offset=0x4000,
            entry_pc_raw=0x02000800,
            load_address=0x02000000,
            size=0x001023F8,
            entry_pc=0x02000800,
            entry_mode="arm",
            sha1=extractor.EXPECTED_ARM9_SEGMENT_SHA1,
        )

        entries = extractor.entry_points_for_segment(arm9)
        self.assertEqual(
            entries,
            (
                (0x02000800, "arm", "rom_entry"),
                (0x020C2EBC, "arm", "observed_dispatch_thunk_020C2EBC"),
                (0x020C2ECC, "arm", "observed_branch_table_entry_020C2ECC"),
                (0x020C2F18, "arm", "observed_indirect_function_020C2F18"),
            ),
        )
        config = extractor.toml_for_segment(arm9)
        self.assertIn("addr = 0x020C2EBC", config)
        self.assertIn('name = "observed_dispatch_thunk_020C2EBC"', config)
        self.assertIn("addr = 0x020C2ECC", config)
        self.assertIn('name = "observed_branch_table_entry_020C2ECC"', config)
        self.assertIn("addr = 0x020C2F18", config)
        self.assertIn('name = "observed_indirect_function_020C2F18"', config)

        # The extra seed is bound to the exact USA Rev 1 ARM9 image identity.
        arm7 = extractor.Segment(
            cpu="arm7",
            rom_offset=0x00409800,
            entry_pc_raw=0x02380000,
            load_address=0x02380000,
            size=0x000277FC,
            entry_pc=0x02380000,
            entry_mode="arm",
            sha1=extractor.EXPECTED_ARM9_SEGMENT_SHA1,
        )
        self.assertEqual(
            extractor.entry_points_for_segment(arm7),
            ((0x02380000, "arm", "rom_entry"),),
        )

    def test_rejects_verified_static_seed_outside_image(self) -> None:
        arm9 = extractor.Segment(
            cpu="arm9",
            rom_offset=0x4000,
            entry_pc_raw=0x02000800,
            load_address=0x02000000,
            size=0x00010000,
            entry_pc=0x02000800,
            entry_mode="arm",
            sha1=extractor.EXPECTED_ARM9_SEGMENT_SHA1,
        )
        with self.assertRaisesRegex(ValueError, "outside the verified image"):
            extractor.toml_for_segment(arm9)

    def test_rejects_rom_identity_mismatch_before_creating_outputs(self) -> None:
        self.make_rom()
        extractor.EXPECTED_ROM_SHA1 = "0" * 40
        output = self.root / "must-not-exist"

        with self.assertRaisesRegex(ValueError, "identity mismatch"):
            extractor.extract(self.rom_path, output)

        self.assertFalse(output.exists())

    def test_rejects_overlapping_rom_segments_before_creating_outputs(self) -> None:
        self.make_rom(arm7_offset=0x202)
        output = self.root / "overlap"

        with self.assertRaisesRegex(ValueError, "segments overlap"):
            extractor.extract(self.rom_path, output)

        self.assertFalse(output.exists())

    def test_rejects_entry_pc_outside_loaded_image(self) -> None:
        self.make_rom(arm9_entry_raw=0x02000004)
        output = self.root / "bad-entry"

        with self.assertRaisesRegex(ValueError, "outside loaded image"):
            extractor.extract(self.rom_path, output)

        self.assertFalse(output.exists())

    def test_rejects_guest_load_range_overflow(self) -> None:
        self.make_rom(arm9_load=0xFFFFFFFC, arm9_size=8, arm9_entry_raw=0xFFFFFFFC)
        output = self.root / "overflow"

        with self.assertRaisesRegex(ValueError, "overflows"):
            extractor.extract(self.rom_path, output)

        self.assertFalse(output.exists())

    def test_rejects_segment_past_rom_end(self) -> None:
        self.make_rom()
        image = bytearray(self.rom_path.read_bytes())
        # Point ARM7 beyond the file without changing the rest of the fixture.
        struct.pack_into("<I", image, 0x30, len(image) - 2)
        self.rom_path.write_bytes(image)
        extractor.EXPECTED_ROM_SHA1 = hashlib.sha1(image).hexdigest()
        output = self.root / "past-end"

        with self.assertRaisesRegex(ValueError, "exceeds file bounds"):
            extractor.extract(self.rom_path, output)

        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
