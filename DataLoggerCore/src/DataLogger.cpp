#include "DataLogger/DataLogger.h"

#include "DataLogger/BinaryDecoder.h"
#include "DataLogger/SchemaLoaderCsv.h"

#include <iostream>
#include <utility>

namespace DataLoggerCore
{
namespace
{
// Convert the table policy to stable debug text without exposing enum values.
const char* existingTablePolicyName(ExistingTablePolicy policy)
{
    switch (policy)
    {
    case ExistingTablePolicy::Drop:
        return "Drop";
    case ExistingTablePolicy::RenameWithTimestampSuffix:
        return "RenameWithTimestampSuffix";
    case ExistingTablePolicy::ContinueCurrentTable:
        return "ContinueCurrentTable";
    }

    return "Unknown";
}
}

// Store the backend dependency supplied by the application.
DataLogger::DataLogger(std::unique_ptr<IDBBackend> backend)
    : backend_(std::move(backend))
{
}

// Attempt the documented shutdown flush when the logger leaves scope.
DataLogger::~DataLogger()
{
    shutdown();
}

// Load and validate schemas, initialize backend tables, prepare inserts, and build buffers.
bool DataLogger::initialize(const DataLoggerConfig& config)
{
    clearError();
    resetRuntimeState();
    config_ = config;

    if (config.schemaDirectory.empty())
    {
        setError(ErrorCode::InvalidConfig, "Schema directory must not be empty.");
        return false;
    }

    if (config.connectionString.empty())
    {
        setError(ErrorCode::InvalidConfig, "ODBC connection string must not be empty.");
        return false;
    }

    if (config.sqlSchemaName.empty())
    {
        setError(ErrorCode::InvalidConfig, "SQL schema name must not be empty.");
        return false;
    }

    if (config.batchSizeRows == 0)
    {
        setError(ErrorCode::InvalidConfig, "Batch size must be at least 1 row.");
        return false;
    }

    if (backend_ == nullptr)
    {
        setError(ErrorCode::InvalidConfig, "DataLogger requires a database backend.");
        return false;
    }

    DataLoggerError loadError;
    if (!loadSchemaDirectory(config.schemaDirectory, schemaRegistry_, loadError))
    {
        setError(loadError);
        resetRuntimeState();
        return false;
    }

    for (std::size_t i = 0; i < schemaRegistry_.tables.size(); ++i)
    {
        const TableSchema& table = schemaRegistry_.tables[i];
        tableNameToIndex_[table.tableName] = i;
    }

    if (!backend_->connect(config.connectionString))
    {
        setBackendError(ErrorCode::BackendConnectFailed, "Backend connection failed");
        resetRuntimeState();
        return false;
    }

    if (!backend_->initializeTables(schemaRegistry_, config.sqlSchemaName, config.existingTablePolicy))
    {
        setBackendError(ErrorCode::BackendTableInitFailed, "Backend table initialization failed");
        resetRuntimeState();
        return false;
    }

    if (!backend_->prepareInsertStatements(schemaRegistry_, config.sqlSchemaName, config.batchSizeRows))
    {
        setBackendError(ErrorCode::BackendPrepareFailed, "Backend insert preparation failed");
        resetRuntimeState();
        return false;
    }

    tableBuffers_.reserve(schemaRegistry_.tables.size());
    for (std::size_t i = 0; i < schemaRegistry_.tables.size(); ++i)
    {
        TableBuffer buffer;
        buffer.handle = TableHandle(i);
        buffer.schema = &schemaRegistry_.tables[i];
        initializeColumnBatch(*buffer.schema, config.batchSizeRows, buffer.batch);
        tableBuffers_.push_back(std::move(buffer));
    }

    initialized_ = true;
    printInitializationSuccess();
    return true;
}

// Explicit shutdown prefers a successful flush; failed buffers stay available for retry.
void DataLogger::shutdown()
{
    if (!initialized_)
    {
        return;
    }

    if (!flush())
    {
        return;
    }

    resetRuntimeState();
    clearError();
}

// Resolve a validated schema table name into a compact handle for later write calls.
TableHandle DataLogger::registerTable(const std::string& tableName)
{
    clearError();

    if (!initialized_)
    {
        setError(ErrorCode::InvalidConfig, "DataLogger must be initialized before registering tables.");
        return TableHandle::invalid();
    }

    const auto found = tableNameToIndex_.find(tableName);
    if (found == tableNameToIndex_.end())
    {
        setError(ErrorCode::InvalidTableHandle, "No schema table named '" + tableName + "' is loaded.");
        return TableHandle::invalid();
    }

    return TableHandle(found->second);
}

// Cache handles for every loaded table so callers can write split schemas together.
bool DataLogger::autoRegisterTables()
{
    clearError();

    if (!initialized_)
    {
        setError(ErrorCode::InvalidConfig, "DataLogger must be initialized before automatic table registration.");
        return false;
    }

    autoRegisteredTables_.clear();
    autoRegisteredTables_.reserve(tableBuffers_.size());

    for (const TableBuffer& buffer : tableBuffers_)
    {
        autoRegisteredTables_.push_back(buffer.handle);
    }

    return true;
}

// Decode and buffer one row, then flush this table if the configured batch size is reached.
bool DataLogger::write(TableHandle table, std::int64_t timestampMs, const void* structPtr)
{
    clearError();

    if (!initialized_)
    {
        setError(ErrorCode::InvalidConfig, "DataLogger must be initialized before writing rows.");
        return false;
    }

    if (!isValidTableHandle(table))
    {
        setError(ErrorCode::InvalidTableHandle, "Cannot write using an invalid table handle.");
        return false;
    }

    TableBuffer& buffer = tableBuffers_[table.index()];
    if (buffer.batch.rowCount >= buffer.batch.rowCapacity && !flush(table))
    {
        return false;
    }

    DataLoggerError decodeError;
    if (!decodeIntoColumnBatch(*buffer.schema, timestampMs, structPtr, buffer.batch, decodeError))
    {
        setError(decodeError);
        return false;
    }

    if (buffer.batch.rowCount >= config_.batchSizeRows)
    {
        return flush(table);
    }

    return true;
}

// Write one decoded row into every table selected by automatic registration.
bool DataLogger::autoWrite(std::int64_t timestampMs, const void* structPtr)
{
    clearError();

    if (!initialized_)
    {
        setError(ErrorCode::InvalidConfig, "DataLogger must be initialized before automatic writes.");
        return false;
    }

    if (autoRegisteredTables_.empty())
    {
        setError(ErrorCode::InvalidConfig, "Call autoRegisterTables() before autoWrite().");
        return false;
    }

    for (TableHandle table : autoRegisteredTables_)
    {
        if (!write(table, timestampMs, structPtr))
        {
            return false;
        }
    }

    return true;
}

// Flush every non-empty table buffer in deterministic schema order.
bool DataLogger::flush()
{
    clearError();

    if (!initialized_)
    {
        setError(ErrorCode::InvalidConfig, "DataLogger must be initialized before flushing rows.");
        return false;
    }

    for (const TableBuffer& buffer : tableBuffers_)
    {
        if (buffer.batch.rowCount > 0 && !flush(buffer.handle))
        {
            return false;
        }
    }

    return true;
}

// Persist one table buffer. On backend failure, keep the rows intact for retry.
bool DataLogger::flush(TableHandle table)
{
    clearError();

    if (!initialized_)
    {
        setError(ErrorCode::InvalidConfig, "DataLogger must be initialized before flushing rows.");
        return false;
    }

    if (!isValidTableHandle(table))
    {
        setError(ErrorCode::InvalidTableHandle, "Cannot flush using an invalid table handle.");
        return false;
    }

    TableBuffer& buffer = tableBuffers_[table.index()];
    if (buffer.batch.rowCount == 0)
    {
        return true;
    }

    if (!backend_->insertBatch(*buffer.schema, buffer.batch))
    {
        setBackendError(ErrorCode::BackendInsertFailed, "Backend batch insert failed");
        return false;
    }

    buffer.batch.rowCount = 0;
    return true;
}

// A valid handle is an in-range table buffer index.
bool DataLogger::isValidTableHandle(TableHandle handle) const
{
    return handle.isValid() && handle.index() < tableBuffers_.size();
}

// Return the schema behind a handle, or nullptr if the caller supplied an invalid handle.
const TableSchema* DataLogger::tableSchema(TableHandle handle) const
{
    if (!isValidTableHandle(handle))
    {
        return nullptr;
    }

    return tableBuffers_[handle.index()].schema;
}

// Expose immutable runtime schema metadata for backend/table creation code and diagnostics.
const SchemaRegistry& DataLogger::schemaRegistry() const
{
    return schemaRegistry_;
}

// Keep error state queryable without printing or throwing from core code.
const DataLoggerError& DataLogger::lastError() const
{
    return lastError_;
}

// Reset the last operation error before starting a new public operation.
void DataLogger::clearError()
{
    lastError_ = DataLoggerError{};
}

// Store a small structured error object for the caller to inspect.
void DataLogger::setError(ErrorCode code, const std::string& message)
{
    setError(DataLoggerError{ code, message });
}

// Store the latest error and print it when the caller leaves error printing on.
void DataLogger::setError(const DataLoggerError& error)
{
    lastError_ = error;
    if (config_.printErrorFlag && !lastError_.message.empty())
    {
        std::cerr << "DataLogger error:" << '\n'
                  << lastError_.message << '\n';
    }
}

// Preserve backend detail without exposing backend-specific code through DataLogger operations.
void DataLogger::setBackendError(ErrorCode code, const std::string& prefix)
{
    BackendError backendError;
    if (backend_ != nullptr)
    {
        backendError = backend_->lastError();
    }

    std::string message = prefix + ".";
    if (!backendError.message.empty())
    {
        message += "\nBackend error: " + backendError.message;
    }

    if (!backendError.diagnostics.empty())
    {
        message += "\nODBC diagnostics:";
        for (const OdbcDiagnostic& diagnostic : backendError.diagnostics)
        {
            message += "\n  [" + diagnostic.sqlState + ", " + std::to_string(diagnostic.nativeError) + "] " + diagnostic.message;
        }
    }

    setError(code, message);
}

// Emit line-oriented initialization details after database setup succeeds.
void DataLogger::printInitializationSuccess() const
{
    if (!config_.printInfoFlag)
    {
        return;
    }

    std::cout << "DataLogger initialization succeeded:" << '\n'
              << "Database backend: connected" << '\n'
              << "SQL schema: [" << config_.sqlSchemaName << "]" << '\n';

    std::cout << "Tables initialized:" << '\n';
    for (const TableSchema& table : schemaRegistry_.tables)
    {
        std::cout << "  [" << config_.sqlSchemaName << "].[" << table.tableName << "]" << '\n';
    }

    std::cout << "Existing-table policy: " << existingTablePolicyName(config_.existingTablePolicy) << '\n'
              << "Insert statements: prepared" << '\n'
              << "Batch size rows: " << config_.batchSizeRows << '\n';
}

// Reset schema-owned runtime state while keeping the injected backend instance.
void DataLogger::resetRuntimeState()
{
    initialized_ = false;
    config_ = DataLoggerConfig{};
    schemaRegistry_ = SchemaRegistry{};
    tableNameToIndex_.clear();
    tableBuffers_.clear();
    autoRegisteredTables_.clear();
}
}
