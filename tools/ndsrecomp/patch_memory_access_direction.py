#!/usr/bin/env python3
"""Patch pinned NDSRecomp C output with explicit memory-access direction.

The pinned code generator deliberately exposes only address, width and bus
sequence to runtime_mem_cycles(). Oracis needs the load/store direction for
ARM946E-S TCM load mode. This source-only adapter uses the bus operation emitted
immediately after each timing call; it does not change or rebuild upstream
compiler sources.

The adapter fails closed when it cannot associate a timing call with a
subsequent guest bus read/write.
"""

from __future__ import annotations

from bisect import bisect_right
import re

ADAPTER_IDENTITY = "oracis-memory-direction-v1"

_TIMING_CALL = re.compile(r"\bruntime_mem_cycles\s*\(")
_BUS_CALL = re.compile(r"\bbus_(read|write)_u(?:8|16|32)\s*\(")


def _mask_non_code(source: str) -> str:
    """Return a same-length view with comments and literals blanked out."""
    masked = list(source)
    state = "code"
    index = 0
    while index < len(source):
        char = source[index]
        next_char = source[index + 1] if index + 1 < len(source) else ""

        if state == "line_comment":
            if char == "\n":
                state = "code"
            else:
                masked[index] = " "
        elif state == "block_comment":
            if char == "*" and next_char == "/":
                masked[index] = masked[index + 1] = " "
                index += 1
                state = "code"
            elif char != "\n":
                masked[index] = " "
        elif state in ("string", "character"):
            if char == "\\":
                masked[index] = " "
                if index + 1 < len(source):
                    if source[index + 1] != "\n":
                        masked[index + 1] = " "
                    index += 1
            elif (state == "string" and char == '"') or (
                state == "character" and char == "'"
            ):
                masked[index] = " "
                state = "code"
            elif char != "\n":
                masked[index] = " "
        else:
            if char == "/" and next_char == "/":
                masked[index] = masked[index + 1] = " "
                index += 1
                state = "line_comment"
            elif char == "/" and next_char == "*":
                masked[index] = masked[index + 1] = " "
                index += 1
                state = "block_comment"
            elif char == '"':
                masked[index] = " "
                state = "string"
            elif char == "'":
                masked[index] = " "
                state = "character"
        index += 1
    return "".join(masked)


def _matching_close_parenthesis(masked: str, opening: int) -> int:
    depth = 0
    for index in range(opening, len(masked)):
        if masked[index] == "(":
            depth += 1
        elif masked[index] == ")":
            depth -= 1
            if depth == 0:
                return index
    raise ValueError("unterminated runtime_mem_cycles() call in generated C")


_CONTROL_FLOW_WORDS = frozenset({
    "if", "for", "while", "switch", "catch", "sizeof", "_Alignof",
})


def _is_function_body_open(masked: str, opening: int) -> bool:
    """Recognize a C function body opening brace.

    Generated banks are C99. A function declarator ends with a balanced
    parameter list immediately before its body. Control-flow blocks and
    initializer/aggregate braces are not treated as functions.
    """
    cursor = opening - 1
    while cursor >= 0 and masked[cursor].isspace():
        cursor -= 1
    if cursor < 0 or masked[cursor] != ")":
        return False

    depth = 0
    parameter_open = -1
    while cursor >= 0:
        char = masked[cursor]
        if char == ")":
            depth += 1
        elif char == "(":
            depth -= 1
            if depth == 0:
                parameter_open = cursor
                break
        cursor -= 1

    if parameter_open < 0:
        return False

    cursor = parameter_open - 1
    while cursor >= 0 and masked[cursor].isspace():
        cursor -= 1
    name_end = cursor + 1
    while cursor >= 0 and (masked[cursor].isalnum() or masked[cursor] == "_"):
        cursor -= 1
    name = masked[cursor + 1:name_end]
    return bool(name) and name not in _CONTROL_FLOW_WORDS


def _function_body_ranges(masked: str) -> list[tuple[int, int]]:
    """Return function-body content ranges, including functions in extern-C blocks.

    Nested control-flow braces remain inside their containing function range.
    """
    stack: list[tuple[int, bool]] = []
    ranges: list[tuple[int, int]] = []

    for index, char in enumerate(masked):
        if char == "{":
            stack.append((index, _is_function_body_open(masked, index)))
        elif char == "}":
            if not stack:
                raise ValueError("unbalanced closing brace in generated C")
            opening, is_function = stack.pop()
            if is_function:
                ranges.append((opening + 1, index))

    if stack:
        raise ValueError("unbalanced opening brace in generated C")

    ranges.sort()
    return ranges


def _function_scope_index(
    position: int,
    ranges: list[tuple[int, int]],
    starts: list[int],
) -> int | None:
    """Find the function body containing a source position."""
    candidate = bisect_right(starts, position) - 1
    if candidate < 0 or position >= ranges[candidate][1]:
        return None
    return candidate


def patch_generated_source(source: str) -> tuple[str, int, int]:
    """Rewrite timing calls to runtime_mem_cycles_ex(..., direction).

    Direction is inferred from the next guest bus operation emitted after the
    timing call, but only within the same C function. Adjacent timing calls
    (for example, the two words of LDRD or STRD) all map to the same following
    access and therefore keep one instruction's load/store direction.

    Cross-function matching is rejected: a missing bus operation in one bank
    function must never borrow an unrelated access from the next function.
    """
    masked = _mask_non_code(source)
    timing_calls = list(_TIMING_CALL.finditer(masked))
    if not timing_calls:
        return source, 0, 0

    function_ranges = _function_body_ranges(masked)
    function_starts = [start for start, _ in function_ranges]
    buses_by_function: list[list[re.Match[str]]] = [
        [] for _ in function_ranges
    ]

    for bus in _BUS_CALL.finditer(masked):
        scope_index = _function_scope_index(
            bus.start(), function_ranges, function_starts
        )
        if scope_index is not None:
            buses_by_function[scope_index].append(bus)

    replacements: list[tuple[int, int, str]] = []
    read_count = 0
    write_count = 0
    bus_cursors = [0 for _ in function_ranges]

    for timing in timing_calls:
        line = source.count("\n", 0, timing.start()) + 1
        scope_index = _function_scope_index(
            timing.start(), function_ranges, function_starts
        )
        if scope_index is None:
            raise ValueError(
                "cannot determine enclosing function for "
                "runtime_mem_cycles() at generated C line "
                f"{line}"
            )

        opening = masked.find("(", timing.start(), timing.end())
        closing = _matching_close_parenthesis(masked, opening)
        function_buses = buses_by_function[scope_index]
        bus_index = bus_cursors[scope_index]

        while (
            bus_index < len(function_buses)
            and function_buses[bus_index].start() <= closing
        ):
            bus_index += 1

        if bus_index >= len(function_buses):
            raise ValueError(
                "cannot determine memory direction for runtime_mem_cycles() "
                f"at generated C line {line}: no subsequent bus_read/write "
                "in enclosing function"
            )

        bus = function_buses[bus_index]
        bus_cursors[scope_index] = bus_index
        access_direction = bus.group(1)
        if access_direction == "read":
            direction_token = "ORACIS_MEMORY_ACCESS_READ"
            read_count += 1
        else:
            direction_token = "ORACIS_MEMORY_ACCESS_WRITE"
            write_count += 1

        replacements.append((
            timing.start(),
            closing,
            direction_token,
        ))

    result = source
    for call_start, closing, direction_token in reversed(replacements):
        result = (
            result[:closing]
            + ", "
            + direction_token
            + result[closing:]
        )
        result = (
            result[:call_start]
            + "runtime_mem_cycles_ex"
            + result[call_start + len("runtime_mem_cycles"):]
        )

    return result, read_count, write_count


if __name__ == "__main__":
    import argparse
    import sys
    from pathlib import Path

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, nargs="?")
    parser.add_argument("--identity", action="store_true")
    args = parser.parse_args()
    if args.identity:
        print(ADAPTER_IDENTITY)
        raise SystemExit(0)
    if args.source is None:
        parser.error("provide a generated C source path or --identity")
    original = args.source.read_text(encoding="utf-8")
    adapted, reads, writes = patch_generated_source(original)
    args.source.write_text(adapted, encoding="utf-8")
    print(f"{args.source}: read calls={reads}, write calls={writes}")
