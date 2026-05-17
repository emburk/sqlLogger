# 02 Architecture

This document describes the architecture of the DataLogger project.

## 1. Architecture overview

The DataLogger is a schema-driven telemetry ingestion system. It decodes fixed-layout C/C++ structs using external CSV schemas, buffers rows per table, and writes batches to Microsoft SQL Server using a real ODBC backend.

```text
+----------------------+       +---------------------------+
| Main Application     |       | schemas/*.csv             |
| - owns structs       |       | one CSV = one SQL table   |
| - supplies timestamp |       +-------------+-------------+
+----------+-----------+                     |
           |                                 v
           | write(handle, timestamp, ptr)   |
           v                                 |
+----------------------------------------------------------+
| DataLogger                                               |
|----------------------------------------------------------|
| - loads CSV schemas                                      |
| - owns immutable schema registry                         |
| - registers table handles                                |
| - decodes fixed-offset binary structs                    |
| - flattens arrays                                        |
| - stores timestamp_ms                                    |
| - buffers decoded rows per table                         |
| - triggers flush by batch size or explicit flush call    |
+-----------------------------+----------------------------+
                              |
                              | insertBatch(table, rows)
                              v
+----------------------------------------------------------+
| IDBBackend                                               |
|----------------------------------------------------------|
| Abstract persistence interface.                          |
| Does not know about structs, offsets, or telemetry types. |
+-----------------------------+----------------------------+
                              |
                              v
+----------------------------------------------------------+
| SqlServerOdbcBackend                                     |
|----------------------------------------------------------|
| - manages ODBC connection                                |
| - creates/renames/drops SQL tables                       |
| - prepares INSERT statements                             |
| - converts decoded row batches to ODBC parameter arrays  |
| - executes batch inside transaction                      |
+-----------------------------+----------------------------+
                              |
                              v
+----------------------+       +---------------------------+
| Microsoft SQL Server |<------| ODBC Driver               |
+----------------------+       +---------------------------+
```

## 2. Responsibility split

### ARCH-001: DataLogger responsibility
`DataLogger` is responsible for understanding telemetry schema and converting user-provided struct memory into decoded rows.

It owns:

- schema loading;
- schema validation;
- table handle registration;
- fixed-offset decoding;
- array flattening;
- timestamp insertion;
- per-table buffers;
- batch-size flush checks;
- flush orchestration.

### ARCH-002: Backend responsibility
`IDBBackend` and `SqlServerOdbcBackend` are responsible for database transport only.

The backend owns:

- SQL Server connection;
- ODBC environment/connection/statement handles;
- table creation;
- table deletion/rename policy execution;
- prepared insert statements;
- ODBC parameter array binding;
- transactions;
- SQL diagnostics.

### ARCH-003: Backend does not decode structs
The backend must not know C++ struct offsets, padding, or raw telemetry memory. It receives already decoded row data and schema metadata.

## 3. Main runtime flow

### 3.1 Initialization flow

```text
DataLoggerConfig
   |
   v
DataLogger::initialize(config)
   |
   +--> SchemaDirectoryLoader loads all CSV files
   |
   +--> SchemaRegistry validates tables and expanded columns
   |
   +--> SqlServerOdbcBackend connects through ODBC
   |
   +--> Existing table policy is applied
   |       - DROP existing table
   |       - or RENAME existing table with YYYYMMDD-hhmmss suffix
   |
   +--> Backend creates fresh SQL tables
   |
   +--> Backend prepares one INSERT statement per table
   |
   +--> DataLogger returns initialized state
```

### 3.2 Table registration flow

```cpp
TableHandle imu = logger.registerTable("imu_data");
```

The handle maps to an internal table index. This avoids repeated string lookups during high-frequency or repeated writes.

### 3.3 Write flow

```cpp
logger.write(imu, timestampMs, &imuStruct);
```

The logger performs:

```text
validate handle
   -> find table schema
   -> decode struct fields by offset
   -> flatten array fields
   -> prepend timestamp_ms
   -> append decoded row to table buffer
   -> if buffer.size >= batchSize: flush table
```

### 3.4 Clock ownership

Production logger code does not read wall-clock time. The application supplies `timestampMs` for storage, and example/test code may use wall clock only to produce those caller-supplied timestamps.

Pending buffers are flushed automatically by batch size or manually through `flush()`.

### 3.5 Manual flush flow

```cpp
logger.flush();       // all tables
logger.flush(imu);    // one table
```

Flush sends decoded row batches to the backend. On success, the buffer is cleared. On failure, the buffer is kept.

## 4. Core class model

### ARCH-010: DataLoggerConfig
Suggested configuration object:

```cpp
enum class ExistingTablePolicy
{
    Drop,
    RenameWithTimestampSuffix
};

struct DataLoggerConfig
{
    std::string connectionString;
    std::string schemaDirectory;
    std::string sqlSchemaName = "dbo";

    std::size_t batchSizeRows = 100;

    ExistingTablePolicy existingTablePolicy = ExistingTablePolicy::RenameWithTimestampSuffix;
};
```

### ARCH-011: TableHandle
Suggested handle type:

```cpp
class TableHandle
{
public:
    static TableHandle invalid();
    bool isValid() const;

private:
    std::size_t index_;
};
```

A simple alternative is:

```cpp
using TableHandle = std::size_t;
```

but a wrapper type is more readable and safer.

### ARCH-012: DataLogger public API
Suggested public API:

```cpp
class DataLogger
{
public:
    bool initialize(const DataLoggerConfig& config);
    void shutdown();

    TableHandle registerTable(const std::string& tableName);

    bool write(TableHandle table,
               std::int64_t timestampMs,
               const void* structPtr);

    bool flush();
    bool flush(TableHandle table);

    const DataLoggerError& lastError() const;
};
```

### ARCH-013: Schema model
Suggested schema classes:

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

struct ColumnSchema
{
    std::string baseName;
    std::string unit;
    std::string description;

    std::size_t offset = 0;
    DataType datatype;
    std::size_t size = 0;
    std::size_t length = 1;
};

struct ExpandedColumnSchema
{
    std::string sqlName;
    std::size_t offset = 0;
    DataType datatype;
    std::size_t size = 0;
};

struct TableSchema
{
    std::string tableName;
    std::vector<ColumnSchema> columns;
    std::vector<ExpandedColumnSchema> expandedColumns;
};

struct SchemaRegistry
{
    std::vector<TableSchema> tables;
};
```

### ARCH-014: Decoded row model
A simple row representation may be variant-based:

```cpp
using FieldValue = std::variant<
    std::int8_t,
    std::uint8_t,
    std::int16_t,
    std::uint16_t,
    std::int32_t,
    std::uint32_t,
    std::int64_t,
    std::uint64_t,
    float,
    double
>;

struct DecodedRow
{
    std::int64_t timestampMs;
    std::vector<FieldValue> values;
};
```

The order of `values` must match `TableSchema::expandedColumns`.

### ARCH-015: Per-table buffer
Suggested buffer:

```cpp
struct TableBuffer
{
    TableHandle handle;
    const TableSchema* schema = nullptr;
    std::vector<DecodedRow> rows;
};
```

### ARCH-016: Backend interface
Suggested backend interface:

```cpp
class IDBBackend
{
public:
    virtual ~IDBBackend() = default;

    virtual bool connect(const std::string& connectionString) = 0;

    virtual bool initializeTables(
        const SchemaRegistry& registry,
        const std::string& sqlSchemaName,
        ExistingTablePolicy policy) = 0;

    virtual bool prepareInsertStatements(
        const SchemaRegistry& registry,
        const std::string& sqlSchemaName) = 0;

    virtual bool insertBatch(
        const TableSchema& table,
        const std::vector<DecodedRow>& rows) = 0;

    virtual BackendError lastError() const = 0;
};
```

## 5. CSV schema architecture

### ARCH-020: CSV directory
All CSV files in the schema directory are loaded.

Example:

```text
schemas/
  imu_data.csv
  orbit_data.csv
  thermal_data.csv
```

### ARCH-021: Filename to table mapping
Each filename without extension becomes the table name.

### ARCH-022: CSV example

```csv
column_name,offset,datatype,size,length,unit,description
gyro,8,float,4,3,rad/s,body angular rate
accel,20,float,4,3,m/s2,body acceleration
temperature,32,double,8,1,degC,electronics temperature
status,40,uint16,2,1,,status code
```

This expands to SQL columns:

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

## 6. SQL generation architecture

### ARCH-030: Create table pattern
The backend generates SQL like:

```sql
CREATE TABLE [dbo].[imu_data]
(
    [timestamp_ms] BIGINT NOT NULL,
    [gyro_0] REAL NOT NULL,
    [gyro_1] REAL NOT NULL,
    [gyro_2] REAL NOT NULL,
    [accel_0] REAL NOT NULL,
    [accel_1] REAL NOT NULL,
    [accel_2] REAL NOT NULL,
    [temperature] FLOAT(53) NOT NULL,
    [status] INT NOT NULL
);
```

### ARCH-031: Timestamp index
Create a nonclustered index on `timestamp_ms`:

```sql
CREATE INDEX [IX_imu_data_timestamp_ms]
ON [dbo].[imu_data] ([timestamp_ms]);
```

### ARCH-032: Existing table policy
If `ExistingTablePolicy::Drop`:

```sql
IF OBJECT_ID(N'[dbo].[imu_data]', N'U') IS NOT NULL
    DROP TABLE [dbo].[imu_data];
```

If `ExistingTablePolicy::RenameWithTimestampSuffix`, rename first, then create a fresh table.

Suggested suffix:

```text
imu_data_20260516-143012
```

## 7. ODBC parameter array binding architecture

### ARCH-040: Prepared insert SQL
For each table:

```sql
INSERT INTO [dbo].[imu_data]
(
    [timestamp_ms],
    [gyro_0], [gyro_1], [gyro_2],
    [accel_0], [accel_1], [accel_2],
    [temperature], [status]
)
VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);
```

### ARCH-041: Parameter count
The prepared insert has one parameter marker per SQL column.

### ARCH-042: Batch execution
The backend binds arrays of values to each parameter and executes the prepared statement for the whole parameter set.

### ARCH-043: Row-to-column conversion localized to backend
The public logger API remains row-oriented. During flush, the backend converts:

```text
row buffer:
  row0: timestamp, gyro_0, gyro_1, ...
  row1: timestamp, gyro_0, gyro_1, ...
```

into ODBC parameter arrays:

```text
timestamp_ms[]
gyro_0[]
gyro_1[]
...
```

This conversion is an implementation detail of `SqlServerOdbcBackend`.

### ARCH-044: Column-wise binding default
The initial implementation should use column-wise parameter array binding because it maps naturally to one array per SQL column.

### ARCH-045: Indicator arrays
Each parameter array shall have a corresponding ODBC indicator array. Since initial payload values are non-null numeric values, indicators will normally represent valid non-null values.

### ARCH-046: Processed/status diagnostics
The backend should use processed-row counters and parameter status arrays for diagnostics.

## 8. Error handling architecture

### ARCH-050: Error object
Use a small project-level error object rather than printing errors directly.

Suggested model:

```cpp
enum class ErrorCode
{
    None,
    InvalidConfig,
    SchemaLoadFailed,
    SchemaValidationFailed,
    InvalidTableHandle,
    DecodeFailed,
    BackendConnectFailed,
    BackendTableInitFailed,
    BackendPrepareFailed,
    BackendInsertFailed
};

struct DataLoggerError
{
    ErrorCode code = ErrorCode::None;
    std::string message;
};
```

### ARCH-051: ODBC diagnostics
Backend diagnostics should include:

```cpp
struct OdbcDiagnostic
{
    std::string sqlState;
    int nativeError = 0;
    std::string message;
};
```

### ARCH-052: No silent failures
No schema, SQL, ODBC, or insertion error shall be silently ignored.

## 9. SQL Server wide-table risk

### ARCH-060: Why this matters
The system can receive low-frequency data, around 10 Hz, but each struct can be large, up to around 16 KB. If the struct is flattened into many scalar SQL columns, SQL Server table-width and statement limits can become the dominant design constraint.

### ARCH-061: Required validation
The implementation must validate expanded table schemas before SQL generation.

At minimum, check:

- number of expanded columns including `timestamp_ms`;
- number of parameter markers in prepared insert;
- SQL Server table column limits;
- duplicate generated column names;
- invalid SQL identifiers.

### ARCH-062: If validation fails
If a schema is too wide, initialization must fail clearly. The recommended user action is to split one wide telemetry struct into multiple logical CSV/table schemas.

Example split:

```text
payload.csv
payload_power.csv
payload_thermal.csv
payload_attitude.csv
```

Each table receives the same external timestamp value from the main application.
