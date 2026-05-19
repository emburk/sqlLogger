# Phase 13: C Compatibility API

## Phase objective

Add C-callable public headers and wrapper implementations so C applications can
consume the existing static libraries without including C++ headers.

The phase is additive. It does not replace the C++ public API, and it keeps the
existing C++ examples unchanged.

## Design notes

- `DataLoggerCore` remains backend-agnostic and still depends only on
  `IDBBackend`.
- `SqlServerBackend` still owns construction of the concrete
  `SqlServerOdbcBackend`.
- The C API uses opaque handles so C callers do not depend on C++ object layout.
- The C API uses prefixed function names instead of namespaces or method-style
  calls.
- The C API uses integer success values: `1` for success and `0` for failure.
- Error strings are copied into a caller-owned buffer. Callers may pass
  `NULL, 0` to query the required byte count, including the null terminator.
- The implementation remains single-threaded and does not add locks, async
  behavior, background flushes, or new ownership threads.

## Public C API ownership model

The SQL Server backend library creates a backend handle:

```c
DataLoggerBackend_c* backend = sqlserver_backend_create_c();
```

The core logger creation function takes ownership of that backend handle:

```c
DataLogger_c* logger = datalogger_create_c(backend);
```

After successful `datalogger_create_c()`, callers must not destroy or reuse the
backend handle directly. If backend creation succeeds but logger creation is not
called, the caller can release the backend with:

```c
sqlserver_backend_destroy_c(backend);
```

The logger is released through:

```c
datalogger_destroy_c(logger);
```

## Public headers

- `DataLoggerCore/include/DataLogger/DataLoggerC.h`
  - Pure C logger facade.
  - Includes only C-compatible standard headers.
  - Exposes `DataLogger_c`, `DataLoggerBackend_c`, `DataLoggerConfig_c`, and
    `DataLoggerTableHandle_c`.
  - Exposes init, registration, write, flush, shutdown, destroy, and error
    retrieval functions.
- `SqlServerBackend/include/SqlServerBackend/SqlServerOdbcBackendC.h`
  - Pure C SQL Server backend factory facade.
  - Includes `DataLogger/DataLoggerC.h`.
  - Exposes backend create/destroy functions.
- `DataLoggerCore/include/DataLogger/detail/DataLoggerCBridge.h`
  - Private C++ bridge header.
  - Must not be included from C.
  - Owns the internal `std::unique_ptr<DataLoggerCore::IDBBackend>` inside the
    opaque backend handle.

## Wrapper implementations

- `DataLoggerCore/src/DataLoggerC.cpp`
  - Converts `DataLoggerConfig_c` into `DataLoggerCore::DataLoggerConfig`.
  - Converts `DataLoggerTableHandle_c` to and from the C++ `TableHandle`.
  - Owns a `std::unique_ptr<DataLoggerCore::DataLogger>` inside `DataLogger_c`.
  - Stores wrapper-level errors for failures that happen before the C++ logger
    can record an error.
- `SqlServerBackend/src/SqlServerOdbcBackendC.cpp`
  - Constructs `SqlServerBackend::SqlServerOdbcBackend`.
  - Wraps it in the shared opaque backend handle used by the core C facade.

## Important behavior

- `datalogger_config_default_c()` fills a C config object with the same defaults
  as the C++ config:
  - `sqlSchemaName = "dbo"`;
  - `batchSizeRows = 100`;
  - existing-table policy is rename with timestamp suffix;
  - info and error printing are enabled.
- `DataLoggerConfig_c.connectionString` and
  `DataLoggerConfig_c.schemaDirectory` default to `NULL`, and callers must set
  them before initialization.
- `datalogger_get_error_string_c()` returns the required byte count. If the
  supplied buffer is too small, the copied string is truncated but still
  null-terminated when `bufferSize > 0`.
- `datalogger_shutdown_c()` forwards to the C++ logger shutdown behavior. If a
  flush fails, rows remain available inside the logger just as they do through
  the C++ API.

## Project wiring

The new headers and source files were added to:

- `DataLoggerCore/DataLoggerCore.vcxproj`
- `DataLoggerCore/DataLoggerCore.vcxproj.filters`
- `SqlServerBackend/SqlServerBackend.vcxproj`
- `SqlServerBackend/SqlServerBackend.vcxproj.filters`
- `DataLogger.vcxproj`
- `DataLogger.vcxproj.filters`

The existing `ExampleAppLib` source remains C++ and continues to include the
existing C++ headers.

`ExampleAppLibC` was added as a separate C example solution. It mirrors the
static-library linkage model from `ExampleAppLib`, but compiles `main.c` and
includes only the C facade headers for logger/backend access.

## C example behavior

- `ExampleAppLibC/main.c` reads `SQLLOGGER_CONNECTION_STRING` from the process
  environment.
- It creates a SQL Server backend with `sqlserver_backend_create_c()`.
- It transfers backend ownership into `datalogger_create_c()`.
- It initializes the logger from `DataLoggerConfig_c`.
- It registers the `imu_data` table through `datalogger_register_table_c()`.
- It writes two packed `ImuData` rows through `datalogger_write_c()`.
- It flushes and shuts down through the C facade.
- It reports logger errors through `datalogger_get_error_string_c()`.

## Verification

Release x64 verification was performed with Visual Studio 2019 MSBuild:

- `SqlServerBackend/SqlServerBackend.vcxproj` built successfully with
  0 warnings and 0 errors.
- `ExampleAppLib/ExampleAppLib.sln` built successfully with 0 errors and one
  `LNK4020` PDB warning.
- `ExampleAppLibC/ExampleAppLibC.sln` built successfully with 0 warnings and
  0 errors. The application source compiled as C using `/TC`.
- `DataLogger.vcxproj` built successfully with 0 warnings and 0 errors.
- `DataLoggerC.h` and `SqlServerOdbcBackendC.h` were preprocessed as C headers
  using `/TC`.

The local environment reports that `pwsh.exe` is missing during vcpkg applocal,
then falls back to Windows PowerShell. The builds still complete successfully.

## Files changed

- `DataLoggerCore/include/DataLogger/DataLoggerC.h`
- `DataLoggerCore/include/DataLogger/detail/DataLoggerCBridge.h`
- `DataLoggerCore/src/DataLoggerC.cpp`
- `SqlServerBackend/include/SqlServerBackend/SqlServerOdbcBackendC.h`
- `SqlServerBackend/src/SqlServerOdbcBackendC.cpp`
- `DataLoggerCore/DataLoggerCore.vcxproj`
- `DataLoggerCore/DataLoggerCore.vcxproj.filters`
- `SqlServerBackend/SqlServerBackend.vcxproj`
- `SqlServerBackend/SqlServerBackend.vcxproj.filters`
- `DataLogger.vcxproj`
- `DataLogger.vcxproj.filters`
- `ExampleAppLibC/main.c`
- `ExampleAppLibC/ExampleAppLibC.sln`
- `ExampleAppLibC/ExampleAppLibC.vcxproj`
- `ExampleAppLibC/ExampleAppLibC.vcxproj.filters`
- `docs/01_REQUIREMENTS.md`
- `docs/02_ARCHITECTURE.md`
- `docs/03_DESIGN_DECISIONS.md`
- `docs/05_IMPLEMENTATION_PLAN.md`
