#!/usr/bin/env python3
"""Safety and correctness tests for the generated-bank C splitter."""
from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "ndsrecomp" / "split_generated_bank.py"
SPEC = importlib.util.spec_from_file_location("oracis_split_generated_bank_test", SCRIPT)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"Cannot load generated-bank splitter: {SCRIPT}")
splitter = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = splitter
SPEC.loader.exec_module(splitter)


class SplitGeneratedBankTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="oracis-bank-split-")
        self.root = Path(self.temp.name)
        self.source = self.root / "test_bank.c"
        self.header = self.root / "test_bank.h"
        self.output = self.root / "output"
        self.output.mkdir()
        self.names = [f"bank_function_{index}" for index in range(5)]
        self.header.write_text(
            "".join(f"void {name}(void);\n" for name in self.names),
            encoding="utf-8",
        )
        bodies = []
        for index, name in enumerate(self.names):
            bodies.append(
                f"void {name}(void) {{\n"
                f'    const char *text = "brace }} {{";\n'
                f"    /* nested fake braces {{ }} */\n"
                f"    if ({index}u) {{ value += {index}u; }}\n"
                f"    (void)text;\n"
                f"}}\n"
            )
        self.source.write_text(
            '#include "runtime_arm.h"\n'
            + "".join(f"void {name}(void);\n" for name in self.names)
            + "\n" + "\n".join(bodies),
            encoding="utf-8",
        )

    def tearDown(self) -> None:
        self.temp.cleanup()

    def test_splits_all_functions_and_preserves_prototypes(self) -> None:
        metadata = splitter.split_generated_bank(
            self.source, self.header, self.output, "test_bank", 2
        )
        self.assertEqual(metadata["function_count"], 5)
        self.assertEqual(metadata["functions_per_shard"], 2)
        self.assertEqual(
            set(metadata["files"]),
            {"bank_part_0000.c", "bank_part_0001.c", "bank_part_0002.c"},
        )
        directory = self.output / "test_bank_shards"
        sources = [path.read_text(encoding="utf-8")
                   for path in sorted(directory.glob("*.c"))]
        self.assertEqual(len(sources), 3)
        self.assertEqual(
            sum(source.count("void bank_function_") for source in sources), 5
        )
        self.assertTrue(all('#include "test_bank.h"' in source for source in sources))
        self.assertTrue(all("brace } {" in source for source in sources))

    def test_rejects_missing_header_prototype(self) -> None:
        self.header.write_text("void bank_function_0(void);\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "missing function prototypes"):
            splitter.split_generated_bank(
                self.source, self.header, self.output, "test_bank", 2
            )

    def test_rejects_unbalanced_function_body(self) -> None:
        self.source.write_text("void bank_function_0(void) {\n", encoding="utf-8")
        self.header.write_text("void bank_function_0(void);\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "unterminated function"):
            splitter.split_generated_bank(
                self.source, self.header, self.output, "test_bank", 2
            )

    def test_rejects_file_scope_state_in_generated_bank(self) -> None:
        self.source.write_text(
            '#include "runtime_arm.h"\n'
            "uint32_t hidden_state;\n"
            "void bank_function_0(void);\n"
            "void bank_function_0(void) {}\n",
            encoding="utf-8",
        )
        self.header.write_text("void bank_function_0(void);\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "file-scope content before"):
            splitter.split_generated_bank(
                self.source, self.header, self.output, "test_bank", 2
            )

    def test_rejects_unrelated_content_in_existing_shard_directory(self) -> None:
        directory = self.output / "test_bank_shards"
        directory.mkdir()
        note = directory / "notes.txt"
        note.write_text("user data", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "unrelated or unsafe content"):
            splitter.split_generated_bank(
                self.source, self.header, self.output, "test_bank", 2
            )
        self.assertEqual(note.read_text(encoding="utf-8"), "user data")

    def test_replaces_previous_valid_shards_as_a_complete_set(self) -> None:
        metadata = splitter.split_generated_bank(
            self.source, self.header, self.output, "test_bank", 4
        )
        self.assertEqual(len(metadata["files"]), 2)
        metadata = splitter.split_generated_bank(
            self.source, self.header, self.output, "test_bank", 2
        )
        self.assertEqual(len(metadata["files"]), 3)
        self.assertEqual(
            len(list((self.output / "test_bank_shards").glob("*.c"))), 3
        )

    def test_rejects_zero_shard_size(self) -> None:
        with self.assertRaisesRegex(ValueError, "positive integer"):
            splitter.split_generated_bank(
                self.source, self.header, self.output, "test_bank", 0
            )


if __name__ == "__main__":
    unittest.main(verbosity=2)
