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
                f'[[entry_point]]\nmode = "{mode}"\n',
                encoding="utf-8",
            )
            self.records.append({
                "cpu": cpu,
                "load_address": load,
                "size": len(data),
                "entry_pc": load,
                "entry_mode": mode,
                "sha1": digest,
            })

        (self.extracted / "manifest.json").write_text(json.dumps({
            "schema_version": 1,
            "rom_sha1": EXPECTED_ROM_SHA1,
            "segments": self.records,
        }), encoding="utf-8")

    def tearDown(self) -> None:
        self.temp.cleanup()

    @staticmethod
    def fake_runner(commands, *, omit_dispatch=False):
        def run(command):
            commands.append(command)
            bank = command[command.index("--bank") + 1]
            output = Path(command[command.index("--out") + 1])
            (output / f"{bank}.c").write_text(
                "/* synthetic bank body */\\n", encoding="utf-8"
            )
            (output / f"{bank}.h").write_text(
                "/* synthetic bank header */\\n", encoding="utf-8"
            )
            if not (omit_dispatch and bank.endswith("_arm7")):
                (output / f"{bank}_dispatch.c").write_text(
                    f"const NdsDispatchEntry g_dispatch_{bank}[] = {{}};\\n"
                    f"const unsigned g_dispatch_{bank}_len = 0u;\\n",
                    encoding="utf-8",
                )
        return run

    def test_emits_two_banks_with_live_byte_validation_and_manifest(self) -> None:
        commands = []
        result = emit_banks(
            self.recompiler, self.extracted, self.output,
            self.fake_runner(commands),
        )

        self.assertEqual(len(commands), 2)
        for cpu, command in zip(("arm9", "arm7"), commands):
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
        (self.extracted / "arm7.bin").write_bytes(b"tampered")
        commands = []
        with self.assertRaisesRegex(ValueError, "SHA-1 mismatch"):
            emit_banks(
                self.recompiler, self.extracted, self.output,
                self.fake_runner(commands),
            )
        self.assertEqual(commands, [])
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

    def test_rejects_missing_dispatch_table(self) -> None:
        commands = []
        with self.assertRaisesRegex(RuntimeError, "dispatch table"):
            emit_banks(
                self.recompiler, self.extracted, self.output,
                self.fake_runner(commands, omit_dispatch=True),
            )
        self.assertEqual(len(commands), 2)
        self.assertFalse((self.output / "oracis_banks.json").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
