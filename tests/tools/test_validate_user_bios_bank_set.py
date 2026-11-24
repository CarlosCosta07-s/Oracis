#!/usr/bin/env python3
"""Synthetic manifest and safety tests for user BIOS bank-set validation."""
from __future__ import annotations
import hashlib, importlib.util, json, sys, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FIXTURE_SCRIPT = ROOT / "tools/ndsrecomp/create_synthetic_bios_bank_fixture.py"
VALIDATOR_SCRIPT = ROOT / "tools/ndsrecomp/validate_user_bios_bank_set.py"

def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module

fixture = load_module("oracis_synthetic_bios_fixture", FIXTURE_SCRIPT)
validator = load_module("oracis_user_bios_validator", VALIDATOR_SCRIPT)

class ValidateUserBiosBankSetTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="oracis-bios-bank-validator-")
        self.bank_dir = Path(self.temp.name) / "banks"
        fixture.create_fixture(self.bank_dir)

    def tearDown(self):
        self.temp.cleanup()

    def test_accepts_explicitly_enabled_synthetic_fixture(self):
        result = validator.validate_bank_set(self.bank_dir, allow_synthetic_fixture=True)
        self.assertEqual(result["source_kind"], "synthetic-fixture")
        self.assertEqual(set(result["banks"]), {"arm9", "arm7"})

    def test_rejects_synthetic_fixture_without_explicit_opt_in(self):
        with self.assertRaisesRegex(ValueError, "explicit --allow-synthetic-fixture"):
            validator.validate_bank_set(self.bank_dir)

    def test_rejects_tampered_bank_source(self):
        path = self.bank_dir / "oracis_user_bios_arm9.c"
        path.write_text(path.read_text(encoding="utf-8") + "/* tampered */\n")
        with self.assertRaisesRegex(ValueError, "hash mismatch"):
            validator.validate_bank_set(self.bank_dir, allow_synthetic_fixture=True)

    def test_rejects_missing_reset_vector_in_dispatch(self):
        dispatch = self.bank_dir / "oracis_user_bios_arm9_dispatch.c"
        content = dispatch.read_text(encoding="utf-8").replace("0xFFFF0000", "0xFFFF0004")
        dispatch.write_text(content, encoding="utf-8")
        manifest_path = self.bank_dir / "oracis_bios_banks.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["banks"]["arm9"]["files"][dispatch.name] = hashlib.sha1(dispatch.read_bytes()).hexdigest()
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "reset vector"):
            validator.validate_bank_set(self.bank_dir, allow_synthetic_fixture=True)

    def test_rejects_unlisted_files(self):
        (self.bank_dir / "bios9.bin").write_bytes(b"not a BIOS")
        with self.assertRaisesRegex(ValueError, "unrelated files"):
            validator.validate_bank_set(self.bank_dir, allow_synthetic_fixture=True)

    def test_rejects_symlinked_shard(self):
        shard_dir = self.bank_dir / "oracis_user_bios_arm7_shards"
        shard = shard_dir / "bank_part_0000.c"
        original = shard.read_text(encoding="utf-8")
        shard.unlink()
        external = Path(self.temp.name) / "outside.c"
        external.write_text(original, encoding="utf-8")
        try:
            shard.symlink_to(external)
        except (OSError, NotImplementedError):
            self.skipTest("symlinks are unavailable on this platform")
        with self.assertRaisesRegex(ValueError, "symlink"):
            validator.validate_bank_set(self.bank_dir, allow_synthetic_fixture=True)

if __name__ == "__main__":
    unittest.main(verbosity=2)
