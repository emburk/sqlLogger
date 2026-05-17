#pragma once

#include "DataLogger/DataLoggerConfig.h"
#include "DataLogger/Error.h"
#include "DataLogger/Schema.h"
#include "DataLogger/TableHandle.h"

#include <string>
#include <unordered_map>

namespace DataLoggerCore
{
class DataLogger
{
public:
    bool initialize(const DataLoggerConfig& config);

    TableHandle registerTable(const std::string& tableName);
    bool isValidTableHandle(TableHandle handle) const;

    const TableSchema* tableSchema(TableHandle handle) const;
    const SchemaRegistry& schemaRegistry() const;
    const DataLoggerError& lastError() const;

private:
    void clearError();
    void setError(ErrorCode code, const std::string& message);

    DataLoggerConfig config_;
    SchemaRegistry schemaRegistry_;
    std::unordered_map<std::string, std::size_t> tableNameToIndex_;
    DataLoggerError lastError_;
    bool initialized_ = false;
};
}
