# DataLogger

Schema-driven telemetry logger for Microsoft SQL Server.

The project loads CSV schema files, decodes fixed-layout C++ structs by byte offset, adds caller-supplied `timestamp_ms` values, batches rows per table, and writes them to SQL Server through real ODBC parameter-array binding.

## Project Layout

- `DataLoggerCore/` - schema loading, validation, binary decoding, table handles, buffering, and flush orchestration.
- `SqlServerBackend/` - SQL Server ODBC backend, DDL generation, prepared inserts, parameter arrays, diagnostics, and transactions.
- `ExampleApp/` - minimal real-ODBC example app plus the retained test harness source.
- `ExampleApp/schemas/` - CSV schema files, one file per SQL table.
- `docs/` - requirements, architecture, design decisions, implementation plan, and test notes.

## Build

Open `DataLoggerSolution.sln` in Visual Studio 2019 and build `Debug|x64` or `Release|x64`.

From PowerShell:

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' DataLoggerSolution.sln /p:Configuration=Debug /p:Platform=x64 /m:1
```

The project targets C++17, Windows x64, and links `odbc32.lib`.

## Run The Example

Set `SQLLOGGER_CONNECTION_STRING` to a SQL Server ODBC connection string, then run the executable.

```powershell
$env:SQLLOGGER_CONNECTION_STRING = 'Driver={ODBC Driver 18 for SQL Server};Server=tcp:127.0.0.1,1433;Database=SensorDataDB;Uid=<user>;Pwd=<password>;Encrypt=no;TrustServerCertificate=yes;'
.\x64\Debug\DataLogger.exe
```

The example loads `ExampleApp/schemas/imu_data.csv`, creates/recreates `[dbo].[imu_data]` according to the configured table policy, writes two rows, and flushes them.

`DataLoggerConfig::printInfoFlag` and `DataLoggerConfig::printErrorFlag` are enabled by default. Successful initialization prints database setup details line by line, and recorded logger errors are printed line by line while remaining available through `lastError()`.

`ExampleApp/test.cpp` contains the earlier smoke and Phase 9 validation harness. It is retained as a test-tool source file and is not compiled into the default example app.

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
