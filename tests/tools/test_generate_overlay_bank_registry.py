from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

SCRIPT = (
    Path(__file__).resolve().parents[2]
    / "tools" / "ndsrecomp" / "generate_overlay_bank_registry.py"
)
SPEC = importlib.util.spec_from_file_location("oracis_overlay_registry_target", SCRIPT)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"Cannot load overlay registry generator: {SCRIPT}")
generator = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = generator
SPEC.loader.exec_module(generator)


class GenerateOverlayBankRegistryTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="oracis-overlay-registry-")
        self.root = Path(self.temp.name)
        self.bank_dir = self.root / "banks"
        self.bank_dir.mkdir()
        self.overlay_image_dir = self.root / "extracted-overlays"
        self.overlay_image_dir.mkdir()
        self.source_overlays: list[dict] = []

    def tearDown(self) -> None:
        self.temp.cleanup()

    def add_bank(self, overlay_id: int, *, status: str = "emitted") -> dict:
        bank = (
            f"pokemon_platinum_usa_rev1_arm9_overlay_{overlay_id:04d}"
        )
        folder = self.bank_dir / f"overlay_{overlay_id:04d}"
        folder.mkdir()
        for suffix, contents in (
            (".c", "void generated_body(void) {}\n"),
            (".h", "void generated_body(void);\n"),
            ("_dispatch.c", "/* synthetic dispatch fixture */\n"),
        ):
            (folder / f"{bank}{suffix}").write_text(contents, encoding="utf-8")
        image = bytes([overlay_id & 0xFF]) * 16
        image_name = f"arm9_overlay_{overlay_id:04d}.bin"
        (self.overlay_image_dir / image_name).write_bytes(image)
        image_sha1 = hashlib.sha1(image).hexdigest()
        image_sha256 = hashlib.sha256(image).hexdigest()
        self.source_overlays.append({
            "cpu": "arm9",
            "overlay_id": overlay_id,
            "ram_address": 0x02000000 + overlay_id * 0x100,
            "ram_size": len(image),
            "bss_size": 0,
            "static_init_start": 0,
            "static_init_end": 0,
            "image_file": image_name,
            "image_size": len(image),
            "image_sha1": image_sha1,
            "image_sha256": image_sha256,
        })
        body = folder / f"{bank}.c"
        header = folder / f"{bank}.h"
        dispatch = folder / f"{bank}_dispatch.c"
        shard_dir = folder / f"{bank}_shards"
        shard_dir.mkdir()
        shard = shard_dir / "bank_part_0000.c"
        shard.write_text("void generated_body(void) {}\\n", encoding="utf-8")
        return {
            "overlay_id": overlay_id,
            "bank": bank,
            "status": status,
            "functions_total": 12,
            "image_sha1": image_sha1,
            "sha256": {
                path.name: generator.sha256(path)
                for path in (body, header, dispatch)
            },
            "shards": {
                "directory": shard_dir.name,
                "function_count": 12,
                "functions_per_shard": 512,
                "files": {shard.name: generator.sha1(shard)},
            },
        }

    def write_report(self, rows: list[dict]) -> None:
        report = {
            "schema": "oracis-overlay-bank-emission-v1",
            "codegen": "nds-codegen-v3",
            "rom_sha1": "0862ec35b24de5c7e2dcb88c9eea0873110d755c",
            "direction_adapter": {"status": "passed"},
            "results": rows,
        }
        (self.bank_dir / "emission_report.json").write_text(
            json.dumps(report), encoding="utf-8"
        )
        source_manifest = {
            "schema": "oracis-nds-overlays-v1",
            "rom_sha1": "0862ec35b24de5c7e2dcb88c9eea0873110d755c",
            "overlay_count": len(self.source_overlays),
            "overlays": self.source_overlays,
            "generated_files": [row["image_file"] for row in self.source_overlays],
        }
        (self.overlay_image_dir / "manifest.json").write_text(
            json.dumps(source_manifest), encoding="utf-8"
        )

    def test_generates_sorted_idempotent_arm9_registrar_and_hash_manifest(self) -> None:
        rows = [self.add_bank(9), self.add_bank(5), self.add_bank(7, status="skipped")]
        self.write_report(rows)
        source = self.root / "generated" / "registry.cpp"
        manifest_path = self.root / "generated" / "manifest.json"

        result = generator.generate(self.bank_dir, self.overlay_image_dir, source, manifest_path)
        generated = source.read_text(encoding="utf-8")
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

        self.assertEqual(result["bank_count"], 2)
        self.assertEqual(result["function_count"], 24)
        self.assertLess(
            generated.index("overlay_0005"),
            generated.index("overlay_0009"),
        )
        self.assertIn("oracis_static_dispatch_register_bank", generated)
        self.assertIn("oracis_static_dispatch_unregister_bank", generated)
        self.assertEqual(manifest["schema"], "oracis-overlay-registry-v1")
        self.assertEqual(
            [entry["overlay_id"] for entry in manifest["banks"]], [5, 9]
        )
        self.assertEqual(manifest["source_overlay_count"], 3)
        self.assertEqual(manifest["compiled_overlay_count"], 2)
        self.assertEqual(manifest["uncompiled_overlay_count"], 1)
        self.assertEqual(manifest["uncompiled_overlay_ids"], [7])
        self.assertEqual(manifest["shard_source_count"], 2)
        self.assertEqual(manifest["banks"][0]["shard_count"], 1)
        self.assertEqual(len(manifest["banks"][0]["compile_sources"]), 1)
        self.assertTrue(manifest["banks"][0]["compile_sources"][0].endswith(
            "bank_part_0000.c"
        ))
        self.assertEqual(
            manifest["banks"][0]["dispatch_sha256"],
            generator.sha256(
                self.bank_dir
                / "overlay_0005"
                / "pokemon_platinum_usa_rev1_arm9_overlay_0005_dispatch.c"
            ),
        )

    def test_rejects_unknown_codegen_identity(self) -> None:
        self.add_bank(5)
        self.write_report([self.add_bank(6)])
        report_path = self.bank_dir / "emission_report.json"
        report = json.loads(report_path.read_text(encoding="utf-8"))
        report["codegen"] = "unverified-codegen"
        report_path.write_text(json.dumps(report), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "nds-codegen-v3"):
            generator.generate(
                self.bank_dir, self.overlay_image_dir,
                self.root / "registry.cpp", self.root / "manifest.json",
            )

    def test_rejects_missing_or_empty_dispatch_source(self) -> None:
        self.write_report([self.add_bank(5)])
        dispatch = (
            self.bank_dir / "overlay_0005"
            / "pokemon_platinum_usa_rev1_arm9_overlay_0005_dispatch.c"
        )
        dispatch.write_text("", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "missing, empty, or unsafe"):
            generator.generate(
                self.bank_dir, self.overlay_image_dir,
                self.root / "registry.cpp", self.root / "manifest.json",
            )


    def test_rejects_compiled_image_hash_that_differs_from_extraction(self) -> None:
        row = self.add_bank(5)
        row["image_sha1"] = "0" * 40
        self.write_report([row])
        with self.assertRaisesRegex(ValueError, "image hash does not match"):
            generator.generate(
                self.bank_dir, self.overlay_image_dir,
                self.root / "registry.cpp", self.root / "manifest.json",
            )

    def test_rejects_corrupted_extracted_image(self) -> None:
        row = self.add_bank(5)
        self.write_report([row])
        image_path = self.overlay_image_dir / "arm9_overlay_0005.bin"
        image_path.write_bytes(b"corrupted image")
        with self.assertRaisesRegex(ValueError, "overlay image hash mismatch"):
            generator.generate(
                self.bank_dir, self.overlay_image_dir,
                self.root / "registry.cpp", self.root / "manifest.json",
            )

    def test_rejects_tampered_shard_source(self) -> None:
        row = self.add_bank(5)
        self.write_report([row])
        shard = (
            self.bank_dir / "overlay_0005"
            / "pokemon_platinum_usa_rev1_arm9_overlay_0005_shards"
            / "bank_part_0000.c"
        )
        shard.write_text("void tampered(void) {}\\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "shard SHA-1 mismatch"):
            generator.generate(
                self.bank_dir, self.overlay_image_dir,
                self.root / "registry.cpp", self.root / "manifest.json",
            )

    def test_supports_legacy_monolithic_overlay_bank_without_shards(self) -> None:
        row = self.add_bank(5)
        row.pop("shards")
        self.write_report([row])
        result = generator.generate(
            self.bank_dir, self.overlay_image_dir,
            self.root / "registry.cpp", self.root / "manifest.json",
        )
        self.assertEqual(result["banks"][0]["shard_count"], 0)
        self.assertTrue(result["banks"][0]["compile_sources"][0].endswith(".c"))

    def test_rejects_duplicate_overlay_ids(self) -> None:
        row = self.add_bank(5)
        self.write_report([row, dict(row)])
        with self.assertRaisesRegex(ValueError, "duplicate overlay ID"):
            generator.generate(
                self.bank_dir, self.overlay_image_dir,
                self.root / "registry.cpp", self.root / "manifest.json",
            )


if __name__ == "__main__":
    unittest.main(verbosity=2)
