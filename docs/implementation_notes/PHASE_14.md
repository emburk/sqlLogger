# Phase 14: Schema Generator Struct Array Expansion

## Phase objective

Update `tools/schemaGenerator.py` so exported struct-array layout rows produce
DataLogger schema fields for every numeric descendant of every struct element.

The change is limited to the schema-generation helper. It does not change the
runtime DataLogger CSV contract, SQL schema loading, decoding, ODBC backend,
threading model, or batching architecture.

## Behavior

- Struct rows remain non-payload rows.
- Struct rows with `lengthDim1 * lengthDim2 > 1` now act as array contexts for
  descendant numeric fields.
- Generated column names insert the struct element index immediately after the
  array struct path.
- Offsets are adjusted by `elementIndex * (structByteSize / structLength)`.
- Nested struct arrays are expanded recursively by applying each ancestor array
  index combination.
- Numeric arrays keep the existing mode behavior. Mode 0 expands them into
  scalar rows, while modes 1, 2, and 3 preserve `length` until DataLogger
  expansion.

## Representative example

Added:

- `tools/examples/struct_array_layout.csv`
- `tools/examples/struct_array_schema.csv`

The fixture covers:

- an array of structs;
- numeric scalar descendants;
- a nested array of structs;
- a numeric array outside the struct array.

## Files changed

- `tools/schemaGenerator.py`
- `tools/examples/struct_array_layout.csv`
- `tools/examples/struct_array_schema.csv`
- `README.md`
- `docs/generateSchema.md`
- `docs/03_DESIGN_DECISIONS.md`
- `docs/04_PROJECT_SUMMARY.md`
- `docs/implementation_notes/PHASE_14.md`
- `docs/test_notes/PHASE_14.md`
