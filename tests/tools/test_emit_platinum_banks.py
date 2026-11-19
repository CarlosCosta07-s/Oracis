#!/usr/bin/env python3
"""Synthetic tests for the local Platinum static-bank emitter."""

from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "ndsrecomp" / "emit_platinum_banks.py"
SPEC = importlib.util.spec_from_file_location(
    "oracis_emit_platinum_banks_test_target", SCRIPT
)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"Cannot load bank emitter module: {SCRIPT}")
emitter = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = emitter
SPEC.loader.exec_module(emitter)

BANK_PREFIX = emitter.BANK_PREFIX
EXPECTED_ROM_SHA1 = emitter.EXPECTED_ROM_SHA1
EXPECTED_CODEGEN_IDENTITY = emitter.EXPECTED_CODEGEN_IDENTITY
emit_banks = emitter.emit_banks


class EmitPlatinumBanksTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="oracis-bank-emitter-")
        self.root = Path(self.temp.name)
        self.extracted = self.root / "extracted"
        self.output = self.root / "generated"
        self.extracted.mkdir()
        self.recompiler = self.root / "nds_recompile"
        self.recompiler.write_bytes(b"synthetic test executable")
        self.original_expected_segments = emitter.EXPECTED_SEGMENTS

        self.records = []
        for cpu, load, mode, data in (
            ("arm9", 0x02000000, "arm", b"ARM9"),
            ("arm7", 0x02380000, "thumb", b"A7"),
        ):
            digest = hashlib.sha1(data).hexdigest()
            (self.extracted / f"{cpu}.bin").write_bytes(data)
            (self.extracted / f"{cpu}.toml").write_text(
                f'[program]\nid = "{BANK_PREFIX}_{cpu}"\n'
                f'load_address = 0x{load:08X}\n'
                f'size = 0x{len(data):08X}\n'
                f'entry_pc = 0x{load:08X}\n'
                f'[identity]\nsha1 = "{digest}"\n'
                f'[[entry_point]]\naddr = 0x{load:08X}\n'
                f'mode = "{mode}"\n'
                f'name = "rom_entry"\n',
                encoding="utf-8",
            )
            self.records.append({
                "cpu": cpu,
                "rom_offset": 0x200 if cpu == "arm9" else 0x204,
                "entry_pc_raw": load | (1 if mode == "thumb" else 0),
                "load_address": load,
                "size": len(data),
                "entry_pc": load,
                "entry_mode": mode,
                "sha1": digest,
            })

        emitter.EXPECTED_SEGMENTS = {
            record["cpu"]: {
                key: record[key] for key in (
                    "rom_offset", "entry_pc_raw", "load_address", "size",
                    "entry_pc", "entry_mode", "sha1",
                )
            }
            for record in self.records
        }

        (self.extracted / "manifest.json").write_text(json.dumps({
            "schema_version": 1,
            "rom_sha1": EXPECTED_ROM_SHA1,
            "segments": self.records,
        }), encoding="utf-8")

    def tearDown(self) -> None:
        emitter.EXPECTED_SEGMENTS = self.original_expected_segments
        self.temp.cleanup()

    @staticmethod
    def fake_runner(
        commands, *, omit_dispatch=False, fail_bank=None,
        codegen_identity=EXPECTED_CODEGEN_IDENTITY,
    ):
        def run(command):
            commands.append(command)
            if command[-1] == "--codegen-identity":
                return codegen_identity + "\n"

            bank = command[command.index("--bank") + 1]
            if bank == fail_bank:
                raise RuntimeError(f"synthetic {bank} emission failure")
            output = Path(command[command.index("--out") + 1])
            (output / f"{bank}.c").write_text(
                "/* synthetic bank body */\n", encoding="utf-8"
            )
            (output / f"{bank}.h").write_text(
                "/* synthetic bank header */\n", encoding="utf-8"
            )
            if not (omit_dispatch and bank.endswith("_arm7")):
                (output / f"{bank}_dispatch.c").write_text(
                    f"const NdsDispatchEntry g_dispatch_{bank}[] = {{}};\n"
                    f"const unsigned g_dispatch_{bank}_len = 0u;\n",
                    encoding="utf-8",
                )
            return ""
        return run

    def test_emits_two_banks_with_live_byte_validation_and_manifest(self) -> None:
        commands = []
        result = emit_banks(
            self.recompiler, self.extracted, self.output,
            self.fake_runner(commands),
        )

        self.assertEqual(
            commands[0], [str(self.recompiler), "--codegen-identity"]
        )
        self.assertEqual(len(commands), 3)
        for cpu, command in zip(("arm9", "arm7"), commands[1:]):
            self.assertIn("--validate-live-bytes", command)
            self.assertEqual(
                command[command.index("--bank") + 1],
                f"{BANK_PREFIX}_{cpu}",
            )
            self.assertEqual(
                command[command.index("--config") + 1],
                str(self.extracted / f"{cpu}.toml"),
            )
            self.assertEqual(
                command[command.index("--bin") + 1],
                str(self.extracted / f"{cpu}.bin"),
            )

        self.assertEqual(set(result["banks"]), {"arm9", "arm7"})
        self.assertEqual(
            result["banks"]["arm9"]["entry_mode"], "arm"
        )
        self.assertEqual(
            result["banks"]["arm7"]["entry_mode"], "thumb"
        )
        on_disk = json.loads(
            (self.output / "oracis_banks.json").read_text(encoding="utf-8")
        )
        self.assertEqual(on_disk, result)
        self.assertEqual(
            set(result["banks"]["arm9"]["files"]),
            {
                f"{BANK_PREFIX}_arm9.c",
                f"{BANK_PREFIX}_arm9.h",
                f"{BANK_PREFIX}_arm9_dispatch.c",
            },
        )

    def test_rejects_changed_segment_before_invoking_recompiler(self) -> None:
        (self.extracted / "arm7.bin").write_bytes(b"ZZ")
        commands = []
        with self.assertRaisesRegex(ValueError, "SHA-1 mismatch"):
            emit_banks(
                self.recompiler, self.extracted, self.output,
                self.fake_runner(commands),
            )
        self.assertEqual(commands, [])
        self.assertFalse(self.output.exists())

    def test_rejects_unpinned_codegen_identity_before_emission(self) -> None:
        commands = []
        with self.assertRaisesRegex(RuntimeError, "codegen identity mismatch"):
            emit_banks(
                self.recompiler, self.extracted, self.output,
                self.fake_runner(commands, codegen_identity="nds-codegen-v2"),
            )
        self.assertEqual(commands, [[str(self.recompiler), "--codegen-identity"]])
        self.assertFalse(self.output.exists())

    def test_rejects_wrong_rom_identity_before_invoking_recompiler(self) -> None:
        manifest_path = self.extracted / "manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["rom_sha1"] = "0" * 40
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        commands = []
        with self.assertRaisesRegex(ValueError, "ROM identity mismatch"):
            emit_banks(
                self.recompiler, self.extracted, self.output,
                self.fake_runner(commands),
            )
        self.assertEqual(commands, [])

    def test_replaces_previous_generated_set_without_stale_files(self) -> None:
        self.output.mkdir()
        (self.output / "oracis_banks.json").write_text(
            '{"schema_version": 0, "banks": {}}', encoding="utf-8"
        )
        (self.output / f"{BANK_PREFIX}_arm9.c").write_text(
            "old ARM9 body", encoding="utf-8"
        )
        (self.output / f"{BANK_PREFIX}_arm7.h").write_text(
            "old ARM7 header", encoding="utf-8"
        )
        commands = []

        emit_banks(
            self.recompiler, self.extracted, self.output,
            self.fake_runner(commands),
        )

        expected = {"oracis_banks.json"}
        for cpu in ("arm9", "arm7"):
            bank = f"{BANK_PREFIX}_{cpu}"
            expected.update({f"{bank}.c", f"{bank}.h", f"{bank}_dispatch.c"})
        self.assertEqual({entry.name for entry in self.output.iterdir()}, expected)
        self.assertNotIn("old ARM9 body", (self.output / f"{BANK_PREFIX}_arm9.c").read_text())

    def test_preserves_previous_output_when_arm7_emission_fails(self) -> None:
        self.output.mkdir()
        previous_manifest = self.output / "oracis_banks.json"
        previous_arm9 = self.output / f"{BANK_PREFIX}_arm9.c"
        previous_manifest.write_text('{"previous": true}', encoding="utf-8")
        previous_arm9.write_text("previous complete bank set", encoding="utf-8")
        before = {entry.name: entry.read_bytes() for entry in self.output.iterdir()}
        commands = []

        with self.assertRaisesRegex(RuntimeError, "synthetic .*arm7 emission failure"):
            emit_banks(
                self.recompiler, self.extracted, self.output,
                self.fake_runner(
                    commands, fail_bank=f"{BANK_PREFIX}_arm7"
                ),
            )

        after = {entry.name: entry.read_bytes() for entry in self.output.iterdir()}
        self.assertEqual(after, before)
        self.assertEqual(len(commands), 3)

    def test_rejects_unrelated_files_in_existing_output_directory(self) -> None:
        self.output.mkdir()
        note = self.output / "notes.txt"
        note.write_text("user data", encoding="utf-8")
        commands = []

        with self.assertRaisesRegex(ValueError, "unrelated or unsafe content"):
            emit_banks(
                self.recompiler, self.extracted, self.output,
                self.fake_runner(commands),
            )

        self.assertEqual(commands, [])
        self.assertEqual(note.read_text(encoding="utf-8"), "user data")

    def test_rejects_output_directory_overlapping_extraction(self) -> None:
        commands = []
        with self.assertRaisesRegex(ValueError, "separate from the extraction"):
            emit_banks(
                self.recompiler, self.extracted, self.extracted,
                self.fake_runner(commands),
            )
        self.assertEqual(commands, [])

    def test_rejects_missing_dispatch_table(self) -> None:
        commands = []
        with self.assertRaisesRegex(RuntimeError, "dispatch"):
            emit_banks(
                self.recompiler, self.extracted, self.output,
                self.fake_runner(commands, omit_dispatch=True),
            )
        self.assertEqual(len(commands), 3)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
