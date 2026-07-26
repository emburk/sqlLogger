#include "DataLogger/BinaryDecoder.h"
#include "DataLogger/DataLogger.h"
#include "DataLogger/SchemaLoaderCsv.h"
#include "DataLogger/SchemaValidator.h"
#include "SqlServerBackend/SqlServerOdbcBackend.h"

#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
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
                          DataLoggerCore::ExistingTablePolicy policy,
                          DataLoggerCore::SqlServerIndexMode indexMode) override
    {
        (void)policy;
        (void)indexMode;
        tablesInitialized_ = connected_ && !registry.tables.empty() && !sqlSchemaName.empty();
        return tablesInitialized_;
    }

    // Mark statements as prepared only after the smoke tables are initialized.
    bool prepareInsertStatements(const DataLoggerCore::SchemaRegistry& registry,
                                 const std::string& sqlSchemaName,
                                 std::size_t batchSizeRows) override
    {
        (void)batchSizeRows;
        statementsPrepared_ = tablesInitialized_ && !registry.tables.empty() && !sqlSchemaName.empty();
        return statementsPrepared_;
    }

    // Count inserted rows so the example can verify DataLogger flush behavior.
    bool insertBatch(const DataLoggerCore::TableSchema& table,
                     const DataLoggerCore::ColumnBatch& batch) override
    {
        if (!statementsPrepared_ || batch.rowCount == 0)
        {
            return false;
        }

        insertedTableName_ = table.tableName;
        insertedRowCount_ += batch.rowCount;
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

class Phase9RetryBackend final : public DataLoggerCore::IDBBackend
{
public:
    bool failNextInsert = true;
    std::size_t insertAttempts = 0;
    std::size_t insertedRows = 0;

    // Accept any non-empty connection string for local DataLogger behavior tests.
    bool connect(const std::string& connectionString) override
    {
        connected_ = !connectionString.empty();
        return connected_;
    }

    // Confirm schema loading reached the backend initialization stage.
    bool initializeTables(const DataLoggerCore::SchemaRegistry& registry,
                          const std::string& sqlSchemaName,
                          DataLoggerCore::ExistingTablePolicy policy,
                          DataLoggerCore::SqlServerIndexMode indexMode) override
    {
        (void)policy;
        (void)indexMode;
        initialized_ = connected_ && !registry.tables.empty() && !sqlSchemaName.empty();
        return initialized_;
    }

    // Confirm insert preparation was requested after table initialization.
    bool prepareInsertStatements(const DataLoggerCore::SchemaRegistry& registry,
                                 const std::string& sqlSchemaName,
                                 std::size_t batchSizeRows) override
    {
        (void)batchSizeRows;
        prepared_ = initialized_ && !registry.tables.empty() && !sqlSchemaName.empty();
        return prepared_;
    }

    // Fail the first insert on demand, then accept rows to verify retry behavior.
    bool insertBatch(const DataLoggerCore::TableSchema& table,
                     const DataLoggerCore::ColumnBatch& batch) override
    {
        (void)table;
        ++insertAttempts;
        if (failNextInsert)
        {
            failNextInsert = false;
            error_.message = "Intentional Phase 9 insert failure.";
            return false;
        }

        insertedRows += batch.rowCount;
        error_ = {};
        return true;
    }

    // Return the latest intentional backend error for DataLogger error wrapping.
    DataLoggerCore::BackendError lastError() const override
    {
        return error_;
    }

private:
    bool connected_ = false;
    bool initialized_ = false;
    bool prepared_ = false;
    DataLoggerCore::BackendError error_;
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

// Return the workspace-local directory used by the opt-in Phase 9 harness.
std::filesystem::path phase9Root()
{
    return std::filesystem::path("x64") / "Phase9TestData";
}

// Write one test schema file, creating parent directories when needed.
bool writeTextFile(const std::filesystem::path& path, const std::string& text)
{
    std::error_code createError;
    std::filesystem::create_directories(path.parent_path(), createError);
    if (createError)
    {
        return false;
    }

    std::ofstream output(path, std::ios::trunc);
    if (!output)
    {
        return false;
    }

    output << text;
    return static_cast<bool>(output);
}

// Record a failed assertion while keeping the remaining Phase 9 checks running.
bool expectPhase9(bool condition, const std::string& message, std::vector<std::string>& failures)
{
    if (!condition)
    {
        failures.push_back(message);
        return false;
    }

    return true;
}

// Create the valid schema used by parsing, expansion, decoding, and retry tests.
bool prepareValidPhase9Schema(const std::filesystem::path& directory)
{
    const std::string schema =
        "column_name,offset,datatype,size,length,unit,description\n"
        "# comments should be ignored\n"
        "gyro,0,float,4,3,rad/s,\"body angular, rate\"\n"
        "status,12,uint16,2,1,,status code\n";
    return writeTextFile(directory / "phase9_valid.csv", schema);
}

// Verify CSV loading, quoted metadata parsing, and scalar/array expansion order.
bool testSchemaParsingAndExpansion(std::vector<std::string>& failures)
{
    const std::filesystem::path directory = phase9Root() / "valid";
    if (!expectPhase9(prepareValidPhase9Schema(directory), "failed to create valid Phase 9 schema", failures))
    {
        return false;
    }

    DataLoggerCore::SchemaRegistry registry;
    DataLoggerCore::DataLoggerError error;
    const bool loaded = DataLoggerCore::loadSchemaDirectory(directory.string(), registry, error);

    bool passed = true;
    passed &= expectPhase9(loaded, "valid schema did not load: " + error.message, failures);
    passed &= expectPhase9(registry.tables.size() == 1, "valid schema registry table count was not 1", failures);
    if (!loaded || registry.tables.empty())
    {
        return false;
    }

    const DataLoggerCore::TableSchema& table = registry.tables.front();
    passed &= expectPhase9(table.tableName == "phase9_valid", "table name was not derived from filename", failures);
    passed &= expectPhase9(table.columns.size() == 2, "base column count was not 2", failures);
    passed &= expectPhase9(table.columns[0].description == "body angular, rate", "quoted description was not parsed correctly", failures);
    passed &= expectPhase9(table.expandedColumns.size() == 4, "expanded column count was not 4", failures);
    passed &= expectPhase9(table.expandedColumns[0].sqlName == "gyro_0", "expanded column 0 was not gyro_0", failures);
    passed &= expectPhase9(table.expandedColumns[1].sqlName == "gyro_1", "expanded column 1 was not gyro_1", failures);
    passed &= expectPhase9(table.expandedColumns[2].sqlName == "gyro_2", "expanded column 2 was not gyro_2", failures);
    passed &= expectPhase9(table.expandedColumns[3].sqlName == "status", "scalar column was not kept as status", failures);
    return passed;
}

// Verify duplicate expanded SQL names are rejected by schema validation.
bool testDuplicateColumnRejection(std::vector<std::string>& failures)
{
    const std::filesystem::path directory = phase9Root() / "duplicate";
    const std::string schema =
        "column_name,offset,datatype,size,length\n"
        "gyro,0,float,4,2\n"
        "gyro_0,8,float,4,1\n";
    if (!expectPhase9(writeTextFile(directory / "phase9_duplicate.csv", schema), "failed to create duplicate schema", failures))
    {
        return false;
    }

    DataLoggerCore::SchemaRegistry registry;
    DataLoggerCore::DataLoggerError error;
    const bool loaded = DataLoggerCore::loadSchemaDirectory(directory.string(), registry, error);
    bool passed = true;
    passed &= expectPhase9(!loaded, "duplicate expanded column schema unexpectedly loaded", failures);
    passed &= expectPhase9(error.message.find("Duplicate expanded column") != std::string::npos,
                           "duplicate schema did not report duplicate expanded column",
                           failures);
    return passed;
}

// Verify fixed-offset binary decoding fills typed column vectors in schema order.
bool testBinaryDecoding(std::vector<std::string>& failures)
{
    const std::filesystem::path directory = phase9Root() / "valid";
    DataLoggerCore::SchemaRegistry registry;
    DataLoggerCore::DataLoggerError error;
    if (!DataLoggerCore::loadSchemaDirectory(directory.string(), registry, error) || registry.tables.empty())
    {
        failures.push_back("valid schema was unavailable for binary decoding: " + error.message);
        return false;
    }

#pragma pack(push, 1)
    struct Phase9Payload
    {
        float gyro[3] = {};
        std::uint16_t status = 0;
    };
#pragma pack(pop)

    Phase9Payload payload;
    payload.gyro[0] = 1.25F;
    payload.gyro[1] = 2.5F;
    payload.gyro[2] = 3.75F;
    payload.status = 12;

    DataLoggerCore::ColumnBatch batch;
    DataLoggerCore::initializeColumnBatch(registry.tables.front(), 1, batch);
    const bool decoded = DataLoggerCore::decodeIntoColumnBatch(registry.tables.front(), 987654321, &payload, batch, error);

    bool passed = true;
    passed &= expectPhase9(decoded, "binary row did not decode: " + error.message, failures);
    passed &= expectPhase9(batch.rowCount == 1, "decoded row count was not 1", failures);
    passed &= expectPhase9(batch.timestamps[0] == 987654321, "decoded timestamp did not match caller value", failures);
    passed &= expectPhase9(batch.columns.size() == 4, "decoded column count was not 4", failures);
    if (!decoded || batch.columns.size() != 4)
    {
        return false;
    }

    passed &= expectPhase9(batch.columns[0].floatValues[0] == 1.25F, "decoded gyro_0 mismatch", failures);
    passed &= expectPhase9(batch.columns[1].floatValues[0] == 2.5F, "decoded gyro_1 mismatch", failures);
    passed &= expectPhase9(batch.columns[2].floatValues[0] == 3.75F, "decoded gyro_2 mismatch", failures);
    passed &= expectPhase9(batch.columns[3].int32Values[0] == 12, "decoded status mismatch", failures);
    return passed;
}

// Verify a failed flush keeps rows buffered so a later retry can persist them.
bool testFlushFailureRetention(std::vector<std::string>& failures)
{
    auto backend = std::make_unique<Phase9RetryBackend>();
    Phase9RetryBackend* backendView = backend.get();

    DataLoggerCore::DataLoggerConfig config;
    config.connectionString = "Phase9RetryBackend";
    config.schemaDirectory = (phase9Root() / "valid").string();
    config.batchSizeRows = 1;

    DataLoggerCore::DataLogger logger(std::move(backend));
    bool passed = true;
    passed &= expectPhase9(logger.initialize(config), "retry logger did not initialize: " + logger.lastError().message, failures);
    const DataLoggerCore::TableHandle handle = logger.registerTable("phase9_valid");
    passed &= expectPhase9(handle.isValid(), "retry logger did not register phase9_valid", failures);
    if (!passed)
    {
        return false;
    }

#pragma pack(push, 1)
    struct Phase9Payload
    {
        float gyro[3] = {};
        std::uint16_t status = 0;
    };
#pragma pack(pop)

    Phase9Payload payload;
    payload.gyro[0] = 4.0F;
    payload.gyro[1] = 5.0F;
    payload.gyro[2] = 6.0F;
    payload.status = 7;

    passed &= expectPhase9(!logger.write(handle, 1000, &payload), "write unexpectedly succeeded during intentional insert failure", failures);
    passed &= expectPhase9(backendView->insertAttempts == 1, "first failed insert was not attempted exactly once", failures);
    passed &= expectPhase9(logger.flush(handle), "retry flush did not succeed: " + logger.lastError().message, failures);
    passed &= expectPhase9(backendView->insertAttempts == 2, "retry flush did not attempt insert exactly once", failures);
    passed &= expectPhase9(backendView->insertedRows == 1, "retained row was not inserted after retry", failures);
    logger.shutdown();
    return passed;
}

// Verify a schema exceeding the configured SQL column limit fails clearly.
bool testWideSchemaRejection(std::vector<std::string>& failures)
{
    std::ostringstream schema;
    schema << "column_name,offset,datatype,size,length\n";
    schema << "wide,0,float,4," << DataLoggerCore::kSqlServerMaxColumnsPerTable << "\n";

    const std::filesystem::path directory = phase9Root() / "wide";
    if (!expectPhase9(writeTextFile(directory / "phase9_wide.csv", schema.str()), "failed to create wide schema", failures))
    {
        return false;
    }

    DataLoggerCore::SchemaRegistry registry;
    DataLoggerCore::DataLoggerError error;
    const bool loaded = DataLoggerCore::loadSchemaDirectory(directory.string(), registry, error);

    bool passed = true;
    passed &= expectPhase9(!loaded, "wide schema unexpectedly loaded", failures);
    passed &= expectPhase9(error.message.find("Split the schema") != std::string::npos,
                           "wide schema did not include split-schema guidance",
                           failures);
    return passed;
}

// Run the opt-in Phase 9 non-database validation suite.
int runPhase9Tests()
{
    std::vector<std::string> failures;
    testSchemaParsingAndExpansion(failures);
    testDuplicateColumnRejection(failures);
    testBinaryDecoding(failures);
    testFlushFailureRetention(failures);
    testWideSchemaRejection(failures);

    if (!failures.empty())
    {
        for (const std::string& failure : failures)
        {
            std::cerr << "PHASE9_TEST_FAILURE: " << failure << '\n';
        }
        return 1;
    }

    std::cout << "PHASE9_TESTS_PASSED\n";
    return 0;
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
int main(int argc, char* argv[])
{
    if (argc == 2 && std::string(argv[1]) == "--phase9-tests")
    {
        return runPhase9Tests();
    }

    // Use a real ODBC backend when SQLLOGGER_CONNECTION_STRING is provided.
    const std::string connectionString = readConnectionString();
    DataLoggerCore::DataLoggerConfig config;
    config.connectionString = !connectionString.empty() ? connectionString : "SmokeTestBackend";
    config.schemaDirectory = "examples\\ExampleApp\\schemas";
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
    DataLoggerCore::ColumnBatch batch;
    DataLoggerCore::initializeColumnBatch(*schema, 1, batch);
    DataLoggerCore::DataLoggerError error;
    if (!DataLoggerCore::decodeIntoColumnBatch(*schema, 123456789, &sample, batch, error))
    {
        return 1;
    }

    if (batch.rowCount != 1 || batch.timestamps[0] != 123456789 || batch.columns.size() != 8)
    {
        return 1;
    }

    if (batch.columns[0].floatValues[0] != 1.0F ||
        batch.columns[1].floatValues[0] != 2.0F ||
        batch.columns[2].floatValues[0] != 3.0F ||
        batch.columns[3].floatValues[0] != 4.0F ||
        batch.columns[4].floatValues[0] != 5.0F ||
        batch.columns[5].floatValues[0] != 6.0F ||
        batch.columns[6].doubleValues[0] != 7.5 ||
        batch.columns[7].int32Values[0] != 9)
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
