# Phase 08 Test Notes

## Phase scope

Phase 8 covers the example application and schema:

- `ExampleApp/main.cpp`
- `ExampleApp/schemas/imu_data.csv`

The example is expected to demonstrate:

- the packed `ImuData` struct with offsets matching the CSV schema;
- logger configuration and backend creation;
- schema loading from `ExampleApp/schemas`;
- table registration by name;
- row writes with caller-supplied timestamps;
- automatic batch flush at `batchSizeRows`;
- explicit flush before shutdown;
- smoke execution without SQL Server;
- real ODBC execution when `SQLLOGGER_CONNECTION_STRING` is provided.

## Test plan

### Automatic non-database checks

These checks do not require SQL Server and can be run automatically.

1. Build the solution in `Debug|x64`.
2. Build the solution in `Release|x64`.
3. Run the Debug executable without `SQLLOGGER_CONNECTION_STRING` so it uses the smoke backend.
4. Run the Release executable without `SQLLOGGER_CONNECTION_STRING` so it uses the smoke backend.
5. Scan for design-rule violations:
   - no threading or async primitives;
   - no JSON dependency;
   - no SQLite dependency.
6. Scan the SQL Server backend for required ODBC API usage:
   - `SQLDriverConnect`;
   - `SQLPrepare`;
   - `SQLBindParameter`;
   - `SQL_ATTR_PARAMSET_SIZE`;
   - `SQLExecute`;
   - `SQLEndTran`;
   - `SQLGetDiagRec`.

### Database-gated checks

These checks require explicit approval before each database step.

1. Confirm the SQL Server test connection string.
2. Confirm the target database is safe for example-table creation/recreation.
3. Run the example with `SQLLOGGER_CONNECTION_STRING` set.
4. Verify `[dbo].[imu_data]` exists with the expected columns.
5. Verify the timestamp index exists.
6. Verify the example inserted the expected 2 rows.

### Skipped checks

- Existing-table rename behavior is skipped for this phase because it has already been tested manually.

## Results

Test run date: 2026-05-17

### Automatic non-database checks

| Check | Result | Notes |
|---|---|---|
| Debug x64 build | Passed | MSBuild completed with 0 warnings and 0 errors. |
| Release x64 build | Passed | MSBuild completed with 0 warnings and 0 errors. |
| Debug smoke run | Passed | `x64\Debug\DataLogger.exe` exited with code 0 without `SQLLOGGER_CONNECTION_STRING`. |
| Release smoke run | Passed | `x64\Release\DataLogger.exe` exited with code 0 without `SQLLOGGER_CONNECTION_STRING`. |
| Design-rule scan | Passed | No matches for threading/async primitives, JSON libraries, or SQLite in `DataLoggerCore`, `ExampleApp`, or `SqlServerBackend`. |
| ODBC API scan | Passed | Required real ODBC calls are present in `SqlServerBackend/src/SqlServerBackend.cpp`. |

Build note:

- MSBuild tried to run `pwsh.exe` for the vcpkg applocal step. `pwsh.exe` was not found, then the build fell back to Windows PowerShell and still succeeded.

ODBC calls confirmed by scan:

- `SQLDriverConnectA`
- `SQLPrepareA`
- `SQLBindParameter`
- `SQL_ATTR_PARAMSET_SIZE`
- `SQLExecute`
- `SQLEndTran`
- `SQLGetDiagRecA`
- `SQLExecDirectA`

### Database-gated checks

Status: completed by manual user validation.

Validated checks:

1. Run the example with `SQLLOGGER_CONNECTION_STRING` set.
2. Verify `[dbo].[imu_data]` exists with expected columns.
3. Verify the timestamp index exists.
4. Verify the expected 2 rows were inserted.

Skipped:

- Existing-table rename retest is skipped because rename behavior was already tested manually.
