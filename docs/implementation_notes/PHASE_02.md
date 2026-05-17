# Phase 02: Core Schema Loading and Table Registry

## Phase objective

Implement the current Phase 2 scope requested for this branch:

- CSV schema loader
- schema validation
- table handle registry
- no SQL insertion yet

This also covers the core type definitions from `docs/05_IMPLEMENTATION_PLAN.md` Phase 2 and the schema loading/validation work described in later plan sections, without introducing backend SQL behavior.

## Files changed

- `DataLoggerCore/include/DataLogger/DataLogger.h`
- `DataLoggerCore/include/DataLogger/DataLoggerConfig.h`
- `DataLoggerCore/include/DataLogger/Error.h`
- `DataLoggerCore/include/DataLogger/Schema.h`
- `DataLoggerCore/include/DataLogger/SchemaLoaderCsv.h`
- `DataLoggerCore/include/DataLogger/SchemaValidator.h`
- `DataLoggerCore/include/DataLogger/TableHandle.h`
- `DataLoggerCore/src/DataLogger.cpp`
- `DataLoggerCore/src/Schema.cpp`
- `DataLoggerCore/src/SchemaLoaderCsv.cpp`
- `DataLoggerCore/src/SchemaValidator.cpp`
- `DataLoggerCore/src/TableHandle.cpp`
- `DataLogger.vcxproj`
- `DataLogger.vcxproj.filters`
- `ExampleApp/main.cpp`

## Implementation summary

- Added core schema types: `DataType`, `ColumnSchema`, `ExpandedColumnSchema`, `TableSchema`, `SchemaRegistry`, `FieldValue`, and `DecodedRow`.
- Added `DataLoggerConfig`, `DataLoggerError`, and `TableHandle`.
- Added CSV directory loading for `.csv` schema files.
- Added CSV parsing with required header validation, optional `unit` and `description`, quoted fields, and comment-line skipping for lines beginning with `#`.
- Added schema validation for SQL identifier names, datatype sizes, nonzero lengths, duplicate expanded columns, reserved `timestamp_ms`, and SQL Server column/parameter count limits.
- Added array expansion into scalar SQL column metadata.
- Added minimal `DataLogger::initialize`, `registerTable`, `isValidTableHandle`, and schema lookup behavior.
- Updated the example app to load `ExampleApp\schemas` and register the `imu_data` table handle.
- Updated Visual Studio project metadata for the new files.

## Build/test commands run

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' DataLoggerSolution.sln /p:Configuration=Debug /p:Platform=x64
& 'C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' DataLoggerSolution.sln /p:Configuration=Release /p:Platform=x64
.\x64\Debug\DataLogger.exe
.\x64\Release\DataLogger.exe
rg -n "std::thread|mutex|async|future|condition_variable|json|nlohmann|rapidjson|sqlite|insertBatch|SQLExec|SQLBind|SQLAlloc|SQLPrepare" DataLoggerCore ExampleApp SqlServerBackend
```

## Build/test results

- Debug x64 build passed with 0 warnings and 0 errors.
- Release x64 build passed with 0 warnings and 0 errors.
- Debug executable exited with code 0.
- Release executable exited with code 0.
- Constraint scan found no async/concurrency, JSON/SQLite, SQL insertion, or direct ODBC call paths in the current Phase 2 implementation.

## Known limitations

- No binary struct decoding yet.
- No row buffering or flushing yet.
- No SQL Server table creation yet.
- No ODBC connection, diagnostics, prepared statements, parameter array binding, or transactions yet.
- No dedicated unit test project yet; validation is currently covered by build, source scan, and example smoke execution.

## Deviations from AGENTS.md or docs

- No deviation from `AGENTS.md`.
- The committed Phase 02 scope includes CSV loading and validation in addition to the core types listed under `docs/05_IMPLEMENTATION_PLAN.md` Phase 2 because the active user request defined Phase 2 as "CSV schema loader, schema validation, table handle registry, no SQL insertion yet."
