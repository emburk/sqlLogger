#include "AsyncLogger/SqlLoggerC.h"

#include "AsyncLogger/AsyncDataLogger.h"
#include "SqlServerBackend/SqlServerOdbcBackend.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <string>

struct SqlLoggerAsync_c
{
    std::unique_ptr<AsyncLogger::AsyncDataLogger> logger;
    AsyncLogger::AsyncDataLoggerConfig config;
    std::string wrapperError;
};

namespace
{
// Translate the C table policy enum into the C++ core configuration enum.
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
    }

    return false;
}

// Translate the existing C facade configuration into the C++ DataLogger config.
bool convertDataConfig(const DataLoggerConfig_c* input,
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

// Translate the async C configuration into the C++ async configuration type.
bool convertAsyncConfig(const AsyncDataLoggerConfig_c* input,
                        AsyncLogger::AsyncDataLoggerConfig& output,
                        std::string& error)
{
    if (input == nullptr)
    {
        error = "AsyncDataLoggerConfig_c pointer must not be null.";
        return false;
    }

    output.queueCapacity = input->queue_capacity;
    output.maxPayloadBytes = input->max_payload_bytes;
    output.flushOnStop = input->flush_on_stop != 0;
    output.autoRegisterTablesOnStart = input->auto_register_tables_on_start != 0;

    switch (input->overflow_policy)
    {
    case SQLLOGGER_ASYNC_OVERFLOW_DROP_NEWEST:
        output.overflowPolicy = AsyncLogger::AsyncOverflowPolicy::DropNewest;
        break;
    case SQLLOGGER_ASYNC_OVERFLOW_DROP_OLDEST:
        output.overflowPolicy = AsyncLogger::AsyncOverflowPolicy::DropOldest;
        break;
    default:
        error = "AsyncDataLoggerConfig_c contains an unknown overflow policy.";
        return false;
    }

    switch (input->worker_priority)
    {
    case SQLLOGGER_ASYNC_WORKER_PRIORITY_NORMAL:
        output.workerPriority = AsyncLogger::AsyncWorkerPriority::Normal;
        break;
    case SQLLOGGER_ASYNC_WORKER_PRIORITY_BELOW_NORMAL:
        output.workerPriority = AsyncLogger::AsyncWorkerPriority::BelowNormal;
        break;
    case SQLLOGGER_ASYNC_WORKER_PRIORITY_LOWEST:
        output.workerPriority = AsyncLogger::AsyncWorkerPriority::Lowest;
        break;
    default:
        error = "AsyncDataLoggerConfig_c contains an unknown worker priority.";
        return false;
    }

    return true;
}

// Translate a C table handle value into the C++ handle wrapper.
DataLoggerCore::TableHandle toCppHandle(DataLoggerTableHandle_c table)
{
    if (!datalogger_table_handle_is_valid_c(table))
    {
        return DataLoggerCore::TableHandle::invalid();
    }

    return DataLoggerCore::TableHandle(table.index);
}

// Translate a C++ table handle wrapper into the C value type.
DataLoggerTableHandle_c toCHandle(DataLoggerCore::TableHandle table)
{
    DataLoggerTableHandle_c result = datalogger_table_handle_invalid_c();
    if (table.isValid())
    {
        result.index = table.index();
    }

    return result;
}

// Copy C++ async stats into the C ABI stats shape.
void copyStats(const AsyncLogger::AsyncDataLoggerStats& input,
               AsyncDataLoggerStats_c* output)
{
    output->pushed_samples = input.pushedSamples;
    output->dropped_samples = input.droppedSamples;
    output->popped_samples = input.poppedSamples;
    output->auto_write_ops = input.autoWriteOps;
    output->write_ops = input.writeOps;
    output->flush_requests = input.flushRequests;
    output->worker_errors = input.workerErrors;
    output->max_queue_depth = input.maxQueueDepth;
    output->current_queue_depth = input.currentQueueDepth;
    output->try_push_max_ns = input.tryPushMaxNs;
    output->worker_loop_max_ns = input.workerLoopMaxNs;
    output->flush_max_ns = input.flushMaxNs;
}

// Map C++ async statuses onto the public C status-code constants.
int mapStatus(AsyncLogger::AsyncDataLoggerStatus status)
{
    switch (status)
    {
    case AsyncLogger::AsyncDataLoggerStatus::Ok:
        return SQLLOGGER_OK;
    case AsyncLogger::AsyncDataLoggerStatus::QueueFull:
        return SQLLOGGER_QUEUE_FULL;
    case AsyncLogger::AsyncDataLoggerStatus::InvalidArgument:
        return SQLLOGGER_INVALID_ARGUMENT;
    case AsyncLogger::AsyncDataLoggerStatus::NotInitialized:
        return SQLLOGGER_NOT_INITIALIZED;
    case AsyncLogger::AsyncDataLoggerStatus::AlreadyStarted:
        return SQLLOGGER_ALREADY_STARTED;
    case AsyncLogger::AsyncDataLoggerStatus::PayloadTooLarge:
        return SQLLOGGER_PAYLOAD_TOO_LARGE;
    case AsyncLogger::AsyncDataLoggerStatus::Error:
        return SQLLOGGER_ERROR;
    }

    return SQLLOGGER_ERROR;
}

// Return the wrapper-level error before falling back to the C++ async logger error.
std::string currentError(const SqlLoggerAsync_c* logger)
{
    if (logger == nullptr)
    {
        return {};
    }

    if (!logger->wrapperError.empty())
    {
        return logger->wrapperError;
    }

    if (logger->logger != nullptr)
    {
        return logger->logger->lastError().message;
    }

    return {};
}
}

// Fill the async C configuration with the same defaults as the C++ async config.
void sql_logger_async_config_default_c(AsyncDataLoggerConfig_c* config)
{
    if (config == nullptr)
    {
        return;
    }

    config->queue_capacity = 512;
    config->max_payload_bytes = 0;
    config->overflow_policy = SQLLOGGER_ASYNC_OVERFLOW_DROP_NEWEST;
    config->worker_priority = SQLLOGGER_ASYNC_WORKER_PRIORITY_BELOW_NORMAL;
    config->flush_on_stop = 1;
    config->auto_register_tables_on_start = 0;
}

// Create the combined C facade and initialize the wrapped async logger.
int sql_logger_async_create_c(const DataLoggerConfig_c* data_config,
                              const AsyncDataLoggerConfig_c* async_config,
                              SqlLoggerAsync_c** out_logger)
{
    if (out_logger == nullptr)
    {
        return SQLLOGGER_INVALID_ARGUMENT;
    }

    *out_logger = nullptr;

    DataLoggerCore::DataLoggerConfig cppDataConfig;
    AsyncLogger::AsyncDataLoggerConfig cppAsyncConfig;
    std::string error;
    if (!convertDataConfig(data_config, cppDataConfig, error) ||
        !convertAsyncConfig(async_config, cppAsyncConfig, error))
    {
        return SQLLOGGER_INVALID_ARGUMENT;
    }

    try
    {
        std::unique_ptr<SqlLoggerAsync_c> wrapper(new SqlLoggerAsync_c());
        auto backend = std::make_unique<SqlServerBackend::SqlServerOdbcBackend>();
        wrapper->logger = std::make_unique<AsyncLogger::AsyncDataLogger>(std::move(backend));
        wrapper->config = cppAsyncConfig;

        if (!wrapper->logger->initialize(cppDataConfig, cppAsyncConfig))
        {
            wrapper->wrapperError = wrapper->logger->lastError().message;
            const int status = mapStatus(wrapper->logger->lastStatus());
            *out_logger = wrapper.release();
            return status == SQLLOGGER_OK ? SQLLOGGER_ERROR : status;
        }

        if (cppAsyncConfig.autoRegisterTablesOnStart && !wrapper->logger->autoRegisterTables())
        {
            wrapper->wrapperError = wrapper->logger->lastError().message;
            const int status = mapStatus(wrapper->logger->lastStatus());
            *out_logger = wrapper.release();
            return status == SQLLOGGER_OK ? SQLLOGGER_ERROR : status;
        }

        *out_logger = wrapper.release();
        return SQLLOGGER_OK;
    }
    catch (...)
    {
        return SQLLOGGER_ERROR;
    }
}

// Destroy the combined async C facade wrapper.
int sql_logger_async_destroy_c(SqlLoggerAsync_c* logger)
{
    delete logger;
    return SQLLOGGER_OK;
}

// Start the async logger through the C facade.
int sql_logger_async_start_c(SqlLoggerAsync_c* logger)
{
    if (logger == nullptr || logger->logger == nullptr)
    {
        return SQLLOGGER_INVALID_ARGUMENT;
    }

    logger->wrapperError.clear();
    return logger->logger->start() ? SQLLOGGER_OK : mapStatus(logger->logger->lastStatus());
}

// Stop the async logger through the C facade.
int sql_logger_async_stop_c(SqlLoggerAsync_c* logger)
{
    if (logger == nullptr || logger->logger == nullptr)
    {
        return SQLLOGGER_INVALID_ARGUMENT;
    }

    logger->wrapperError.clear();
    logger->logger->stop();
    return SQLLOGGER_OK;
}

// Stop the async logger and flush the wrapped DataLogger.
int sql_logger_async_stop_and_flush_c(SqlLoggerAsync_c* logger)
{
    if (logger == nullptr || logger->logger == nullptr)
    {
        return SQLLOGGER_INVALID_ARGUMENT;
    }

    logger->wrapperError.clear();
    return logger->logger->stopAndFlush() ? SQLLOGGER_OK : mapStatus(logger->logger->lastStatus());
}

// Register all schema tables for automatic async writes.
int sql_logger_async_auto_register_tables_c(SqlLoggerAsync_c* logger)
{
    if (logger == nullptr || logger->logger == nullptr)
    {
        return SQLLOGGER_INVALID_ARGUMENT;
    }

    logger->wrapperError.clear();
    return logger->logger->autoRegisterTables() ? SQLLOGGER_OK : mapStatus(logger->logger->lastStatus());
}

// Register one table and return the C table handle through the output pointer.
int sql_logger_async_register_table_c(SqlLoggerAsync_c* logger,
                                      const char* table_name,
                                      DataLoggerTableHandle_c* out_handle)
{
    if (logger == nullptr || logger->logger == nullptr || table_name == nullptr || out_handle == nullptr)
    {
        return SQLLOGGER_INVALID_ARGUMENT;
    }

    logger->wrapperError.clear();
    DataLoggerCore::TableHandle table;
    if (!logger->logger->registerTable(table_name, &table))
    {
        *out_handle = datalogger_table_handle_invalid_c();
        return mapStatus(logger->logger->lastStatus());
    }

    *out_handle = toCHandle(table);
    return SQLLOGGER_OK;
}

// Try to enqueue one automatic multi-table payload copy.
int sql_logger_async_try_auto_write_c(SqlLoggerAsync_c* logger,
                                      int64_t timestamp_ms,
                                      const void* struct_ptr,
                                      size_t struct_size)
{
    if (logger == nullptr || logger->logger == nullptr || struct_ptr == nullptr)
    {
        return SQLLOGGER_INVALID_ARGUMENT;
    }

    if (struct_size > logger->config.maxPayloadBytes)
    {
        return SQLLOGGER_PAYLOAD_TOO_LARGE;
    }

    return logger->logger->tryAutoWrite(timestamp_ms, struct_ptr, struct_size)
               ? SQLLOGGER_OK
               : mapStatus(logger->logger->lastStatus());
}

// Try to enqueue one table-specific payload copy.
int sql_logger_async_try_write_c(SqlLoggerAsync_c* logger,
                                 DataLoggerTableHandle_c table,
                                 int64_t timestamp_ms,
                                 const void* struct_ptr,
                                 size_t struct_size)
{
    if (logger == nullptr || logger->logger == nullptr || struct_ptr == nullptr)
    {
        return SQLLOGGER_INVALID_ARGUMENT;
    }

    if (struct_size > logger->config.maxPayloadBytes)
    {
        return SQLLOGGER_PAYLOAD_TOO_LARGE;
    }

    return logger->logger->tryWrite(toCppHandle(table), timestamp_ms, struct_ptr, struct_size)
               ? SQLLOGGER_OK
               : mapStatus(logger->logger->lastStatus());
}

// Try to enqueue an all-table flush request.
int sql_logger_async_request_flush_c(SqlLoggerAsync_c* logger)
{
    if (logger == nullptr || logger->logger == nullptr)
    {
        return SQLLOGGER_INVALID_ARGUMENT;
    }

    return logger->logger->requestFlush() ? SQLLOGGER_OK : mapStatus(logger->logger->lastStatus());
}

// Copy async stats into a caller-owned C struct.
int sql_logger_async_get_stats_c(SqlLoggerAsync_c* logger,
                                 AsyncDataLoggerStats_c* out_stats)
{
    if (logger == nullptr || logger->logger == nullptr || out_stats == nullptr)
    {
        return SQLLOGGER_INVALID_ARGUMENT;
    }

    copyStats(logger->logger->stats(), out_stats);
    return SQLLOGGER_OK;
}

// Copy the last async error string into a caller buffer.
size_t sql_logger_async_get_error_string_c(const SqlLoggerAsync_c* logger,
                                           char* buffer,
                                           size_t buffer_size)
{
    const std::string message = currentError(logger);
    const size_t required_size = message.size() + 1;

    if (buffer != nullptr && buffer_size > 0)
    {
        const size_t copy_size = std::min(message.size(), buffer_size - 1);
        std::memcpy(buffer, message.data(), copy_size);
        buffer[copy_size] = '\0';
    }

    return required_size;
}
