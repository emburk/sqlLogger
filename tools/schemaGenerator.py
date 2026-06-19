#!/usr/bin/env python3
r"""Generate a DataLogger CSV schema from an exported C/C++ struct layout CSV.

Objective:
    Convert numeric fields from a compiler/exported struct layout into one or
    more CSV schema files consumed by DataLogger.

Input CSV format:
    field_name, offset, byteSize, lengthDim1, lengthDim2, classname
    For struct arrays, byteSize is the total array byte span.

Output CSV format:
    column_name,offset,datatype,size,length

Usage example:
    python tools\schemaGenerator.py examples\ExampleApp\local\structLayout.csv examples\ExampleApp\local\schemas\ao_main.csv --mode 3

Modes:
    0: Default. Expand every element into a scalar output row, length=1.
    1: Preserve array rows by writing length=lengthDim1*lengthDim2.
    2: Preserve array rows, split output files at max 1000 expanded elements.
    3: Group by top-level struct, split each output file at max 1000 elements.
"""

import argparse
import csv
import re
import sys
from dataclasses import dataclass
from pathlib import Path


SCHEMA_HEADER = ["column_name", "offset", "datatype", "size", "length"]
MAX_ELEMENTS_PER_SCHEMA = 1000

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


@dataclass(frozen=True)
class SchemaRow:
    """Store one DataLogger schema row before it is written to CSV."""
    column_name: str
    offset: int
    datatype: str
    size: int
    length: int
    struct_group: str


@dataclass(frozen=True)
class LayoutRow:
    """Store one normalized source layout row with parsed numeric fields."""
    field_name: str
    offset: int
    byte_size: int
    length: int
    class_name: str
    line_number: int


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
    parser.add_argument(
        "--mode",
        type=int,
        choices=[0, 1, 2, 3],
        default=0,
        help="Output mode: 0 scalar, 1 length, 2 split by count, 3 split by struct.",
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


def make_struct_group(field_name: str) -> str:
    """Return the top-level struct path used by mode 3 grouping."""
    parts = field_name.split(".")
    if len(parts) >= 2:
        return make_column_name(".".join(parts[:2]))

    return make_column_name(field_name)


def parse_positive_int(row: dict[str, str], field_name: str, line_number: int) -> int:
    """Parse a required positive integer field from a normalized CSV row."""
    try:
        value = int(row.get(field_name, ""))
    except ValueError as exc:
        raise ValueError(
            f"line {line_number}: invalid {field_name} '{row.get(field_name, '')}'"
        ) from exc

    if value <= 0:
        raise ValueError(
            f"line {line_number}: {field_name} must be positive, got {value}"
        )

    return value


def parse_required_int(row: dict[str, str], field_name: str, line_number: int) -> int:
    """Parse a required integer field from a normalized CSV row."""
    try:
        return int(row.get(field_name, ""))
    except ValueError as exc:
        raise ValueError(
            f"line {line_number}: invalid {field_name} '{row.get(field_name, '')}'"
        ) from exc


def parse_layout_rows(input_path: Path) -> list[LayoutRow]:
    """Read structLayout.csv rows and parse the fields needed for expansion."""
    layout_rows: list[LayoutRow] = []

    with input_path.open(newline="") as source:
        reader = csv.DictReader(source)
        for line_number, raw_row in enumerate(reader, start=2):
            row = normalized_row(raw_row)
            length_dim1 = parse_positive_int(row, "lengthDim1", line_number)
            length_dim2 = parse_positive_int(row, "lengthDim2", line_number)
            layout_rows.append(
                LayoutRow(
                    field_name=row.get("field_name", ""),
                    offset=parse_required_int(row, "offset", line_number),
                    byte_size=parse_positive_int(row, "byteSize", line_number),
                    length=length_dim1 * length_dim2,
                    class_name=row.get("classname", ""),
                    line_number=line_number,
                )
            )

    return layout_rows


def indexed_field_name(field_name: str, indexed_structs: dict[str, int]) -> str:
    """Insert struct-array indexes into a dotted source field path."""
    output_parts: list[str] = []
    current_path: list[str] = []

    for part in field_name.split("."):
        current_path.append(part)
        output_parts.append(part)

        path = ".".join(current_path)
        if path in indexed_structs:
            output_parts.append(str(indexed_structs[path]))

    return ".".join(output_parts)


def nearest_struct_parent(row: LayoutRow, struct_names: set[str]) -> str | None:
    """Find the nearest declared struct parent for one source layout row."""
    parts = row.field_name.split(".")
    for count in range(len(parts) - 1, 0, -1):
        candidate = ".".join(parts[:count])
        if candidate in struct_names:
            return candidate

    return None


def struct_element_stride(row: LayoutRow) -> int:
    """Return the per-element byte stride for a struct layout row."""
    if row.length == 1:
        return row.byte_size

    if row.byte_size % row.length != 0:
        raise ValueError(
            f"line {row.line_number}: struct byteSize {row.byte_size} "
            f"must divide evenly by length {row.length}"
        )

    return row.byte_size // row.length


def build_layout_tree(rows: list[LayoutRow]) -> tuple[list[LayoutRow], dict[str, list[LayoutRow]]]:
    """Build a source-order tree from dotted field paths and struct rows."""
    struct_names = {
        row.field_name
        for row in rows
        if row.class_name == "struct"
    }
    roots: list[LayoutRow] = []
    children: dict[str, list[LayoutRow]] = {}

    for row in rows:
        parent = nearest_struct_parent(row, struct_names)
        if parent is None:
            roots.append(row)
        else:
            children.setdefault(parent, []).append(row)

    return roots, children


def append_schema_rows_from_layout(
    row: LayoutRow,
    children: dict[str, list[LayoutRow]],
    indexed_structs: dict[str, int],
    offset_delta: int,
    generated_rows: list[SchemaRow],
    seen_names: set[str],
) -> None:
    """Append schema rows by recursively walking expanded struct-array elements."""
    if row.class_name == "struct":
        if row.length > 1:
            stride = struct_element_stride(row)
            for index in range(row.length):
                next_indexed_structs = dict(indexed_structs)
                next_indexed_structs[row.field_name] = index
                next_offset_delta = offset_delta + index * stride
                for child in children.get(row.field_name, []):
                    append_schema_rows_from_layout(
                        child,
                        children,
                        next_indexed_structs,
                        next_offset_delta,
                        generated_rows,
                        seen_names,
                    )
            return

        for child in children.get(row.field_name, []):
            append_schema_rows_from_layout(
                child,
                children,
                indexed_structs,
                offset_delta,
                generated_rows,
                seen_names,
            )
        return

    if row.class_name not in TYPE_MAP:
        raise ValueError(
            f"line {row.line_number}: unsupported classname '{row.class_name}'"
        )

    datatype, size = TYPE_MAP[row.class_name]
    if row.byte_size != size * row.length:
        print(
            f"warning: line {row.line_number}: byteSize {row.byte_size} "
            f"differs from datatype size {size} * length {row.length}.",
            file=sys.stderr,
        )

    column_name = make_column_name(indexed_field_name(row.field_name, indexed_structs))
    if column_name in seen_names:
        raise ValueError(
            f"line {row.line_number}: duplicate generated column '{column_name}'"
        )
    seen_names.add(column_name)
    generated_rows.append(
        SchemaRow(
            column_name=column_name,
            offset=row.offset + offset_delta,
            datatype=datatype,
            size=size,
            length=row.length,
            struct_group=make_struct_group(row.field_name),
        )
    )


def element_count(rows: list[SchemaRow]) -> int:
    """Count expanded payload elements represented by schema rows."""
    return sum(row.length for row in rows)


def to_csv_row(row: SchemaRow) -> list[str]:
    """Convert an internal schema row to the exact DataLogger CSV fields."""
    return [
        row.column_name,
        str(row.offset),
        row.datatype,
        str(row.size),
        str(row.length),
    ]


def generate_schema_rows(input_path: Path) -> list[SchemaRow]:
    """Read structLayout.csv and return DataLogger schema rows with array lengths."""
    generated_rows: list[SchemaRow] = []
    seen_names: set[str] = set()
    layout_rows = parse_layout_rows(input_path)
    roots, children = build_layout_tree(layout_rows)

    for root in roots:
        append_schema_rows_from_layout(root, children, {}, 0, generated_rows, seen_names)

    return generated_rows


def expand_to_scalar_rows(rows: list[SchemaRow]) -> list[SchemaRow]:
    """Expand array rows into one scalar row per element with adjusted offsets."""
    scalar_rows: list[SchemaRow] = []

    for row in rows:
        # Scalar rows keep the base name; arrays get the same suffixes DataLogger would create.
        if row.length == 1:
            scalar_rows.append(row)
            continue

        for index in range(row.length):
            scalar_rows.append(
                SchemaRow(
                    column_name=f"{row.column_name}_{index}",
                    offset=row.offset + index * row.size,
                    datatype=row.datatype,
                    size=row.size,
                    length=1,
                    struct_group=row.struct_group,
                )
            )

    return scalar_rows


def expanded_column_names(rows: list[SchemaRow]) -> list[str]:
    """Return the final SQL payload names that DataLogger will create."""
    names: list[str] = []
    for row in rows:
        if row.length == 1:
            names.append(row.column_name)
        else:
            names.extend(f"{row.column_name}_{index}" for index in range(row.length))

    return names


def validate_expanded_names(rows: list[SchemaRow], context: str) -> None:
    """Reject duplicate expanded column names before writing a schema file."""
    seen_names: set[str] = set()
    for name in expanded_column_names(rows):
        if name in seen_names:
            raise ValueError(f"{context}: duplicate expanded column '{name}'")
        seen_names.add(name)


def split_large_row(row: SchemaRow) -> list[SchemaRow]:
    """Split one over-limit array row into smaller array rows if needed."""
    if row.length <= MAX_ELEMENTS_PER_SCHEMA:
        return [row]

    parts: list[SchemaRow] = []
    remaining = row.length
    start_index = 0
    part_index = 1

    while remaining > 0:
        # Keep each generated fragment under the hard-coded SQL column limit.
        part_length = min(remaining, MAX_ELEMENTS_PER_SCHEMA)
        parts.append(
            SchemaRow(
                column_name=f"{row.column_name}_part{part_index}",
                offset=row.offset + start_index * row.size,
                datatype=row.datatype,
                size=row.size,
                length=part_length,
                struct_group=row.struct_group,
            )
        )
        remaining -= part_length
        start_index += part_length
        part_index += 1

    return parts


def split_rows_by_count(rows: list[SchemaRow]) -> list[list[SchemaRow]]:
    """Split schema rows into chunks with at most 1000 expanded elements."""
    chunks: list[list[SchemaRow]] = []
    current_rows: list[SchemaRow] = []
    current_count = 0

    for row in rows:
        for part in split_large_row(row):
            # Start a new output file before this row would exceed the element limit.
            if current_rows and current_count + part.length > MAX_ELEMENTS_PER_SCHEMA:
                chunks.append(current_rows)
                current_rows = []
                current_count = 0

            current_rows.append(part)
            current_count += part.length

    if current_rows:
        chunks.append(current_rows)

    return chunks


def grouped_by_struct(rows: list[SchemaRow]) -> list[tuple[str, list[SchemaRow]]]:
    """Group rows by top-level struct while preserving first-seen order."""
    grouped_rows: dict[str, list[SchemaRow]] = {}
    group_order: list[str] = []

    for row in rows:
        # Keep output deterministic and aligned with the source layout order.
        if row.struct_group not in grouped_rows:
            grouped_rows[row.struct_group] = []
            group_order.append(row.struct_group)
        grouped_rows[row.struct_group].append(row)

    return [(group_name, grouped_rows[group_name]) for group_name in group_order]


def numbered_output_path(output_path: Path, index: int) -> Path:
    """Build a numbered output path by appending _1, _2, and so on."""
    return output_path.with_name(f"{output_path.stem}_{index}{output_path.suffix}")


def write_schema(output_path: Path, rows: list[SchemaRow]) -> None:
    """Write the generated rows with only the required DataLogger schema fields."""
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", newline="") as target:
        writer = csv.writer(target, lineterminator="\n")
        writer.writerow(SCHEMA_HEADER)
        writer.writerows(to_csv_row(row) for row in rows)


def warn_if_over_limit(rows: list[SchemaRow]) -> None:
    """Print a warning when one output schema exceeds the hard-coded element limit."""
    total_elements = element_count(rows)
    if total_elements > MAX_ELEMENTS_PER_SCHEMA:
        print(
            f"warning: schema has {total_elements} expanded elements; "
            f"limit target is {MAX_ELEMENTS_PER_SCHEMA}.",
            file=sys.stderr,
        )


def write_single_output(output_path: Path, rows: list[SchemaRow]) -> None:
    """Validate and write one schema file, then print its row and element counts."""
    validate_expanded_names(rows, str(output_path))
    write_schema(output_path, rows)
    warn_if_over_limit(rows)
    print(
        f"generated {len(rows)} schema rows, "
        f"{element_count(rows)} expanded elements: {output_path}"
    )


def write_split_outputs(output_path: Path, chunks: list[list[SchemaRow]]) -> None:
    """Write numbered split schema files and print filename plus element count."""
    for index, rows in enumerate(chunks, start=1):
        path = numbered_output_path(output_path, index)
        validate_expanded_names(rows, str(path))
        write_schema(path, rows)
        print(f"{path}: {element_count(rows)} elements")


def write_struct_outputs(output_path: Path, rows: list[SchemaRow]) -> None:
    """Write numbered schema files grouped by top-level struct and size limit."""
    output_index = 1
    for group_name, group_rows in grouped_by_struct(rows):
        for chunk in split_rows_by_count(group_rows):
            path = numbered_output_path(output_path, output_index)
            validate_expanded_names(chunk, str(path))
            write_schema(path, chunk)
            print(f"{path}: {element_count(chunk)} elements ({group_name})")
            output_index += 1


def main(argv: list[str] | None = None) -> int:
    """Run schema generation and report the output path and row count."""
    args = parse_args(argv)
    input_path = Path(args.input)
    output_path = Path(args.output)

    try:
        rows = generate_schema_rows(input_path)
        if args.mode == 0:
            write_single_output(output_path, expand_to_scalar_rows(rows))
        elif args.mode == 1:
            write_single_output(output_path, rows)
        elif args.mode == 2:
            write_split_outputs(output_path, split_rows_by_count(rows))
        elif args.mode == 3:
            write_struct_outputs(output_path, rows)
    except (OSError, ValueError) as exc:
        print(f"schema generation failed: {exc}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
