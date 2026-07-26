#pragma once

#include "DataLogger/DataLoggerConfig.h"
#include "DataLogger/Error.h"
#include "DataLogger/Schema.h"

#include <string>

namespace DataLoggerCore
{
class IDBBackend
{
public:
    // Allow concrete backend implementations to release ODBC resources via RAII.
    virtual ~IDBBackend() = default;

    // Open the backend connection using the configured ODBC connection string.
    virtual bool connect(const std::string& connectionString) = 0;

    // Apply existing-table policy and prepare SQL tables for the loaded schemas.
    virtual bool initializeTables(const SchemaRegistry& registry,
                                  const std::string& sqlSchemaName,
                                  ExistingTablePolicy policy) = 0;

    // Prepare one reusable insert statement and reusable buffers for each table.
    virtual bool prepareInsertStatements(const SchemaRegistry& registry,
                                         const std::string& sqlSchemaName,
                                         std::size_t batchSizeRows) = 0;

    // Persist one decoded column batch; callers keep rows when this returns false.
    virtual bool insertBatch(const TableSchema& table,
                             const ColumnBatch& batch) = 0;

    // Return the last backend error with optional ODBC diagnostics.
    virtual BackendError lastError() const = 0;
};
}
