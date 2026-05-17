#pragma once

#include "DataLogger/DataLoggerConfig.h"
#include "DataLogger/Error.h"
#include "DataLogger/IDBBackend.h"
#include "DataLogger/Schema.h"
#include "DataLogger/TableHandle.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace DataLoggerCore
{
class DataLogger
{
public:
    // Own the database backend used for table setup and batch persistence.
    explicit DataLogger(std::unique_ptr<IDBBackend> backend);
    // Flush remaining rows on destruction when the logger is still initialized.
    ~DataLogger();

    // Load schemas, initialize backend tables, prepare inserts, and create buffers.
    bool initialize(const DataLoggerConfig& config);
    // Flush pending rows and clear runtime state when the flush succeeds.
    void shutdown();

    // Resolve a loaded schema table name to a reusable runtime handle.
    TableHandle registerTable(const std::string& tableName);
    // Decode one caller-owned struct row and append it to the table buffer.
    bool write(TableHandle table, std::int64_t timestampMs, const void* structPtr);
    // Flush all non-empty table buffers in schema order.
    bool flush();
    // Flush one table buffer while preserving rows if backend insertion fails.
    bool flush(TableHandle table);

    // Check whether a handle refers to an initialized table buffer.
    bool isValidTableHandle(TableHandle handle) const;

    // Return immutable schema metadata for a valid handle.
    const TableSchema* tableSchema(TableHandle handle) const;
    // Expose the immutable schema registry loaded during initialization.
    const SchemaRegistry& schemaRegistry() const;
    // Return the last structured logger error.
    const DataLoggerError& lastError() const;

private:
    struct TableBuffer
    {
        TableHandle handle;
        const TableSchema* schema = nullptr;
        std::vector<DecodedRow> rows;
    };

    // Clear the last public-operation error.
    void clearError();
    // Store a logger-owned error code and human-readable message.
    void setError(ErrorCode code, const std::string& message);
    // Store an already-built logger error and optionally print it.
    void setError(const DataLoggerError& error);
    // Convert backend diagnostics into the logger error surface.
    void setBackendError(ErrorCode code, const std::string& prefix);
    // Print one initialization summary when configured debug info is enabled.
    void printInitializationSuccess() const;
    // Clear schema-derived runtime state while preserving backend ownership.
    void resetRuntimeState();

    std::unique_ptr<IDBBackend> backend_;
    DataLoggerConfig config_;
    SchemaRegistry schemaRegistry_;
    std::unordered_map<std::string, std::size_t> tableNameToIndex_;
    std::vector<TableBuffer> tableBuffers_;
    DataLoggerError lastError_;
    bool initialized_ = false;
};
}
