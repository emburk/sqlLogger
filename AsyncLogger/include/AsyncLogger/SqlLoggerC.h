#pragma once

#include "DataLogger/DataLoggerC.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SqlLogger_c SqlLogger_c;
typedef struct SqlLoggerAsync_c SqlLoggerAsync_c;

#define SQLLOGGER_OK 0
#define SQLLOGGER_ERROR 1
#define SQLLOGGER_QUEUE_FULL 2
#define SQLLOGGER_INVALID_ARGUMENT 3
#define SQLLOGGER_NOT_INITIALIZED 4
#define SQLLOGGER_ALREADY_STARTED 5
#define SQLLOGGER_PAYLOAD_TOO_LARGE 6

typedef enum SqlLoggerAsyncOverflowPolicy_c
{
    SQLLOGGER_ASYNC_OVERFLOW_DROP_NEWEST = 0,
    SQLLOGGER_ASYNC_OVERFLOW_DROP_OLDEST = 1
} SqlLoggerAsyncOverflowPolicy_c;

typedef enum SqlLoggerAsyncWorkerPriority_c
{
    SQLLOGGER_ASYNC_WORKER_PRIORITY_NORMAL = 0,
    SQLLOGGER_ASYNC_WORKER_PRIORITY_BELOW_NORMAL = 1,
    SQLLOGGER_ASYNC_WORKER_PRIORITY_LOWEST = 2
} SqlLoggerAsyncWorkerPriority_c;

typedef struct AsyncDataLoggerConfig_c
{
    size_t queue_capacity;
    size_t max_payload_bytes;
    int overflow_policy;
    int worker_priority;
    int flush_on_stop;
    int auto_register_tables_on_start;
} AsyncDataLoggerConfig_c;

typedef struct AsyncDataLoggerStats_c
{
    uint64_t pushed_samples;
    uint64_t dropped_samples;
    uint64_t popped_samples;
    uint64_t auto_write_ops;
    uint64_t write_ops;
    uint64_t flush_requests;
    uint64_t worker_errors;
    uint64_t max_queue_depth;
    uint64_t current_queue_depth;
    uint64_t try_push_max_ns;
    uint64_t worker_loop_max_ns;
    uint64_t flush_max_ns;
} AsyncDataLoggerStats_c;

// Fill the async C configuration with the same defaults as the C++ async config.
void sql_logger_async_config_default_c(AsyncDataLoggerConfig_c* config);

// Create a combined SQL Server async logger and initialize its wrapped DataLogger.
int sql_logger_async_create_c(const DataLoggerConfig_c* data_config,
                              const AsyncDataLoggerConfig_c* async_config,
                              SqlLoggerAsync_c** out_logger);

// Destroy a combined async logger handle created by sql_logger_async_create_c().
int sql_logger_async_destroy_c(SqlLoggerAsync_c* logger);

// Start accepting async producer operations after initialization.
int sql_logger_async_start_c(SqlLoggerAsync_c* logger);

// Stop accepting async producer operations without forcing a final flush.
int sql_logger_async_stop_c(SqlLoggerAsync_c* logger);

// Stop accepting async producer operations and flush pending logger buffers.
int sql_logger_async_stop_and_flush_c(SqlLoggerAsync_c* logger);

// Register every loaded schema table for automatic async writes.
int sql_logger_async_auto_register_tables_c(SqlLoggerAsync_c* logger);

// Resolve one loaded table name to a reusable C table handle.
int sql_logger_async_register_table_c(SqlLoggerAsync_c* logger,
                                      const char* table_name,
                                      DataLoggerTableHandle_c* out_handle);

// Try to enqueue one automatic multi-table payload copy.
int sql_logger_async_try_auto_write_c(SqlLoggerAsync_c* logger,
                                      int64_t timestamp_ms,
                                      const void* struct_ptr,
                                      size_t struct_size);

// Try to enqueue one table-specific payload copy.
int sql_logger_async_try_write_c(SqlLoggerAsync_c* logger,
                                 DataLoggerTableHandle_c table,
                                 int64_t timestamp_ms,
                                 const void* struct_ptr,
                                 size_t struct_size);

// Try to enqueue an all-table flush request.
int sql_logger_async_request_flush_c(SqlLoggerAsync_c* logger);

// Copy async counters into the caller-owned stats struct.
int sql_logger_async_get_stats_c(SqlLoggerAsync_c* logger,
                                 AsyncDataLoggerStats_c* out_stats);

// Copy the last async error string and return the required byte count.
size_t sql_logger_async_get_error_string_c(const SqlLoggerAsync_c* logger,
                                           char* buffer,
                                           size_t buffer_size);

#ifdef __cplusplus
}
#endif
