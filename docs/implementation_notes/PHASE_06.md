# Phase 06: DataLogger

## Phase objective

Implement `DataLogger` orchestration around the already implemented schema loader,
validator, and binary decoder.

The phase adds:

- backend injection through `IDBBackend`;
- initialization flow through schema load, validation, backend connect, table setup,
  and prepared statement setup;
- table registration;
- row decoding and per-table buffering;
- batch-size-triggered flush;
- manual per-table and all-table flush;
- failed-flush buffer retention.

## Files changed

- `DataLoggerCore/include/DataLogger/DataLogger.h`
- `DataLoggerCore/include/DataLogger/Error.h`
- `DataLoggerCore/include/DataLogger/IDBBackend.h`
- `DataLoggerCore/src/DataLogger.cpp`
- `DataLogger.vcxproj`
- `DataLogger.vcxproj.filters`
- `ExampleApp/main.cpp`
- `docs/implementation_notes/PHASE_06.md`

## Implementation summary

- Added `IDBBackend` as the core database abstraction.
- Added `BackendError` and `OdbcDiagnostic` data structures for backend error
  propagation.
- Added `DataLogger(std::unique_ptr<IDBBackend>)`.
- Updated `initialize()` to:
  - validate basic config;
  - load and validate schemas;
  - connect the injected backend;
  - initialize/recreate backend tables;
  - prepare backend insert statements;
  - initialize table buffers.
- Added `write()` to decode a caller-owned struct immediately and append the row to
  the table buffer.
- Added automatic flush when a table buffer reaches `batchSizeRows`.
- Added `flush(TableHandle)` and `flush()`; successful flushes clear buffers,
  failed flushes preserve buffers for retry.
- Added `shutdown()`; if its flush fails, runtime state is kept so the caller can
  inspect `lastError()` and retry.
- Updated the example app to use a local `SmokeTestBackend` until the real ODBC
  backend is implemented in Phase 7.

## Design notes

- `DataLoggerCore` still does not depend on `SqlServerBackend`.
- The example's `SmokeTestBackend` is only a Phase 6 smoke-test double. It does not
  replace the Phase 7 real ODBC backend.
- `flush()` currently stops at the first failed table and leaves later buffers
  untouched. This keeps `lastError()` unambiguous and preserves retry behavior.

## Build/test commands run

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' DataLoggerSolution.sln /p:Configuration=Debug /p:Platform=x64
.\x64\Debug\DataLogger.exe
& 'C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' DataLoggerSolution.sln /p:Configuration=Release /p:Platform=x64
.\x64\Release\DataLogger.exe
rg -n "std::thread|mutex|async|future|condition_variable|json|nlohmann|rapidjson|sqlite" DataLoggerCore ExampleApp SqlServerBackend
rg -n "SQLExec|SQLBind|SQLAlloc|SQLPrepare|SQL_ATTR_PARAMSET_SIZE" DataLoggerCore ExampleApp SqlServerBackend
```

## Build/test results

- Debug x64 build passed with 0 warnings and 0 errors.
- Release x64 build passed with 0 warnings and 0 errors.
- Debug executable exited with code 0.
- Release executable exited with code 0.
- Constraint scans found no async/concurrency, JSON/SQLite, or direct ODBC call
  paths in Phase 6 code.

## Known limitations

- The real SQL Server ODBC backend is not implemented yet.
- No real SQL connection, DDL, prepared statement, parameter-array binding, or
  transaction logic is present yet; those remain Phase 7 work.
- No dedicated unit test project exists yet; Phase 6 coverage is currently through
  the example smoke execution.

## Deviations from AGENTS.md or docs

- No deviation from `AGENTS.md`.
- The example uses a local backend test double temporarily because Phase 7 has not
  implemented `SqlServerOdbcBackend` yet.
