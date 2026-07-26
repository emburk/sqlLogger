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
#include "DataLogger/DataLogger.h"
#include "SqlServerBackend/SqlServerOdbcBackend.h"

#include <memory>

DataLoggerCore::DataLoggerConfig config;
config.connectionString = "Driver={ODBC Driver 18 for SQL Server};Server=localhost;Database=Telemetry;Trusted_Connection=yes;";
config.schemaDirectory = "schemas";
config.sqlSchemaName = "dbo";
config.batchSizeRows = 100;
config.existingTablePolicy = DataLoggerCore::ExistingTablePolicy::RenameWithTimestampSuffix;
config.printInfoFlag = true;
config.printErrorFlag = true;

auto backend = std::make_unique<SqlServerBackend::SqlServerOdbcBackend>();
DataLoggerCore::DataLogger logger(std::move(backend));
logger.initialize(config);

DataLoggerCore::TableHandle imu = logger.registerTable("imu_data");

ImuData data{};
std::int64_t timestampMs = getCurrentUnixTimeMilliseconds();

logger.write(imu, timestampMs, &data);
logger.flush();
```

`dbo` is SQL Server's common default schema. The logger uses `DataLoggerConfig::sqlSchemaName` to generate schema-qualified table names such as `[dbo].[imu_data]`; callers can set another schema name when the target database and permissions require it.

`DataLoggerConfig::existingTablePolicy` defaults to `RenameWithTimestampSuffix`. Callers may explicitly select `ContinueCurrentTable` to append to a matching existing SQL table; the backend validates the existing table metadata before preparing inserts.

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
- optionally prints initialization success and recorded errors;
- exposes table handles;
- can auto-register every loaded table from the schema directory;
- accepts struct pointers and timestamps;
- can write one struct row to every auto-registered table;
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

## What the C facade does

The C facade is an additive compatibility layer over the same C++ implementation.

It:

- exposes opaque logger and backend handles;
- provides C-compatible configuration and table-handle structs;
- returns integer success values;
- copies the last error string into a caller-provided buffer;
- lets C code create a SQL Server backend and pass it into the core logger.

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

## Current project structure

The current repository includes both the retained monolithic build and separate static-library consumption examples:

```text
DataLoggerCore/
SqlServerBackend/
examples/
  ExampleApp/
    DataLoggerSolution.sln
    DataLogger.vcxproj
  ExampleAppLib/
  ExampleAppLibC/
tools/
docs/
```

Important current files:

```text
DataLoggerCore/
  include/
    DataLogger/
      DataLogger.h
      DataLoggerC.h
      DataLoggerConfig.h
      TableHandle.h
      Schema.h
      SchemaLoaderCsv.h
      Error.h
      IDBBackend.h
  src/
    DataLogger.cpp
    DataLoggerC.cpp
    SchemaLoaderCsv.cpp
    SchemaValidator.cpp
    BinaryDecoder.cpp
  DataLoggerCore.vcxproj

SqlServerBackend/
  include/
    SqlServerBackend/
      SqlServerOdbcBackend.h
      SqlServerOdbcBackendC.h
  src/
    SqlServerBackend.cpp
    SqlServerOdbcBackendC.cpp
  SqlServerBackend.vcxproj

examples/
  ExampleApp/
    DataLoggerSolution.sln
    DataLogger.vcxproj
    main.cpp
    test.cpp
    schemas/
      imu_data.csv
  ExampleAppLib/
    ExampleAppLib.sln
    ExampleAppLib.vcxproj
  ExampleAppLibC/
    ExampleAppLibC.sln
    ExampleAppLibC.vcxproj
    main.c
```

`examples/ExampleApp/test.cpp` contains the historical smoke and Phase 9 validation harness. The normal projects compile `examples/ExampleApp/main.cpp`; to repeat the old `DataLogger.exe --phase9-tests` run, temporarily switch the executable project source to `examples/ExampleApp/test.cpp`, rebuild, run the test, and then restore `examples/ExampleApp/main.cpp`.

## Tooling note

`tools/schemaGenerator.py` converts exported struct layout CSV files into
DataLogger schema CSV files. When an exported row describes an array of structs,
the generator clones numeric descendants for each struct element and inserts
the element index into the generated column name. This also applies recursively
to nested struct arrays.

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
