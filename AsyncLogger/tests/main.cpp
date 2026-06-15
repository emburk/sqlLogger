#include "AsyncLogger/AsyncDataLogger.h"
#include "AsyncLogger/SqlLoggerC.h"

#include "DataLogger/IDBBackend.h"
#include "SqlServerBackend/SqlServerOdbcBackend.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sqlext.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;

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

class MockBackend final : public DataLoggerCore::IDBBackend
{
public:
    // Accept the mock connection without touching SQL Server or ODBC.
    bool connect(const std::string& connectionString) override
    {
        (void)connectionString;
        connectCalls.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Record table initialization calls while leaving table metadata unchanged.
    bool initializeTables(const DataLoggerCore::SchemaRegistry& registry,
                          const std::string& sqlSchemaName,
                          DataLoggerCore::ExistingTablePolicy policy) override
    {
        (void)registry;
        (void)sqlSchemaName;
        (void)policy;
        initializeCalls.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Record insert preparation calls without preparing real ODBC statements.
    bool prepareInsertStatements(const DataLoggerCore::SchemaRegistry& registry,
                                 const std::string& sqlSchemaName) override
    {
        (void)registry;
        (void)sqlSchemaName;
        prepareCalls.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Capture decoded rows and optionally block to make queue-full tests deterministic.
    bool insertBatch(const DataLoggerCore::TableSchema& table,
                     const std::vector<DataLoggerCore::DecodedRow>& rows) override
    {
        {
            std::lock_guard<std::mutex> lock(recordMutex);
            lastTableName = table.tableName;
            lastBatchRows = rows.size();
            insertThreadHash = std::hash<std::thread::id>{}(std::this_thread::get_id());
        }

        insertEntered.store(true, std::memory_order_release);
        while (blockInserts.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }

        insertCalls.fetch_add(1, std::memory_order_relaxed);
        rowsInserted.fetch_add(static_cast<std::uint64_t>(rows.size()), std::memory_order_relaxed);
        return insertSucceeds.load(std::memory_order_acquire);
    }

    // Return the configured mock backend error for failure-path tests.
    DataLoggerCore::BackendError lastError() const override
    {
        return error;
    }

    std::atomic<std::uint64_t> connectCalls{ 0 };
    std::atomic<std::uint64_t> initializeCalls{ 0 };
    std::atomic<std::uint64_t> prepareCalls{ 0 };
    std::atomic<std::uint64_t> insertCalls{ 0 };
    std::atomic<std::uint64_t> rowsInserted{ 0 };
    std::atomic<bool> blockInserts{ false };
    std::atomic<bool> insertEntered{ false };
    std::atomic<bool> insertSucceeds{ true };
    mutable std::mutex recordMutex;
    std::string lastTableName;
    std::size_t lastBatchRows = 0;
    std::size_t insertThreadHash = 0;
    DataLoggerCore::BackendError error;
};

struct RealSqlSummary
{
    std::int64_t rowCount = 0;
    double gyroMin = 0.0;
    double gyroMax = 0.0;
    std::int64_t statusMin = 0;
    std::int64_t statusMax = 0;
};

class OdbcConnection
{
public:
    // Release ODBC connection and environment handles after each smoke operation.
    ~OdbcConnection()
    {
        disconnect();
    }

    // Open an ODBC connection using the same connection string as the logger.
    bool connect(const std::string& connectionString)
    {
        if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env_)))
        {
            error_ = "Unable to allocate ODBC environment handle.";
            return false;
        }

        if (!SQL_SUCCEEDED(SQLSetEnvAttr(env_, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0)))
        {
            error_ = "Unable to set ODBC version.";
            return false;
        }

        if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_DBC, env_, &connection_)))
        {
            error_ = "Unable to allocate ODBC connection handle.";
            return false;
        }

        SQLCHAR output[1024] = {};
        SQLSMALLINT outputLength = 0;
        const SQLRETURN result = SQLDriverConnectA(connection_,
                                                  nullptr,
                                                  reinterpret_cast<SQLCHAR*>(const_cast<char*>(connectionString.c_str())),
                                                  SQL_NTS,
                                                  output,
                                                  static_cast<SQLSMALLINT>(sizeof(output)),
                                                  &outputLength,
                                                  SQL_DRIVER_NOPROMPT);
        if (!SQL_SUCCEEDED(result))
        {
            error_ = collectDiagnostics(SQL_HANDLE_DBC, connection_, "ODBC connection failed");
            return false;
        }

        return true;
    }

    // Execute a SQL statement that does not return a result set.
    bool exec(const std::string& sql)
    {
        SQLHSTMT statement = SQL_NULL_HSTMT;
        if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, connection_, &statement)))
        {
            error_ = "Unable to allocate ODBC statement handle.";
            return false;
        }

        const SQLRETURN result = SQLExecDirectA(statement,
                                               reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql.c_str())),
                                               SQL_NTS);
        if (!SQL_SUCCEEDED(result))
        {
            error_ = collectDiagnostics(SQL_HANDLE_STMT, statement, "SQL execution failed");
            SQLFreeHandle(SQL_HANDLE_STMT, statement);
            return false;
        }

        SQLFreeHandle(SQL_HANDLE_STMT, statement);
        return true;
    }

    // Query row count and representative decoded values from the smoke table.
    bool querySmokeSummary(RealSqlSummary& summary)
    {
        const std::string sql =
            "SELECT COUNT_BIG(*), "
            "MIN(CAST([gyro_0] AS FLOAT)), MAX(CAST([gyro_0] AS FLOAT)), "
            "MIN(CAST([status] AS BIGINT)), MAX(CAST([status] AS BIGINT)) "
            "FROM [dbo].[async_phase1_smoke];";

        SQLHSTMT statement = SQL_NULL_HSTMT;
        if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, connection_, &statement)))
        {
            error_ = "Unable to allocate ODBC statement handle.";
            return false;
        }

        SQLRETURN result = SQLExecDirectA(statement,
                                         reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql.c_str())),
                                         SQL_NTS);
        if (!SQL_SUCCEEDED(result))
        {
            error_ = collectDiagnostics(SQL_HANDLE_STMT, statement, "Smoke summary query failed");
            SQLFreeHandle(SQL_HANDLE_STMT, statement);
            return false;
        }

        result = SQLFetch(statement);
        if (!SQL_SUCCEEDED(result))
        {
            error_ = collectDiagnostics(SQL_HANDLE_STMT, statement, "Smoke summary fetch failed");
            SQLFreeHandle(SQL_HANDLE_STMT, statement);
            return false;
        }

        SQLLEN indicator = 0;
        SQLGetData(statement, 1, SQL_C_SBIGINT, &summary.rowCount, sizeof(summary.rowCount), &indicator);
        SQLGetData(statement, 2, SQL_C_DOUBLE, &summary.gyroMin, sizeof(summary.gyroMin), &indicator);
        SQLGetData(statement, 3, SQL_C_DOUBLE, &summary.gyroMax, sizeof(summary.gyroMax), &indicator);
        SQLGetData(statement, 4, SQL_C_SBIGINT, &summary.statusMin, sizeof(summary.statusMin), &indicator);
        SQLGetData(statement, 5, SQL_C_SBIGINT, &summary.statusMax, sizeof(summary.statusMax), &indicator);

        SQLFreeHandle(SQL_HANDLE_STMT, statement);
        return true;
    }

    // Return the last ODBC helper error for test diagnostics.
    const std::string& error() const
    {
        return error_;
    }

private:
    // Disconnect and free any allocated ODBC handles.
    void disconnect()
    {
        if (connection_ != SQL_NULL_HDBC)
        {
            SQLDisconnect(connection_);
            SQLFreeHandle(SQL_HANDLE_DBC, connection_);
            connection_ = SQL_NULL_HDBC;
        }

        if (env_ != SQL_NULL_HENV)
        {
            SQLFreeHandle(SQL_HANDLE_ENV, env_);
            env_ = SQL_NULL_HENV;
        }
    }

    // Collect readable diagnostic text from an ODBC handle.
    std::string collectDiagnostics(SQLSMALLINT handleType,
                                   SQLHANDLE handle,
                                   const std::string& prefix) const
    {
        std::string message = prefix;
        SQLCHAR state[6] = {};
        SQLINTEGER nativeError = 0;
        SQLCHAR text[512] = {};
        SQLSMALLINT textLength = 0;

        for (SQLSMALLINT record = 1;
             SQL_SUCCEEDED(SQLGetDiagRecA(handleType,
                                          handle,
                                          record,
                                          state,
                                          &nativeError,
                                          text,
                                          static_cast<SQLSMALLINT>(sizeof(text)),
                                          &textLength));
             ++record)
        {
            message += "\n  [";
            message += reinterpret_cast<const char*>(state);
            message += ", ";
            message += std::to_string(nativeError);
            message += "] ";
            message += reinterpret_cast<const char*>(text);
        }

        return message;
    }

    SQLHENV env_ = SQL_NULL_HENV;
    SQLHDBC connection_ = SQL_NULL_HDBC;
    std::string error_;
};

// Record a named expectation failure without adding a test framework dependency.
int expect(bool condition, const char* message)
{
    if (condition)
    {
        return 0;
    }

    std::printf("FAILED: %s\n", message);
    return 1;
}

// Find the repository schema directory from the current test working directory.
std::string findSchemaDirectory()
{
    std::filesystem::path current = std::filesystem::current_path();
    for (int i = 0; i < 8; ++i)
    {
        const std::filesystem::path candidate = current / "ExampleApp" / "schemas";
        if (std::filesystem::exists(candidate))
        {
            return candidate.string();
        }

        if (!current.has_parent_path())
        {
            break;
        }

        current = current.parent_path();
    }

    return "ExampleApp\\schemas";
}

// Find the dedicated async SQL smoke schema directory.
std::string findAsyncSmokeSchemaDirectory()
{
    std::filesystem::path current = std::filesystem::current_path();
    for (int i = 0; i < 8; ++i)
    {
        const std::filesystem::path candidate = current / "AsyncLogger" / "tests" / "schemas";
        if (std::filesystem::exists(candidate))
        {
            return candidate.string();
        }

        if (!current.has_parent_path())
        {
            break;
        }

        current = current.parent_path();
    }

    return "AsyncLogger\\tests\\schemas";
}

// Read the SQL Server ODBC connection string from the environment.
std::string readConnectionString()
{
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, "SQLLOGGER_CONNECTION_STRING") != 0 || value == nullptr)
    {
        return {};
    }

    std::string connectionString(value);
    std::free(value);
    return connectionString;
}

// Drop the dedicated smoke table created by the real SQL test.
bool cleanupSmokeTable(const std::string& connectionString)
{
    OdbcConnection connection;
    if (!connection.connect(connectionString))
    {
        std::printf("FAILED: cleanup connect: %s\n", connection.error().c_str());
        return false;
    }

    const std::string cleanupSql =
        "IF OBJECT_ID(N'[dbo].[async_phase1_smoke]', N'U') IS NOT NULL "
        "DROP TABLE [dbo].[async_phase1_smoke];";

    if (!connection.exec(cleanupSql))
    {
        std::printf("FAILED: cleanup drop table: %s\n", connection.error().c_str());
        return false;
    }

    return true;
}

// Build one deterministic payload whose layout matches ExampleApp/schemas/imu_data.csv.
ImuData makeSample(std::int64_t sequence)
{
    ImuData sample{};
    sample.unusedOrSequence = sequence;
    sample.gyro[0] = 1.0F;
    sample.gyro[1] = 2.0F;
    sample.gyro[2] = 3.0F;
    sample.accel[0] = 4.0F;
    sample.accel[1] = 5.0F;
    sample.accel[2] = 6.0F;
    sample.temperature = 7.5;
    sample.status = 9;
    return sample;
}

// Build a normal DataLogger config for tests that should not touch SQL Server.
DataLoggerCore::DataLoggerConfig makeDataConfig(std::size_t batchSizeRows)
{
    DataLoggerCore::DataLoggerConfig config;
    config.connectionString = "mock";
    config.schemaDirectory = findSchemaDirectory();
    config.sqlSchemaName = "dbo";
    config.batchSizeRows = batchSizeRows;
    config.existingTablePolicy = DataLoggerCore::ExistingTablePolicy::Drop;
    config.printInfoFlag = false;
    config.printErrorFlag = false;
    return config;
}

// Build async config with explicit payload size and queue behavior.
AsyncLogger::AsyncDataLoggerConfig makeAsyncConfig(std::size_t queueCapacity,
                                                   AsyncLogger::AsyncOverflowPolicy overflowPolicy =
                                                       AsyncLogger::AsyncOverflowPolicy::DropNewest)
{
    AsyncLogger::AsyncDataLoggerConfig config;
    config.queueCapacity = queueCapacity;
    config.maxPayloadBytes = sizeof(ImuData);
    config.overflowPolicy = overflowPolicy;
    config.workerPriority = AsyncLogger::AsyncWorkerPriority::BelowNormal;
    config.flushOnStop = true;
    config.autoRegisterTablesOnStart = false;
    return config;
}

// Create and initialize an async logger with a mock backend owned by the logger.
std::unique_ptr<AsyncLogger::AsyncDataLogger> makeLogger(MockBackend*& backend,
                                                         std::size_t queueCapacity,
                                                         std::size_t batchSizeRows,
                                                         bool flushOnStop = true,
                                                         AsyncLogger::AsyncOverflowPolicy overflowPolicy =
                                                             AsyncLogger::AsyncOverflowPolicy::DropNewest)
{
    auto mock = std::make_unique<MockBackend>();
    backend = mock.get();

    auto logger = std::make_unique<AsyncLogger::AsyncDataLogger>(std::move(mock));
    auto asyncConfig = makeAsyncConfig(queueCapacity, overflowPolicy);
    asyncConfig.flushOnStop = flushOnStop;
    if (!logger->initialize(makeDataConfig(batchSizeRows), asyncConfig))
    {
        std::printf("FAILED: logger initialize: %s\n", logger->lastError().message.c_str());
        return nullptr;
    }

    return logger;
}

// Wait for an atomic predicate so worker-thread tests stay deterministic.
template<typename Predicate>
bool waitUntil(Predicate predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline)
    {
        if (predicate())
        {
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return predicate();
}

// Verify the C++ and C async defaults match the phase contract.
int testDefaults()
{
    int failures = 0;

    const AsyncLogger::AsyncDataLoggerConfig cppConfig;
    failures += expect(cppConfig.queueCapacity == 512, "C++ default queue capacity");
    failures += expect(cppConfig.maxPayloadBytes == 0, "C++ default max payload bytes");
    failures += expect(cppConfig.overflowPolicy == AsyncLogger::AsyncOverflowPolicy::DropNewest,
                       "C++ default overflow policy");
    failures += expect(cppConfig.workerPriority == AsyncLogger::AsyncWorkerPriority::BelowNormal,
                       "C++ default worker priority");
    failures += expect(cppConfig.flushOnStop, "C++ default flush on stop");
    failures += expect(!cppConfig.autoRegisterTablesOnStart, "C++ default auto register flag");

    AsyncDataLoggerConfig_c cConfig;
    sql_logger_async_config_default_c(&cConfig);
    failures += expect(cConfig.queue_capacity == 512, "C default queue capacity");
    failures += expect(cConfig.max_payload_bytes == 0, "C default max payload bytes");
    failures += expect(cConfig.overflow_policy == SQLLOGGER_ASYNC_OVERFLOW_DROP_NEWEST,
                       "C default overflow policy");
    failures += expect(cConfig.worker_priority == SQLLOGGER_ASYNC_WORKER_PRIORITY_BELOW_NORMAL,
                       "C default worker priority");
    failures += expect(cConfig.flush_on_stop == 1, "C default flush on stop");
    failures += expect(cConfig.auto_register_tables_on_start == 0, "C default auto register flag");
    return failures;
}

// Verify tryAutoWrite accepts work and the worker drains it through DataLogger.
int testTryAutoWriteDrains()
{
    MockBackend* backend = nullptr;
    auto logger = makeLogger(backend, 8, 100);
    if (!logger)
    {
        return 1;
    }

    int failures = 0;
    failures += expect(logger->autoRegisterTables(), "autoRegisterTables succeeds");
    failures += expect(logger->start(), "start succeeds");

    const ImuData sample = makeSample(1);
    failures += expect(logger->tryAutoWrite(1000, &sample, sizeof(sample)), "tryAutoWrite succeeds");
    failures += expect(logger->stopAndFlush(), "stopAndFlush succeeds");

    const AsyncLogger::AsyncDataLoggerStats stats = logger->stats();
    failures += expect(stats.pushedSamples == 1, "one sample pushed");
    failures += expect(stats.poppedSamples == 1, "one sample popped");
    failures += expect(stats.autoWriteOps == 1, "one auto write processed");
    failures += expect(stats.droppedSamples == 0, "no samples dropped");
    failures += expect(backend->rowsInserted.load(std::memory_order_relaxed) == 1, "one row flushed");
    return failures;
}

// Verify DropNewest returns immediately when the preallocated queue is full.
int testQueueFullDropNewest()
{
    MockBackend* backend = nullptr;
    auto logger = makeLogger(backend, 2, 1);
    if (!logger)
    {
        return 1;
    }

    int failures = 0;
    backend->blockInserts.store(true, std::memory_order_release);
    failures += expect(logger->autoRegisterTables(), "autoRegisterTables succeeds for full test");
    failures += expect(logger->start(), "start succeeds for full test");

    const ImuData sample = makeSample(2);
    failures += expect(logger->tryAutoWrite(2000, &sample, sizeof(sample)), "first write reaches worker");
    failures += expect(waitUntil([&]() {
                    return backend->insertEntered.load(std::memory_order_acquire);
                },
                                std::chrono::milliseconds(1000)),
                       "worker entered blocking insert");

    failures += expect(logger->tryAutoWrite(2001, &sample, sizeof(sample)), "queue fill write 1 succeeds");
    failures += expect(logger->tryAutoWrite(2002, &sample, sizeof(sample)), "queue fill write 2 succeeds");

    const auto startedAt = Clock::now();
    const bool accepted = logger->tryAutoWrite(2003, &sample, sizeof(sample));
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - startedAt).count();

    failures += expect(!accepted, "full queue rejects newest sample");
    failures += expect(logger->lastStatus() == AsyncLogger::AsyncDataLoggerStatus::QueueFull,
                       "queue full status is reported");
    failures += expect(elapsedMs < 100, "queue full path does not block");

    backend->blockInserts.store(false, std::memory_order_release);
    failures += expect(logger->stopAndFlush(), "stopAndFlush succeeds after queue full");

    const AsyncLogger::AsyncDataLoggerStats stats = logger->stats();
    failures += expect(stats.pushedSamples == 3, "three samples accepted before full");
    failures += expect(stats.droppedSamples == 1, "one newest sample dropped");
    return failures;
}

// Verify DropOldest makes room for a new sample without blocking the producer.
int testQueueFullDropOldest()
{
    MockBackend* backend = nullptr;
    auto logger = makeLogger(backend, 2, 1, true, AsyncLogger::AsyncOverflowPolicy::DropOldest);
    if (!logger)
    {
        return 1;
    }

    int failures = 0;
    backend->blockInserts.store(true, std::memory_order_release);
    failures += expect(logger->autoRegisterTables(), "autoRegisterTables succeeds for drop-oldest");
    failures += expect(logger->start(), "start succeeds for drop-oldest");

    const ImuData sample = makeSample(3);
    failures += expect(logger->tryAutoWrite(3000, &sample, sizeof(sample)), "drop-oldest first write reaches worker");
    failures += expect(waitUntil([&]() {
                    return backend->insertEntered.load(std::memory_order_acquire);
                },
                                std::chrono::milliseconds(1000)),
                       "drop-oldest worker entered blocking insert");

    failures += expect(logger->tryAutoWrite(3001, &sample, sizeof(sample)), "drop-oldest queued write 1");
    failures += expect(logger->tryAutoWrite(3002, &sample, sizeof(sample)), "drop-oldest queued write 2");
    failures += expect(logger->tryAutoWrite(3003, &sample, sizeof(sample)), "drop-oldest replaces oldest queued sample");

    backend->blockInserts.store(false, std::memory_order_release);
    failures += expect(logger->stopAndFlush(), "drop-oldest stopAndFlush succeeds");

    const AsyncLogger::AsyncDataLoggerStats stats = logger->stats();
    failures += expect(stats.pushedSamples == 4, "drop-oldest accepted four producer samples");
    failures += expect(stats.droppedSamples == 1, "drop-oldest dropped one queued sample");
    return failures;
}

// Verify tryWrite routes a registered table handle through the worker.
int testTryWriteHandle()
{
    MockBackend* backend = nullptr;
    auto logger = makeLogger(backend, 8, 100);
    if (!logger)
    {
        return 1;
    }

    int failures = 0;
    DataLoggerCore::TableHandle imu;
    failures += expect(logger->registerTable("imu_data", &imu), "registerTable succeeds");
    failures += expect(logger->start(), "start succeeds for tryWrite");

    const ImuData sample = makeSample(4);
    failures += expect(logger->tryWrite(imu, 4000, &sample, sizeof(sample)), "tryWrite succeeds");
    failures += expect(logger->stopAndFlush(), "tryWrite stopAndFlush succeeds");

    std::string tableName;
    {
        std::lock_guard<std::mutex> lock(backend->recordMutex);
        tableName = backend->lastTableName;
    }

    const AsyncLogger::AsyncDataLoggerStats stats = logger->stats();
    failures += expect(stats.writeOps == 1, "one table write processed");
    failures += expect(tableName == "imu_data", "tryWrite reached imu_data table");
    failures += expect(backend->rowsInserted.load(std::memory_order_relaxed) == 1, "tryWrite flushed one row");
    return failures;
}

// Verify an explicit flush request is consumed by the worker.
int testRequestFlush()
{
    MockBackend* backend = nullptr;
    auto logger = makeLogger(backend, 8, 100, false);
    if (!logger)
    {
        return 1;
    }

    int failures = 0;
    failures += expect(logger->autoRegisterTables(), "autoRegisterTables succeeds for flush request");
    failures += expect(logger->start(), "start succeeds for flush request");

    const ImuData sample = makeSample(5);
    failures += expect(logger->tryAutoWrite(5000, &sample, sizeof(sample)), "flush request write queued");
    failures += expect(logger->requestFlush(), "requestFlush queued");
    failures += expect(waitUntil([&]() {
                    return backend->insertCalls.load(std::memory_order_acquire) > 0;
                },
                                std::chrono::milliseconds(1000)),
                       "worker processed requestFlush");

    logger->stop();
    const AsyncLogger::AsyncDataLoggerStats stats = logger->stats();
    failures += expect(stats.flushRequests == 1, "one flush request accepted");
    failures += expect(backend->rowsInserted.load(std::memory_order_relaxed) == 1, "requestFlush inserted buffered row");
    return failures;
}

// Verify stopAndFlush flushes pending rows that did not reach batch size.
int testStopAndFlush()
{
    MockBackend* backend = nullptr;
    auto logger = makeLogger(backend, 8, 100);
    if (!logger)
    {
        return 1;
    }

    int failures = 0;
    failures += expect(logger->autoRegisterTables(), "autoRegisterTables succeeds for stopAndFlush");
    failures += expect(logger->start(), "start succeeds for stopAndFlush");

    const ImuData sample = makeSample(6);
    failures += expect(logger->tryAutoWrite(6000, &sample, sizeof(sample)), "stopAndFlush write queued");
    failures += expect(logger->stopAndFlush(), "stopAndFlush flushes pending row");
    failures += expect(backend->rowsInserted.load(std::memory_order_relaxed) == 1, "stopAndFlush inserted row");
    return failures;
}

// Verify all allowed worker priorities start without requiring realtime priority.
int testWorkerPriorities()
{
    int failures = 0;
    const AsyncLogger::AsyncWorkerPriority priorities[] = {
        AsyncLogger::AsyncWorkerPriority::Normal,
        AsyncLogger::AsyncWorkerPriority::BelowNormal,
        AsyncLogger::AsyncWorkerPriority::Lowest
    };

    for (AsyncLogger::AsyncWorkerPriority priority : priorities)
    {
        auto mock = std::make_unique<MockBackend>();
        auto logger = std::make_unique<AsyncLogger::AsyncDataLogger>(std::move(mock));
        AsyncLogger::AsyncDataLoggerConfig asyncConfig = makeAsyncConfig(4);
        asyncConfig.workerPriority = priority;
        asyncConfig.flushOnStop = false;
        failures += expect(logger->initialize(makeDataConfig(100), asyncConfig), "worker priority logger initializes");
        failures += expect(logger->start(), "worker priority start succeeds");
        logger->stop();
    }

    return failures;
}

// Verify oversized payloads are rejected before any queue copy occurs.
int testPayloadTooLarge()
{
    MockBackend* backend = nullptr;
    auto mock = std::make_unique<MockBackend>();
    backend = mock.get();

    auto logger = std::make_unique<AsyncLogger::AsyncDataLogger>(std::move(mock));
    AsyncLogger::AsyncDataLoggerConfig asyncConfig = makeAsyncConfig(4);
    asyncConfig.maxPayloadBytes = sizeof(ImuData) - 1;

    int failures = 0;
    failures += expect(logger->initialize(makeDataConfig(100), asyncConfig), "payload limit logger initializes");
    failures += expect(logger->autoRegisterTables(), "payload limit autoRegisterTables succeeds");
    failures += expect(logger->start(), "payload limit start succeeds");

    const ImuData sample = makeSample(7);
    failures += expect(!logger->tryAutoWrite(7000, &sample, sizeof(sample)), "oversized payload rejected");
    failures += expect(logger->lastStatus() == AsyncLogger::AsyncDataLoggerStatus::PayloadTooLarge,
                       "oversized payload status reported");
    logger->stop();

    const AsyncLogger::AsyncDataLoggerStats stats = logger->stats();
    failures += expect(stats.droppedSamples == 1, "oversized payload counted as dropped");
    failures += expect(backend->rowsInserted.load(std::memory_order_relaxed) == 0, "oversized payload never reaches backend");
    return failures;
}

// Verify backend insert work runs on the worker thread, not the producer thread.
int testProducerDoesNotCallBackend()
{
    MockBackend* backend = nullptr;
    auto logger = makeLogger(backend, 4, 1);
    if (!logger)
    {
        return 1;
    }

    int failures = 0;
    backend->blockInserts.store(true, std::memory_order_release);
    failures += expect(logger->autoRegisterTables(), "producer-thread autoRegisterTables succeeds");
    failures += expect(logger->start(), "producer-thread start succeeds");

    const std::size_t producerHash = std::hash<std::thread::id>{}(std::this_thread::get_id());
    const ImuData sample = makeSample(8);
    failures += expect(logger->tryAutoWrite(8000, &sample, sizeof(sample)), "producer-thread write queued");
    failures += expect(waitUntil([&]() {
                    return backend->insertEntered.load(std::memory_order_acquire);
                },
                                std::chrono::milliseconds(1000)),
                       "backend insert reached by worker");

    {
        std::lock_guard<std::mutex> lock(backend->recordMutex);
        failures += expect(backend->insertThreadHash != producerHash, "backend insert did not run on producer thread");
    }

    backend->blockInserts.store(false, std::memory_order_release);
    failures += expect(logger->stopAndFlush(), "producer-thread stopAndFlush succeeds");
    return failures;
}

// Run a real SQL Server async smoke test and drop the dedicated table afterward.
int testRealSqlSmoke()
{
    const std::string connectionString = readConnectionString();
    if (connectionString.empty())
    {
        std::printf("FAILED: SQLLOGGER_CONNECTION_STRING is not set for --real-sql.\n");
        return 1;
    }

    int failures = 0;
    constexpr int kSampleCount = 1000;

    DataLoggerCore::DataLoggerConfig dataConfig;
    dataConfig.connectionString = connectionString;
    dataConfig.schemaDirectory = findAsyncSmokeSchemaDirectory();
    dataConfig.sqlSchemaName = "dbo";
    dataConfig.batchSizeRows = 100;
    dataConfig.existingTablePolicy = DataLoggerCore::ExistingTablePolicy::Drop;
    dataConfig.printInfoFlag = false;
    dataConfig.printErrorFlag = false;

    AsyncLogger::AsyncDataLoggerConfig asyncConfig = makeAsyncConfig(2048);
    asyncConfig.autoRegisterTablesOnStart = true;
    asyncConfig.flushOnStop = true;

    auto backend = std::make_unique<SqlServerBackend::SqlServerOdbcBackend>();
    AsyncLogger::AsyncDataLogger logger(std::move(backend));

    if (!logger.initialize(dataConfig, asyncConfig))
    {
        std::printf("FAILED: real SQL async initialize: %s\n", logger.lastError().message.c_str());
        failures += 1;
    }
    else if (!logger.start())
    {
        std::printf("FAILED: real SQL async start: %s\n", logger.lastError().message.c_str());
        failures += 1;
    }
    else
    {
        int accepted = 0;
        for (int i = 0; i < kSampleCount; ++i)
        {
            const ImuData sample = makeSample(i);
            if (logger.tryAutoWrite(9000000 + i, &sample, sizeof(sample)))
            {
                ++accepted;
            }
        }

        failures += expect(logger.stopAndFlush(), "real SQL stopAndFlush succeeds");

        const AsyncLogger::AsyncDataLoggerStats stats = logger.stats();
        failures += expect(accepted == kSampleCount, "real SQL accepted all samples");
        failures += expect(stats.droppedSamples == 0, "real SQL dropped no samples");

        OdbcConnection connection;
        if (!connection.connect(connectionString))
        {
            std::printf("FAILED: real SQL verification connect: %s\n", connection.error().c_str());
            failures += 1;
        }
        else
        {
            RealSqlSummary summary;
            if (!connection.querySmokeSummary(summary))
            {
                std::printf("FAILED: real SQL summary query: %s\n", connection.error().c_str());
                failures += 1;
            }
            else
            {
                failures += expect(summary.rowCount == accepted, "real SQL row count matches accepted samples");
                failures += expect(std::fabs(summary.gyroMin - 1.0) < 0.0001, "real SQL gyro_0 min matches sample");
                failures += expect(std::fabs(summary.gyroMax - 1.0) < 0.0001, "real SQL gyro_0 max matches sample");
                failures += expect(summary.statusMin == 9, "real SQL status min matches sample");
                failures += expect(summary.statusMax == 9, "real SQL status max matches sample");
            }
        }
    }

    failures += expect(cleanupSmokeTable(connectionString), "real SQL smoke table cleanup succeeds");
    return failures;
}
}

// Run the Phase 1 unit-style async logger checks, with optional real SQL smoke.
int runPhase1Checks(int argc, char** argv)
{
    int failures = 0;
    bool runRealSql = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::string(argv[i]) == "--real-sql")
        {
            runRealSql = true;
        }
    }

    failures += testDefaults();
    failures += testTryAutoWriteDrains();
    failures += testQueueFullDropNewest();
    failures += testQueueFullDropOldest();
    failures += testTryWriteHandle();
    failures += testRequestFlush();
    failures += testStopAndFlush();
    failures += testWorkerPriorities();
    failures += testPayloadTooLarge();
    failures += testProducerDoesNotCallBackend();

    if (runRealSql)
    {
        failures += testRealSqlSmoke();
    }

    if (failures == 0)
    {
        std::printf("AsyncLogger Phase 1 checks passed.\n");
    }

    return failures == 0 ? 0 : 1;
}

// Show compact async logger setup, automatic table registration, writes, and shutdown.
int main(int argc, char** argv)
{
    bool runTests = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        if (argument == "--run-tests" || argument == "--real-sql")
        {
            runTests = true;
        }
    }

    if (runTests)
    {
        return runPhase1Checks(argc, argv);
    }

    // 1) Configure the synchronous logger: SQL connection, CSV schemas, table policy, and batch size.
    const std::string connectionString = readConnectionString();
    if (connectionString.empty())
    {
        std::printf("Set SQLLOGGER_CONNECTION_STRING, or run with --run-tests for mock checks.\n");
        return 1;
    }

    DataLoggerCore::DataLoggerConfig dataConfig;
    dataConfig.connectionString = connectionString;
    dataConfig.schemaDirectory = findAsyncSmokeSchemaDirectory();
    dataConfig.sqlSchemaName = "dbo";
    dataConfig.batchSizeRows = 100;
    dataConfig.existingTablePolicy = DataLoggerCore::ExistingTablePolicy::Drop;
    dataConfig.printInfoFlag = true;
    dataConfig.printErrorFlag = true;

    // 2) Configure the async shell: preallocated queue, payload size, and auto registration on start.
    AsyncLogger::AsyncDataLoggerConfig asyncConfig;
    asyncConfig.queueCapacity = 2048;
    asyncConfig.maxPayloadBytes = sizeof(ImuData);
    asyncConfig.overflowPolicy = AsyncLogger::AsyncOverflowPolicy::DropNewest;
    asyncConfig.workerPriority = AsyncLogger::AsyncWorkerPriority::BelowNormal;
    asyncConfig.flushOnStop = true;
    asyncConfig.autoRegisterTablesOnStart = true;

    auto backend = std::make_unique<SqlServerBackend::SqlServerOdbcBackend>();
    AsyncLogger::AsyncDataLogger logger(std::move(backend));

    // 3) Initialize SQL tables from CSV, start the worker, then enqueue payload copies.
    if (!logger.initialize(dataConfig, asyncConfig) || !logger.start())
    {
        std::printf("Async logger startup failed: %s\n", logger.lastError().message.c_str());
        cleanupSmokeTable(connectionString);
        return 1;
    }

    for (int i = 0; i < 1000; ++i)
    {
        const ImuData sample = makeSample(i);
        if (!logger.tryAutoWrite(9000000 + i, &sample, sizeof(sample)))
        {
            std::printf("Queue rejected sample %d with status %d.\n", i, static_cast<int>(logger.lastStatus()));
        }
    }

    // 4) Stop the worker after draining queued writes, flush pending rows, then clean up the smoke table.
    if (!logger.stopAndFlush())
    {
        std::printf("Async logger shutdown failed: %s\n", logger.lastError().message.c_str());
        cleanupSmokeTable(connectionString);
        return 1;
    }

    const AsyncLogger::AsyncDataLoggerStats stats = logger.stats();
    std::printf("Async tutorial wrote %llu samples, dropped %llu samples.\n",
                static_cast<unsigned long long>(stats.pushedSamples),
                static_cast<unsigned long long>(stats.droppedSamples));

    if (!cleanupSmokeTable(connectionString))
    {
        return 1;
    }

    return 0;
}
