from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "ndsrecomp" / "emit_platinum_overlay_banks.py"
SPEC = importlib.util.spec_from_file_location("oracis_overlay_emitter_target", SCRIPT)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"Cannot load overlay emitter: {SCRIPT}")
emitter = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = emitter
SPEC.loader.exec_module(emitter)

ROM_SHA1 = "0862ec35b24de5c7e2dcb88c9eea0873110d755c"


class EmitPlatinumOverlayBanksTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="oracis-overlay-emitter-")
        self.root = Path(self.temp.name)
        self.image_dir = self.root / "images"
        self.image_dir.mkdir()
        self.config_dir = self.root / "configs"
        self.config_dir.mkdir()
        self.output_dir = self.root / "generated"
        self.recompiler = self.root / "nds_recompile"
        self.recompiler.write_text("fake compiler marker\n", encoding="utf-8")
        self.rows = []
        for overlay_id in (5, 6):
            image = bytes([overlay_id, 0x34, 0x56, 0x78]) * 4
            image_file = f"arm9_overlay_{overlay_id:04d}.bin"
            (self.image_dir / image_file).write_bytes(image)
            row = {
                "cpu": "arm9", "overlay_id": overlay_id,
                "ram_address": 0x02000000 + (overlay_id - 5) * 0x100,
                "ram_size": len(image), "image_size": len(image),
                "image_file": image_file,
                "image_sha1": hashlib.sha1(image).hexdigest(),
                "image_sha256": hashlib.sha256(image).hexdigest(),
            }
            self.rows.append(row)
        self.image_sha1 = self.rows[0]["image_sha1"]
        (self.image_dir / "manifest.json").write_text(json.dumps({
            "schema": "oracis-nds-overlays-v1",
            "rom_sha1": ROM_SHA1,
            "overlay_count": len(self.rows),
            "overlays": self.rows,
        }), encoding="utf-8")

        for row in self.rows:
            overlay_id = row["overlay_id"]
            (self.config_dir / f"arm9_overlay_{overlay_id:04d}.toml").write_text(
                "[program]\n"
                f'name = "Pokémon Platinum USA Rev 1 ARM9 overlay {overlay_id}"\n'
                f'id = "pokemon_platinum_usa_rev1_arm9_overlay_{overlay_id:04d}"\n'
                f"load_address = 0x{row['ram_address']:08X}\n"
                f"size = 0x{row['ram_size']:08X}\n"
                f"entry_pc = 0x{row['ram_address']:08X}\n"
                "authoritative_entry_points = false\n\n"
                "[identity]\n"
                f'sha1 = "{row["image_sha1"]}"\n\n'
                "[[entry_point]]\n"
                f"addr = 0x{row['ram_address']:08X}\n"
                'mode = "thumb"\n'
                f'name = "ov{overlay_id}_{row["ram_address"]:08X}"\n',
                encoding="utf-8",
            )

        (self.config_dir / "anchor_discovery_report.json").write_text(json.dumps({
            "schema": "oracis-overlay-anchor-discovery-v1",
            "rom_sha1": ROM_SHA1,
            "results": [
                {
                    "overlay_id": row["overlay_id"],
                    "status": "config-ready",
                    "config": f"arm9_overlay_{row['overlay_id']:04d}.toml",
                    "anchor_count": 1,
                }
                for row in self.rows
            ],
        }), encoding="utf-8")

    def tearDown(self) -> None:
        self.temp.cleanup()

    def fake_runner(self, command: list[str], env: dict[str, str]) -> str:
        self.assertEqual(env.get("NDSRECOMP_NO_RELOC_SCAN"), "1")
        if command[-1] == "--codegen-identity":
            return "nds-codegen-v3\n"
        if "--audit" in command:
            return (
                "[finder] functions=1 (arm=0 thumb=1) indirect=0 undefined=0\n"
                "[audit] decoded 4 instructions across 1 functions\n"
                "[audit] codegen NOT-IMPLEMENTED ops (v4t gaps to close):\n"
                "    (none)\n\n"
                "[audit] UNDEFINED encodings (distinct=0 arm, 0 thumb)\n"
            )
        output_dir = Path(command[command.index("--out") + 1])
        bank = command[command.index("--bank") + 1]
        (output_dir / f"{bank}.c").write_text(
            '#include "runtime_arm.h"\nvoid afunc(void);\nvoid afunc(void) {}\n',
            encoding="utf-8",
        )
        (output_dir / f"{bank}.h").write_text(
            "void afunc(void);\n", encoding="utf-8"
        )
        (output_dir / f"{bank}_dispatch.c").write_text(
            f"const NdsDispatchEntry g_dispatch_{bank}[] = {{}};\n"
            f"const unsigned g_dispatch_{bank}_len = 0u;\n",
            encoding="utf-8",
        )
        return "emitted\n"

    def test_emits_audited_identity_bound_bank_and_shards_atomically(self) -> None:
        report = emitter.emit_overlay_banks(
            self.recompiler,
            self.image_dir,
            self.config_dir,
            self.output_dir,
            functions_per_shard=1,
            runner=self.fake_runner,
            overlay_ids={5},
        )
        self.assertEqual(report["compiled_overlay_count"], 1)
        self.assertEqual(report["uncompiled_overlay_count"], 1)
        self.assertEqual(report["results"][0]["functions_total"], 1)
        self.assertEqual(report["results"][0]["image_sha1"], self.image_sha1)
        self.assertEqual(report["direction_adapter"]["status"], "passed")
        bank_dir = self.output_dir / "overlay_0005"
        self.assertTrue((bank_dir / "pokemon_platinum_usa_rev1_arm9_overlay_0005.c").is_file())
        shard = bank_dir / "pokemon_platinum_usa_rev1_arm9_overlay_0005_shards" / "bank_part_0000.c"
        self.assertTrue(shard.is_file())
        saved = json.loads((self.output_dir / "emission_report.json").read_text())
        self.assertEqual(saved["schema"], "oracis-overlay-bank-emission-v1")
        self.assertEqual(saved["results"][0]["audit_status"], "passed")

    def test_reuses_unselected_bank_only_after_hash_verification(self) -> None:
        existing_dir = self.root / "previous-banks"
        old_folder = existing_dir / "overlay_0006"
        old_folder.mkdir(parents=True)
        bank = "pokemon_platinum_usa_rev1_arm9_overlay_0006"
        contents = {
            f"{bank}.c": '#include "runtime_arm.h"\nvoid oldfunc(void);\nvoid oldfunc(void) {}\n',
            f"{bank}.h": "void oldfunc(void);\n",
            f"{bank}_dispatch.c": f"const NdsDispatchEntry g_dispatch_{bank}[] = {{}};\nconst unsigned g_dispatch_{bank}_len = 0u;\n",
        }
        for name, value in contents.items():
            (old_folder / name).write_text(value, encoding="utf-8")
        shard_dir = old_folder / f"{bank}_shards"
        shard_dir.mkdir()
        shard = shard_dir / "bank_part_0000.c"
        shard.write_text("void oldfunc(void) {}\\n", encoding="utf-8")
        old_row = {
            "overlay_id": 6,
            "bank": bank,
            "status": "emitted",
            "functions_total": 1,
            "image_sha1": self.rows[1]["image_sha1"],
            "sha256": {
                name: hashlib.sha256(value.encode("utf-8")).hexdigest()
                for name, value in contents.items()
            },
            "shards": {
                "directory": shard_dir.name,
                "functions_per_shard": 512,
                "function_count": 1,
                "files": {shard.name: hashlib.sha1(shard.read_bytes()).hexdigest()},
            },
        }
        (existing_dir / "emission_report.json").write_text(json.dumps({
            "schema": "oracis-overlay-bank-emission-v1",
            "rom_sha1": ROM_SHA1,
            "codegen": "nds-codegen-v3",
            "direction_adapter": {"status": "passed"},
            "results": [old_row],
        }), encoding="utf-8")

        report = emitter.emit_overlay_banks(
            self.recompiler, self.image_dir, self.config_dir,
            self.output_dir, runner=self.fake_runner,
            overlay_ids={5}, existing_bank_dir=existing_dir,
        )
        self.assertEqual(report["compiled_overlay_count"], 2)
        self.assertEqual(report["emitted_this_run_count"], 1)
        self.assertEqual(report["reused_overlay_count"], 1)
        reused = next(row for row in report["results"] if row["overlay_id"] == 6)
        self.assertTrue(reused["reused"])
        self.assertTrue((self.output_dir / "overlay_0006" / f"{bank}.c").is_file())
        copied_shard = (
            self.output_dir / "overlay_0006" / f"{bank}_shards" / "bank_part_0000.c"
        )
        self.assertTrue(copied_shard.is_file())
        self.assertEqual(
            hashlib.sha1(copied_shard.read_bytes()).hexdigest(),
            old_row["shards"]["files"]["bank_part_0000.c"],
        )

    def test_rejects_codegen_gaps_before_publishing(self) -> None:
        def runner_with_gap(command: list[str], env: dict[str, str]) -> str:
            if command[-1] == "--codegen-identity":
                return "nds-codegen-v3\n"
            if "--audit" in command:
                return (
                    "[finder] functions=1 (arm=0 thumb=1) indirect=0 undefined=0\n"
                    "[audit] decoded 4 instructions across 1 functions\n"
                    "[audit] codegen NOT-IMPLEMENTED ops (v4t gaps to close):\n"
                    "    undefined_opcode\n\n"
                    "[audit] UNDEFINED encodings (distinct=0 arm, 0 thumb)\n"
                )
            raise AssertionError("emit command must not run after audit gaps")

        with self.assertRaisesRegex(RuntimeError, "codegen gaps"):
            emitter.emit_overlay_banks(
                self.recompiler, self.image_dir, self.config_dir,
                self.output_dir, runner=runner_with_gap, overlay_ids={5},
            )
        self.assertFalse(self.output_dir.exists())

    def test_rejects_tampered_reusable_bank(self) -> None:
        existing_dir = self.root / "previous-banks"
        folder = existing_dir / "overlay_0006"
        folder.mkdir(parents=True)
        bank = "pokemon_platinum_usa_rev1_arm9_overlay_0006"
        contents = {
            f"{bank}.c": '#include "runtime_arm.h"\nvoid oldfunc(void);\nvoid oldfunc(void) {}\n',
            f"{bank}.h": "void oldfunc(void);\n",
            f"{bank}_dispatch.c": f"const NdsDispatchEntry g_dispatch_{bank}[] = {{}};\nconst unsigned g_dispatch_{bank}_len = 0u;\n",
        }
        for name, value in contents.items():
            (folder / name).write_text(value, encoding="utf-8")
        (folder / f"{bank}.c").write_text("tampered\n", encoding="utf-8")
        hashes = {
            name: hashlib.sha256(value.encode("utf-8")).hexdigest()
            for name, value in contents.items()
        }
        (existing_dir / "emission_report.json").write_text(json.dumps({
            "schema": "oracis-overlay-bank-emission-v1",
            "rom_sha1": ROM_SHA1,
            "codegen": "nds-codegen-v3",
            "direction_adapter": {"status": "passed"},
            "results": [{
                "overlay_id": 6, "bank": bank, "status": "emitted",
                "functions_total": 1,
                "image_sha1": self.rows[1]["image_sha1"],
                "sha256": hashes,
            }],
        }), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "SHA-256 validation"):
            emitter.emit_overlay_banks(
                self.recompiler, self.image_dir, self.config_dir,
                self.output_dir, runner=self.fake_runner,
                overlay_ids={5}, existing_bank_dir=existing_dir,
            )
        self.assertFalse(self.output_dir.exists())

    def test_rejects_config_identity_mismatch_before_publishing(self) -> None:
        config = self.config_dir / "arm9_overlay_0005.toml"
        config.write_text(config.read_text().replace(self.image_sha1, "0" * 40))
        with self.assertRaisesRegex(ValueError, "image identity mismatch"):
            emitter.emit_overlay_banks(
                self.recompiler, self.image_dir, self.config_dir,
                self.output_dir, runner=self.fake_runner, overlay_ids={5},
            )
        self.assertFalse(self.output_dir.exists())

    def test_refuses_nonempty_output_directory(self) -> None:
        self.output_dir.mkdir()
        (self.output_dir / "keep.txt").write_text("keep", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "already exists and is not empty"):
            emitter.emit_overlay_banks(
                self.recompiler, self.image_dir, self.config_dir,
                self.output_dir, runner=self.fake_runner, overlay_ids={5},
            )
        self.assertEqual((self.output_dir / "keep.txt").read_text(), "keep")


if __name__ == "__main__":
    unittest.main(verbosity=2)
