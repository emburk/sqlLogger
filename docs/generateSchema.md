# Generate DataLogger Schema

Use `tools/schemaGenerator.py` to generate DataLogger CSV schema files from an
exported C/C++ struct layout CSV.

## Example

```powershell
python tools\schemaGenerator.py examples\ExampleApp\local\structLayout.csv examples\ExampleApp\local\schemas\ao_main.csv --mode 3
```

## Inputs

The source layout CSV must use:

```csv
field_name, offset, byteSize, lengthDim1, lengthDim2, classname
```

`classname` may be `struct` or one of the supported numeric DataLogger types.
Struct rows describe nesting and byte span. Numeric rows become DataLogger
schema rows.

## Struct arrays

When a `struct` row has `lengthDim1 * lengthDim2 > 1`, the generator expands
numeric descendants once per struct element and inserts the struct index into
the generated column name. The per-element stride is derived from
`byteSize / (lengthDim1 * lengthDim2)`.

For example:

```csv
a.b, 1512, 800, 20, 1, struct
a.b.c, 1512, 8, 1, 1, double
a.b.d, 1520, 8, 1, 1, double
a.e, 2312, 4, 2, 1, uint16
```

In scalar mode, this produces names like:

```text
a_b_0_c
a_b_0_d
...
a_b_19_c
a_b_19_d
a_e_0
a_e_1
```

Nested struct arrays are expanded recursively. See
`tools/examples/struct_array_layout.csv` and
`tools/examples/struct_array_schema.csv` for a representative fixture.

## Modes

- `--mode 0`: expand every numeric array into scalar rows with `length=1`.
- `--mode 1`: preserve numeric array rows with `length=lengthDim1*lengthDim2`.
- `--mode 2`: preserve numeric array rows and split files at 1000 expanded
  elements.
- `--mode 3`: group by top-level struct and split each file at 1000 expanded
  elements.
