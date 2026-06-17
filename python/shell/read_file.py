#!/usr/bin/env python3

from __future__ import annotations

import argparse
import sys
from pathlib import Path


def positive_int(value: str) -> int:
    try:
        parsed = int(value, 10)
    except ValueError:
        raise argparse.ArgumentTypeError("must be a positive integer") from None
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return parsed


def nonnegative_int(value: str) -> int:
    try:
        parsed = int(value, 10)
    except ValueError:
        raise argparse.ArgumentTypeError("must be a non-negative integer") from None
    if parsed < 0:
        raise argparse.ArgumentTypeError("must be a non-negative integer")
    return parsed


def print_doc() -> None:
    print(
        "read_file prints a text file with 1-based line numbers.\n"
        "\n"
        "Usage:\n"
        "  read_file [--offset N] [--limit N] PATH\n"
        "  read_file --doc\n"
        "\n"
        "Options:\n"
        "  -o, --offset N   First 1-based line number to print. Default: 1\n"
        "  -l, --limit N    Maximum number of lines to print. Default: no limit\n"
        "  --doc            Print this usage text and exit.\n"
        "\n"
        "Output:\n"
        "  Each selected line is printed as '<line>: <text>'.\n"
        "\n"
        "Examples:\n"
        "  read_file src/main.cpp\n"
        "  read_file --offset 20 --limit 40 src/main.cpp"
    )


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="read_file",
        description="Print a file with line numbers",
    )
    parser.add_argument("path", nargs="?", help="File path")
    parser.add_argument(
        "-o",
        "--offset",
        type=positive_int,
        default=1,
        help="First 1-based line number to print",
    )
    parser.add_argument(
        "-l",
        "--limit",
        type=nonnegative_int,
        default=None,
        help="Maximum number of lines to print",
    )
    parser.add_argument("--doc", action="store_true", help="print usage and exit")
    return parser


def read_lines(path: Path, offset: int, limit: int | None) -> list[str]:
    lines: list[str] = []
    try:
        with path.open("r", encoding="utf-8", errors="surrogateescape") as input_file:
            for line_number, line in enumerate(input_file, 1):
                if line_number < offset:
                    continue
                if limit is not None and len(lines) >= limit:
                    break
                lines.append(line.rstrip("\n"))
    except OSError as e:
        print(f"readfile: cannot open {path}: {e.strerror}", file=sys.stderr)
        raise SystemExit(1) from None
    return lines


def main(argv: list[str] | None = None) -> int:
    parser = make_parser()
    args = parser.parse_args(argv)

    if args.doc:
        print_doc()
        return 0

    if not args.path:
        parser.error("the following arguments are required: path")

    lines = read_lines(Path(args.path), args.offset, args.limit)
    for index, line in enumerate(lines):
        print(f"{args.offset + index}: {line}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
