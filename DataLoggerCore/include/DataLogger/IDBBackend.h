#pragma once

#include "DataLogger/DataLoggerConfig.h"
#include "DataLogger/Error.h"
#include "DataLogger/Schema.h"

#include <string>
#include <vector>

namespace DataLoggerCore
{
class IDBBackend
{
public:
    // Allow concrete backend implementations to release ODBC resources via RAII.
    virtual ~IDBBackend() = default;

    // Open the backend connection using the configured ODBC connection string.
    virtual bool connect(const std::string& connectionString) = 0;

    // Apply existing-table policy and create fresh tables for the loaded schemas.
    virtual bool initializeTables(const SchemaRegistry& registry,
                                  const std::string& sqlSchemaName,
                                  ExistingTablePolicy policy) = 0;

    // Prepare one reusable insert statement for each schema table.
    virtual bool prepareInsertStatements(const SchemaRegistry& registry,
                                         const std::string& sqlSchemaName) = 0;

    // Persist one decoded table batch; callers keep rows when this returns false.
    virtual bool insertBatch(const TableSchema& table,
                             const std::vector<DecodedRow>& rows) = 0;

    // Return the last backend error with optional ODBC diagnostics.
    virtual BackendError lastError() const = 0;
};
}
