#include "AsyncLogger/AsyncDataLogger.h"
#include "AsyncLogger/SqlLoggerC.h"

#include "DataLogger/BinaryDecoder.h"
#include "DataLogger/DataLogger.h"
#include "DataLogger/IDBBackend.h"
#include "DataLogger/Schema.h"
#include "SqlServerBackend/SqlServerOdbcBackend.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sqlext.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
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

#pragma pack(push, 1)
struct AllNumericPayload
{
    std::int8_t i8;
    std::uint8_t u8;
    std::int16_t i16;
    std::uint16_t u16;
    std::int32_t i32;
    std::uint32_t u32;
    std::int64_t i64;
    std::uint64_t u64;
    float f32;
    double f64;
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
                          DataLoggerCore::ExistingTablePolicy policy,
                          DataLoggerCore::SqlServerIndexMode indexMode) override
    {
        (void)registry;
        (void)sqlSchemaName;
        (void)policy;
        (void)indexMode;
        initializeCalls.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Record insert preparation calls without preparing real ODBC statements.
    bool prepareInsertStatements(const DataLoggerCore::SchemaRegistry& registry,
                                 const std::string& sqlSchemaName,
                                 std::size_t batchSizeRows) override
    {
        (void)registry;
        (void)sqlSchemaName;
        (void)batchSizeRows;
        prepareCalls.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Capture decoded batch counts and optionally block to make queue-full tests deterministic.
    bool insertBatch(const DataLoggerCore::TableSchema& table,
                     const DataLoggerCore::ColumnBatch& batch) override
    {
        {
            std::lock_guard<std::mutex> lock(recordMutex);
            lastTableName = table.tableName;
            lastBatchRows = batch.rowCount;
            insertThreadHash = std::hash<std::thread::id>{}(std::this_thread::get_id());
            observedBatchRows.push_back(batch.rowCount);
            observedTables.push_back(table.tableName);
            observedCapacityStable = observedCapacityStable &&
                batch.timestamps.size() == batch.rowCapacity &&
                batch.rowCapacity > 0;
            for (const DataLoggerCore::ColumnStorage& column : batch.columns)
            {
                observedCapacityStable = observedCapacityStable &&
                    DataLoggerCore::columnStorageSize(column) == batch.rowCapacity;
            }
        }

        insertEntered.store(true, std::memory_order_release);
        while (blockInserts.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }

        insertCalls.fetch_add(1, std::memory_order_relaxed);
        rowsInserted.fetch_add(static_cast<std::uint64_t>(batch.rowCount), std::memory_order_relaxed);
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
    bool observedCapacityStable = true;
    std::vector<std::size_t> observedBatchRows;
    std::vector<std::string> observedTables;
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

struct SqlIndexBenchmarkResult
{
    const char* modeName = "";
    std::size_t requestedRows = 0;
    std::int64_t storedRows = 0;
    double initializeMs = 0.0;
    double insertAndFlushMs = 0.0;
    double queryMs = 0.0;
    double rowsPerSecond = 0.0;
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
        const std::filesystem::path candidate = current / "examples" / "ExampleApp" / "schemas";
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

    return "examples\\ExampleApp\\schemas";
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

// Trim whitespace and common UTF-8 BOM bytes around file-based connection strings.
std::string trimConnectionString(std::string value)
{
    if (value.size() >= 3 &&
        static_cast<unsigned char>(value[0]) == 0xEF &&
        static_cast<unsigned char>(value[1]) == 0xBB &&
        static_cast<unsigned char>(value[2]) == 0xBF)
    {
        value.erase(0, 3);
    }

    const std::string whitespace = " \t\r\n";
    const std::size_t first = value.find_first_not_of(whitespace);
    if (first == std::string::npos)
    {
        return {};
    }

    const std::size_t last = value.find_last_not_of(whitespace);
    return value.substr(first, last - first + 1);
}

// Read a whole connection-string file when a local or CLI path is provided.
bool readConnectionStringFile(const std::filesystem::path& path,
                              std::string& connectionString,
                              std::string& error)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        error = "Unable to open connection-string file '" + path.string() + "'.";
        return false;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    connectionString = trimConnectionString(buffer.str());
    if (connectionString.empty())
    {
        error = "Connection-string file '" + path.string() + "' is empty.";
        return false;
    }

    return true;
}

// Find an optional ignored local connection-string file near the async tests.
std::filesystem::path findLocalConnectionStringFile()
{
    std::filesystem::path current = std::filesystem::current_path();
    for (int i = 0; i < 8; ++i)
    {
        const std::filesystem::path candidate = current / "AsyncLogger" / "tests" / "sql_connection.txt";
        if (std::filesystem::exists(candidate))
        {
            return candidate;
        }

        if (!current.has_parent_path())
        {
            break;
        }

        current = current.parent_path();
    }

    return {};
}

// Resolve the SQL connection from CLI, environment, or an ignored local file.
bool readConnectionString(int argc,
                          char** argv,
                          std::string& connectionString,
                          std::string& error)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        const std::string inlinePrefix = "--connection-string=";
        const std::string filePrefix = "--connection-string-file=";

        if (argument == "--connection-string")
        {
            if (i + 1 >= argc)
            {
                error = "--connection-string requires a following value.";
                return false;
            }

            connectionString = trimConnectionString(argv[++i]);
            return !connectionString.empty();
        }

        if (argument.rfind(inlinePrefix, 0) == 0)
        {
            connectionString = trimConnectionString(argument.substr(inlinePrefix.size()));
            return !connectionString.empty();
        }

        if (argument == "--connection-string-file")
        {
            if (i + 1 >= argc)
            {
                error = "--connection-string-file requires a following path.";
                return false;
            }

            return readConnectionStringFile(argv[++i], connectionString, error);
        }

        if (argument.rfind(filePrefix, 0) == 0)
        {
            return readConnectionStringFile(argument.substr(filePrefix.size()), connectionString, error);
        }
    }

    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, "SQLLOGGER_CONNECTION_STRING") == 0 && value != nullptr)
    {
        connectionString = trimConnectionString(value);
        std::free(value);
        if (!connectionString.empty())
        {
            return true;
        }
    }

    const std::filesystem::path localFile = findLocalConnectionStringFile();
    if (!localFile.empty())
    {
        return readConnectionStringFile(localFile, connectionString, error);
    }

    error = "SQL connection string was not provided. Use SQLLOGGER_CONNECTION_STRING, --connection-string, --connection-string-file, or AsyncLogger/tests/sql_connection.txt.";
    return false;
}

// Read a positive size_t option in either "--name value" or "--name=value" form.
bool readSizeArgument(int argc,
                      char** argv,
                      const std::string& optionName,
                      std::size_t defaultValue,
                      std::size_t& value,
                      std::string& error)
{
    value = defaultValue;
    const std::string inlinePrefix = optionName + "=";
    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        std::string text;
        if (argument == optionName)
        {
            if (i + 1 >= argc)
            {
                error = optionName + " requires a following value.";
                return false;
            }

            text = argv[++i];
        }
        else if (argument.rfind(inlinePrefix, 0) == 0)
        {
            text = argument.substr(inlinePrefix.size());
        }
        else
        {
            continue;
        }

        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
        if (text.empty() || end == text.c_str() || *end != '\0' || parsed == 0)
        {
            error = optionName + " must be a positive integer.";
            return false;
        }

        value = static_cast<std::size_t>(parsed);
        return true;
    }

    return true;
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

// Return elapsed milliseconds for compact benchmark reporting.
double elapsedMs(Clock::time_point begin, Clock::time_point end)
{
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

// Build one deterministic payload whose layout matches examples/ExampleApp/schemas/imu_data.csv.
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

// Run one real SQL logging benchmark for the requested SQL Server index mode.
bool runSqlIndexBenchmarkMode(const std::string& connectionString,
                              DataLoggerCore::SqlServerIndexMode indexMode,
                              const char* modeName,
                              std::size_t rowCount,
                              std::size_t batchSizeRows,
                              SqlIndexBenchmarkResult& result)
{
    result = {};
    result.modeName = modeName;
    result.requestedRows = rowCount;

    if (!cleanupSmokeTable(connectionString))
    {
        return false;
    }

    DataLoggerCore::DataLoggerConfig dataConfig;
    dataConfig.connectionString = connectionString;
    dataConfig.schemaDirectory = findAsyncSmokeSchemaDirectory();
    dataConfig.sqlSchemaName = "dbo";
    dataConfig.batchSizeRows = batchSizeRows;
    dataConfig.existingTablePolicy = DataLoggerCore::ExistingTablePolicy::Drop;
    dataConfig.sqlServerIndexMode = indexMode;
    dataConfig.printInfoFlag = false;
    dataConfig.printErrorFlag = false;

    {
        auto backend = std::make_unique<SqlServerBackend::SqlServerOdbcBackend>();
        DataLoggerCore::DataLogger logger(std::move(backend));

        const Clock::time_point initBegin = Clock::now();
        if (!logger.initialize(dataConfig))
        {
            std::printf("FAILED: %s initialize: %s\n", modeName, logger.lastError().message.c_str());
            return false;
        }

        if (!logger.autoRegisterTables())
        {
            std::printf("FAILED: %s autoRegisterTables: %s\n", modeName, logger.lastError().message.c_str());
            return false;
        }

        result.initializeMs = elapsedMs(initBegin, Clock::now());

        const Clock::time_point insertBegin = Clock::now();
        for (std::size_t i = 0; i < rowCount; ++i)
        {
            const ImuData sample = makeSample(static_cast<std::int64_t>(i));
            if (!logger.autoWrite(1000000000LL + static_cast<std::int64_t>(i), &sample))
            {
                std::printf("FAILED: %s autoWrite row %llu: %s\n",
                            modeName,
                            static_cast<unsigned long long>(i),
                            logger.lastError().message.c_str());
                return false;
            }
        }

        if (!logger.flush())
        {
            std::printf("FAILED: %s flush: %s\n", modeName, logger.lastError().message.c_str());
            return false;
        }

        result.insertAndFlushMs = elapsedMs(insertBegin, Clock::now());
        if (result.insertAndFlushMs > 0.0)
        {
            result.rowsPerSecond = static_cast<double>(rowCount) / (result.insertAndFlushMs / 1000.0);
        }
    }

    OdbcConnection queryConnection;
    if (!queryConnection.connect(connectionString))
    {
        std::printf("FAILED: %s query connect: %s\n", modeName, queryConnection.error().c_str());
        cleanupSmokeTable(connectionString);
        return false;
    }

    RealSqlSummary summary;
    const Clock::time_point queryBegin = Clock::now();
    if (!queryConnection.querySmokeSummary(summary))
    {
        std::printf("FAILED: %s summary query: %s\n", modeName, queryConnection.error().c_str());
        cleanupSmokeTable(connectionString);
        return false;
    }

    result.queryMs = elapsedMs(queryBegin, Clock::now());
    result.storedRows = summary.rowCount;
    if (result.storedRows != static_cast<std::int64_t>(rowCount))
    {
        std::printf("FAILED: %s stored %lld rows, expected %llu rows.\n",
                    modeName,
                    static_cast<long long>(result.storedRows),
                    static_cast<unsigned long long>(rowCount));
        cleanupSmokeTable(connectionString);
        return false;
    }

    if (!cleanupSmokeTable(connectionString))
    {
        return false;
    }

    return true;
}

// Compare default rowstore indexing against optional nonclustered columnstore indexing.
int runSqlIndexBenchmark(int argc, char** argv)
{
    std::string connectionString;
    std::string error;
    if (!readConnectionString(argc, argv, connectionString, error))
    {
        std::printf("FAILED: %s\n", error.c_str());
        return 1;
    }

    std::size_t rowCount = 250000;
    if (!readSizeArgument(argc, argv, "--benchmark-rows", rowCount, rowCount, error))
    {
        std::printf("FAILED: %s\n", error.c_str());
        return 1;
    }

    std::size_t batchSizeRows = 1000;
    if (!readSizeArgument(argc, argv, "--benchmark-batch", batchSizeRows, batchSizeRows, error))
    {
        std::printf("FAILED: %s\n", error.c_str());
        return 1;
    }

    SqlIndexBenchmarkResult rowstore;
    SqlIndexBenchmarkResult columnstore;
    const bool rowstoreOk = runSqlIndexBenchmarkMode(connectionString,
                                                     DataLoggerCore::SqlServerIndexMode::RowstoreTimestampOnly,
                                                     "rowstore_timestamp_only",
                                                     rowCount,
                                                     batchSizeRows,
                                                     rowstore);
    const bool columnstoreOk = runSqlIndexBenchmarkMode(connectionString,
                                                        DataLoggerCore::SqlServerIndexMode::RowstoreWithNonclusteredColumnstore,
                                                        "rowstore_with_nonclustered_columnstore",
                                                        rowCount,
                                                        batchSizeRows,
                                                        columnstore);
    if (!rowstoreOk || !columnstoreOk)
    {
        return 1;
    }

    std::printf("SQL Server index benchmark rows=%llu batch=%llu\n",
                static_cast<unsigned long long>(rowCount),
                static_cast<unsigned long long>(batchSizeRows));
    std::printf("%-42s %12s %12s %12s %14s\n",
                "mode",
                "init_ms",
                "insert_ms",
                "query_ms",
                "rows_per_sec");
    std::printf("%-42s %12.2f %12.2f %12.2f %14.2f\n",
                rowstore.modeName,
                rowstore.initializeMs,
                rowstore.insertAndFlushMs,
                rowstore.queryMs,
                rowstore.rowsPerSecond);
    std::printf("%-42s %12.2f %12.2f %12.2f %14.2f\n",
                columnstore.modeName,
                columnstore.initializeMs,
                columnstore.insertAndFlushMs,
                columnstore.queryMs,
                columnstore.rowsPerSecond);

    if (rowstore.insertAndFlushMs > 0.0 && columnstore.insertAndFlushMs > 0.0)
    {
        std::printf("insert_ms columnstore / rowstore = %.3f\n",
                    columnstore.insertAndFlushMs / rowstore.insertAndFlushMs);
    }

    if (rowstore.queryMs > 0.0 && columnstore.queryMs > 0.0)
    {
        std::printf("query_ms columnstore / rowstore = %.3f\n",
                    columnstore.queryMs / rowstore.queryMs);
    }

    return 0;
}

// Create a schema directory under x64 so generated test schemas stay out of source.
std::filesystem::path makeGeneratedSchemaDirectory(const std::string& name)
{
    const std::string uniqueName = name + "_" + std::to_string(GetCurrentProcessId());
    const std::filesystem::path directory = std::filesystem::path("x64") / "AsyncLoggerGeneratedSchemas" / uniqueName;
    std::filesystem::create_directories(directory);
    return directory;
}

// Write one generated CSV schema file for focused test coverage.
bool writeGeneratedSchema(const std::filesystem::path& directory,
                          const std::string& fileName,
                          const std::string& text)
{
    std::filesystem::create_directories(directory);
    std::ofstream output(directory / fileName, std::ios::trunc);
    if (!output)
    {
        return false;
    }

    output << text;
    return static_cast<bool>(output);
}

// Build a normal DataLogger config against a generated schema directory.
DataLoggerCore::DataLoggerConfig makeGeneratedDataConfig(const std::filesystem::path& schemaDirectory,
                                                         std::size_t batchSizeRows)
{
    DataLoggerCore::DataLoggerConfig config = makeDataConfig(batchSizeRows);
    config.schemaDirectory = schemaDirectory.string();
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
    const DataLoggerCore::DataLoggerConfig dataCppConfig;
    failures += expect(dataCppConfig.existingTablePolicy == DataLoggerCore::ExistingTablePolicy::RenameWithTimestampSuffix,
                       "C++ data logger default table policy remains rename");
    failures += expect(dataCppConfig.sqlServerIndexMode == DataLoggerCore::SqlServerIndexMode::RowstoreTimestampOnly,
                       "C++ data logger default index mode remains rowstore timestamp only");
    failures += expect(cppConfig.queueCapacity == 512, "C++ default queue capacity");
    failures += expect(cppConfig.maxPayloadBytes == 0, "C++ default max payload bytes");
    failures += expect(cppConfig.overflowPolicy == AsyncLogger::AsyncOverflowPolicy::DropNewest,
                       "C++ default overflow policy");
    failures += expect(cppConfig.workerPriority == AsyncLogger::AsyncWorkerPriority::BelowNormal,
                       "C++ default worker priority");
    failures += expect(cppConfig.flushOnStop, "C++ default flush on stop");
    failures += expect(!cppConfig.autoRegisterTablesOnStart, "C++ default auto register flag");

    AsyncDataLoggerConfig_c cConfig;
    DataLoggerConfig_c dataCConfig;
    datalogger_config_default_c(&dataCConfig);
    failures += expect(dataCConfig.existingTablePolicy == DATALOGGER_EXISTING_TABLE_POLICY_RENAME_WITH_TIMESTAMP_SUFFIX,
                       "C data logger default table policy remains rename");
    failures += expect(dataCConfig.sqlServerIndexMode == DATALOGGER_SQL_SERVER_INDEX_MODE_ROWSTORE_TIMESTAMP_ONLY,
                       "C data logger default index mode remains rowstore timestamp only");

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

// Verify the typed column decoder stores every supported numeric type correctly.
int testTypedColumnBatchAllNumericTypes()
{
    DataLoggerCore::TableSchema table;
    table.tableName = "all_numeric";
    table.expandedColumns = {
        { "i8", offsetof(AllNumericPayload, i8), DataLoggerCore::DataType::Int8, sizeof(std::int8_t) },
        { "u8", offsetof(AllNumericPayload, u8), DataLoggerCore::DataType::UInt8, sizeof(std::uint8_t) },
        { "i16", offsetof(AllNumericPayload, i16), DataLoggerCore::DataType::Int16, sizeof(std::int16_t) },
        { "u16", offsetof(AllNumericPayload, u16), DataLoggerCore::DataType::UInt16, sizeof(std::uint16_t) },
        { "i32", offsetof(AllNumericPayload, i32), DataLoggerCore::DataType::Int32, sizeof(std::int32_t) },
        { "u32", offsetof(AllNumericPayload, u32), DataLoggerCore::DataType::UInt32, sizeof(std::uint32_t) },
        { "i64", offsetof(AllNumericPayload, i64), DataLoggerCore::DataType::Int64, sizeof(std::int64_t) },
        { "u64", offsetof(AllNumericPayload, u64), DataLoggerCore::DataType::UInt64, sizeof(std::uint64_t) },
        { "f32", offsetof(AllNumericPayload, f32), DataLoggerCore::DataType::Float, sizeof(float) },
        { "f64", offsetof(AllNumericPayload, f64), DataLoggerCore::DataType::Double, sizeof(double) }
    };

    DataLoggerCore::ColumnBatch batch;
    DataLoggerCore::initializeColumnBatch(table, 2, batch);

    AllNumericPayload first{};
    first.i8 = -8;
    first.u8 = 250;
    first.i16 = -1234;
    first.u16 = 65000;
    first.i32 = -1234567;
    first.u32 = 4000000000U;
    first.i64 = -1234567890123LL;
    first.u64 = std::numeric_limits<std::uint64_t>::max();
    first.f32 = 1.25F;
    first.f64 = -9.5;

    DataLoggerCore::DataLoggerError error;
    int failures = 0;
    failures += expect(DataLoggerCore::decodeIntoColumnBatch(table, 111, &first, batch, error), "all numeric first row decodes");
    failures += expect(batch.rowCount == 1, "all numeric row count increments");
    failures += expect(batch.timestamps[0] == 111, "all numeric timestamp stored");
    failures += expect(batch.columns[0].int16Values[0] == -8, "int8 stored as smallint");
    failures += expect(batch.columns[1].uint8Values[0] == 250, "uint8 stored as tinyint");
    failures += expect(batch.columns[2].int16Values[0] == -1234, "int16 stored");
    failures += expect(batch.columns[3].int32Values[0] == 65000, "uint16 stored as int");
    failures += expect(batch.columns[4].int32Values[0] == -1234567, "int32 stored");
    failures += expect(batch.columns[5].int64Values[0] == 4000000000LL, "uint32 stored as bigint");
    failures += expect(batch.columns[6].int64Values[0] == -1234567890123LL, "int64 stored");
    failures += expect(batch.columns[7].uint64Values[0] == std::numeric_limits<std::uint64_t>::max(), "uint64 stored");
    failures += expect(std::fabs(batch.columns[8].floatValues[0] - 1.25F) < 0.0001F, "float stored");
    failures += expect(std::fabs(batch.columns[9].doubleValues[0] + 9.5) < 0.0001, "double stored");
    return failures;
}

// Verify generated split schemas auto-write to every table through the async shell.
int testGeneratedManyTableAutoWrite()
{
    const std::filesystem::path directory = makeGeneratedSchemaDirectory("many_tables");
    const bool wroteA = writeGeneratedSchema(directory,
                                             "split_a.csv",
                                             "column_name,offset,datatype,size,length,unit,description\n"
                                             "gyro,8,float,4,3,rad/s,gyro\n");
    const bool wroteB = writeGeneratedSchema(directory,
                                             "split_b.csv",
                                             "column_name,offset,datatype,size,length,unit,description\n"
                                             "status,40,uint16,2,1,,status\n");

    int failures = 0;
    failures += expect(wroteA && wroteB, "generated split schemas written");

    auto mock = std::make_unique<MockBackend>();
    MockBackend* backend = mock.get();
    AsyncLogger::AsyncDataLogger logger(std::move(mock));
    failures += expect(logger.initialize(makeGeneratedDataConfig(directory, 100), makeAsyncConfig(8)), "many-table logger initializes");
    failures += expect(logger.autoRegisterTables(), "many-table autoRegisterTables succeeds");
    failures += expect(logger.start(), "many-table logger starts");

    const ImuData sample = makeSample(11);
    failures += expect(logger.tryAutoWrite(11000, &sample, sizeof(sample)), "many-table auto write accepted");
    failures += expect(logger.stopAndFlush(), "many-table stopAndFlush succeeds");
    failures += expect(backend->rowsInserted.load(std::memory_order_relaxed) == 2, "many-table inserted one row per table");
    failures += expect(backend->insertCalls.load(std::memory_order_relaxed) == 2, "many-table flushed both tables");
    return failures;
}

// Verify exact and partial flushes reuse fixed column capacities across batches.
int testFullPartialAndRepeatedBatchCapacities()
{
    MockBackend* backend = nullptr;
    auto logger = makeLogger(backend, 16, 2);
    if (!logger)
    {
        return 1;
    }

    int failures = 0;
    failures += expect(logger->autoRegisterTables(), "capacity autoRegisterTables succeeds");
    failures += expect(logger->start(), "capacity logger starts");

    const ImuData sample = makeSample(12);
    failures += expect(logger->tryAutoWrite(12000, &sample, sizeof(sample)), "capacity first write accepted");
    failures += expect(logger->tryAutoWrite(12001, &sample, sizeof(sample)), "capacity second write accepted");
    failures += expect(waitUntil([&]() {
                    return backend->insertCalls.load(std::memory_order_acquire) >= 1;
                },
                                std::chrono::milliseconds(1000)),
                       "capacity full batch flushed");
    failures += expect(logger->tryAutoWrite(12002, &sample, sizeof(sample)), "capacity partial write accepted");
    failures += expect(logger->stopAndFlush(), "capacity stopAndFlush succeeds");

    std::vector<std::size_t> rows;
    bool stable = false;
    {
        std::lock_guard<std::mutex> lock(backend->recordMutex);
        rows = backend->observedBatchRows;
        stable = backend->observedCapacityStable;
    }

    failures += expect(rows.size() == 2, "capacity observed full and partial flushes");
    if (rows.size() == 2)
    {
        failures += expect(rows[0] == 2, "capacity first flush full batch");
        failures += expect(rows[1] == 1, "capacity second flush partial batch");
    }
    failures += expect(stable, "capacity column storage stayed preallocated");
    return failures;
}

// Run a real SQL Server async smoke test and drop the dedicated table afterward.
int testRealSqlSmoke(int argc, char** argv)
{
    std::string connectionString;
    std::string connectionError;
    if (!readConnectionString(argc, argv, connectionString, connectionError))
    {
        std::printf("FAILED: %s\n", connectionError.c_str());
        return 1;
    }

    int failures = 0;
    bool tableMayExist = false;
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
    else
    {
        tableMayExist = true;
        int accepted = 0;
        if (!logger.start())
        {
            std::printf("FAILED: real SQL async start: %s\n", logger.lastError().message.c_str());
            failures += 1;
        }
        else
        {
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

        DataLoggerCore::DataLoggerConfig appendConfig = dataConfig;
        appendConfig.existingTablePolicy = DataLoggerCore::ExistingTablePolicy::ContinueCurrentTable;
        auto appendBackend = std::make_unique<SqlServerBackend::SqlServerOdbcBackend>();
        AsyncLogger::AsyncDataLogger appendLogger(std::move(appendBackend));
        int appendAccepted = 0;

        if (!appendLogger.initialize(appendConfig, asyncConfig))
        {
            std::printf("FAILED: real SQL append initialize: %s\n", appendLogger.lastError().message.c_str());
            failures += 1;
        }
        else if (!appendLogger.start())
        {
            std::printf("FAILED: real SQL append start: %s\n", appendLogger.lastError().message.c_str());
            failures += 1;
        }
        else
        {
            for (int i = 0; i < kSampleCount; ++i)
            {
                const ImuData sample = makeSample(i);
                if (appendLogger.tryAutoWrite(9100000 + i, &sample, sizeof(sample)))
                {
                    ++appendAccepted;
                }
            }

            failures += expect(appendLogger.stopAndFlush(), "real SQL append stopAndFlush succeeds");
            failures += expect(appendAccepted == kSampleCount, "real SQL append accepted all samples");

            OdbcConnection connection;
            if (!connection.connect(connectionString))
            {
                std::printf("FAILED: real SQL append verification connect: %s\n", connection.error().c_str());
                failures += 1;
            }
            else
            {
                RealSqlSummary summary;
                if (!connection.querySmokeSummary(summary))
                {
                    std::printf("FAILED: real SQL append summary query: %s\n", connection.error().c_str());
                    failures += 1;
                }
                else
                {
                    failures += expect(summary.rowCount == accepted + appendAccepted,
                                       "real SQL continue-current-table appends rows");
                }
            }
        }
    }

    if (tableMayExist)
    {
        failures += expect(cleanupSmokeTable(connectionString), "real SQL smoke table cleanup succeeds");
    }

    return failures;
}

// Run a long producer-only pressure loop and print latency distribution details.
int runPressureChecks()
{
    constexpr int kCycles = 100000;
    constexpr std::size_t kQueueCapacity = 131072;

    MockBackend* backend = nullptr;
    auto logger = makeLogger(backend, kQueueCapacity, kCycles);
    if (!logger)
    {
        return 1;
    }

    int failures = 0;
    failures += expect(logger->autoRegisterTables(), "pressure autoRegisterTables succeeds");
    failures += expect(logger->start(), "pressure start succeeds");

    const ImuData sample = makeSample(42);
    std::vector<std::uint64_t> durations;
    durations.resize(kCycles);

    int accepted = 0;
    for (int i = 0; i < kCycles; ++i)
    {
        const auto startedAt = Clock::now();
        if (logger->tryAutoWrite(1000000 + i, &sample, sizeof(sample)))
        {
            ++accepted;
        }

        durations[static_cast<std::size_t>(i)] =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - startedAt).count());
    }

    failures += expect(logger->stopAndFlush(), "pressure stopAndFlush succeeds");
    failures += expect(accepted == kCycles, "pressure accepted every sample");

    auto percentile = [&](double fraction) -> std::uint64_t {
        std::vector<std::uint64_t> copy = durations;
        const std::size_t index = static_cast<std::size_t>((copy.size() - 1) * fraction);
        std::nth_element(copy.begin(), copy.begin() + index, copy.end());
        return copy[index];
    };

    const std::uint64_t p99 = percentile(0.99);
    const std::uint64_t p999 = percentile(0.999);
    const std::uint64_t maxDuration = *std::max_element(durations.begin(), durations.end());
    const AsyncLogger::AsyncDataLoggerStats stats = logger->stats();

    std::printf("PRESSURE producer_samples=%d accepted=%d dropped=%llu p99_ns=%llu p999_ns=%llu max_ns=%llu stats_try_push_max_ns=%llu max_queue_depth=%llu flush_max_ns=%llu rows_inserted=%llu\n",
                kCycles,
                accepted,
                static_cast<unsigned long long>(stats.droppedSamples),
                static_cast<unsigned long long>(p99),
                static_cast<unsigned long long>(p999),
                static_cast<unsigned long long>(maxDuration),
                static_cast<unsigned long long>(stats.tryPushMaxNs),
                static_cast<unsigned long long>(stats.maxQueueDepth),
                static_cast<unsigned long long>(stats.flushMaxNs),
                static_cast<unsigned long long>(backend->rowsInserted.load(std::memory_order_relaxed)));

    return failures == 0 ? 0 : 1;
}

// Run all non-SQL checks that cover async behavior and typed column storage.
int runUnitChecks()
{
    int failures = 0;
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
    failures += testTypedColumnBatchAllNumericTypes();
    failures += testGeneratedManyTableAutoWrite();
    failures += testFullPartialAndRepeatedBatchCapacities();
    return failures;
}
}

// Run the unit-style async logger checks, with optional real SQL smoke.
int runPhaseChecks(int argc, char** argv)
{
    int failures = runUnitChecks();
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
        failures += testRealSqlSmoke(argc, argv);
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
    bool runPressure = false;
    bool runColumnstoreBenchmark = false;
    bool runAll = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        if (argument == "--run-tests" || argument == "--real-sql")
        {
            runTests = true;
        }
        else if (argument == "--pressure")
        {
            runPressure = true;
        }
        else if (argument == "--columnstore-benchmark")
        {
            runColumnstoreBenchmark = true;
        }
        else if (argument == "--all")
        {
            runAll = true;
        }
    }

    if (runAll)
    {
        const int testResult = runPhaseChecks(argc, argv);
        const int pressureResult = runPressureChecks();
        return testResult == 0 && pressureResult == 0 ? 0 : 1;
    }

    if (runPressure)
    {
        return runPressureChecks();
    }

    if (runColumnstoreBenchmark)
    {
        return runSqlIndexBenchmark(argc, argv);
    }

    if (runTests)
    {
        return runPhaseChecks(argc, argv);
    }

    // 1) Configure the synchronous logger: SQL connection, CSV schemas, table policy, and batch size.
    std::string connectionString;
    std::string connectionError;
    if (!readConnectionString(argc, argv, connectionString, connectionError))
    {
        std::printf("%s\n", connectionError.c_str());
        std::printf("Run with --run-tests for mock checks.\n");
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
