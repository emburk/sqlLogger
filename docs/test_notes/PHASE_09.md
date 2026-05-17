# Phase 09 Test Notes

## Phase scope

Phase 9 covers the testing checklist from `docs/05_IMPLEMENTATION_PLAN.md`:

- schema parsing;
- schema expansion;
- duplicate-column rejection;
- binary decoding;
- SQL Server table creation;
- SQL Server insert verification;
- rollback/buffer-retention behavior;
- wide-schema validation.

## Test plan

### Automatic non-database checks

These checks do not require SQL Server and can be run automatically.

1. Add an explicit Phase 9 validation mode to the example executable.
2. Build the solution in `Debug|x64`.
3. Run `x64\Debug\DataLogger.exe --phase9-tests`.
4. Verify valid CSV parsing and quoted metadata support.
5. Verify scalar and array expansion order.
6. Verify duplicate expanded column rejection.
7. Verify binary decoding from a known packed struct.
8. Verify failed backend flush preserves buffered rows and allows retry.
9. Verify wide-schema validation fails clearly.

### Database-gated checks

These checks require explicit approval before each database step.

1. Confirm the SQL Server test connection string.
2. Confirm the target database is safe for example-table creation/recreation.
3. Run the example with `SQLLOGGER_CONNECTION_STRING` set.
4. Verify `[dbo].[imu_data]` exists with the expected columns.
5. Verify the timestamp index exists.
6. Verify the example inserted the expected 2 rows.

### Skipped checks

- Existing-table rename retest remains skipped because rename behavior was already tested manually.

## Results

Test run date: 2026-05-17

### Automatic non-database checks

| Check | Result | Notes |
|---|---|---|
| Add Phase 9 validation mode | Passed | Added opt-in `--phase9-tests` mode to the example executable without changing the single-project layout. |
| Debug x64 build | Passed | Final build completed with 0 warnings and 0 errors. |
| Debug Phase 9 run | Passed | `x64\Debug\DataLogger.exe --phase9-tests` printed `PHASE9_TESTS_PASSED` and exited with code 0. |
| Release x64 build | Passed | Final build completed with 0 warnings and 0 errors. |
| Release Phase 9 run | Passed | `x64\Release\DataLogger.exe --phase9-tests` printed `PHASE9_TESTS_PASSED` and exited with code 0. |
| Debug normal smoke run | Passed | `x64\Debug\DataLogger.exe` exited with code 0 without `SQLLOGGER_CONNECTION_STRING`. |
| Release normal smoke run | Passed | `x64\Release\DataLogger.exe` exited with code 0 without `SQLLOGGER_CONNECTION_STRING`. |

Coverage from `--phase9-tests`:

1. Valid CSV parsing with comment lines and quoted metadata.
2. Scalar and array expansion order.
3. Duplicate expanded column rejection.
4. Fixed-offset binary decoding with caller-supplied timestamp.
5. Flush failure buffer retention and retry with a local fake backend.
6. Wide-schema validation failure with split-schema guidance.

Build note:

- The first Debug build after adding the harness failed because `ExampleApp/main.cpp` used schema loader and validator APIs without including their headers directly. The includes were added, then Debug and Release builds passed cleanly.
- MSBuild still reports that `pwsh.exe` is not found for the vcpkg applocal step, then falls back to Windows PowerShell and succeeds.
- Later project cleanup moved this harness source to `ExampleApp/test.cpp`; the default `ExampleApp/main.cpp` is now a minimal real-ODBC example app.

### Database-gated checks

Status: completed by manual user validation.

Validated checks:

1. Run the example with `SQLLOGGER_CONNECTION_STRING` set.
2. Verify `[dbo].[imu_data]` exists with expected columns.
3. Verify the timestamp index exists.
4. Verify the expected 2 rows were inserted.

Skipped:

- Existing-table rename retest is skipped because rename behavior was already tested manually.
