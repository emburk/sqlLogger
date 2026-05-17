#!/usr/bin/env python3
"""Generate a DataLogger CSV schema from an exported C/C++ struct layout CSV.

Objective:
    Convert scalar numeric fields from a compiler/exported struct layout into the
    CSV schema format consumed by DataLogger.

Input CSV format:
    field_name, offset, byteSize, lengthDim1, lengthDim2, classname

Output CSV format:
    column_name,offset,datatype,size,length
"""

import argparse
import csv
import re
import sys
from pathlib import Path


SCHEMA_HEADER = ["column_name", "offset", "datatype", "size", "length"]

TYPE_MAP = {
    "int8": ("int8", 1),
    "uint8": ("uint8", 1),
    "int16": ("int16", 2),
    "uint16": ("uint16", 2),
    "int32": ("int32", 4),
    "uint32": ("uint32", 4),
    "int64": ("int64", 8),
    "uint64": ("uint64", 8),
    "float": ("float", 4),
    "single": ("float", 4),
    "double": ("double", 8),
}


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """Parse source and destination paths from command line arguments."""
    parser = argparse.ArgumentParser(
        description="Generate a DataLogger schema CSV from structLayout.csv."
    )
    parser.add_argument(
        "input",
        help="Path to the source struct layout CSV.",
    )
    parser.add_argument(
        "output",
        help="Path to write the generated DataLogger schema CSV.",
    )
    return parser.parse_args(argv)


def normalized_row(row: dict[str, str]) -> dict[str, str]:
    """Trim header names and field values so exported CSV spacing is harmless."""
    # Some layout exporters include spaces after commas in the header row.
    return {
        (key or "").strip(): (value or "").strip()
        for key, value in row.items()
    }


def make_column_name(field_name: str) -> str:
    """Convert a dotted field path into a SQL-safe DataLogger column name."""
    # DataLogger SQL identifiers cannot contain dots or other punctuation.
    name = field_name.replace(".", "_")
    name = re.sub(r"[^A-Za-z0-9_]", "_", name)
    name = re.sub(r"_+", "_", name).strip("_")

    # Empty names and digit-starting names would fail schema validation later.
    if not name:
        raise ValueError(f"field_name '{field_name}' produced an empty column name")
    if not re.match(r"^[A-Za-z_]", name):
        name = "_" + name

    return name


def generate_schema_rows(input_path: Path) -> list[list[str]]:
    """Read structLayout.csv and return scalar DataLogger schema rows."""
    generated_rows: list[list[str]] = []
    seen_names: set[str] = set()

    with input_path.open(newline="") as source:
        reader = csv.DictReader(source)
        for line_number, raw_row in enumerate(reader, start=2):
            row = normalized_row(raw_row)
            class_name = row.get("classname", "")

            # Struct rows only describe nesting; DataLogger schema rows are scalar payload fields.
            if class_name == "struct":
                continue
            if class_name not in TYPE_MAP:
                raise ValueError(
                    f"line {line_number}: unsupported classname '{class_name}'"
                )

            # Column names must be unique after field path normalization.
            column_name = make_column_name(row.get("field_name", ""))
            if column_name in seen_names:
                raise ValueError(
                    f"line {line_number}: duplicate generated column '{column_name}'"
                )
            seen_names.add(column_name)

            # Length dimensions are informational for this export; every output row is scalar.
            try:
                offset = str(int(row.get("offset", "")))
            except ValueError as exc:
                raise ValueError(
                    f"line {line_number}: invalid offset '{row.get('offset', '')}'"
                ) from exc

            datatype, size = TYPE_MAP[class_name]
            generated_rows.append([column_name, offset, datatype, str(size), "1"])

    return generated_rows


def write_schema(output_path: Path, rows: list[list[str]]) -> None:
    """Write the generated rows with only the required DataLogger schema fields."""
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", newline="") as target:
        writer = csv.writer(target, lineterminator="\n")
        writer.writerow(SCHEMA_HEADER)
        writer.writerows(rows)


def main(argv: list[str] | None = None) -> int:
    """Run schema generation and report the output path and row count."""
    args = parse_args(argv)
    input_path = Path(args.input)
    output_path = Path(args.output)

    try:
        rows = generate_schema_rows(input_path)
        write_schema(output_path, rows)
    except (OSError, ValueError) as exc:
        print(f"schema generation failed: {exc}", file=sys.stderr)
        return 1

    print(f"generated {len(rows)} schema rows: {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
