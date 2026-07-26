# DataLogger

Schema-driven telemetry logger for Microsoft SQL Server.

The project loads CSV schema files, decodes fixed-layout C++ structs by byte offset, adds caller-supplied `timestamp_ms` values, batches rows per table, and writes them to SQL Server through real ODBC parameter-array binding.

For split schemas that all map to the same source struct, `autoRegisterTables()` registers every loaded CSV table and `autoWrite()` writes one timestamped struct row to all of them.

## Project Layout

```text
sqlLogger/
  README.md
  DataLoggerCore/
  SqlServerBackend/
  AsyncLogger/
  examples/
    ExampleApp/
      DataLoggerSolution.sln
      DataLogger.vcxproj
    ExampleAppLib/
    ExampleAppLibC/
  tools/
  docs/
```

- `README.md` - repository entry point with build, run, schema, and tooling notes.
- `DataLoggerCore/` - database-independent logger core: CSV schema loading, validation, binary decoding, table handles, column batching, buffering, and flush orchestration.
- `SqlServerBackend/` - SQL Server implementation of the backend interface using real ODBC, generated DDL, prepared inserts, parameter-array binding, diagnostics, and transactions.
- `AsyncLogger/` - optional outer async shell around the single-threaded logger/backend path; producer calls enqueue copied payloads while the worker owns logger calls.
- `examples/` - all example applications and example Visual Studio solutions.
- `examples/ExampleApp/` - shared C++ example application source, retained phase test harness source, example CSV schemas, and the retained monolithic example solution.
- `examples/ExampleApp/DataLoggerSolution.sln` - retained monolithic example solution; builds one executable project that directly compiles core, backend, and `ExampleApp`.
- `examples/ExampleApp/DataLogger.vcxproj` - single-project example used by `DataLoggerSolution.sln`.
- `examples/ExampleApp/schemas/` - sample CSV table schemas; each file maps to one SQL table.
- `examples/ExampleAppLib/` - C++ static-library consumption example; builds `DataLoggerCore` and `SqlServerBackend` as libraries, then links the example executable.
- `examples/ExampleAppLibC/` - C facade consumption example; compiles a `.c` application against the public C wrappers.
- `tools/` - helper scripts for generating DataLogger CSV schemas and Grafana dashboards.
- `tools/examples/` - sample input/output files for the schema generator.
- `docs/` - project requirements, architecture, design decisions, implementation notes, development logs, and moved agent/context notes.

## Build

Open `examples/ExampleApp/DataLoggerSolution.sln` in Visual Studio 2019 to build the retained monolithic example project.

Open `examples/ExampleAppLib/ExampleAppLib.sln` to build the library-based example. That solution produces static libraries for `DataLoggerCore` and `SqlServerBackend`, then links them into the `ExampleAppLib` executable.

Open `examples/ExampleAppLibC/ExampleAppLibC.sln` to build the C facade example. It compiles `examples/ExampleAppLibC/main.c` and includes only the public C headers.

From PowerShell:

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' examples\ExampleApp\DataLoggerSolution.sln /p:Configuration=Debug /p:Platform=x64 /m:1
& 'C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' examples\ExampleAppLib\ExampleAppLib.sln /p:Configuration=Debug /p:Platform=x64 /m:1
& 'C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' examples\ExampleAppLibC\ExampleAppLibC.sln /p:Configuration=Debug /p:Platform=x64 /m:1
```

The project targets C++17, Windows x64, and links `odbc32.lib`.

## Run The Example

Set `SQLLOGGER_CONNECTION_STRING` to a SQL Server ODBC connection string, then run the executable.

```powershell
$env:SQLLOGGER_CONNECTION_STRING = 'Driver={ODBC Driver 18 for SQL Server};Server=tcp:127.0.0.1,1433;Database=SensorDataDB;Uid=<user>;Pwd=<password>;Encrypt=no;TrustServerCertificate=yes;'
.\examples\ExampleApp\x64\Debug\DataLogger.exe
```

By default, the example loads `examples/ExampleApp/schemas/imu_data.csv`, creates/recreates `[dbo].[imu_data]` according to the configured table policy, writes two simple sensor rows, and flushes them.

`DataLoggerConfig::existingTablePolicy` still defaults to `RenameWithTimestampSuffix`. To continue appending to the current table, explicitly set `ExistingTablePolicy::ContinueCurrentTable`; SQL Server will reuse the table only when its metadata matches the CSV schema.

`dbo` is the default SQL Server schema name. In generated SQL, it is the schema qualifier in names such as `[dbo].[imu_data]`; changing `DataLoggerConfig::sqlSchemaName` writes the same generated tables under a different SQL Server schema, if that schema exists and the connection has permission to use it.

The larger local AO stress payload is guarded by `LOCAL_TEST` in `examples/ExampleApp/main.cpp`. Do not define `LOCAL_TEST` in committed project settings; define it only in a local developer configuration when that private/local payload should be used.

`DataLoggerConfig::printInfoFlag` and `DataLoggerConfig::printErrorFlag` are enabled by default. Successful initialization prints database setup details line by line, and recorded logger errors are printed line by line while remaining available through `lastError()`.

`examples/ExampleApp/test.cpp` contains the earlier smoke and Phase 9 validation harness. It is retained as a test-tool source file and is not compiled into the default example app. To repeat the old `DataLogger.exe --phase9-tests` run, temporarily switch the executable project entry source from `examples/ExampleApp/main.cpp` to `examples/ExampleApp/test.cpp`, rebuild, run the test command, then switch back to `examples/ExampleApp/main.cpp` for the normal real-ODBC example.

## Schema Format

Each CSV file describes one SQL table. The table name comes from the filename.

Required columns:

```csv
column_name,offset,datatype,size,length
```

Optional metadata columns:

```csv
unit,description
```

Example:

```csv
column_name,offset,datatype,size,length,unit,description
gyro,8,float,4,3,rad/s,body angular rate
accel,20,float,4,3,m/s2,body acceleration
temperature,32,double,8,1,degC,electronics temperature
status,40,uint16,2,1,,status code
```

Arrays are flattened into SQL columns such as `gyro_0`, `gyro_1`, and `gyro_2`. The logger automatically prepends `timestamp_ms BIGINT NOT NULL`; timestamps are supplied by the application and are not listed in the CSV.

## Schema Generator

`tools/schemaGenerator.py` converts exported struct layout CSV files with this input shape:

```csv
field_name, offset, byteSize, lengthDim1, lengthDim2, classname
```

The generator preserves the DataLogger CSV contract and expands array-typed struct parents before writing schema rows. For example, a source field path below `a.b` where `a.b` is a struct array becomes columns such as `a_b_0_c`, `a_b_0_d`, through `a_b_19_c`, `a_b_19_d`. Nested struct arrays are expanded recursively.
For struct-array rows, `byteSize` is treated as the total array byte span, so element offsets use `byteSize / (lengthDim1 * lengthDim2)` as the stride.

Example:

```powershell
python tools\schemaGenerator.py tools\examples\struct_array_layout.csv tools\examples\struct_array_schema.csv --mode 0
```

## Grafana Query Example

```sql
SELECT
    DATEADD(ms, [timestamp_ms] % 1000,
        DATEADD(second, [timestamp_ms] / 1000, '1970-01-01')) AS time,
    [gyro_0]
FROM [dbo].[imu_data]
ORDER BY [timestamp_ms];
```

## Test Notes

- Phase 8 and Phase 9 test plans/results are in `docs/test_notes/`.
- Non-database checks passed in Debug and Release.
- Database checks were manually validated by the project owner.
- Phase 9 reruns require the temporary `main.cpp` to `test.cpp` project-source switch described above.
