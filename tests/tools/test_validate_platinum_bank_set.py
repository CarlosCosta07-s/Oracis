#!/usr/bin/env python3
"""Integrity and policy tests for local Platinum bank-set validation."""

from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools" / "ndsrecomp"
sys.path.insert(0, str(TOOLS))

SPEC = importlib.util.spec_from_file_location(
    "oracis_validate_bank_set_test_target",
    TOOLS / "validate_platinum_bank_set.py",
)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("Cannot load bank-set validator")
validator = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = validator
SPEC.loader.exec_module(validator)


class ValidatePlatinumBankSetTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="oracis-bank-set-")
        self.directory = Path(self.temp.name)
        self.write_manifest("retail-rom")

    def tearDown(self) -> None:
        self.temp.cleanup()

    def write_manifest(self, source_kind: str) -> dict:
        banks = {}
        for cpu in validator.CPUS:
            bank_name = f"{validator.BANK_PREFIX}_{cpu}"
            names = (
                f"{bank_name}.c",
                f"{bank_name}.h",
                f"{bank_name}_dispatch.c",
            )
            files = {}
            for name in names:
                payload = f"synthetic test content for {name}\n".encode("utf-8")
                (self.directory / name).write_bytes(payload)
                files[name] = hashlib.sha1(payload).hexdigest()

            if source_kind == "retail-rom":
                expected = validator.EXPECTED_SEGMENTS[cpu]
                record = {
                    "bank": bank_name,
                    "load_address": expected["load_address"],
                    "entry_pc": expected["entry_pc"],
                    "entry_mode": expected["entry_mode"],
                    "size": expected["size"],
                    "binary_sha1": expected["sha1"],
                    "memory_direction_adapter": validator.ADAPTER_IDENTITY,
                    "memory_direction_calls": {"reads": 12, "writes": 8},
                    "files": files,
                }
            else:
                record = {
                    "bank": bank_name,
                    "load_address": 0x02000000 if cpu == "arm9" else 0x02380000,
                    "entry_pc": 0x02000800 if cpu == "arm9" else 0x02380000,
                    "entry_mode": "arm",
                    "size": 16,
                    "binary_sha1": None,
                    "memory_direction_adapter": "synthetic-fixture",
                    "memory_direction_calls": {"reads": 0, "writes": 0},
                    "files": files,
                }
            banks[cpu] = record

        manifest = {
            "schema_version": 1,
            "source_kind": source_kind,
            "rom_sha1": validator.EXPECTED_ROM_SHA1 if source_kind == "retail-rom" else None,
            "codegen_identity": (
                validator.EXPECTED_CODEGEN_IDENTITY
                if source_kind == "retail-rom" else "synthetic-fixture"
            ),
            "banks": banks,
            "notice": "Synthetic test manifest only.",
        }
        (self.directory / validator.MANIFEST_NAME).write_text(
            json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
        )
        return manifest

    def update_manifest(self, manifest: dict) -> None:
        (self.directory / validator.MANIFEST_NAME).write_text(
            json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
        )

    def test_accepts_complete_retail_bank_set(self) -> None:
        manifest = validator.validate_bank_set(self.directory)
        self.assertEqual(manifest["source_kind"], "retail-rom")

    def test_rejects_wrong_rom_identity(self) -> None:
        manifest = json.loads((self.directory / validator.MANIFEST_NAME).read_text())
        manifest["rom_sha1"] = "0" * 40
        self.update_manifest(manifest)
        with self.assertRaisesRegex(validator.BankSetError, "ROM SHA-1"):
            validator.validate_bank_set(self.directory)

    def test_rejects_unpinned_codegen_identity(self) -> None:
        manifest = json.loads((self.directory / validator.MANIFEST_NAME).read_text())
        manifest["codegen_identity"] = "nds-codegen-v2"
        self.update_manifest(manifest)
        with self.assertRaisesRegex(validator.BankSetError, "codegen identity"):
            validator.validate_bank_set(self.directory)

    def test_rejects_tampered_generated_source(self) -> None:
        target = self.directory / f"{validator.BANK_PREFIX}_arm9.c"
        target.write_text("modified after emission", encoding="utf-8")
        with self.assertRaisesRegex(validator.BankSetError, "hash mismatch"):
            validator.validate_bank_set(self.directory)

    def test_rejects_missing_dispatch_translation_unit(self) -> None:
        (self.directory / f"{validator.BANK_PREFIX}_arm7_dispatch.c").unlink()
        with self.assertRaisesRegex(validator.BankSetError, "missing"):
            validator.validate_bank_set(self.directory)

    def test_rejects_unexpected_file(self) -> None:
        (self.directory / "stale.o").write_bytes(b"stale object")
        with self.assertRaisesRegex(validator.BankSetError, "unexpected files"):
            validator.validate_bank_set(self.directory)

    def test_rejects_unpinned_adapter_identity(self) -> None:
        manifest = json.loads((self.directory / validator.MANIFEST_NAME).read_text())
        manifest["banks"]["arm9"]["memory_direction_adapter"] = "foreign-adapter"
        self.update_manifest(manifest)
        with self.assertRaisesRegex(validator.BankSetError, "adapter"):
            validator.validate_bank_set(self.directory)

    def test_rejects_symlinked_bank_file(self) -> None:
        target = self.directory / f"{validator.BANK_PREFIX}_arm9.c"
        replacement = self.directory / "outside.c"
        replacement.write_text(target.read_text(encoding="utf-8"), encoding="utf-8")
        target.unlink()
        try:
            target.symlink_to(replacement)
        except (NotImplementedError, OSError):
            self.skipTest("symlinks are unavailable on this platform")
        with self.assertRaisesRegex(validator.BankSetError, "symlinks"):
            validator.validate_bank_set(self.directory)

    def test_rejects_synthetic_fixtures_without_explicit_opt_in(self) -> None:
        self.write_manifest("synthetic-fixture")
        with self.assertRaisesRegex(validator.BankSetError, "Synthetic bank fixtures are disabled"):
            validator.validate_bank_set(self.directory)

    def test_accepts_explicitly_enabled_synthetic_fixtures(self) -> None:
        self.write_manifest("synthetic-fixture")
        manifest = validator.validate_bank_set(
            self.directory, allow_synthetic_fixture=True
        )
        self.assertEqual(manifest["source_kind"], "synthetic-fixture")

    def test_synthetic_fixture_cannot_claim_retail_codegen_identity(self) -> None:
        manifest = self.write_manifest("synthetic-fixture")
        manifest["codegen_identity"] = validator.EXPECTED_CODEGEN_IDENTITY
        self.update_manifest(manifest)
        with self.assertRaisesRegex(validator.BankSetError, "codegen_identity marker"):
            validator.validate_bank_set(
                self.directory, allow_synthetic_fixture=True
            )

    def test_synthetic_fixture_cannot_claim_retail_rom_identity(self) -> None:
        manifest = self.write_manifest("synthetic-fixture")
        manifest["rom_sha1"] = validator.EXPECTED_ROM_SHA1
        self.update_manifest(manifest)
        with self.assertRaisesRegex(validator.BankSetError, "must not claim"):
            validator.validate_bank_set(
                self.directory, allow_synthetic_fixture=True
            )


if __name__ == "__main__":
    unittest.main(verbosity=2)
