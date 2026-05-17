# 05 Implementation Plan

This document gives a practical implementation sequence for Codex or another code-generation agent.

## Phase 1: Create Visual Studio solution structure

### PLAN-001: Create solution
Create:

```text
DataLoggerSolution.sln
```

for Visual Studio 2019.

### PLAN-002: Create project
Create one initial project:

```text
DataLogger.vcxproj
```

### PLAN-003: Configure C++ standard
Set the project to C++17.

### PLAN-004: Configure x64
Set Debug x64 and Release x64 configurations.

### PLAN-005: Link ODBC
The single project shall link against:

```text
odbc32.lib
```

### PLAN-006: Source module dependencies
Keep source-level dependencies clean even though there is only one `.vcxproj`.

`ExampleApp` may include/use both `DataLoggerCore` and `SqlServerBackend`.

`DataLoggerCore` should not depend on `SqlServerBackend` if possible. The concrete backend can be constructed in `ExampleApp` and injected into `DataLogger`, or `DataLogger` can be configured with a backend factory if simpler for the first implementation.

Recommended for clean architecture:

```cpp
auto backend = std::make_unique<SqlServerOdbcBackend>();
DataLogger logger(std::move(backend));
```

## Phase 2: Define core types

### PLAN-010: Define DataType enum
Implement the supported datatype enum:

```cpp
enum class DataType
{
    Int8,
    UInt8,
    Int16,
    UInt16,
    Int32,
    UInt32,
    Int64,
    UInt64,
    Float,
    Double
};
```

### PLAN-011: Define schema structs
Implement:

- `ColumnSchema`;
- `ExpandedColumnSchema`;
- `TableSchema`;
- `SchemaRegistry`.

### PLAN-012: Define value variant
Implement:

```cpp
using FieldValue = std::variant<...>;
```

with all supported numeric types.

### PLAN-013: Define DecodedRow
Implement:

```cpp
struct DecodedRow
{
    std::int64_t timestampMs;
    std::vector<FieldValue> values;
};
```

### PLAN-014: Define error types
Implement project-level error structures for DataLogger and backend diagnostics.

## Phase 3: Implement CSV schema loader

### PLAN-020: Directory enumeration
Load all `.csv` files from `DataLoggerConfig::schemaDirectory`.

### PLAN-021: Filename parsing
Derive table name from filename without extension.

### PLAN-022: CSV parsing
Implement a CSV parser that supports:

- header row;
- quoted fields;
- optional empty metadata fields;
- optional `#` comment lines if practical.

### PLAN-023: Required columns
Require:

```csv
column_name,offset,datatype,size,length
```

### PLAN-024: Optional columns
Allow:

```csv
unit,description
```

### PLAN-025: Datatype parser
Parse datatype strings case-insensitively or document exact lowercase requirement.

Recommended: accept lowercase only for clarity.

### PLAN-026: Numeric parser
Parse `offset`, `size`, and `length` as unsigned integer values.

### PLAN-027: Schema expansion
For every `ColumnSchema`, generate `ExpandedColumnSchema` entries:

- `length == 1`: `column_name`
- `length > 1`: `column_name_0`, `column_name_1`, ...

### PLAN-028: Store metadata
Store `unit` and `description` if provided.

## Phase 4: Implement schema validation

### PLAN-030: Validate table names
Allow only safe SQL identifiers:

```text
[A-Za-z_][A-Za-z0-9_]*
```

### PLAN-031: Validate column names
Apply the same identifier rule to base and expanded column names.

### PLAN-032: Reject timestamp conflict
Reject any expanded payload column named:

```text
timestamp_ms
```

### PLAN-033: Reject duplicates
Reject duplicate expanded columns in a table.

### PLAN-034: Validate datatype size
Check expected size for each datatype.

Recommended expected sizes:

| Type | Size |
|---|---:|
| int8/uint8 | 1 |
| int16/uint16 | 2 |
| int32/uint32/float | 4 |
| int64/uint64/double | 8 |

### PLAN-035: Validate length
`length` must be at least `1`.

### PLAN-036: Validate SQL Server table width
Validate expanded SQL columns including `timestamp_ms` before table creation.

Use named constants for the initial validation limits so tests and future changes can update them deliberately.

### PLAN-037: Validate prepared statement parameter count
Validate that the number of parameter markers required by the prepared insert is acceptable for SQL Server/ODBC.

Use named constants for the initial parameter-count limits so tests and future changes can update them deliberately.

### PLAN-038: Fail fast
If validation fails, initialization fails with a clear message.

## Phase 5: Implement binary decoder

### PLAN-040: Safe read helper
Implement a safe read helper:

```cpp
template<typename T>
T readValue(const void* base, std::size_t offset)
{
    T value{};
    std::memcpy(&value,
                static_cast<const unsigned char*>(base) + offset,
                sizeof(T));
    return value;
}
```

### PLAN-041: Decode expanded column
For each `ExpandedColumnSchema`, read the typed value from:

```text
base + expandedColumn.offset
```

### PLAN-042: Decode full row
Build a `DecodedRow` with:

- external `timestampMs`;
- vector of field values in expanded column order.

### PLAN-043: No pointer retention
Do not store user struct pointers after `write()` returns.

## Phase 6: Implement DataLogger

### PLAN-050: Constructor
Allow backend injection:

```cpp
explicit DataLogger(std::unique_ptr<IDBBackend> backend);
```

### PLAN-051: Initialize
In `initialize(config)`:

1. Load CSV schemas.
2. Validate schemas.
3. Store schema registry.
4. Connect backend.
5. Initialize/recreate tables.
6. Prepare insert statements.
7. Initialize table buffers.

### PLAN-052: Register table
Implement:

```cpp
TableHandle registerTable(const std::string& tableName);
```

### PLAN-053: Write
Implement:

```cpp
bool write(TableHandle table, std::int64_t timestampMs, const void* structPtr);
```

Steps:

1. Validate initialized state.
2. Validate handle.
3. Decode row.
4. Append row to that table buffer.
5. If batch size reached, flush that table.

### PLAN-054: Clock ownership
Do not implement production logger wall-clock reads or time-based flush checks.

Example/test code may use wall-clock time only to produce caller-supplied timestamps.

### PLAN-055: Flush one table
Implement:

```cpp
bool flush(TableHandle table);
```

If buffer is empty, return success.

If backend insert succeeds, clear buffer.

If backend insert fails, keep buffer and return false.

### PLAN-056: Flush all tables
Implement:

```cpp
bool flush();
```

Flush all non-empty table buffers.

### PLAN-057: Shutdown
Implement explicit `shutdown()` to flush and disconnect/cleanup.

## Phase 7: Implement SQL Server ODBC backend

### PLAN-060: RAII handle wrappers
Implement RAII wrappers for:

- environment handle;
- connection handle;
- statement handle.

### PLAN-061: Connect
Use ODBC connection string and allocate/connect handles.

### PLAN-062: Diagnostics
Implement a utility to collect ODBC diagnostics using `SQLGetDiagRec`.

### PLAN-063: Existing table policy
For each table:

- if drop policy: drop old table if exists;
- if rename policy: rename old table with suffix, then create fresh table.

### PLAN-064: Generate CREATE TABLE
Generate table DDL from `TableSchema`.

### PLAN-065: Generate timestamp index
Create index on `timestamp_ms`.

### PLAN-066: SQL type mapping
Implement type mapping from `DataType` to SQL Server type.

Recommended:

| DataType | SQL Server |
|---|---|
| Int8 | SMALLINT |
| UInt8 | TINYINT |
| Int16 | SMALLINT |
| UInt16 | INT |
| Int32 | INT |
| UInt32 | BIGINT |
| Int64 | BIGINT |
| UInt64 | DECIMAL(20,0) |
| Float | REAL |
| Double | FLOAT(53) |

### PLAN-067: Prepare insert statements
For each table, prepare:

```sql
INSERT INTO [schema].[table] ([timestamp_ms], [col1], [col2], ...)
VALUES (?, ?, ?, ...);
```

### PLAN-068: Bind parameter arrays
For each flush batch:

1. Allocate one array per SQL column.
2. Fill arrays from decoded rows.
3. Allocate indicator arrays.
4. Set `SQL_ATTR_PARAMSET_SIZE` to row count.
5. Set column-wise binding.
6. Bind each parameter.
7. Execute statement.

### PLAN-069: Transaction
For each batch:

1. Disable autocommit or begin manual commit mode.
2. Execute ODBC batch.
3. Commit if success.
4. Roll back if failure.
5. Restore state if required.

### PLAN-070: Handle uint64
Implement a correct strategy for `uint64` -> `DECIMAL(20,0)`.

Recommended path:

- use `SQL_NUMERIC_STRUCT`, or
- implement a clearly documented conversion path accepted by ODBC/SQL Server.

Do not silently cast full-range `uint64` to signed `int64`.

## Phase 8: Example app

### PLAN-080: Define example struct
Example:

```cpp
#pragma pack(push, 1)
struct ImuData
{
    std::int64_t unusedOrSequence;
    float gyro[3];
    float accel[3];
    double temperature;
    std::uint16_t status;
};
#pragma pack(pop)
```

The actual offsets in CSV must match the chosen struct layout.

### PLAN-081: Example CSV
Create:

```text
ExampleApp/schemas/imu_data.csv
```

### PLAN-082: Example initialization
Show config setup, backend creation, logger initialization, table registration, writes, and flush.

### PLAN-083: Example SQL query for Grafana
Provide a sample query:

```sql
SELECT
    DATEADD(ms, [timestamp_ms] % 1000,
        DATEADD(second, [timestamp_ms] / 1000, '1970-01-01')) AS time,
    [gyro_0]
FROM [dbo].[imu_data]
ORDER BY [timestamp_ms];
```

## Phase 9: Testing checklist

### PLAN-090: Unit test schema parsing
Test valid and invalid CSV files.

### PLAN-091: Unit test expansion
Test scalar and array flattening.

### PLAN-092: Unit test duplicate rejection
Test duplicate columns after expansion.

### PLAN-093: Unit test binary decoding
Use a known struct and verify decoded values.

### PLAN-094: Integration test table creation
Connect to a test SQL Server database and verify generated tables.

### PLAN-095: Integration test insert
Insert a small batch and verify rows in SQL Server.

### PLAN-096: Failure test rollback
Force backend failure and verify buffer remains and no partial batch is committed.

### PLAN-097: Wide schema test
Create a schema exceeding configured SQL limits and verify initialization fails clearly.

## Phase 10: Suggested implementation order

1. Implement schema model.
2. Implement CSV parser.
3. Implement schema validator.
4. Implement binary decoder.
5. Implement DataLogger with a temporary fake backend for local testing.
6. Implement ODBC diagnostics and handle wrappers.
7. Implement SQL table creation.
8. Implement prepared insert statements.
9. Implement parameter array binding.
10. Add transaction handling.
11. Add example app.
12. Add Visual Studio 2019 solution/project files.
13. Add tests or manual validation utilities.
