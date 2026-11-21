#!/usr/bin/env python3
"""Synthetic tests for the local generated-bank symbol auditor."""

from __future__ import annotations

import json
from pathlib import Path
import importlib.util
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "ndsrecomp" / "audit_bank_symbols.py"
SPEC = importlib.util.spec_from_file_location(
    "oracis_audit_bank_symbols_test_target", SCRIPT
)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"Cannot load symbol auditor module: {SCRIPT}")
auditor = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = auditor
SPEC.loader.exec_module(auditor)

AuditError = auditor.AuditError
audit_bank_symbols = auditor.audit_bank_symbols
build_json_report = auditor.build_json_report
parse_nm_posix = auditor.parse_nm_posix
write_json_report = auditor.write_json_report


class AuditBankSymbolsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="oracis-symbol-audit-")
        self.root = Path(self.temp.name)
        self.arm9 = self.root / "arm9.o"
        self.arm7 = self.root / "arm7.o"
        self.runtime = self.root / "liboracis.a"
        for path in (self.arm9, self.arm7, self.runtime):
            path.write_bytes(b"synthetic input")

    def tearDown(self) -> None:
        self.temp.cleanup()

    def test_parses_posix_nm_undefined_and_defined_symbols(self) -> None:
        self.assertEqual(
            parse_nm_posix(
                "callee U         \nweak_ref w         \nignored T 0 4\n",
                undefined=True,
            ),
            {"callee", "weak_ref"},
        )
        self.assertEqual(
            parse_nm_posix(
                "callee T 00000000 4\nobject D 00000004 4\n",
                undefined=False,
            ),
            {"callee", "object"},
        )

    def test_resolves_cross_bank_and_runtime_symbols_and_allowlists_platform(self) -> None:
        outputs = {
            ("-u", "arm9.o"):
                "arm7_service U\nmemcpy U\nshared_helper U\n",
            ("-u", "arm7.o"):
                "arm9_service U\nshared_helper U\n",
            ("--defined-only", "arm9.o"):
                "arm9_service T 0 4\n",
            ("--defined-only", "arm7.o"):
                "arm7_service T 0 4\n",
            ("--defined-only", "liboracis.a"):
                "shared_helper T 0 4\n",
        }

        def runner(command, **kwargs):
            mode = "-u" if "-u" in command else "--defined-only"
            key = (mode, Path(command[-1]).name)
            return subprocess.CompletedProcess(command, 0, outputs[key], "")

        allowlist = self.root / "symbols.allow"
        allowlist.write_text(
            "# platform C library\nmemcpy\n", encoding="utf-8"
        )
        report = audit_bank_symbols(
            [self.arm9, self.arm7],
            [self.runtime],
            nm="llvm-nm",
            allowlist=allowlist,
            runner=runner,
        )
        self.assertEqual(report["undefined_symbols"], {
            "arm7_service", "arm9_service", "memcpy", "shared_helper"
        })
        self.assertEqual(report["resolved_symbols"], {
            "arm7_service", "arm9_service", "shared_helper"
        })
        self.assertEqual(report["allowed_symbols"], {"memcpy"})
        self.assertEqual(report["unresolved_symbols"], set())
        self.assertEqual(report["providers_by_symbol"]["arm7_service"], {self.arm7})
        self.assertEqual(report["providers_by_symbol"]["shared_helper"], {self.runtime})

        structured = build_json_report(report)
        entries = {
            entry["symbol"]: entry for entry in structured["symbol_resolution"]
        }
        self.assertEqual(structured["schema"], "oracis-bank-symbol-audit-v1")
        self.assertEqual(structured["status"], "passed")
        self.assertEqual(
            entries["shared_helper"]["provided_by"], [str(self.runtime)]
        )
        self.assertEqual(entries["memcpy"]["status"], "allowlisted")

    def test_writes_complete_json_report_to_requested_local_path(self) -> None:
        def runner(command, **kwargs):
            if "-u" in command:
                output = "missing_nitro_service U\n"
            else:
                output = "runtime_dispatch T 0 8\n"
            return subprocess.CompletedProcess(command, 0, output, "")

        report = audit_bank_symbols(
            [self.arm9], [self.runtime], nm="nm", runner=runner
        )
        destination = self.root / "reports" / "symbols.json"
        write_json_report(destination, report)

        decoded = json.loads(destination.read_text(encoding="utf-8"))
        self.assertEqual(decoded["status"], "failed")
        self.assertEqual(decoded["summary"]["unresolved_symbols"], 1)
        self.assertEqual(
            decoded["symbol_resolution"][0]["symbol"], "missing_nitro_service"
        )
        self.assertEqual(
            decoded["symbol_resolution"][0]["status"], "unresolved"
        )

    def test_reports_unresolved_symbol(self) -> None:
        def runner(command, **kwargs):
            if "-u" in command:
                output = "missing_nitro_service U\n"
            else:
                output = "runtime_dispatch T 0 8\n"
            return subprocess.CompletedProcess(command, 0, output, "")

        report = audit_bank_symbols(
            [self.arm9], [self.runtime], nm="nm", runner=runner
        )
        self.assertEqual(
            report["unresolved_symbols"], {"missing_nitro_service"}
        )

    def test_rejects_missing_inputs_before_invoking_nm(self) -> None:
        called = False

        def runner(command, **kwargs):
            nonlocal called
            called = True
            return subprocess.CompletedProcess(command, 0, "", "")

        with self.assertRaisesRegex(AuditError, "do not exist"):
            audit_bank_symbols(
                [self.arm9], [self.root / "missing.a"], nm="nm", runner=runner
            )
        self.assertFalse(called)

    def test_reports_nm_failure_with_file_context(self) -> None:
        def runner(command, **kwargs):
            return subprocess.CompletedProcess(command, 1, "", "bad object")

        with self.assertRaisesRegex(AuditError, "arm9.o.*bad object"):
            audit_bank_symbols(
                [self.arm9], [self.runtime], nm="nm", runner=runner
            )


if __name__ == "__main__":
    unittest.main(verbosity=2)
