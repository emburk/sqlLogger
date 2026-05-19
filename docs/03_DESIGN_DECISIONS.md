# 03 Design Decisions and Justification

This document records the final design decisions and their reasoning.

## DEC-001: Use CSV schema files instead of JSON

### Decision
Use CSV files as the external schema format.

### Reasoning
The schema is fundamentally tabular: column name, offset, type, size, and length. CSV is easier to edit, review, and maintain when there are many fields. It also works well with spreadsheet tools.

### Consequence
The project does not need a JSON library. A robust CSV parser is required.

## DEC-002: One CSV file equals one SQL table

### Decision
Each `.csv` file in the schema directory defines one SQL table.

### Reasoning
This gives a clean and direct relation between schema files and tables.

### Consequence
The schema directory becomes the table registry.

## DEC-003: Filename is table name

### Decision
The SQL table name is derived from the CSV filename without the extension.

### Example

```text
imu_data.csv -> imu_data
```

### Reasoning
This avoids duplicating table names inside every CSV file.

### Consequence
Filename validation is required.

## DEC-004: Timestamp is external to struct and schema

### Decision
The application passes `timestampMs` explicitly for every write.

### Reasoning
The timestamp is semantically different from payload data. It is the time axis for SQL/Grafana, not a normal struct field.

### Consequence
The CSV schema describes payload only. The SQL table automatically gets `timestamp_ms BIGINT NOT NULL`.

## DEC-005: Timestamp uses int64 milliseconds since epoch

### Decision
Use signed 64-bit integer milliseconds since Unix epoch.

### Reasoning
This is precise enough for 10 Hz telemetry, easy to bind through ODBC, easy to sort in SQL, and Grafana-compatible through query conversion if needed.

### Consequence
The SQL column is `BIGINT`.

## DEC-006: Use registered table handles

### Decision
The application obtains a `TableHandle` from a table name, then writes through the handle.

### Example

```cpp
TableHandle imu = logger.registerTable("imu_data");
logger.write(imu, timestampMs, &imuStruct);
```

### Reasoning
This avoids repeated string lookup during runtime while remaining easy to understand.

### Consequence
`DataLogger` must validate handles and map them to internal table buffers.

## DEC-007: Remove row ID / sequence ID

### Decision
Do not add a separate row ID or sequence number column initially.

### Reasoning
Grafana and SQL time-series queries should order by timestamp. A separate row ID is redundant unless deterministic replay ordering is required independently of time.

### Consequence
Ordering queries should use `ORDER BY timestamp_ms`.

## DEC-008: Flatten arrays

### Decision
Flatten array fields into scalar SQL columns using suffixes `_0`, `_1`, ...

### Example

```csv
gyro,8,float,4,3
```

expands to:

```text
gyro_0, gyro_1, gyro_2
```

### Reasoning
Flattened scalar columns are simple, SQL Server-compatible, and Grafana-friendly.

### Consequence
Very large arrays may create wide SQL tables and must be validated against SQL Server limits.

## DEC-009: Numeric-only initial implementation

### Decision
Support only numeric fields initially.

### Supported types

```text
int8, uint8, int16, uint16,
int32, uint32, int64, uint64,
float, double
```

### Reasoning
The initial telemetry requirement is numeric. Avoiding strings keeps ODBC binding and schema validation simpler.

### Consequence
String and binary blob fields are out of scope for the first implementation.

## DEC-010: Fixed-offset binary decoding

### Decision
Decode structs by fixed byte offsets defined in CSV.

### Reasoning
The caller knows the exact struct layout, padding, field sizes, and offsets.

### Consequence
The logger trusts the schema and caller. It should still use safe reads via `std::memcpy`.

## DEC-011: DataLogger owns schema during runtime

### Decision
`DataLogger` loads schema files during initialization, owns the schema registry, and treats it as immutable.

### Reasoning
This avoids runtime reload complexity and keeps all write paths simple.

### Consequence
Changing schema requires restarting/reinitializing the logger.

## DEC-012: DataLogger owns batching

### Decision
The main app writes rows. `DataLogger` decides when to flush automatically based on configured batch size.

### Reasoning
Batching is an internal logging concern. The main application should not need to track row grouping.

### Consequence
The logger must maintain per-table buffers. Remaining rows are flushed through explicit manual flush calls or shutdown handling.

## DEC-013: Batch-size-only flush policy

### Decision
Flush automatically when batch size is reached. Flush remaining rows through explicit `flush()` calls.

### Reasoning
Size-based flushing provides deterministic SQL writes without requiring production logger code to read wall-clock time.

### Consequence
The application should call `flush()` at appropriate lifecycle points, especially before shutdown.

## DEC-014: Single-threaded design

### Decision
No background worker thread, queue, or locks initially.

### Reasoning
The expected data frequency is around 10 Hz, and readability/maintainability is the priority.

### Consequence
The production logger remains deterministic and does not depend on wall-clock time. Example/test code may use wall-clock time only to produce caller-supplied timestamps.

## DEC-015: Real ODBC backend

### Decision
Implement SQL Server access through real ODBC API calls.

### Reasoning
The goal is a working SQL Server logger, not only an architecture mock.

### Consequence
The project must manage ODBC environment, connection, statement handles, diagnostics, transactions, and binding.

## DEC-016: Use ODBC parameter array binding

### Decision
Use ODBC parameter arrays for batch inserts.

### Reasoning
This is the highest-performance ODBC batching path while still using standard ODBC concepts.

### Consequence
The backend must create per-column arrays during flush. This conversion is isolated inside `SqlServerOdbcBackend`.

## DEC-017: Keep external API row-oriented

### Decision
The application writes one struct row at a time. The backend may internally transform rows to column-wise ODBC arrays.

### Reasoning
This preserves caller simplicity and keeps ODBC complexity out of the application and `DataLogger` public interface.

### Consequence
`SqlServerOdbcBackend` must allocate and fill type-specific parameter buffers per column during flush.

## DEC-018: Use transaction per batch

### Decision
Each batch insert is executed inside a transaction.

### Reasoning
The chosen failure behavior is “return error and keep buffer.” That is only safe if a failed batch is rolled back and cannot partially commit.

### Consequence
The backend must disable autocommit around batch execution, commit on success, and rollback on failure.

## DEC-019: Existing table policy is configurable

### Decision
On initialization, existing tables are either dropped or renamed with a timestamp suffix before fresh tables are created.

### Reasoning
This supports both destructive clean-run behavior and archive-preserving behavior.

### Consequence
The configuration must include `ExistingTablePolicy`.

## DEC-020: Rename suffix format

### Decision
Use suffix format `YYYYMMDD-hhmmss`.

### Example

```text
imu_data_20260516-143012
```

### Reasoning
The format is sortable and readable.

### Consequence
The implementation needs a datetime formatter.

## DEC-021: SQL tables are generated by the logger

### Decision
The logger creates SQL tables from CSV schema definitions during initialization.

### Reasoning
This avoids mismatches between schema files and database tables.

### Consequence
The backend needs SQL DDL generation.

## DEC-022: SQL identifiers are validated and quoted

### Decision
Table and column identifiers are validated and quoted using SQL Server bracket syntax.

### Reasoning
Validation prevents injection and accidental invalid identifiers. Quoting protects against reserved words and special cases.

### Consequence
Identifier validation is part of schema loading.

## DEC-023: Index timestamp column

### Decision
Create an index on `timestamp_ms`.

### Reasoning
Grafana/time-series queries will filter and order by timestamp.

### Consequence
Table creation should include index creation.

## DEC-024: Validate SQL Server width limits

### Decision
Validate expanded table width and prepared-statement parameter count before initialization succeeds.

### Reasoning
The telemetry struct can be large, and flattening can create many columns. SQL Server has limits on columns and parameters.

### Consequence
Some large telemetry definitions may need to be split into multiple CSV/table schemas.

## DEC-025: No runtime plugin loading initially

### Decision
Do not implement DLL plugin backend loading initially.

### Reasoning
The first target is one real SQL Server backend.

### Consequence
The project still has an interface boundary, so plugin loading can be added later.

## DEC-026: No JSON support initially

### Decision
Do not support JSON schema initially.

### Reasoning
CSV is now the confirmed schema format.

### Consequence
Earlier JSON examples are obsolete.

## DEC-027: Metadata is parsed but not inserted

### Decision
Optional `unit` and `description` fields are allowed in the CSV but do not become SQL payload columns.

### Reasoning
Metadata is useful for documentation and future Grafana/dashboard generation, but it should not affect the storage model initially.

### Consequence
Metadata may be kept in memory or ignored after validation, depending on implementation needs.

## DEC-028: uint64 maps to DECIMAL(20,0)

### Decision
Full `uint64` support should map to `DECIMAL(20,0)` in SQL Server.

### Reasoning
SQL Server `BIGINT` is signed and cannot represent the full `uint64` range.

### Consequence
ODBC binding for `uint64` is more complex than signed 64-bit types. The implementation can use `SQL_NUMERIC_STRUCT` or another explicit numeric conversion path.

## DEC-029: Use one Visual Studio solution and project initially

### Decision
Use a single Visual Studio 2019 solution containing one C++ project for the first implementation, while keeping the source code separated into clear folders such as `DataLoggerCore`, `SqlServerBackend`, and `ExampleApp`.

### Reasoning
A single `.vcxproj` keeps the project easy to open, build, and debug without requiring generated static libraries or multi-project solution dependency handling. The code can still preserve architectural boundaries through namespaces, headers, interfaces, and folder organization.

### Consequence
`DataLoggerCore` and `SqlServerBackend` are source-level modules rather than separate static-library projects initially. The single project links `odbc32.lib` and builds one example executable. Once the project settles, the same folder boundaries can be split back into multiple Visual Studio projects/libraries inside the solution.

## DEC-030: Add configurable debug printing

### Decision
Add `DataLoggerConfig::printInfoFlag` and `DataLoggerConfig::printErrorFlag`, both enabled by default.

### Reasoning
The logger already returns structured errors through `lastError()`, but the example and integration workflow benefit from immediate console visibility. Line-oriented success details confirm initialization database work completed, and line-oriented printed errors make failed SQL/ODBC/schema paths visible without forcing every caller to duplicate error-printing boilerplate.

### Consequence
The structured error object remains authoritative for program flow and retry behavior. Console printing is controlled by configuration and can be disabled by callers that need silent library behavior.

## DEC-031: Add automatic table registration and write helpers

### Decision
Add `autoRegisterTables()` and `autoWrite(timestampMs, structPtr)` as convenience APIs for schema directories where every loaded CSV table maps to the same source struct.

### Reasoning
Large structs may be split across many CSV/table files to stay within SQL Server limits. Since `DataLogger` already owns the loaded schema registry, requiring the application to repeat every table name creates avoidable boilerplate and a drift risk between the schema folder and the example code.

### Consequence
Manual `registerTable()` and `write()` remain available for selective table usage. The automatic path uses the existing per-table decode, buffer, and flush behavior and does not add cross-table transaction semantics.

## DEC-032: Add a separate static-library example solution

### Decision
Keep the original monolithic Visual Studio solution and add a separate `ExampleAppLib/ExampleAppLib.sln` solution that builds `DataLoggerCore` and `SqlServerBackend` as static library projects, then links them into an `ExampleAppLib` executable.

### Reasoning
The monolithic project remains useful as the original simple open/build path. The additional solution validates the intended library consumption model without moving source ownership or changing the runtime architecture.

### Consequence
`DataLoggerCore` remains independent of `SqlServerBackend`. `SqlServerBackend` depends on `DataLoggerCore`, and the example executable links both libraries plus `odbc32.lib`.

## Phase 13 decisions: C compatibility API

The following decisions define the approved C compatibility API phase.

## DEC-033: Add a C compatibility facade without replacing C++ internals

### Decision
Add C-callable headers and wrapper implementation files for library consumers that cannot include C++ headers.

The C facade will wrap the existing C++ `DataLogger` and `SqlServerOdbcBackend` classes.

### Reasoning
The current public C++ headers expose `namespace`, classes, templates, `std::variant`, and other C++ constructs. C applications or mixed C/CSPICE applications cannot include those headers directly.

### Consequence
The project keeps the existing C++ implementation and adds a narrow ABI layer. C callers see only opaque handles, C structs, integer status values, and raw pointers.

## DEC-034: Keep backend injection across the C API boundary

### Decision
Expose a C backend handle created by the SQL Server backend library, then pass that backend handle into a core logger creation function.

Example shape:

```c
DataLoggerBackend_c* backend = sqlserver_backend_create_c();
DataLogger_c* logger = datalogger_create_c(backend);
```

After successful logger creation, the logger owns the backend handle.

### Reasoning
This preserves the existing architecture where `DataLoggerCore` depends only on the backend interface and does not directly construct `SqlServerOdbcBackend`.

### Consequence
C callers need two headers: one from `DataLoggerCore` and one from `SqlServerBackend`. This avoids making the core static library depend on the concrete SQL Server backend.

## DEC-035: Use explicit C function prefixes

### Decision
Use plain prefixed C function names such as `datalogger_init_c()` and `sqlserver_backend_create_c()`.

### Reasoning
C has no namespaces, methods, or overloads. Prefixes make symbol ownership clear and avoid collisions in larger C applications.

### Consequence
The user-facing API will be close to the requested `logger.init_c(...)` shape, but expressed as C functions that take an explicit logger handle.

## DEC-036: Represent C table handles as a small value type

### Decision
Expose table handles as a C struct containing the underlying index and provide an invalid sentinel helper.

Example shape:

```c
typedef struct DataLoggerTableHandle_c
{
    size_t index;
} DataLoggerTableHandle_c;
```

### Reasoning
This mirrors the C++ `TableHandle` wrapper while remaining C-compatible and easy to pass by value.

### Consequence
The C implementation must translate between `DataLoggerTableHandle_c` and the internal C++ `TableHandle`.

## DEC-037: Prefer caller-buffer error retrieval

### Decision
Prefer an error retrieval function that copies the last error into a caller-provided buffer and returns the required byte count.

Example shape:

```c
size_t datalogger_get_error_string_c(const DataLogger_c* logger,
                                     char* buffer,
                                     size_t bufferSize);
```

### Reasoning
Caller-buffer copying avoids C callers freeing C++ memory and avoids exposing string lifetime rules that can be easy to misuse.

### Consequence
Callers can pass `NULL, 0` to query the required size, then allocate their own buffer if needed. A simpler `const char*` API remains possible if approved instead.

## DEC-038: Keep existing C++ examples and add a separate C facade example

### Decision
Keep the existing `ExampleApp` and `ExampleAppLib` C++ examples unchanged while adding C-callable headers and wrappers beside the current C++ public API.

Add a separate `ExampleAppLibC` solution/project to validate C application consumption through the C facade.

### Reasoning
The immediate goal is to make C applications able to consume the libraries without breaking or replacing the current C++ examples and include paths. A separate C example proves the facade works from a `.c` translation unit while preserving the existing C++ validation path.

### Consequence
The existing library example continues to validate the C++ static-library consumption path. `ExampleAppLibC` validates the C static-library consumption path.
