# sqlLogger Real-Time Safety Refactor Plan

## Global objective

Refactor `sqlLogger` so that a 20 Hz / 50 ms host application can call the logger without being exposed to SQL Server, ODBC, transaction commit, table flush, heap allocation bursts, or Grafana-related database load.

The real-time caller must be able to do only a bounded memory copy into a preallocated queue and return immediately. SQL work must happen outside the strict cycle.

Target platform and constraints:

* Visual Studio 2019
* Windows x64
* C++17
* Existing SQL Server ODBC backend must continue to work
* Existing C++ API should remain compatible where practical
* Existing C ABI should remain compatible where practical
* Add a new single-library C ABI entry point for easy C integration
* Avoid third-party dependencies unless absolutely necessary
* Each phase must compile and pass tests before moving to the next phase
* Commit after each phase with a clear commit message

Do not implement all phases in one large change. Advance phase by phase.

---

# Phase 0 — Baseline, instrumentation, and safety checks

## Objective

Create a measurable baseline before changing architecture. Add minimal diagnostics to prove where time is spent and to protect against regressions.

## Implementation tasks

1. Add lightweight timing counters around:

   * `DataLogger::write`
   * `DataLogger::autoWrite`
   * `DataLogger::flush`
   * decode path
   * backend `insertBatch`
   * ODBC buffer construction
   * `SQLExecute`
   * transaction commit / rollback path

2. Add a statistics struct, for example:

```cpp
struct DataLoggerStats {
    uint64_t writeCalls;
    uint64_t autoWriteCalls;
    uint64_t flushCalls;
    uint64_t rowsWritten;
    uint64_t rowsFlushed;
    uint64_t writeMaxNs;
    uint64_t autoWriteMaxNs;
    uint64_t flushMaxNs;
    uint64_t decodeMaxNs;
    uint64_t backendInsertMaxNs;
    uint64_t sqlExecuteMaxNs;
    uint64_t sqlCommitMaxNs;
};
```

3. Expose stats through:

   * C++ API
   * C ABI if simple
   * example app printout

4. Add a high-rate synthetic stress example:

   * configurable number of tables
   * configurable batch size
   * configurable payload size
   * configurable number of cycles
   * no real-time sleeping required initially

## Acceptance tests

1. Existing examples still compile.
2. Existing C++ library example still compiles.
3. Existing C facade example still compiles.
4. Normal SQL example still writes correct rows.
5. Stats counters increase as expected.
6. No behavior change to existing synchronous logger except added diagnostics.

## Commit

```text
phase0: add logger timing stats and baseline stress diagnostics
```

---

# Phase 1 — Add `AsyncDataLogger`

## Objective

Add an asynchronous logger wrapper so the real-time application can enqueue raw payloads and return immediately. The worker thread owns the existing `DataLogger` and performs decode, buffering, flushing, ODBC execution, and SQL commit outside the caller thread.

This is the highest-priority phase.

## Required behavior

The real-time caller must be able to call:

```cpp
asyncLogger.tryAutoWrite(timestampMs, &payload, sizeof(payload));
```

or from C:

```c
sql_logger_async_try_auto_write_c(logger, timestampMs, &payload, sizeof(payload));
```

The call must not:

* call SQL Server
* call ODBC
* flush tables
* commit transactions
* allocate heap memory during steady-state operation
* block waiting for queue space

If the queue is full, the function must return a failure/drop status immediately.

## Mandatory queue implementation

The AsyncDataLogger queue SHALL be implemented as a bounded
preallocated SPSC ring buffer.

Other Requirements:

- Single producer thread
- Single consumer thread
- No heap allocation after initialize()
- No mutexes in tryAutoWrite()
- No mutexes in tryWrite()
- No blocking operations in producer path
- No condition_variable wait in producer path
- Queue capacity fixed at initialization
- Power-of-two capacity preferred
- Queue full shall return immediately according to overflow policy
- Queue empty shall return immediately
- Producer cost must be bounded by:
    memcpy(payload)
    atomic head update

## Architecture

Add a new class:

```cpp
class AsyncDataLogger {
public:
    bool initialize(const DataLoggerConfig& dataConfig,
                    const AsyncDataLoggerConfig& asyncConfig);

    bool registerTable(const std::string& tableName, TableHandle* outHandle);
    bool autoRegisterTables();

    bool start();

    bool tryAutoWrite(std::int64_t timestampMs,
                      const void* structPtr,
                      std::size_t structSize) noexcept;

    bool tryWrite(TableHandle table,
                  std::int64_t timestampMs,
                  const void* structPtr,
                  std::size_t structSize) noexcept;

    bool requestFlush() noexcept;
    bool requestFlush(TableHandle table) noexcept;

    void stop();
    bool stopAndFlush();

    AsyncDataLoggerStats stats() const noexcept;
    const DataLoggerError& lastError() const;

private:
    void workerLoop();
};
```

The worker thread should be the only thread that calls the internal `DataLogger::write`, `DataLogger::autoWrite`, and `DataLogger::flush`.

## Async config

Add:

```cpp
enum class AsyncOverflowPolicy {
    DropNewest,
    DropOldest
};

enum class AsyncWorkerPriority {
    Normal,
    BelowNormal,
    Lowest
};

struct AsyncDataLoggerConfig {
    std::size_t queueCapacity = 512;
    std::size_t maxPayloadBytes = 0;
    AsyncOverflowPolicy overflowPolicy = AsyncOverflowPolicy::DropNewest;
    AsyncWorkerPriority workerPriority = AsyncWorkerPriority::BelowNormal;
    bool flushOnStop = true;
    bool autoRegisterTablesOnStart = false;
};
```

`maxPayloadBytes` must be known at initialization so the queue can be fully preallocated.

## Queue design

Use a bounded preallocated queue.

Start with SPSC if the expected caller is one real-time producer thread and one logger worker thread.

Queue slot:

```cpp
enum class AsyncLogOperation {
    AutoWrite,
    Write,
    FlushAll,
    FlushTable,
    Stop
};

struct AsyncLogSlot {
    AsyncLogOperation operation;
    TableHandle table;
    std::int64_t timestampMs;
    std::size_t payloadSize;
    std::byte* payloadStorage;
};
```

Prefer a contiguous allocation:

```text
queueCapacity × maxPayloadBytes
```

Do not allocate in `tryAutoWrite()` or `tryWrite()`.

## Overflow policy

Default:

```text
DropNewest
```

That means if the queue is full, the new sample is rejected and a dropped-sample counter increments.

Do not block the real-time caller.

Stats should include:

```cpp
struct AsyncDataLoggerStats {
    uint64_t pushedSamples;
    uint64_t droppedSamples;
    uint64_t poppedSamples;
    uint64_t autoWriteOps;
    uint64_t writeOps;
    uint64_t flushRequests;
    uint64_t workerErrors;
    uint64_t maxQueueDepth;
    uint64_t currentQueueDepth;
    uint64_t tryPushMaxNs;
    uint64_t workerLoopMaxNs;
    uint64_t flushMaxNs;
};
```

## Worker priority

On Windows, implement worker priority using `SetThreadPriority` inside the worker thread.

Allowed values:

* `THREAD_PRIORITY_NORMAL`
* `THREAD_PRIORITY_BELOW_NORMAL`
* `THREAD_PRIORITY_LOWEST`

Do not use real-time priority.

If setting priority fails, record an error or warning but do not fail the logger unless the config explicitly requests strict priority setup.

## Single-library C ABI

Add a combined C facade target that produces one user-facing library.

Suggested library name:

```text
SqlLoggerC
```

Suggested public header:

```c
SqlLoggerC.h
```

This combined C facade should hide C++ internals and hide backend construction from the C caller.

Expose opaque handles:

```c
typedef struct SqlLoggerAsync_c SqlLoggerAsync_c;
typedef struct SqlLogger_c SqlLogger_c;
```

Expose C config structs:

```c
typedef struct DataLoggerConfig_c {
    const char* connection_string;
    const char* schema_directory;
    const char* sql_schema_name;
    size_t batch_size_rows;
    int existing_table_policy;
    int print_info_flag;
    int print_error_flag;
} DataLoggerConfig_c;

typedef struct AsyncDataLoggerConfig_c {
    size_t queue_capacity;
    size_t max_payload_bytes;
    int overflow_policy;
    int worker_priority;
    int flush_on_stop;
    int auto_register_tables_on_start;
} AsyncDataLoggerConfig_c;
```

Add C functions:

```c
int sql_logger_async_create_c(
    const DataLoggerConfig_c* data_config,
    const AsyncDataLoggerConfig_c* async_config,
    SqlLoggerAsync_c** out_logger);

int sql_logger_async_destroy_c(SqlLoggerAsync_c* logger);

int sql_logger_async_start_c(SqlLoggerAsync_c* logger);

int sql_logger_async_stop_c(SqlLoggerAsync_c* logger);

int sql_logger_async_stop_and_flush_c(SqlLoggerAsync_c* logger);

int sql_logger_async_auto_register_tables_c(SqlLoggerAsync_c* logger);

int sql_logger_async_register_table_c(
    SqlLoggerAsync_c* logger,
    const char* table_name,
    DataLoggerTableHandle_c* out_handle);

int sql_logger_async_try_auto_write_c(
    SqlLoggerAsync_c* logger,
    int64_t timestamp_ms,
    const void* struct_ptr,
    size_t struct_size);

int sql_logger_async_try_write_c(
    SqlLoggerAsync_c* logger,
    DataLoggerTableHandle_c table,
    int64_t timestamp_ms,
    const void* struct_ptr,
    size_t struct_size);

int sql_logger_async_request_flush_c(SqlLoggerAsync_c* logger);

int sql_logger_async_get_stats_c(
    SqlLoggerAsync_c* logger,
    AsyncDataLoggerStats_c* out_stats);

size_t sql_logger_async_get_error_string_c(
    const SqlLoggerAsync_c* logger,
    char* buffer,
    size_t buffer_size);
```

Status codes should be explicit:

```c
#define SQLLOGGER_OK 0
#define SQLLOGGER_ERROR 1
#define SQLLOGGER_QUEUE_FULL 2
#define SQLLOGGER_INVALID_ARGUMENT 3
#define SQLLOGGER_NOT_INITIALIZED 4
#define SQLLOGGER_ALREADY_STARTED 5
#define SQLLOGGER_PAYLOAD_TOO_LARGE 6
```

## Phase 1 tests

### Build tests

1. Build original monolithic solution.
2. Build C++ library solution.
3. Build C facade solution.
4. Build new single-library C facade target.

### Unit-style tests without SQL

Use a mock backend if available or add a minimal test backend.

Test:

1. `tryAutoWrite()` succeeds when queue has space.
2. `tryAutoWrite()` returns queue-full status when queue is full.
3. Queue-full path does not block.
4. Worker drains queue.
5. `tryWrite()` with table handle reaches the worker and calls the correct table.
6. `requestFlush()` is processed by the worker.
7. `stop()` exits worker without deadlock.
8. `stopAndFlush()` flushes pending rows.
9. Worker priority config does not break startup.
10. Payload larger than `maxPayloadBytes` is rejected immediately.

### Real SQL smoke test

Use `SQLLOGGER_CONNECTION_STRING`.

Test:

1. Create async logger.
2. Auto-register all tables.
3. Start worker.
4. Push 1000 samples.
5. Stop and flush.
6. Verify row count in each table equals 1000 minus dropped samples.
7. Verify sample values match expected decoded struct values.

### Real-time safety test

Create a synthetic 20 Hz style loop without sleeping:

1. Call `tryAutoWrite()` 100000 times.
2. Record max and p99 call duration.
3. Verify no SQL call occurs on producer thread.
4. Verify no heap allocation occurs in steady-state producer path if practical to instrument.
5. Verify queue drops are counted, not blocking.

## Commit

```text
phase1: add AsyncDataLogger with bounded queue and single-library C ABI
```

---

# Phase 2 — Pre-reserve table row buffers

## Objective

Reduce avoidable vector reallocations in the existing synchronous and async worker paths.

## Implementation tasks

1. In table buffer initialization, reserve row capacity:

```cpp
buffer.rows.reserve(config.batchSizeRows);
```

2. After successful flush, clear rows but do not shrink capacity:

```cpp
buffer.rows.clear();
```

3. Never call `shrink_to_fit()` in normal runtime.
4. Add debug stats for:

   * per-table current row count
   * per-table row capacity
   * row buffer reallocation count if practical

## Tests

1. Initialize logger with `batchSizeRows = 100`.
2. Confirm each registered table buffer has capacity at least 100.
3. Write and flush several batches.
4. Confirm capacity remains stable after flush.
5. Confirm SQL output remains identical.
6. Confirm async worker path still passes.

## Expected impact

Low implementation cost. Medium benefit. This reduces allocation spikes but does not solve SQL blocking by itself.

## Commit

```text
phase2: reserve per-table row buffers at initialization
```

---

# Phase 3 — Avoid allocating `DecodedRow::values` on every write

## Objective

Remove the per-row allocation caused by each `DecodedRow` owning a separate `std::vector<FieldValue>`.

The current row model creates a vector of variants per decoded row. This phase should replace that with preallocated batch storage.

## Preferred internal representation

Introduce a table batch buffer like:

```cpp
struct DecodedBatch {
    const TableSchema* schema = nullptr;
    std::size_t rowCount = 0;
    std::size_t rowCapacity = 0;
    std::size_t columnCount = 0;

    std::vector<std::int64_t> timestamps;
    std::vector<FieldValue> valuesFlat;
};
```

Storage layout:

```text
valuesFlat[rowIndex * columnCount + columnIndex]
```

This keeps one flat vector per table batch instead of one vector per row.

## Implementation tasks

1. Replace or supplement `std::vector<DecodedRow> rows` in `TableBuffer`.
2. Preallocate:

   * `timestamps.resize(batchSizeRows)`
   * `valuesFlat.resize(batchSizeRows * expandedColumnCount)`
3. Modify decoder to append directly into a preallocated slot:

```cpp
bool decodeIntoBatchSlot(
    const TableSchema& schema,
    const void* structPtr,
    std::int64_t timestampMs,
    DecodedBatch& batch);
```

4. Increment `rowCount` after successful decode.
5. On flush success, set `rowCount = 0`; do not free memory.
6. Keep the public C++ and C API unchanged.
7. If the backend still expects `std::vector<DecodedRow>`, add a temporary adapter only inside the worker/backend boundary, but avoid using it long term.

## Tests

1. Decode one row and compare values with the old path.
2. Decode a full batch and compare values with the old path.
3. Flush partial batch.
4. Flush full batch.
5. Flush repeatedly and verify capacity remains stable.
6. Test all supported numeric types:

   * int8
   * uint8
   * int16
   * uint16
   * int32
   * uint32
   * int64
   * uint64
   * float
   * double
7. Test array flattening order.
8. Test split schema with many tables.
9. Verify no per-row `values` vector allocation remains in the hot path.

## Expected impact

Medium to high. This reduces CPU jitter and heap allocator jitter in the logger worker. If synchronous API is still used, it also improves synchronous write behavior.

## Commit

```text
phase3: replace per-row value vectors with preallocated decoded batch storage
```

---

# Phase 4 — Reuse ODBC batch buffers

## Objective

Stop allocating and rebuilding full ODBC parameter-array buffers during every flush.

ODBC buffers should be allocated once per prepared table and reused across flushes.

## Implementation tasks

1. Add a per-table backend state:

```cpp
struct PreparedTableState {
    SQLHSTMT stmt = SQL_NULL_HSTMT;
    std::size_t capacityRows = 0;
    std::size_t columnCount = 0;
    bool parametersBound = false;

    OdbcBatchBuffers buffers;
};
```

2. Allocate `OdbcBatchBuffers` during `prepareInsertStatements()`, not during every `insertBatch()`.

3. Bind parameter arrays once if possible.

4. On each flush:

   * copy/fill only the first `rowCount` entries
   * set `SQL_ATTR_PARAMSET_SIZE` to `rowCount`
   * call `SQLExecute`
   * commit or rollback
   * keep buffers allocated

5. Support partial flush where `rowCount < batchSizeRows`.

6. Ensure `uint64` / `DECIMAL(20,0)` conversion buffers are also reused.

7. Ensure string/identifier SQL construction remains unchanged.

8. Add stats:

   * ODBC buffer allocation count
   * max buffer fill time
   * max SQL execute time
   * max commit time

## Tests

1. Real SQL insert with one row.
2. Real SQL insert with partial batch.
3. Real SQL insert with exact full batch.
4. Multiple batches.
5. Many tables.
6. All numeric types.
7. `uint64` values around:

   * 0
   * INT64_MAX
   * UINT64_MAX
8. Validate row counts and values.
9. Verify ODBC buffer allocation happens at initialization/prepare time, not every flush.
10. Verify repeated flushes do not increase buffer capacity or reallocate.

## Expected impact

Medium to high. This reduces flush-time burst cost. It does not remove SQL execute/commit latency, but makes the local CPU/allocation part of flush much smoother.

## Commit

```text
phase4: reuse ODBC parameter-array buffers across flushes
```

---

# Phase 5 — Preallocate typed column batches and fill columns during each write

## Objective

Move from row-oriented decoded storage to column-oriented typed storage, so the logger fills SQL-ready column batches gradually at each cycle rather than doing row-to-column conversion as a burst at flush time.

This is the final high-performance internal representation.

## Design principle

Do not store a `std::variant` per value.

Instead, store one typed vector per column.

A per-column variant is acceptable:

```cpp
using ColumnStorage = std::variant<
    std::vector<std::int8_t>,
    std::vector<std::uint8_t>,
    std::vector<std::int16_t>,
    std::vector<std::uint16_t>,
    std::vector<std::int32_t>,
    std::vector<std::uint32_t>,
    std::vector<std::int64_t>,
    std::vector<std::uint64_t>,
    std::vector<float>,
    std::vector<double>
>;
```

This means:

```text
bad old model:
    one variant per value

better new model:
    one variant per column
    typed contiguous values per column
```

## Suggested structure

```cpp
struct ColumnBatch {
    const TableSchema* schema = nullptr;
    std::size_t rowCount = 0;
    std::size_t rowCapacity = 0;

    std::vector<std::int64_t> timestamps;
    std::vector<ColumnStorage> columns;
};
```

Each column vector has `batchSizeRows` capacity or size.

## Implementation tasks

1. Add `ColumnBatch` to core.
2. Initialize one `ColumnBatch` per table.
3. Preallocate:

   * timestamp vector
   * one typed vector per expanded SQL column
4. Modify decoder to fill the next row index directly:

```cpp
bool decodeIntoColumnBatch(
    const TableSchema& schema,
    const void* structPtr,
    std::int64_t timestampMs,
    ColumnBatch& batch);
```

5. For each expanded column:

   * read field from struct by fixed offset using `memcpy`
   * write directly to the typed column vector at `rowIndex`
6. Remove row-to-column conversion from flush path.
7. Add backend overload:

```cpp
bool insertBatch(const TableSchema& table, const ColumnBatch& batch);
```

8. Modify SQL Server backend to bind directly from column batch storage where possible.
9. Reuse ODBC indicator arrays and conversion buffers.
10. For now, assume all values are valid/non-null unless future config enables null handling.

## Null/no-data policy

Because the intended real-time telemetry normally has all data valid:

1. Default mode should be `AllValuesValid`.
2. Do not add per-value null checks in the hot path unless explicitly configured.
3. If future null support is required, add a validity bitmap or indicator buffer per column, not a per-value variant.

## Tests

1. Compare output of old row-based path and new column-based path on identical payloads.
2. Verify all numeric types.
3. Verify arrays are flattened in the same column order.
4. Verify many-table `autoWrite` still writes all tables.
5. Verify manual `write(handle, ...)` still writes only selected table.
6. Verify partial batch flush.
7. Verify full batch flush.
8. Verify repeated batches do not allocate in steady state.
9. Verify SQL row counts and values.
10. Verify async logger uses the new column batch path correctly.
11. Stress test:

    * 20 Hz equivalent
    * many tables
    * large payload
    * long run
    * collect p99/max `tryAutoWrite` time
    * collect worker queue depth
    * collect max flush time

## Expected impact

High. This removes the main local CPU/allocation burst before SQL execution. Combined with async logging, it makes the real-time caller bounded and makes the worker smoother.

## Commit

```text
phase5: fill preallocated typed column batches during decode
```

---

# Final integration test plan

After all phases, run the following full test matrix.

## Build matrix

1. Debug x64
2. Release x64
3. Original monolithic example
4. C++ static library example
5. Existing C facade example
6. New single-library C ABI example

## Functional tests

1. Initialize logger with one CSV table.
2. Initialize logger with many CSV tables.
3. Manual `registerTable + write`.
4. `autoRegisterTables + autoWrite`.
5. Async `tryWrite`.
6. Async `tryAutoWrite`.
7. Explicit flush.
8. Stop without flush.
9. Stop with flush.
10. SQL connection failure.
11. Invalid schema.
12. Queue full.
13. Payload too large.
14. Worker thread error propagation.

## Data correctness tests

For every supported type:

* int8
* uint8
* int16
* uint16
* int32
* uint32
* int64
* uint64
* float
* double

Verify:

1. SQL column type is correct.
2. Inserted value is correct.
3. Array flattening order is correct.
4. Timestamp is correct.
5. Row count is correct.
6. Multi-table split schema writes identical timestamp rows to all expected tables.

## Performance tests

Create a synthetic stress payload similar to the real project.

Suggested parameters:

```text
sample rate equivalent: 20 Hz
payload size: 16 KB if possible
tables: 10, 25, 50, 100
batch sizes: 10, 50, 100, 500
queue capacity: 512 or 1024
duration: at least 100000 samples for non-SQL test
```

Measure:

1. Producer `tryAutoWrite` max time.
2. Producer `tryAutoWrite` p99 time.
3. Producer `tryAutoWrite` p99.9 time.
4. Queue max depth.
5. Dropped samples.
6. Worker max flush time.
7. SQL execute max time.
8. SQL commit max time.
9. Total inserted rows.
10. CPU usage.

Acceptance target:

```text
Producer path must not call SQL.
Producer path must not block on queue full.
Producer path should remain bounded by payload memcpy and queue index update.
SQL stalls should appear as queue depth growth, not producer jitter.
```

## Real-time integration test

In the real 50 ms host application, replace:

```cpp
logger.autoWrite(timestampMs, &payload);
```

with:

```cpp
asyncLogger.tryAutoWrite(timestampMs, &payload, sizeof(payload));
```

Then measure:

1. 50 ms cycle duration.
2. missed-cycle count.
3. max cycle duration.
4. p99 cycle duration.
5. queue depth.
6. dropped log samples.
7. SQL flush max duration.

Expected result:

```text
SQL jitter should no longer directly appear in the 50 ms cycle.
If SQL is slow, queue depth should increase.
If SQL remains slow too long, log samples may drop, but the real-time cycle should continue.
```

---

# Important implementation rules

1. Do not let the real-time caller block.
2. Do not let the real-time caller call SQL.
3. Do not allocate memory in the async producer path after initialization.
4. Keep old synchronous API working.
5. Add async API as an extension, not a breaking replacement.
6. Keep `DataLogger` owned by one thread.
7. Do not make the existing `DataLogger` globally thread-safe unless absolutely necessary.
8. Prefer one worker thread per async logger.
9. Record errors and stats instead of printing from timing-sensitive paths.
10. Keep C ABI free of C++ types, classes, references, exceptions, `std::string`, and `std::vector`.

---

# Suggested final architecture

```text
Real-time 50 ms application thread
    |
    | tryAutoWrite(timestamp, payload)
    v
Preallocated bounded queue
    |
    | worker thread, below-normal priority
    v
AsyncDataLogger worker
    |
    | DataLogger::autoWrite / write
    v
Preallocated per-table column batches
    |
    | flush
    v
Reusable ODBC parameter-array buffers
    |
    | SQLExecute + COMMIT
    v
SQL Server
```

The final goal is not to make SQL Server real-time. The final goal is to make SQL Server unable to disturb the 50 ms cycle.
