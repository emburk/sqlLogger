# 04 Project Summary

## Short summary

The DataLogger project is a C++17 / Visual Studio 2019 telemetry logging system. It reads table schemas from CSV files, decodes fixed-layout binary structs by offset, adds an external timestamp per row, batches decoded rows per table, and writes them to Microsoft SQL Server through a real ODBC backend using parameter array binding.

## Problem being solved

The application produces structured telemetry data in C/C++ structs. These structs can contain many numeric fields and arrays. The goal is to log the data into SQL Server without hardcoding SQL insert logic for every struct and every field.

The system must support:

- many tables;
- fixed binary offsets;
- flattened arrays;
- external timestamp per row;
- batch insertion;
- SQL Server table generation;
- Grafana-friendly time-series storage.

## Final system concept

```text
CSV schema files
    -> DataLogger schema registry
        -> fixed-offset decoder
            -> per-table row buffers
                -> ODBC backend
                    -> SQL Server
```

## What the application does

The main application:

1. Configures the logger.
2. Initializes the logger.
3. Registers table handles.
4. Supplies timestamped struct data to the logger.
5. Calls `flush()` before shutdown or whenever remaining buffered rows should be persisted.

Example:

```cpp
DataLoggerConfig config;
config.connectionString = "Driver={ODBC Driver 18 for SQL Server};Server=localhost;Database=Telemetry;Trusted_Connection=yes;";
config.schemaDirectory = "schemas";
config.batchSizeRows = 100;
config.existingTablePolicy = ExistingTablePolicy::RenameWithTimestampSuffix;

DataLogger logger;
logger.initialize(config);

TableHandle imu = logger.registerTable("imu_data");

ImuData data{};
std::int64_t timestampMs = getCurrentUnixTimeMilliseconds();

logger.write(imu, timestampMs, &data);
logger.flush();
```

## What the schema looks like

Example file:

```text
schemas/imu_data.csv
```

Example content:

```csv
column_name,offset,datatype,size,length,unit,description
gyro,8,float,4,3,rad/s,body angular rate
accel,20,float,4,3,m/s2,body acceleration
temperature,32,double,8,1,degC,electronics temperature
status,40,uint16,2,1,,status code
```

Generated SQL columns:

```text
timestamp_ms
gyro_0
gyro_1
gyro_2
accel_0
accel_1
accel_2
temperature
status
```

## What DataLogger does

`DataLogger` is the telemetry/schema layer.

It:

- loads all CSV files;
- validates schema definitions;
- creates an immutable schema registry;
- exposes table handles;
- accepts struct pointers and timestamps;
- decodes fields using offsets;
- flattens arrays;
- buffers rows by table;
- triggers flush by batch size or explicit flush call;
- calls the backend to persist batches.

## What IDBBackend does

`IDBBackend` is the database abstraction.

It:

- connects to storage;
- initializes tables;
- prepares insert statements;
- inserts decoded row batches;
- reports backend errors.

It does not understand structs or offsets.

## What SqlServerOdbcBackend does

`SqlServerOdbcBackend` is the concrete SQL Server implementation.

It:

- uses real ODBC handles;
- connects using an ODBC connection string;
- applies existing table policy;
- creates SQL tables;
- creates timestamp indexes;
- prepares parameterized insert statements;
- uses ODBC parameter array binding;
- wraps each flush in a transaction;
- rolls back failed batches;
- reports ODBC diagnostics.

## Why timestamp is required

Grafana and SQL time-series queries need a time axis. Therefore every row has:

```sql
timestamp_ms BIGINT NOT NULL
```

The timestamp is passed by the application, not read from the struct and not stored in CSV.

## Why no row ID is needed

A separate row ID is not part of the initial design. For Grafana and time-series queries, ordering should use:

```sql
ORDER BY timestamp_ms
```

A row ID can be added later only if deterministic replay order is needed independently of timestamp.

## Important wide-table warning

A 16 KB struct can flatten into many SQL columns. SQL Server has table and statement limits. Therefore schema validation must reject too-wide schemas early and clearly.

If a table is too wide, split it into multiple CSV/table definitions that share the same timestamp.

Example:

```text
payload_power.csv
payload_thermal.csv
payload_attitude.csv
payload_status.csv
```

## First implementation target

The first complete project should include:

```text
DataLoggerSolution.sln
DataLogger.vcxproj
DataLoggerCore/
SqlServerBackend/
ExampleApp/
docs/
```

Suggested project structure:

```text
DataLoggerSolution.sln
DataLogger.vcxproj

DataLoggerCore/
  include/
    DataLogger.h
    DataLoggerConfig.h
    TableHandle.h
    Schema.h
    SchemaLoaderCsv.h
    Error.h
    IDBBackend.h
  src/
    DataLogger.cpp
    SchemaLoaderCsv.cpp
    SchemaValidator.cpp
    BinaryDecoder.cpp

SqlServerBackend/
  include/
    SqlServerOdbcBackend.h
    OdbcHandle.h
    OdbcDiagnostics.h
  src/
    SqlServerOdbcBackend.cpp
    OdbcDiagnostics.cpp
    SqlTypeMapper.cpp

ExampleApp/
  main.cpp
  TelemetryStructs.h
  schemas/
    imu_data.csv
```

## Success definition

The project is successful when:

1. The example app builds in Visual Studio 2019 x64.
2. The logger loads CSV schemas from a folder.
3. SQL Server tables are recreated according to the configured policy.
4. The application writes timestamped structs through table handles.
5. Rows are buffered and flushed by batch size or explicit flush call.
6. SQL Server receives rows through real ODBC parameter array binding.
7. Failed flushes roll back and preserve the logger buffer.
8. Grafana can query tables by `timestamp_ms`.
