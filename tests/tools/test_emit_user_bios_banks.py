#!/usr/bin/env python3
"""Synthetic tests for the local user-BIOS static-bank emitter."""

from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "ndsrecomp" / "emit_user_bios_banks.py"
SPEC = importlib.util.spec_from_file_location("oracis_emit_user_bios_test", SCRIPT)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"Cannot load BIOS bank emitter: {SCRIPT}")
emitter = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = emitter
SPEC.loader.exec_module(emitter)


class EmitUserBiosBanksTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="oracis-user-bios-")
        self.root = Path(self.temp.name)
        self.arm9 = self.root / "bios9.bin"
        self.arm7 = self.root / "bios7.bin"
        self.output = self.root / "generated"
        self.recompiler = self.root / "nds_recompile"
        self.recompiler.write_bytes(b"synthetic compiler placeholder")
        self.arm9_data = bytes((i * 17 + 5) & 0xFF for i in range(0x1000))
        self.arm7_data = bytes((i * 29 + 3) & 0xFF for i in range(0x4000))
        self.arm9.write_bytes(self.arm9_data)
        self.arm7.write_bytes(self.arm7_data)

    def tearDown(self) -> None:
        self.temp.cleanup()

    @staticmethod
    def fake_runner(
        commands: list[list[str]], *,
        codegen_identity: str = emitter.EXPECTED_CODEGEN_IDENTITY,
        fail_cpu: str | None = None,
        omit_vector: tuple[str, int] | None = None,
    ):
        def run(command: list[str]) -> str:
            commands.append(command)
            if command[-1] == "--codegen-identity":
                return codegen_identity + "\n"

            config = Path(command[command.index("--config") + 1]).read_text(
                encoding="utf-8"
            )
            binary = Path(command[command.index("--bin") + 1])
            bank = command[command.index("--bank") + 1]
            cpu = "arm9" if bank.endswith("_arm9") else "arm7"
            if cpu == fail_cpu:
                raise RuntimeError(f"synthetic {cpu} recompile failure")
            out = Path(command[command.index("--out") + 1])
            (out / f"{bank}.c").write_text(
                f"void {bank}_reset(void) {{\n    (void)0;\n}}\n",
                encoding="utf-8",
            )
            (out / f"{bank}.h").write_text(
                f"void {bank}_reset(void);\n", encoding="utf-8"
            )
            addresses = [
                addr for addr, _ in emitter.entry_points(cpu)
                if not (omit_vector == (cpu, addr))
            ]
            rows = ",\n".join(
                f"    {{ 0x{addr:08X}, 0u, &{bank}_reset, NULL }}"
                for addr in addresses
            )
            (out / f"{bank}_dispatch.c").write_text(
                f"const NdsDispatchEntry g_dispatch_{bank}[] = {{\n{rows}\n}};\n"
                f"const unsigned g_dispatch_{bank}_len = {len(addresses)}u;\n",
                encoding="utf-8",
            )
            if len(binary.read_bytes()) != emitter.BIOS_LAYOUT[cpu]["size"]:
                raise AssertionError("compiler input BIOS copy has wrong size")
            if 'mode = "arm"' not in config:
                raise AssertionError("BIOS vector config was not emitted in ARM mode")
            return ""
        return run

    def test_emits_two_local_banks_with_all_exception_vectors(self) -> None:
        commands: list[list[str]] = []
        result = emitter.emit_banks(
            self.recompiler, self.arm9, self.arm7, self.output,
            self.fake_runner(commands),
        )

        self.assertEqual(commands[0], [str(self.recompiler), "--codegen-identity"])
        self.assertEqual(len(commands), 3)
        for cpu, command in zip(("arm9", "arm7"), commands[1:]):
            self.assertIn("--validate-live-bytes", command)
            self.assertEqual(command[command.index("--bank") + 1],
                             f"{emitter.BANK_PREFIX}_{cpu}")
            self.assertEqual(
                command[command.index("--config") + 1].endswith(f"{cpu}.toml"),
                True,
            )
            self.assertEqual(
                Path(command[command.index("--bin") + 1]).read_bytes(),
                self.arm9_data if cpu == "arm9" else self.arm7_data,
            )

        self.assertEqual(result["source_kind"], "user-bios")
        self.assertEqual(set(result["banks"]), {"arm9", "arm7"})
        self.assertEqual(
            [row["name"] for row in result["banks"]["arm9"]["exception_vectors"]],
            ["reset", "undefined", "swi", "prefetch_abort", "data_abort", "irq", "fiq"],
        )
        self.assertEqual(
            result["banks"]["arm9"]["exception_vectors"][0]["address"],
            0xFFFF0000,
        )
        self.assertEqual(
            result["banks"]["arm7"]["exception_vectors"][0]["address"],
            0x00000000,
        )

        expected = {"oracis_bios_banks.json"}
        for cpu in ("arm9", "arm7"):
            bank = f"{emitter.BANK_PREFIX}_{cpu}"
            expected.update({f"{bank}.c", f"{bank}.h", f"{bank}_dispatch.c",
                             f"{bank}_shards"})
        self.assertEqual({x.name for x in self.output.iterdir()}, expected)
        self.assertFalse((self.output / "arm9.bin").exists())
        self.assertFalse((self.output / "arm7.toml").exists())
        on_disk = json.loads(
            (self.output / "oracis_bios_banks.json").read_text(encoding="utf-8")
        )
        self.assertEqual(on_disk, result)

    def test_rejects_wrong_sizes_before_running_recompiler(self) -> None:
        self.arm9.write_bytes(b"short")
        commands: list[list[str]] = []
        with self.assertRaisesRegex(ValueError, "exactly 4096 bytes"):
            emitter.emit_banks(
                self.recompiler, self.arm9, self.arm7, self.output,
                self.fake_runner(commands),
            )
        self.assertEqual(commands, [])
        self.assertFalse(self.output.exists())

    def test_rejects_wrong_codegen_identity_before_emission(self) -> None:
        commands: list[list[str]] = []
        with self.assertRaisesRegex(RuntimeError, "codegen identity mismatch"):
            emitter.emit_banks(
                self.recompiler, self.arm9, self.arm7, self.output,
                self.fake_runner(commands, codegen_identity="nds-codegen-v2"),
            )
        self.assertEqual(len(commands), 1)
        self.assertFalse(self.output.exists())

    def test_rejects_missing_reset_or_exception_vector(self) -> None:
        commands: list[list[str]] = []
        with self.assertRaisesRegex(RuntimeError, "reset vector"):
            emitter.emit_banks(
                self.recompiler, self.arm9, self.arm7, self.output,
                self.fake_runner(commands, omit_vector=("arm9", 0xFFFF0000)),
            )
        self.assertEqual(len(commands), 2)
        self.assertFalse(self.output.exists())

    def test_preserves_previous_output_when_second_bios_fails(self) -> None:
        self.output.mkdir()
        marker = self.output / "oracis_bios_banks.json"
        marker.write_text('{"previous":true}', encoding="utf-8")
        before = {p.name: p.read_bytes() for p in self.output.iterdir()}
        commands: list[list[str]] = []
        with self.assertRaisesRegex(RuntimeError, "synthetic arm7 recompile failure"):
            emitter.emit_banks(
                self.recompiler, self.arm9, self.arm7, self.output,
                self.fake_runner(commands, fail_cpu="arm7"),
            )
        after = {p.name: p.read_bytes() for p in self.output.iterdir()}
        self.assertEqual(after, before)

    def test_rejects_unrelated_output_and_symlink_inputs(self) -> None:
        self.output.mkdir()
        note = self.output / "notes.txt"
        note.write_text("preserve", encoding="utf-8")
        commands: list[list[str]] = []
        with self.assertRaisesRegex(ValueError, "unrelated or unsafe"):
            emitter.emit_banks(
                self.recompiler, self.arm9, self.arm7, self.output,
                self.fake_runner(commands),
            )
        self.assertEqual(commands, [])
        self.assertEqual(note.read_text(encoding="utf-8"), "preserve")


if __name__ == "__main__":
    unittest.main(verbosity=2)
