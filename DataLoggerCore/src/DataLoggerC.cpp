#include "DataLogger/DataLoggerC.h"

#include "DataLogger/DataLogger.h"
#include "DataLogger/detail/DataLoggerCBridge.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <utility>

struct DataLogger_c
{
    std::unique_ptr<DataLoggerCore::DataLogger> logger;
    std::string wrapperError;
};

namespace
{
constexpr std::size_t kInvalidTableIndex = (std::numeric_limits<std::size_t>::max)();

// Store a wrapper-level error for failures before the C++ logger can record one.
void setWrapperError(DataLogger_c* logger, const std::string& message)
{
    if (logger != nullptr)
    {
        logger->wrapperError = message;
    }
}

// Clear wrapper-level errors before forwarding a call to the C++ logger.
void clearWrapperError(DataLogger_c* logger)
{
    if (logger != nullptr)
    {
        logger->wrapperError.clear();
    }
}

// Translate a C table policy enum into the C++ configuration enum.
bool convertPolicy(DataLoggerExistingTablePolicy_c input,
                   DataLoggerCore::ExistingTablePolicy& output)
{
    switch (input)
    {
    case DATALOGGER_EXISTING_TABLE_POLICY_DROP:
        output = DataLoggerCore::ExistingTablePolicy::Drop;
        return true;
    case DATALOGGER_EXISTING_TABLE_POLICY_RENAME_WITH_TIMESTAMP_SUFFIX:
        output = DataLoggerCore::ExistingTablePolicy::RenameWithTimestampSuffix;
        return true;
    case DATALOGGER_EXISTING_TABLE_POLICY_CONTINUE_CURRENT_TABLE:
        output = DataLoggerCore::ExistingTablePolicy::ContinueCurrentTable;
        return true;
    }

    return false;
}

// Translate the C configuration struct into the existing C++ configuration type.
bool convertConfig(const DataLoggerConfig_c* input,
                   DataLoggerCore::DataLoggerConfig& output,
                   std::string& error)
{
    if (input == nullptr)
    {
        error = "DataLoggerConfig_c pointer must not be null.";
        return false;
    }

    if (!convertPolicy(input->existingTablePolicy, output.existingTablePolicy))
    {
        error = "DataLoggerConfig_c contains an unknown existing-table policy.";
        return false;
    }

    output.connectionString = input->connectionString != nullptr ? input->connectionString : "";
    output.schemaDirectory = input->schemaDirectory != nullptr ? input->schemaDirectory : "";
    output.sqlSchemaName = input->sqlSchemaName != nullptr ? input->sqlSchemaName : "dbo";
    output.batchSizeRows = input->batchSizeRows;
    output.printInfoFlag = input->printInfoFlag != 0;
    output.printErrorFlag = input->printErrorFlag != 0;
    return true;
}

// Translate a C table handle value into the C++ handle wrapper.
DataLoggerCore::TableHandle toCppHandle(DataLoggerTableHandle_c table)
{
    if (table.index == kInvalidTableIndex)
    {
        return DataLoggerCore::TableHandle::invalid();
    }

    return DataLoggerCore::TableHandle(table.index);
}

// Translate a C++ table handle wrapper into the C value type.
DataLoggerTableHandle_c toCHandle(DataLoggerCore::TableHandle table)
{
    DataLoggerTableHandle_c result;
    result.index = table.isValid() ? table.index() : kInvalidTableIndex;
    return result;
}

// Return the most relevant error string for a logger wrapper.
const std::string& currentError(const DataLogger_c* logger)
{
    if (logger->wrapperError.empty() && logger->logger != nullptr)
    {
        return logger->logger->lastError().message;
    }

    return logger->wrapperError;
}
}

// Fill a C configuration struct with the same defaults as the C++ configuration.
void datalogger_config_default_c(DataLoggerConfig_c* config)
{
    if (config == nullptr)
    {
        return;
    }

    config->connectionString = nullptr;
    config->schemaDirectory = nullptr;
    config->sqlSchemaName = "dbo";
    config->batchSizeRows = 100;
    config->existingTablePolicy = DATALOGGER_EXISTING_TABLE_POLICY_RENAME_WITH_TIMESTAMP_SUFFIX;
    config->printInfoFlag = 1;
    config->printErrorFlag = 1;
}

// Return the invalid table-handle sentinel used after failed registration.
DataLoggerTableHandle_c datalogger_table_handle_invalid_c(void)
{
    DataLoggerTableHandle_c table;
    table.index = kInvalidTableIndex;
    return table;
}

// Report whether a C table handle is not the invalid sentinel value.
int datalogger_table_handle_is_valid_c(DataLoggerTableHandle_c table)
{
    return table.index != kInvalidTableIndex ? 1 : 0;
}

// Create a logger and take ownership of the supplied backend handle.
DataLogger_c* datalogger_create_c(DataLoggerBackend_c* backend)
{
    std::unique_ptr<DataLoggerBackend_c> backendOwner(backend);
    if (backendOwner == nullptr || backendOwner->backend == nullptr)
    {
        return nullptr;
    }

    try
    {
        std::unique_ptr<DataLogger_c> logger(new DataLogger_c());
        logger->logger = std::make_unique<DataLoggerCore::DataLogger>(std::move(backendOwner->backend));
        return logger.release();
    }
    catch (...)
    {
        return nullptr;
    }
}

// Shutdown and destroy a logger created by datalogger_create_c().
void datalogger_destroy_c(DataLogger_c* logger)
{
    delete logger;
}

// Initialize the logger from a C-compatible configuration object.
int datalogger_init_c(DataLogger_c* logger, const DataLoggerConfig_c* config)
{
    if (logger == nullptr || logger->logger == nullptr)
    {
        return 0;
    }

    clearWrapperError(logger);

    DataLoggerCore::DataLoggerConfig cppConfig;
    std::string error;
    if (!convertConfig(config, cppConfig, error))
    {
        setWrapperError(logger, error);
        return 0;
    }

    return logger->logger->initialize(cppConfig) ? 1 : 0;
}

// Register every loaded schema table for automatic multi-table writes.
int datalogger_auto_register_tables_c(DataLogger_c* logger)
{
    if (logger == nullptr || logger->logger == nullptr)
    {
        return 0;
    }

    clearWrapperError(logger);
    return logger->logger->autoRegisterTables() ? 1 : 0;
}

// Resolve one loaded table name to a reusable table handle.
DataLoggerTableHandle_c datalogger_register_table_c(DataLogger_c* logger, const char* tableName)
{
    if (logger == nullptr || logger->logger == nullptr)
    {
        return datalogger_table_handle_invalid_c();
    }

    clearWrapperError(logger);
    if (tableName == nullptr)
    {
        setWrapperError(logger, "Table name pointer must not be null.");
        return datalogger_table_handle_invalid_c();
    }

    return toCHandle(logger->logger->registerTable(tableName));
}

// Write one caller-owned struct row to every auto-registered table.
int datalogger_auto_write_c(DataLogger_c* logger, int64_t timestampMs, const void* structPtr)
{
    if (logger == nullptr || logger->logger == nullptr)
    {
        return 0;
    }

    clearWrapperError(logger);
    return logger->logger->autoWrite(timestampMs, structPtr) ? 1 : 0;
}

// Write one caller-owned struct row through a registered table handle.
int datalogger_write_c(DataLogger_c* logger, DataLoggerTableHandle_c table, int64_t timestampMs, const void* structPtr)
{
    if (logger == nullptr || logger->logger == nullptr)
    {
        return 0;
    }

    clearWrapperError(logger);
    return logger->logger->write(toCppHandle(table), timestampMs, structPtr) ? 1 : 0;
}

// Flush all non-empty table buffers in schema order.
int datalogger_flush_c(DataLogger_c* logger)
{
    if (logger == nullptr || logger->logger == nullptr)
    {
        return 0;
    }

    clearWrapperError(logger);
    return logger->logger->flush() ? 1 : 0;
}

// Flush one table buffer while preserving rows if backend insertion fails.
int datalogger_flush_table_c(DataLogger_c* logger, DataLoggerTableHandle_c table)
{
    if (logger == nullptr || logger->logger == nullptr)
    {
        return 0;
    }

    clearWrapperError(logger);
    return logger->logger->flush(toCppHandle(table)) ? 1 : 0;
}

// Flush remaining rows and clear runtime state when the flush succeeds.
void datalogger_shutdown_c(DataLogger_c* logger)
{
    if (logger == nullptr || logger->logger == nullptr)
    {
        return;
    }

    clearWrapperError(logger);
    logger->logger->shutdown();
}

// Copy the last error string into the caller buffer and return the required byte count.
size_t datalogger_get_error_string_c(const DataLogger_c* logger, char* buffer, size_t bufferSize)
{
    if (logger == nullptr)
    {
        if (buffer != nullptr && bufferSize > 0)
        {
            buffer[0] = '\0';
        }
        return 0;
    }

    const std::string& message = currentError(logger);
    const std::size_t requiredSize = message.size() + 1;

    if (buffer != nullptr && bufferSize > 0)
    {
        const std::size_t copySize = std::min(message.size(), bufferSize - 1);
        std::memcpy(buffer, message.data(), copySize);
        buffer[copySize] = '\0';
    }

    return requiredSize;
}
