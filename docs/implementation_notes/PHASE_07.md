# Phase 07: SQL Server ODBC Backend

## Phase objective

Implement the concrete SQL Server backend behind `IDBBackend` using real ODBC API
calls.

The phase adds:

- ODBC environment, connection, and statement handle ownership;
- SQL Server connection through `SQLDriverConnect`;
- ODBC diagnostics through `SQLGetDiagRec`;
- existing-table drop or timestamp-suffixed rename policy;
- generated `CREATE TABLE` statements and timestamp indexes;
- prepared insert statements per table;
- column-wise ODBC parameter array binding;
- transaction-protected batch execution;
- `uint64` binding through `SQL_NUMERIC_STRUCT` for `DECIMAL(20,0)`.

## Files changed

- `SqlServerBackend/include/SqlServerBackend/SqlServerOdbcBackend.h`
- `SqlServerBackend/src/SqlServerBackend.cpp`
- `DataLogger.vcxproj`
- `DataLogger.vcxproj.filters`
- `ExampleApp/main.cpp`
- `docs/implementation_notes/PHASE_07.md`

## Implementation summary

- Added `SqlServerOdbcBackend` as the concrete `IDBBackend`.
- Kept ODBC handle details hidden behind a private implementation type.
- Added RAII handle wrappers for ODBC environment, connection, and statement
  handles.
- Added SQL generation for table creation, timestamp indexing, and prepared
  inserts.
- Added existing-table policy handling:
  - `Drop` drops the existing table before recreation;
  - `RenameWithTimestampSuffix` renames existing tables using
    `YYYYMMDD-hhmmss`, with numeric suffix collision handling.
- Added ODBC column-wise parameter arrays and status/processed row tracking.
- Added one transaction per batch using manual autocommit control and
  `SQLEndTran`.
- Added an opt-in example path: when `SQLLOGGER_CONNECTION_STRING` is set, the
  example uses the real ODBC backend; otherwise it keeps the local smoke backend.

## Build/test commands run

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' DataLoggerSolution.sln /p:Configuration=Release /p:Platform=x64 /m:1
& 'C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' DataLoggerSolution.sln /p:Configuration=Debug /p:Platform=x64 /m:1
.\x64\Release\DataLogger.exe
.\x64\Debug\DataLogger.exe
rg -n "std::thread|mutex|async|future|condition_variable|json|nlohmann|rapidjson|sqlite" DataLoggerCore ExampleApp SqlServerBackend
rg -n "SQLExecDirect|SQLPrepare|SQLBindParameter|SQLExecute|SQL_ATTR_PARAMSET_SIZE|SQLEndTran|SQLDriverConnect|SQLGetDiagRec" SqlServerBackend
```

## Build/test results

- Release x64 build passed with 0 warnings and 0 errors.
- Debug x64 build passed with 0 warnings and 0 errors.
- Release executable exited with code 0 using the smoke backend.
- Debug executable exited with code 0 using the smoke backend.
- Live SQL Server integration test passed using ODBC Driver 18 with direct TCP
  connection to `127.0.0.1,1433`.
- The integration test created/recreated `imu_data` in `SensorDataDB` and inserted
  2 rows as expected.
- Constraint scan found no async/concurrency, JSON, or SQLite usage.
- ODBC call scan confirmed real ODBC connection, diagnostics, prepared
  statements, parameter binding, parameter set sizing, execution, and
  transaction calls are present.

## Integration test connection

The live SQL Server integration run used this connection-string shape:

```powershell
$env:SQLLOGGER_CONNECTION_STRING = 'Driver={ODBC Driver 18 for SQL Server};Server=tcp:127.0.0.1,1433;Database=SensorDataDB;Uid=emburk;Pwd=<password>;Encrypt=no;TrustServerCertificate=yes;'
.\x64\Debug\DataLogger.exe
```

The example creates/recreates the `imu_data` table from
`ExampleApp/schemas/imu_data.csv`, inserts two rows through parameter-array
binding, and flushes inside a transaction.

## Known limitations

- The example app still acts as the smoke/integration harness; there is not yet a
  dedicated automated integration test project.

## Deviations from AGENTS.md or docs

- No deviation from `AGENTS.md`.
- The implementation uses real ODBC and parameter-array batching.
