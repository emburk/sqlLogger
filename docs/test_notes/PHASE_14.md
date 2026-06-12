# Phase 14 Test Notes

## Phase scope

Phase 14 verifies `tools/schemaGenerator.py` support for struct-array expansion
from exported layout CSV files.

## Checks

1. Run the generator in scalar mode against
   `tools/examples/struct_array_layout.csv`.
2. Confirm the generated schema matches
   `tools/examples/struct_array_schema.csv`.
3. Confirm nested struct-array fields expand recursively.
4. Confirm normal numeric arrays still expand with `_0`, `_1`, ... suffixes in
   scalar mode.

## Results

Status: completed locally.

The representative fixture generated the expected schema, including:

- `a_b_0_c`
- `a_b_0_inner_0_x`
- `a_b_1_inner_1_x`
- `a_e_0`
- `a_e_1`

No SQL Server or ODBC verification was required because this phase only changes
the offline schema-generation helper.
