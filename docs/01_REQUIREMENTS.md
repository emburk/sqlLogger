# 01 Requirements

This document lists the requirements for the DataLogger project. Each requirement has a stable ID so Codex or another implementation agent can reference it directly.

## 1. Project and build requirements

### REQ-001: Language standard
The project shall be implemented in modern C++ using C++17.

### REQ-002: IDE target
The first implementation shall include a Visual Studio 2019 solution file (`.sln`) and one Visual Studio C++ project file (`.vcxproj`) initially.

The source layout shall keep module boundaries clear so the project can later be split into multiple Visual Studio projects if needed.

Current status: the repository still keeps the original monolithic solution and project, and also includes separate static-library projects and example solutions for C++ and C consumption.

### REQ-003: Platform target
The primary build target shall be Windows x64.

### REQ-004: Project output structure
The generated project shall be a full multi-file project, not a single-file demonstration.

### REQ-005: Readability priority
The implementation shall prioritize readability, maintainability, and clear ownership over aggressive micro-optimization.

### REQ-006: Minimal dependency policy
The implementation shall avoid unnecessary third-party dependencies. The schema format is CSV, so no JSON library is required.

## 2. Database requirements

### REQ-010: Database target
The target database shall be Microsoft SQL Server.

### REQ-011: Database access API
Database access shall be implemented using real ODBC API calls, not a mock backend.

### REQ-012: ODBC link dependency
The Visual Studio project shall link against the Windows ODBC library, typically `odbc32.lib`.

### REQ-013: Backend abstraction
The SQL Server implementation shall sit behind an `IDBBackend` interface.

### REQ-014: Real SQL Server backend
The project shall include a concrete `SqlServerOdbcBackend` implementation.

### REQ-015: Batched insert
Rows shall be inserted in batches rather than one SQL execution per individual row.

### REQ-016: ODBC parameter array binding
The SQL Server backend shall use ODBC parameter array binding for batch execution.

### REQ-017: Prepared statement usage
The backend shall prepare one parameterized `INSERT` statement per table and reuse it for batch execution.

### REQ-018: Transaction-protected flush
Each batch flush shall be executed in a transaction. If the batch fails, the transaction shall roll back.

### REQ-019: Failure buffer retention
If a flush fails, `DataLogger` shall keep the affected buffer so the caller can retry after handling the error.

### REQ-020: SQL diagnostics
ODBC errors shall be converted into a readable diagnostic structure containing SQL state, native error, and message text.

## 3. Schema requirements

### REQ-030: Schema format
The schema format shall be CSV.

### REQ-031: Schema directory
The logger shall load all `.csv` files from a configured schema directory.

### REQ-032: One file per table
Each CSV file shall describe exactly one SQL table.

### REQ-033: Table name from filename
The SQL table name shall be derived from the CSV filename without the `.csv` extension.

Example:

```text
schemas/imu_data.csv -> table name imu_data
```

### REQ-034: Required CSV fields
Each CSV schema file shall support this required header:

```csv
column_name,offset,datatype,size,length
```

### REQ-035: Optional metadata CSV fields
Each CSV schema file may additionally contain:

```csv
unit,description
```

The optional metadata fields shall be parsed and stored but shall not affect SQL insertion.

### REQ-036: CSV payload fields only
The CSV schema shall describe only payload fields from the struct. It shall not include the timestamp column.

### REQ-037: Automatic timestamp column
Every SQL table created by the logger shall automatically include:

```sql
timestamp_ms BIGINT NOT NULL
```

### REQ-038: Immutable runtime schema
`DataLogger` shall load schemas at initialization and own them for the lifetime of the logger instance. Schemas shall not change during runtime.

### REQ-039: CSV comments
The CSV parser may support comment lines beginning with `#`. If supported, comments shall be ignored.

### REQ-040: CSV quoted fields
The CSV parser should support quoted fields so descriptions can contain commas.

### REQ-041: Schema validation
The loader shall validate every schema before allowing runtime logging.

### REQ-042: Invalid schema failure
Invalid schema files shall produce clear errors and prevent initialization.

## 4. CSV column rules

### REQ-050: Column schema fields
Each CSV row shall describe a field using:

- `column_name`: base payload field name;
- `offset`: byte offset from the start of the struct;
- `datatype`: payload type;
- `size`: byte size of one element;
- `length`: number of elements;
- `unit`: optional metadata;
- `description`: optional metadata.

### REQ-051: Supported datatypes
The initial supported datatype list shall be:

```text
int8, uint8, int16, uint16,
int32, uint32, int64, uint64,
float, double
```

### REQ-052: Strings excluded
String and character-array fields shall not be supported initially.

### REQ-053: Numeric-only insertion
All inserted payload fields shall be numeric.

### REQ-054: Array flattening
If `length > 1`, the logger shall flatten the array into multiple SQL columns.

Example:

```csv
gyro,8,float,4,3,rad/s,body angular rate
```

shall expand to:

```text
gyro_0
gyro_1
gyro_2
```

### REQ-055: Scalar naming
If `length == 1`, the SQL column name shall be the base `column_name` without suffix.

### REQ-056: Array element offset
For array element `i`, the byte offset shall be:

```text
offset + i * size
```

### REQ-057: Duplicate column rejection
The loader shall reject duplicate expanded column names.

### REQ-058: Reserved timestamp name
The loader shall reject payload columns that expand to `timestamp_ms`.

### REQ-059: Identifier validation
Table names and column names shall be validated to avoid SQL injection and invalid SQL identifiers.

Recommended allowed pattern:

```text
[A-Za-z_][A-Za-z0-9_]*
```

### REQ-060: SQL identifier quoting
Generated SQL shall still quote identifiers using SQL Server bracket syntax.

Example:

```sql
[dbo].[imu_data]
[timestamp_ms]
[gyro_0]
```

## 5. Struct decoding requirements

### REQ-070: Fixed-offset binary mapping
Payload structs shall be decoded using fixed byte offsets from the CSV schema.

### REQ-071: Caller layout responsibility
The caller is responsible for ensuring the struct memory layout, padding, alignment, and offsets match the schema.

### REQ-072: No runtime reflection
The logger shall not depend on C++ reflection or compile-time struct introspection.

### REQ-073: Safe numeric reads
The decoder should use `std::memcpy` to read numeric values from raw memory into typed values, avoiding undefined behavior from unsafe pointer aliasing.

### REQ-074: No struct ownership
The logger shall not own the source struct pointer beyond the duration of the `write()` call. It shall decode/copy values into its internal buffer during `write()`.

### REQ-075: Row-oriented caller interface
The application shall call the logger row-by-row using a pointer to the struct.

## 6. Timestamp requirements

### REQ-080: External timestamp
Timestamp shall be provided explicitly by the application for each write call.

### REQ-081: Timestamp type
Timestamp shall be represented as signed 64-bit integer milliseconds since Unix epoch.

### REQ-082: Timestamp not in struct
The timestamp shall not be read from the struct.

### REQ-083: Timestamp not in CSV schema
The timestamp shall not be declared in the CSV schema.

### REQ-084: SQL timestamp column
The generated SQL table shall include the timestamp as the first column by default.

### REQ-085: Grafana ordering
Queries intended for Grafana shall order time series by `timestamp_ms`.

## 7. DataLogger API requirements

### REQ-090: Initialization
`DataLogger` shall be initialized using a configuration object.

### REQ-091: Configuration contents
The configuration shall include at least:

- SQL Server ODBC connection string;
- schema directory path;
- batch size in rows;
- existing-table policy;
- optional SQL Server schema name, defaulting to `dbo`;
- optional info-printing flag, defaulting to enabled;
- optional error-printing flag, defaulting to enabled.

### REQ-092: Table handle registration
The application shall register or obtain a table handle by name before writing data.

Example:

```cpp
TableHandle imu = logger.registerTable("imu_data");
```

### REQ-093: Write by handle
The application shall write data using a registered table handle.

Example:

```cpp
logger.write(imu, timestampMs, &imuStruct);
```

### REQ-094: Invalid handle behavior
Writes using invalid handles shall fail with a clear error.

### REQ-095: Manual flush
The API shall provide a manual `flush()` method.

### REQ-096: Per-table flush
The API should provide a method to flush one table buffer by handle.

### REQ-097: All-table flush
The API shall provide a method to flush all table buffers.

### REQ-098: Single-threaded use
The API shall be designed for single-threaded use and does not need internal locks.

Current async-refactor exception: `DataLoggerCore` and `SqlServerBackend` remain single-threaded. Threading and asynchronous producer/worker behavior may exist only in an outer shell such as `AsyncLogger`, where the worker owns calls into the synchronous logger.

### REQ-099: No production wall-clock dependency
Production logger code shall not read wall-clock time. Example or test code may use wall-clock time only to produce caller-supplied timestamps.

### REQ-100: Initialization success printing
`DataLoggerConfig` shall include `printInfoFlag`, enabled by default. When enabled, successful initialization shall print comprehensive line-oriented details after database connection, table initialization, and insert-statement preparation all succeed.

### REQ-101: Error printing
`DataLoggerConfig` shall include `printErrorFlag`, enabled by default. When enabled, DataLogger shall print every structured schema, configuration, decode, handle, backend, SQL, ODBC, or insertion error that it records while still preserving the error through `lastError()`. Multi-part errors, including backend messages and ODBC diagnostics, shall print with each detail on its own line.

### REQ-102: Automatic table registration
The API shall provide `autoRegisterTables()` to register all tables loaded from `.csv` files in the configured schema directory. The registration order shall follow the deterministic schema registry order.

### REQ-103: Automatic multi-table write
The API shall provide `autoWrite(timestampMs, structPtr)` to write the same caller-owned struct pointer to every table registered by `autoRegisterTables()`.

### REQ-104: Automatic API error behavior
Calling `autoRegisterTables()` before initialization or `autoWrite()` before automatic registration shall fail clearly and preserve the error through `lastError()`.

## 8. Batching requirements

### REQ-110: Internal buffering
`DataLogger` shall buffer decoded rows internally per table.

### REQ-111: Batch-size-only flush policy
A table buffer shall be flushed automatically when the configured batch size is reached.

Rows may also be flushed through explicit manual flush calls.

### REQ-112: Batch size configurable
Batch size shall be configurable during initialization.

### REQ-113: No flush interval configuration
Flush interval configuration is not required initially.

### REQ-114: No background thread
No background flushing thread shall be used initially.

Current async-refactor exception: `AsyncLogger` may add one outer worker thread so real-time callers enqueue preallocated payload copies while SQL/decode/flush work stays off the caller thread. The core logger and backend do not become generally thread-safe.

### REQ-115: Flush on shutdown
`DataLogger` shall attempt to flush remaining buffers during explicit shutdown or destruction, but the preferred API is explicit `flush()` before shutdown.

### REQ-116: Buffer retention on failure
If flush fails, the table buffer shall remain intact unless the transaction is known to have committed successfully.

## 9. Table lifecycle requirements

### REQ-120: Table recreation during initialization
During initialization, the logger shall create SQL tables from the loaded CSV schemas.

### REQ-121: Existing table policy
The table lifecycle shall support two policies:

1. Delete/drop existing tables before recreation.
2. Rename existing tables by appending a datetime suffix, then create fresh tables.

### REQ-122: Rename suffix format
The rename suffix shall use:

```text
YYYYMMDD-hhmmss
```

Example:

```text
imu_data_20260516-143012
```

### REQ-123: Rename collision handling
If a renamed target table already exists, the implementation shall append an extra numeric suffix or fail clearly.

### REQ-124: Fresh table creation
After existing table handling, a fresh table shall be created for every schema file.

### REQ-125: Schema-qualified tables
The implementation shall support a SQL schema name, defaulting to `dbo`.

`dbo` is the default SQL Server schema used to qualify generated table names, for example `[dbo].[imu_data]`. A caller may configure another schema name when the target database uses a different schema and the connection has permission to create tables and indexes there.

## 10. SQL table requirements

### REQ-130: SQL type mapping
The implementation shall map CSV datatypes to SQL Server types.

Recommended mapping:

| CSV datatype | SQL Server type |
|---|---|
| int8 | SMALLINT |
| uint8 | TINYINT |
| int16 | SMALLINT |
| uint16 | INT |
| int32 | INT |
| uint32 | BIGINT |
| int64 | BIGINT |
| uint64 | DECIMAL(20,0) |
| float | REAL |
| double | FLOAT(53) |

### REQ-131: Timestamp type
`timestamp_ms` shall use `BIGINT`.

### REQ-132: Primary index recommendation
The created table should include an index on `timestamp_ms` for Grafana/time-series queries.

### REQ-133: No payload primary key initially
The initial implementation shall not require a row ID or sequence ID column.

### REQ-134: SQL limits validation
Before creating tables or preparing statements, the implementation shall validate table width and parameter count against SQL Server practical limits.

Initial limits shall be represented as named constants so tests and future implementation changes can update them deliberately.

### REQ-135: Too-wide table behavior
If an expanded schema is too wide for the selected SQL Server insertion strategy, initialization shall fail clearly and recommend splitting the schema into multiple CSV/table files.

## 11. Backend requirements

### REQ-140: Backend responsibilities
`IDBBackend` shall be responsible only for database communication and SQL execution.

### REQ-141: No struct knowledge in backend
The backend shall not know about C++ struct layouts, offsets, padding, or raw telemetry memory.

### REQ-142: Backend receives decoded batch
The backend shall receive decoded row buffers and table schema metadata from `DataLogger`.

### REQ-143: ODBC column buffers
The `SqlServerOdbcBackend` shall convert row-oriented decoded batches into ODBC parameter arrays during flush.

### REQ-144: Statement preparation
The backend shall prepare one insert statement per table after table creation.

### REQ-145: Parameter status tracking
The backend should use ODBC parameter status and processed counters for diagnostics.

### REQ-146: Manual commit mode
The backend shall use manual commit mode for batch flush transactions.

### REQ-147: Commit on success
The backend shall commit only after the full batch succeeds.

### REQ-148: Rollback on failure
The backend shall roll back on batch failure.

## 12. Non-goals

### REQ-200: No runtime schema reload
Runtime schema reload is not required.

### REQ-201: No multithreading initially
Thread-safe ingestion and worker threads are not required initially.

Current async-refactor exception: the real-time safety wrapper may use an outer SPSC queue and one worker thread. This does not change the single-threaded contract of `DataLoggerCore` or `SqlServerBackend`.

### REQ-202: No JSON schema
JSON schema support is not required.

### REQ-203: No string payload fields initially
String payload support is not required initially.

### REQ-204: No plugin loading initially
DLL/plugin-style backend loading is not required initially.

### REQ-205: No row ID initially
A separate row ID or sequence column is not required initially.

## 13. C compatibility API requirements

### REQ-210: C-callable public headers
The static-library consumption path shall provide C-callable public headers for `DataLoggerCore` and `SqlServerBackend`.

These headers shall be usable from `.c` translation units and shall not expose C++ namespaces, classes, templates, `std::string`, `std::vector`, `std::variant`, references, exceptions, or overloads.

### REQ-211: Preserve C++ implementation ownership
The C API shall be a compatibility layer over the existing C++ implementation. It shall not replace the internal C++ `DataLogger`, `IDBBackend`, or `SqlServerOdbcBackend` architecture.

### REQ-212: Opaque C handles
The C API shall expose logger and backend instances through opaque pointer handles so C callers do not depend on C++ object layout.

### REQ-213: C configuration type
The C API shall provide a `DataLoggerConfig_c` type using C-compatible fields:

- `const char* connectionString`;
- `const char* schemaDirectory`;
- `const char* sqlSchemaName`;
- `size_t batchSizeRows`;
- C enum value for existing-table policy;
- integer flags for info and error printing.

### REQ-214: C table handle type
The C API shall provide a C-compatible table handle type. It shall allow an invalid sentinel and shall be accepted by C write and flush functions.

### REQ-215: C return values
C API functions shall return integer success values, where `1` means success and `0` means failure, unless a function naturally returns a handle.

### REQ-216: C error retrieval
The C API shall provide a way to retrieve the last logger error as a readable C string without transferring ownership of C++ memory to the caller.

The exact retrieval style is a design decision for approval before implementation.

### REQ-217: C automatic registration and write
The C API shall support initialization, automatic table registration, manual table registration, handle-based write, automatic multi-table write, all-table flush, per-table flush, shutdown, and destruction.

### REQ-218: C API remains single-threaded
The C API shall follow the same single-threaded design as the C++ API and shall not add locks, worker threads, async behavior, or background flushing.

### REQ-219: Preserve existing C++ public API and examples
The C-callable headers shall be added alongside the existing C++ headers and shall not replace them.

Existing C++ examples, including `ExampleAppLib`, shall remain unaffected unless a later approved phase explicitly changes them.

### REQ-220: Add C static-library example
The project shall include a separate `ExampleAppLibC` Visual Studio solution and executable project that consumes `DataLoggerCore` and `SqlServerBackend` through only the C public headers.

This example shall reuse the existing CSV schema files and shall not replace or alter the existing C++ examples.
