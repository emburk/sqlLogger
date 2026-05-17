#include "DataLogger/DataLogger.h"

#include "DataLogger/BinaryDecoder.h"
#include "DataLogger/SchemaLoaderCsv.h"

#include <utility>

namespace DataLoggerCore
{
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

    config_ = config;

    DataLoggerError loadError;
    if (!loadSchemaDirectory(config.schemaDirectory, schemaRegistry_, loadError))
    {
        resetRuntimeState();
        lastError_ = loadError;
        return false;
    }

    for (std::size_t i = 0; i < schemaRegistry_.tables.size(); ++i)
    {
        const TableSchema& table = schemaRegistry_.tables[i];
        tableNameToIndex_[table.tableName] = i;
    }

    if (!backend_->connect(config.connectionString))
    {
        resetRuntimeState();
        setBackendError(ErrorCode::BackendConnectFailed, "Backend connection failed");
        return false;
    }

    if (!backend_->initializeTables(schemaRegistry_, config.sqlSchemaName, config.existingTablePolicy))
    {
        resetRuntimeState();
        setBackendError(ErrorCode::BackendTableInitFailed, "Backend table initialization failed");
        return false;
    }

    if (!backend_->prepareInsertStatements(schemaRegistry_, config.sqlSchemaName))
    {
        resetRuntimeState();
        setBackendError(ErrorCode::BackendPrepareFailed, "Backend insert preparation failed");
        return false;
    }

    tableBuffers_.reserve(schemaRegistry_.tables.size());
    for (std::size_t i = 0; i < schemaRegistry_.tables.size(); ++i)
    {
        TableBuffer buffer;
        buffer.handle = TableHandle(i);
        buffer.schema = &schemaRegistry_.tables[i];
        tableBuffers_.push_back(buffer);
    }

    initialized_ = true;
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
    DecodedRow row;
    DataLoggerError decodeError;
    if (!decodeRow(*buffer.schema, timestampMs, structPtr, row, decodeError))
    {
        lastError_ = decodeError;
        return false;
    }

    buffer.rows.push_back(row);

    if (buffer.rows.size() >= config_.batchSizeRows)
    {
        return flush(table);
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
        if (!buffer.rows.empty() && !flush(buffer.handle))
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
    if (buffer.rows.empty())
    {
        return true;
    }

    if (!backend_->insertBatch(*buffer.schema, buffer.rows))
    {
        setBackendError(ErrorCode::BackendInsertFailed, "Backend batch insert failed");
        return false;
    }

    buffer.rows.clear();
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
    lastError_ = { code, message };
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
        message += " " + backendError.message;
    }

    if (!backendError.diagnostics.empty())
    {
        message += " ODBC diagnostics:";
        for (const OdbcDiagnostic& diagnostic : backendError.diagnostics)
        {
            message += " [" + diagnostic.sqlState + ", " + std::to_string(diagnostic.nativeError) + "] " + diagnostic.message;
        }
    }

    setError(code, message);
}

// Reset schema-owned runtime state while keeping the injected backend instance.
void DataLogger::resetRuntimeState()
{
    initialized_ = false;
    config_ = DataLoggerConfig{};
    schemaRegistry_ = SchemaRegistry{};
    tableNameToIndex_.clear();
    tableBuffers_.clear();
}
}
