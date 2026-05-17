#include "DataLogger/BinaryDecoder.h"
#include "DataLogger/DataLogger.h"
#include "SqlServerBackend/SqlServerOdbcBackend.h"

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace
{
class SmokeTestBackend final : public DataLoggerCore::IDBBackend
{
public:
    // Treat any non-empty string as a successful smoke-test connection.
    bool connect(const std::string& connectionString) override
    {
        connected_ = !connectionString.empty();
        return connected_;
    }

    // Record that table initialization would have run for a loaded schema registry.
    bool initializeTables(const DataLoggerCore::SchemaRegistry& registry,
                          const std::string& sqlSchemaName,
                          DataLoggerCore::ExistingTablePolicy policy) override
    {
        (void)policy;
        tablesInitialized_ = connected_ && !registry.tables.empty() && !sqlSchemaName.empty();
        return tablesInitialized_;
    }

    // Mark statements as prepared only after the smoke tables are initialized.
    bool prepareInsertStatements(const DataLoggerCore::SchemaRegistry& registry,
                                 const std::string& sqlSchemaName) override
    {
        statementsPrepared_ = tablesInitialized_ && !registry.tables.empty() && !sqlSchemaName.empty();
        return statementsPrepared_;
    }

    // Count inserted rows so the example can verify DataLogger flush behavior.
    bool insertBatch(const DataLoggerCore::TableSchema& table,
                     const std::vector<DataLoggerCore::DecodedRow>& rows) override
    {
        if (!statementsPrepared_ || rows.empty())
        {
            return false;
        }

        insertedTableName_ = table.tableName;
        insertedRowCount_ += rows.size();
        return true;
    }

    // The smoke backend only succeeds, so no backend diagnostics are produced.
    DataLoggerCore::BackendError lastError() const override
    {
        return {};
    }

    // Report the number of rows accepted by insertBatch for smoke assertions.
    std::size_t insertedRowCount() const
    {
        return insertedRowCount_;
    }

    // Report the last table name accepted by insertBatch for smoke assertions.
    const std::string& insertedTableName() const
    {
        return insertedTableName_;
    }

private:
    bool connected_ = false;
    bool tablesInitialized_ = false;
    bool statementsPrepared_ = false;
    std::size_t insertedRowCount_ = 0;
    std::string insertedTableName_;
};

// Read the optional SQL Server connection string from the process environment.
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

// Use the real ODBC backend only when the caller supplies a connection string.
std::unique_ptr<DataLoggerCore::IDBBackend> createBackend(const std::string& connectionString,
                                                          SmokeTestBackend*& smokeBackendView)
{
    smokeBackendView = nullptr;
    if (!connectionString.empty())
    {
        return std::make_unique<SqlServerBackend::SqlServerOdbcBackend>();
    }

    auto backend = std::make_unique<SmokeTestBackend>();
    smokeBackendView = backend.get();
    return backend;
}

// Print the logger's structured error for smoke and integration diagnostics.
int failWithLastError(const DataLoggerCore::DataLogger& logger)
{
    const DataLoggerCore::DataLoggerError& error = logger.lastError();
    if (!error.message.empty())
    {
        std::cerr << error.message << '\n';
    }
    return 1;
}
}

// The example struct is packed so its offsets intentionally match imu_data.csv.
#pragma pack(push, 1)
struct ImuData
{
    std::int64_t unusedOrSequence = 0;
    float gyro[3] = {};
    float accel[3] = {};
    double temperature = 0.0;
    std::uint16_t status = 0;
};
#pragma pack(pop)

// Load the example schema, verify decoding, and exercise DataLogger buffering.
int main()
{
    // Use a real ODBC backend when SQLLOGGER_CONNECTION_STRING is provided.
    const std::string connectionString = readConnectionString();
    DataLoggerCore::DataLoggerConfig config;
    config.connectionString = !connectionString.empty() ? connectionString : "SmokeTestBackend";
    config.schemaDirectory = "ExampleApp\\schemas";
    config.batchSizeRows = 2;

    SmokeTestBackend* smokeBackendView = nullptr;
    auto backend = createBackend(connectionString, smokeBackendView);

    DataLoggerCore::DataLogger logger(std::move(backend));
    if (!logger.initialize(config))
    {
        return failWithLastError(logger);
    }

    const DataLoggerCore::TableHandle imu = logger.registerTable("imu_data");
    if (!imu.isValid())
    {
        return failWithLastError(logger);
    }

    const DataLoggerCore::TableSchema* schema = logger.tableSchema(imu);
    if (schema == nullptr)
    {
        return 1;
    }

    // Populate known values so the decoder can be checked without SQL or ODBC.
    ImuData sample;
    sample.unusedOrSequence = 42;
    sample.gyro[0] = 1.0F;
    sample.gyro[1] = 2.0F;
    sample.gyro[2] = 3.0F;
    sample.accel[0] = 4.0F;
    sample.accel[1] = 5.0F;
    sample.accel[2] = 6.0F;
    sample.temperature = 7.5;
    sample.status = 9;

    // Decode one row directly and verify timestamp plus expanded payload order.
    DataLoggerCore::DecodedRow row;
    DataLoggerCore::DataLoggerError error;
    if (!DataLoggerCore::decodeRow(*schema, 123456789, &sample, row, error))
    {
        return 1;
    }

    if (row.timestampMs != 123456789 || row.values.size() != 8)
    {
        return 1;
    }

    if (std::get<float>(row.values[0]) != 1.0F ||
        std::get<float>(row.values[1]) != 2.0F ||
        std::get<float>(row.values[2]) != 3.0F ||
        std::get<float>(row.values[3]) != 4.0F ||
        std::get<float>(row.values[4]) != 5.0F ||
        std::get<float>(row.values[5]) != 6.0F ||
        std::get<double>(row.values[6]) != 7.5 ||
        std::get<std::uint16_t>(row.values[7]) != 9)
    {
        return 1;
    }

    // Write two rows so batch-size flushing reaches the backend and clears the buffer.
    if (!logger.write(imu, 123456789, &sample))
    {
        return failWithLastError(logger);
    }

    if (smokeBackendView != nullptr && smokeBackendView->insertedRowCount() != 0)
    {
        return 1;
    }

    if (!logger.write(imu, 123456790, &sample))
    {
        return failWithLastError(logger);
    }

    if (smokeBackendView != nullptr &&
        (smokeBackendView->insertedRowCount() != 2 || smokeBackendView->insertedTableName() != "imu_data"))
    {
        return 1;
    }

    if (!logger.flush(imu) || !logger.flush())
    {
        return failWithLastError(logger);
    }

    logger.shutdown();
    return 0;
}
