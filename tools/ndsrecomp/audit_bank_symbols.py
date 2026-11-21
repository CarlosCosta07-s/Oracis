#!/usr/bin/env python3
"""Audit undefined symbols in locally generated Oracis NDSRecomp bank objects.

No ROM, extracted program, or generated bank is read by CI. This tool is for
local developer validation after compiling the user's locally emitted C banks.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from typing import Callable, Sequence


class AuditError(RuntimeError):
    """Raised when nm cannot inspect an input or its output is unusable."""


def parse_nm_posix(output: str, *, undefined: bool) -> set[str]:
    """Parse GNU/LLVM nm POSIX-format symbol rows (one input file per call)."""
    symbols: set[str] = set()
    for line in output.splitlines():
        fields = line.split()
        if len(fields) < 2:
            continue

        symbol, symbol_type = fields[0], fields[1]
        if undefined:
            if symbol_type in {"U", "w", "v"}:
                symbols.add(symbol)
        elif (
            len(symbol_type) == 1
            and symbol_type.isalpha()
            and symbol_type not in {"U", "u", "w", "v"}
        ):
            symbols.add(symbol)
    return symbols


def read_allowlist(path: Path | None) -> set[str]:
    if path is None:
        return set()
    if not path.is_file():
        raise AuditError(f"allowlist file does not exist: {path}")

    allowed: set[str] = set()
    for line_number, line in enumerate(
        path.read_text(encoding="utf-8").splitlines(), start=1
    ):
        symbol = line.split("#", 1)[0].strip()
        if not symbol:
            continue
        if any(character.isspace() for character in symbol):
            raise AuditError(
                f"invalid symbol on allowlist line {line_number}: {symbol!r}"
            )
        allowed.add(symbol)
    return allowed


def inspect_symbols(
    nm: str,
    path: Path,
    *,
    undefined: bool,
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> set[str]:
    if not path.is_file():
        raise AuditError(f"input file does not exist: {path}")

    arguments = [nm, "-P"]
    if undefined:
        arguments.append("-u")
    else:
        arguments.extend(["-g", "--defined-only"])
    arguments.append(str(path))

    try:
        completed = runner(
            arguments,
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except OSError as exc:
        raise AuditError(f"could not execute nm ({nm}): {exc}") from exc

    if completed.returncode != 0:
        detail = (completed.stderr or completed.stdout or "").strip()
        raise AuditError(
            f"nm failed for {path} with exit code {completed.returncode}"
            + (f": {detail}" if detail else "")
        )
    return parse_nm_posix(completed.stdout, undefined=undefined)


def audit_bank_symbols(
    bank_objects: Sequence[Path],
    runtime_inputs: Sequence[Path],
    *,
    nm: str,
    allowlist: Path | None = None,
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> dict[str, object]:
    if not bank_objects:
        raise AuditError("at least one --banks object is required")
    if not runtime_inputs:
        raise AuditError("at least one --runtime object or archive is required")

    missing_files = [path for path in (*bank_objects, *runtime_inputs) if not path.is_file()]
    if missing_files:
        raise AuditError(
            "input file(s) do not exist: " + ", ".join(str(path) for path in missing_files)
        )

    undefined_by_bank = {
        path: inspect_symbols(nm, path, undefined=True, runner=runner)
        for path in bank_objects
    }
    defined_by_provider = {
        path: inspect_symbols(nm, path, undefined=False, runner=runner)
        for path in (*bank_objects, *runtime_inputs)
    }

    undefined_symbols = set().union(*undefined_by_bank.values())
    defined_symbols = set().union(*defined_by_provider.values())
    allowed_symbols = read_allowlist(allowlist)
    resolved_symbols = undefined_symbols & defined_symbols
    allowed_used = (undefined_symbols - defined_symbols) & allowed_symbols
    unresolved_symbols = undefined_symbols - defined_symbols - allowed_symbols

    providers_by_symbol: dict[str, set[Path]] = {}
    for provider_path, symbols in defined_by_provider.items():
        for symbol in symbols:
            providers_by_symbol.setdefault(symbol, set()).add(provider_path)

    return {
        "undefined_by_bank": undefined_by_bank,
        "defined_by_provider": defined_by_provider,
        "providers_by_symbol": providers_by_symbol,
        "undefined_symbols": undefined_symbols,
        "defined_symbols": defined_symbols,
        "resolved_symbols": resolved_symbols,
        "allowed_symbols": allowed_used,
        "unresolved_symbols": unresolved_symbols,
    }


def build_json_report(report: dict[str, object]) -> dict[str, object]:
    """Build a deterministic report with exact provider paths for every symbol."""
    undefined_by_bank = report["undefined_by_bank"]
    providers_by_symbol = report["providers_by_symbol"]
    allowed_symbols = report["allowed_symbols"]
    unresolved_symbols = report["unresolved_symbols"]

    bank_entries = [
        {
            "object": str(path),
            "undefined_symbols": sorted(symbols),
        }
        for path, symbols in sorted(
            undefined_by_bank.items(), key=lambda item: str(item[0])
        )
    ]

    symbol_entries = []
    for symbol in sorted(report["undefined_symbols"]):
        providers = sorted(
            str(path) for path in providers_by_symbol.get(symbol, set())
        )
        if providers:
            status = "resolved"
        elif symbol in allowed_symbols:
            status = "allowlisted"
        else:
            status = "unresolved"
        symbol_entries.append(
            {
                "symbol": symbol,
                "status": status,
                "referenced_by": sorted(
                    str(path)
                    for path, symbols in undefined_by_bank.items()
                    if symbol in symbols
                ),
                "provided_by": providers,
            }
        )

    return {
        "schema": "oracis-bank-symbol-audit-v1",
        "status": "failed" if unresolved_symbols else "passed",
        "summary": {
            "undefined_symbols": len(report["undefined_symbols"]),
            "resolved_symbols": len(report["resolved_symbols"]),
            "allowlisted_symbols": len(allowed_symbols),
            "unresolved_symbols": len(unresolved_symbols),
            "symbols_with_multiple_providers": sum(
                1
                for entry in symbol_entries
                if len(entry["provided_by"]) > 1
            ),
        },
        "banks": bank_entries,
        "symbol_resolution": symbol_entries,
    }


def write_json_report(path: Path, report: dict[str, object]) -> None:
    """Write a complete JSON report atomically; never leave a partial report."""
    temporary_path: Path | None = None
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        descriptor, temporary_name = tempfile.mkstemp(
            prefix=f".{path.name}.",
            suffix=".tmp",
            dir=str(path.parent),
        )
        temporary_path = Path(temporary_name)
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            json.dump(build_json_report(report), stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary_path, path)
    except (OSError, TypeError, ValueError) as exc:
        if temporary_path is not None:
            try:
                temporary_path.unlink(missing_ok=True)
            except OSError:
                pass
        raise AuditError(f"could not write JSON report {path}: {exc}") from exc


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--banks",
        nargs="+",
        type=Path,
        required=True,
        help="locally compiled ARM9/ARM7 body and dispatch object files",
    )
    parser.add_argument(
        "--runtime",
        nargs="+",
        type=Path,
        required=True,
        help="Oracis runtime object files or static archives to inspect",
    )
    parser.add_argument(
        "--allowlist",
        type=Path,
        help="optional text file listing platform/C-runtime symbols, one per line",
    )
    parser.add_argument(
        "--report-json",
        type=Path,
        help="optional path for a deterministic machine-readable local audit report",
    )
    parser.add_argument(
        "--nm",
        default=shutil.which("llvm-nm") or shutil.which("nm") or "nm",
        help="GNU nm or LLVM nm executable (default: llvm-nm, then nm)",
    )
    args = parser.parse_args(argv)

    try:
        report = audit_bank_symbols(
            args.banks,
            args.runtime,
            nm=args.nm,
            allowlist=args.allowlist,
        )
        if args.report_json is not None:
            write_json_report(args.report_json, report)
    except (AuditError, OSError, UnicodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    undefined = report["undefined_symbols"]
    resolved = report["resolved_symbols"]
    allowed = report["allowed_symbols"]
    unresolved = report["unresolved_symbols"]
    print(f"Generated-bank undefined symbols: {len(undefined)}")
    print(f"Resolved by generated banks/Oracis runtime: {len(resolved)}")
    print(f"Explicitly allowlisted platform symbols: {len(allowed)}")
    print(f"Unresolved symbols: {len(unresolved)}")
    if args.report_json is not None:
        print(f"JSON report: {args.report_json}")

    for symbol in sorted(unresolved):
        referring = sorted(
            path.name
            for path, symbols in report["undefined_by_bank"].items()
            if symbol in symbols
        )
        print(f"  {symbol}  [referenced by: {', '.join(referring)}]")

    if unresolved:
        print(
            "Audit failed. Implement missing Oracis ABI services or explicitly "
            "allowlist verified platform/runtime symbols.",
            file=sys.stderr,
        )
        return 1

    print("Generated-bank symbol audit passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
