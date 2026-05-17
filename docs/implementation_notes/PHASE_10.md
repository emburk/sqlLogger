# Phase 10: Automatic Registration and Multi-Table Write

## Phase objective

Add convenience APIs for schema folders where every loaded CSV table maps to the
same source struct.

The phase adds:

- `autoRegisterTables()` to register every loaded schema table in deterministic
  schema order;
- `autoWrite(timestampMs, structPtr)` to write one timestamped struct row to all
  auto-registered tables;
- an AO example update that no longer hardcodes the split table-name list.

## Implementation plan

- Keep the existing manual `registerTable()` and `write()` APIs unchanged.
- Store auto-registered handles inside `DataLogger`.
- Reuse the existing per-table `write()` path from `autoWrite()` so decoding,
  buffering, batch-size flushing, and failure retention remain consistent.
- Return clear errors if automatic registration is used before initialization or
  automatic writing is used before registration.

## Design notes

- Automatic registration uses the schema registry, which already comes only from
  `.csv` files in `DataLoggerConfig::schemaDirectory`.
- The schema loader sorts CSV paths, so automatic handle order remains stable.
- `autoWrite()` does not add cross-table transactions. Each table keeps the
  existing per-table batch and rollback behavior.

## Files planned for change

- `DataLoggerCore/include/DataLogger/DataLogger.h`
- `DataLoggerCore/src/DataLogger.cpp`
- `ExampleApp/main.cpp`
- `README.md`
- `docs/01_REQUIREMENTS.md`
- `docs/02_ARCHITECTURE.md`
- `docs/03_DESIGN_DECISIONS.md`
- `docs/04_PROJECT_SUMMARY.md`
- `docs/05_IMPLEMENTATION_PLAN.md`
- `docs/implementation_notes/PHASE_10.md`

## Implementation summary

- Added `DataLogger::autoRegisterTables()` and an internal
  `autoRegisteredTables_` handle list.
- Added `DataLogger::autoWrite()` as a convenience wrapper over the existing
  per-table `write()` path.
- Updated the AO example to call `autoRegisterTables()` once after
  initialization and `autoWrite()` for each timestamped payload.
- Documented the new API in requirements, architecture, design decisions,
  project summary, README, and the implementation plan.

## Build/test commands run

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' DataLoggerSolution.sln /p:Configuration=Debug /p:Platform=x64 /m:1
& 'C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' DataLoggerSolution.sln /p:Configuration=Release /p:Platform=x64 /m:1
rg -n "std::thread|mutex|async|future|condition_variable|json|nlohmann|rapidjson|sqlite" DataLoggerCore ExampleApp SqlServerBackend
rg -n "autoRegisterTables|autoWrite|autoRegisteredTables_" DataLoggerCore ExampleApp docs README.md
$env:SQLLOGGER_CONNECTION_STRING=$null; .\x64\Debug\DataLogger.exe
```

## Build/test results

- Debug x64 build passed with 0 warnings and 0 errors.
- Release x64 build passed with 0 warnings and 0 errors.
- Constraint scan found no async/concurrency, JSON, or SQLite usage.
- API scan confirmed `autoRegisterTables()`, `autoWrite()`, and
  `autoRegisteredTables_` are present in code and docs.
- Debug no-connection run exited with code 1 and printed the expected
  `Set SQLLOGGER_CONNECTION_STRING before running the example.` message before
  any database work.

## Deviations from AGENTS.md or docs

- No deviation from `AGENTS.md`.
- The implementation stays single-threaded and keeps the existing per-table
  transaction behavior.
