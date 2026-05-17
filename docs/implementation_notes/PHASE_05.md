# Phase 05: Binary Decoder

## Phase objective

Implement fixed-offset binary decoding from a validated `TableSchema` into a `DecodedRow`.

The decoder must:

- read payload values from caller-owned struct memory using schema offsets;
- use `std::memcpy` for safe numeric reads;
- preserve the caller-supplied external timestamp;
- copy decoded values into logger-owned row data;
- avoid SQL, ODBC, batching, background work, or pointer retention.

## Files changed

- `DataLoggerCore/include/DataLogger/BinaryDecoder.h`
- `DataLoggerCore/include/DataLogger/Error.h`
- `DataLoggerCore/src/BinaryDecoder.cpp`
- `DataLogger.vcxproj`
- `DataLogger.vcxproj.filters`
- `ExampleApp/main.cpp`
- `docs/implementation_notes/PHASE_05.md`

## Implementation summary

- Added `decodeRow` in `DataLoggerCore`.
- Added typed field decoding for all supported numeric `DataType` values.
- Added an internal `readValue<T>` helper that copies bytes with `std::memcpy`.
- Added `ErrorCode::DecodeFailed` for null pointer decode failures.
- Updated the example app smoke path to:
  - load and validate the existing `imu_data.csv`;
  - register the `imu_data` table handle;
  - decode a packed `ImuData` struct whose layout matches the CSV offsets;
  - verify timestamp and decoded field values.
- Updated Visual Studio project metadata for the new decoder files.

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
- Constraint scan found no async/concurrency, JSON/SQLite, SQL insertion, or direct ODBC call paths.

## Known limitations

- No DataLogger `write()` API yet.
- No row buffering or flushing yet.
- No SQL Server table creation or ODBC insertion yet.
- No dedicated unit test project yet; decoder coverage is currently provided by the example smoke execution.
- The decoder trusts that the caller's struct layout matches the CSV schema, per requirements.

## Deviations from AGENTS.md or docs

- No deviation from `AGENTS.md`.
- No SQL or ODBC behavior was added in this phase.
