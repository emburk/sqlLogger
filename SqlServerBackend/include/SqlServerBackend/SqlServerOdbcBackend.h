#pragma once

#include "DataLogger/IDBBackend.h"

#include <cstddef>
#include <memory>

namespace SqlServerBackend
{
class SqlServerOdbcBackend final : public DataLoggerCore::IDBBackend
{
public:
    // Construct the concrete SQL Server backend and its hidden ODBC state.
    SqlServerOdbcBackend();
    // Release prepared statements, connection, and environment handles through RAII.
    ~SqlServerOdbcBackend() override;

    SqlServerOdbcBackend(const SqlServerOdbcBackend&) = delete;
    SqlServerOdbcBackend& operator=(const SqlServerOdbcBackend&) = delete;

    // Open the SQL Server ODBC connection using the caller-provided connection string.
    bool connect(const std::string& connectionString) override;

    // Apply existing-table policy, prepare SQL tables, and create timestamp indexes.
    bool initializeTables(const DataLoggerCore::SchemaRegistry& registry,
                          const std::string& sqlSchemaName,
                          DataLoggerCore::ExistingTablePolicy policy) override;

    // Prepare one reusable parameterized INSERT statement and buffers per table.
    bool prepareInsertStatements(const DataLoggerCore::SchemaRegistry& registry,
                                 const std::string& sqlSchemaName,
                                 std::size_t batchSizeRows) override;

    // Insert one decoded column batch using ODBC column-wise parameter arrays.
    bool insertBatch(const DataLoggerCore::TableSchema& table,
                     const DataLoggerCore::ColumnBatch& batch) override;

    // Return the most recent backend error and collected ODBC diagnostics.
    DataLoggerCore::BackendError lastError() const override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}
