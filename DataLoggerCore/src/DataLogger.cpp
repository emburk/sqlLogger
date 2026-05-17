#include "DataLogger/DataLogger.h"

#include "DataLogger/SchemaLoaderCsv.h"

namespace DataLoggerCore
{
bool DataLogger::initialize(const DataLoggerConfig& config)
{
    clearError();
    initialized_ = false;
    config_ = config;
    schemaRegistry_ = SchemaRegistry{};
    tableNameToIndex_.clear();

    if (config.schemaDirectory.empty())
    {
        setError(ErrorCode::InvalidConfig, "Schema directory must not be empty.");
        return false;
    }

    if (config.batchSizeRows == 0)
    {
        setError(ErrorCode::InvalidConfig, "Batch size must be at least 1 row.");
        return false;
    }

    DataLoggerError loadError;
    if (!loadSchemaDirectory(config.schemaDirectory, schemaRegistry_, loadError))
    {
        lastError_ = loadError;
        return false;
    }

    for (std::size_t i = 0; i < schemaRegistry_.tables.size(); ++i)
    {
        const TableSchema& table = schemaRegistry_.tables[i];
        tableNameToIndex_[table.tableName] = i;
    }

    initialized_ = true;
    return true;
}

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

bool DataLogger::isValidTableHandle(TableHandle handle) const
{
    return handle.isValid() && handle.index() < schemaRegistry_.tables.size();
}

const TableSchema* DataLogger::tableSchema(TableHandle handle) const
{
    if (!isValidTableHandle(handle))
    {
        return nullptr;
    }

    return &schemaRegistry_.tables[handle.index()];
}

const SchemaRegistry& DataLogger::schemaRegistry() const
{
    return schemaRegistry_;
}

const DataLoggerError& DataLogger::lastError() const
{
    return lastError_;
}

void DataLogger::clearError()
{
    lastError_ = DataLoggerError{};
}

void DataLogger::setError(ErrorCode code, const std::string& message)
{
    lastError_ = { code, message };
}
}
