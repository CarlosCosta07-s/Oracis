from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import struct
import tempfile
import unittest
from zipfile import ZipFile

SCRIPT = (
    Path(__file__).resolve().parents[2]
    / "tools" / "ndsrecomp" / "discover_platinum_overlay_configs.py"
)
SPEC = importlib.util.spec_from_file_location("oracis_overlay_discovery_target", SCRIPT)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"Cannot load overlay discovery tool: {SCRIPT}")
discovery = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = discovery
SPEC.loader.exec_module(discovery)

ROM_SHA1 = "0862ec35b24de5c7e2dcb88c9eea0873110d755c"


class DiscoverPlatinumOverlayConfigsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="oracis-overlay-discovery-")
        self.root = Path(self.temp.name)
        self.image_dir = self.root / "images"
        self.image_dir.mkdir()
        self.output_dir = self.root / "configs"
        self.source_zip = self.root / "source.zip"
        self.rows = [self.add_overlay(5), self.add_overlay(6)]
        self.write_manifest()

    def tearDown(self) -> None:
        self.temp.cleanup()

    def add_overlay(self, overlay_id: int) -> dict:
        image = bytes([overlay_id]) * 16
        image_file = f"arm9_overlay_{overlay_id:04d}.bin"
        (self.image_dir / image_file).write_bytes(image)
        return {
            "cpu": "arm9",
            "overlay_id": overlay_id,
            "ram_address": 0x02000000 + overlay_id * 0x100,
            "ram_size": len(image),
            "static_init_start": 0,
            "static_init_end": 0,
            "image_size": len(image),
            "image_file": image_file,
            "image_sha1": hashlib.sha1(image).hexdigest(),
            "image_sha256": hashlib.sha256(image).hexdigest(),
        }

    def write_manifest(self) -> None:
        manifest = {
            "schema": "oracis-nds-overlays-v1",
            "rom_sha1": ROM_SHA1,
            "overlay_count": len(self.rows),
            "overlays": self.rows,
        }
        (self.image_dir / "manifest.json").write_text(
            json.dumps(manifest), encoding="utf-8"
        )

    def write_source_zip(self, entries: list[str]) -> None:
        with ZipFile(self.source_zip, "w") as archive:
            for name in entries:
                archive.writestr(
                    name, "/* source contents are deliberately not used */\n"
                )

    def test_discovers_configs_and_reports_missing_source_anchors(self) -> None:
        self.write_source_zip([
            "pokeplatinum-main/src/overlay005/ov5_02000500.c",
            "pokeplatinum-main/src/overlay005/ov5_02000508.c",
            "pokeplatinum-main/src/overlay005/not_an_anchor.c",
        ])
        report = discovery.discover(
            self.image_dir, self.source_zip, self.output_dir
        )
        self.assertEqual(report["overlay_count"], 2)
        self.assertEqual(report["config_ready_count"], 1)
        self.assertEqual(report["without_source_anchors"], 1)
        self.assertEqual(report["invalid_anchor_overlays"], 0)
        config = (
            self.output_dir / "arm9_overlay_0005.toml"
        ).read_text(encoding="utf-8")
        self.assertIn(
            'id = "pokemon_platinum_usa_rev1_arm9_overlay_0005"', config
        )
        self.assertIn("addr = 0x02000500", config)
        self.assertIn("addr = 0x02000508", config)

    def test_rejects_source_anchor_outside_overlay_ram(self) -> None:
        self.write_source_zip([
            "source/src/overlay005/ov5_02000600.c"
        ])
        report = discovery.discover(
            self.image_dir, self.source_zip, self.output_dir
        )
        self.assertEqual(
            report["results"][0]["status"], "invalid-source-anchor"
        )
        self.assertEqual(report["config_ready_count"], 0)
        self.assertFalse(
            (self.output_dir / "arm9_overlay_0005.toml").exists()
        )

    def test_refuses_to_overwrite_different_existing_config(self) -> None:
        self.write_source_zip([
            "source/src/overlay005/ov5_02000500.c"
        ])
        self.output_dir.mkdir()
        config = self.output_dir / "arm9_overlay_0005.toml"
        config.write_text("user data\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "existing config differs"):
            discovery.discover(
                self.image_dir, self.source_zip, self.output_dir
            )
        self.assertEqual(config.read_text(encoding="utf-8"), "user data\n")

    def test_rejects_corrupt_extracted_image(self) -> None:
        self.write_source_zip([
            "source/src/overlay005/ov5_02000500.c"
        ])
        (self.image_dir / self.rows[0]["image_file"]).write_bytes(b"corrupt")
        with self.assertRaisesRegex(ValueError, "integrity failure"):
            discovery.discover(
                self.image_dir, self.source_zip, self.output_dir
            )

    def test_function_pointer_headers_are_opt_in_candidates(self) -> None:
        self.write_source_zip([
            "pokeplatinum-main/include/overlay005/funcptr_ov5_02000508.h"
        ])
        default_anchors, _, default_count = discovery.source_anchors(
            self.source_zip
        )
        expanded_anchors, _, expanded_count = discovery.source_anchors(
            self.source_zip, include_function_pointer_headers=True
        )
        self.assertEqual(default_count, 0)
        self.assertEqual(default_anchors, {})
        self.assertEqual(expanded_count, 1)
        self.assertEqual(
            expanded_anchors[5], [(0x02000508, "ov5_02000508")]
        )

    def test_static_init_pointers_seed_exact_arm_and_thumb_modes(self) -> None:
        row = self.rows[0]
        image_path = self.image_dir / row["image_file"]
        image = bytearray(image_path.read_bytes())
        struct.pack_into(
            "<II", image, 8,
            row["ram_address"] | 1,
            row["ram_address"] + 4,
        )
        image_path.write_bytes(image)
        row["static_init_start"] = row["ram_address"] + 8
        row["static_init_end"] = row["ram_address"] + 16
        row["image_sha1"] = hashlib.sha1(image).hexdigest()
        row["image_sha256"] = hashlib.sha256(image).hexdigest()
        self.write_manifest()
        self.write_source_zip([])

        output = self.root / "static-init-configs"
        report = discovery.discover(
            self.image_dir,
            self.source_zip,
            output,
            include_static_init_pointers=True,
        )
        self.assertEqual(report["config_ready_count"], 1)
        self.assertEqual(report["without_source_anchors"], 2)
        self.assertEqual(report["static_init_seed_count"], 2)
        self.assertEqual(report["static_init_seed_overlay_count"], 1)
        self.assertEqual(report["without_any_candidates"], 1)
        item = report["results"][0]
        self.assertTrue(item["static_init_only"])
        self.assertEqual(item["source_anchor_count"], 0)
        self.assertEqual(item["static_init_seed_count"], 2)
        modes = {seed["mode"] for seed in item["entry_point_seeds"]}
        self.assertEqual(modes, {"arm", "thumb"})
        config = (output / "arm9_overlay_0005.toml").read_text(encoding="utf-8")
        self.assertIn('mode = "arm"', config)
        self.assertIn('mode = "thumb"', config)
        self.assertIn("# Seed source: overlay static-init pointer", config)
        self.assertIn("authoritative_entry_points = false", config)

    def test_rejects_static_init_pointer_outside_overlay_ram(self) -> None:
        row = self.rows[0]
        image_path = self.image_dir / row["image_file"]
        image = bytearray(image_path.read_bytes())
        struct.pack_into("<I", image, 8, row["ram_address"] + 0x20)
        image_path.write_bytes(image)
        row["static_init_start"] = row["ram_address"] + 8
        row["static_init_end"] = row["ram_address"] + 12
        row["image_sha1"] = hashlib.sha1(image).hexdigest()
        row["image_sha256"] = hashlib.sha256(image).hexdigest()
        self.write_manifest()
        self.write_source_zip([])
        with self.assertRaisesRegex(ValueError, "static-init pointer.*outside/misaligned"):
            discovery.discover(
                self.image_dir,
                self.source_zip,
                self.root / "invalid-static-init",
                include_static_init_pointers=True,
            )

    def test_directory_source_matches_filename_contract(self) -> None:
        source_dir = self.root / "source-tree"
        target = (
            source_dir / "pokeplatinum" / "src" / "overlay005"
            / "ov5_02000500.c"
        )
        target.parent.mkdir(parents=True)
        target.write_text("not parsed or compiled", encoding="utf-8")
        anchors, _, matched = discovery.source_anchors(source_dir)
        self.assertEqual(matched, 1)
        self.assertEqual(anchors[5], [(0x02000500, "ov5_02000500")])


if __name__ == "__main__":
    unittest.main(verbosity=2)
