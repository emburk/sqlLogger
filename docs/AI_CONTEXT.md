# AI Context: DataLogger Project

This document is the high-level context file for Codex or another code-generation agent. It summarizes the intended project and points to the detailed numbered documents in `docs/`.

## Project identity

The project is a C++17 / Visual Studio 2019 DataLogger system for logging fixed-layout binary telemetry structs into Microsoft SQL Server through a real ODBC backend.

The system must prioritize maintainability, readability, deterministic behavior, and a clean separation between telemetry decoding and database transport. It is not intended to be an over-engineered streaming framework.

## Main architectural idea

The application owns the telemetry structs and calls the logger row-by-row. The `DataLogger` owns schema loading, fixed-offset decoding, row buffering, batching, timestamp insertion, and table-handle routing. The SQL backend owns only SQL Server / ODBC concerns.

```text
Application
  -> DataLogger
       -> CSV Schema Registry
       -> fixed-offset decoder
       -> per-table row buffers
       -> flush policy
  -> IDBBackend
       -> SqlServerOdbcBackend
       -> ODBC parameter array binding
  -> Microsoft SQL Server
```

## Final confirmed choices

- Language: C++17.
- IDE/project target: Visual Studio 2019, x64 solution with one C++ project initially.
- Build shape: `DataLoggerCore` and `SqlServerBackend` have standalone static-library projects, while retained examples live under `examples/`.
- Database: Microsoft SQL Server.
- Database access: real ODBC implementation.
- Backend interface: `IDBBackend` abstraction with `SqlServerOdbcBackend` implementation.
- Insert mode: batched insert.
- ODBC batch mode: parameter array binding.
- External schema format: CSV files, not JSON.
- Schema ownership: `DataLogger` loads all CSV schema files during initialization, owns the immutable schema registry, and does not reload schemas during runtime.
- One CSV file equals one SQL table.
- CSV filename without extension equals table name.
- CSV files are loaded from a schema directory.
- Struct mapping: fixed offsets; caller guarantees that offsets, padding, sizes, and pointer validity are correct.
- Timestamp: passed explicitly by the application for every write call; not inside the struct and not inside the CSV schema.
- Timestamp storage: `timestamp_ms BIGINT NOT NULL`, representing milliseconds since Unix epoch.
- Table selection API: registered table handle.
- Arrays: flattened with suffixes `_0`, `_1`, `_2`, ...
- Supported payload types: `int8`, `uint8`, `int16`, `uint16`, `int32`, `uint32`, `int64`, `uint64`, `float`, `double`.
- Strings: not supported initially.
- Metadata: optional `unit` and `description` columns may exist in CSV, but they do not affect SQL insertion.
- Threading: single-threaded.
- Flush policy: batch-size-only; flush when the configured batch size is reached or through explicit manual flush.
- Production logger code must not read wall-clock time. Example/test code may use wall clock only to produce external timestamps supplied by the caller.
- Flush failure behavior: return error and keep buffer.
- To make “keep buffer on failure” safe, backend batch execution must use a transaction and roll back failed batches.
- Table initialization: delete existing tables or rename them with suffix `YYYYMMDD-hhmmss` before creating new tables, controlled by an option flag.

## Important implementation constraint

SQL Server has practical wide-table and parameter-count limits. The implementation must validate expanded table schemas before creating SQL tables or preparing ODBC statements. If a CSV expands to too many SQL columns, the program must fail early with a clear error message rather than creating a broken logger configuration.

At minimum, validate:

- expanded SQL columns per table, including `timestamp_ms`;
- prepared-statement parameter count;
- duplicated column names after array expansion;
- valid SQL identifier names.

If a telemetry struct expands beyond SQL Server limits, the design should be extended by splitting the telemetry into multiple table CSV files or by adding another ingestion mode later. Do not silently truncate fields.

## Document map

- `docs/01_REQUIREMENTS.md`: full requirements with stable IDs.
- `docs/02_ARCHITECTURE.md`: architecture, components, data flow, APIs.
- `docs/03_DESIGN_DECISIONS.md`: final decisions and rationale.
- `docs/04_PROJECT_SUMMARY.md`: concise project summary.
- `docs/05_IMPLEMENTATION_PLAN.md`: suggested build-ready implementation plan.
