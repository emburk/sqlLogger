#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DataLogger_c DataLogger_c;
typedef struct DataLoggerBackend_c DataLoggerBackend_c;

typedef enum DataLoggerExistingTablePolicy_c
{
    DATALOGGER_EXISTING_TABLE_POLICY_DROP = 0,
    DATALOGGER_EXISTING_TABLE_POLICY_RENAME_WITH_TIMESTAMP_SUFFIX = 1,
    DATALOGGER_EXISTING_TABLE_POLICY_CONTINUE_CURRENT_TABLE = 2
} DataLoggerExistingTablePolicy_c;

typedef struct DataLoggerConfig_c
{
    const char* connectionString;
    const char* schemaDirectory;
    const char* sqlSchemaName;
    size_t batchSizeRows;
    DataLoggerExistingTablePolicy_c existingTablePolicy;
    int printInfoFlag;
    int printErrorFlag;
} DataLoggerConfig_c;

typedef struct DataLoggerTableHandle_c
{
    size_t index;
} DataLoggerTableHandle_c;

// Fill a C configuration struct with the same defaults as the C++ configuration.
void datalogger_config_default_c(DataLoggerConfig_c* config);

// Return the invalid table-handle sentinel used after failed registration.
DataLoggerTableHandle_c datalogger_table_handle_invalid_c(void);

// Report whether a C table handle is not the invalid sentinel value.
int datalogger_table_handle_is_valid_c(DataLoggerTableHandle_c table);

// Create a logger and take ownership of the supplied backend handle.
DataLogger_c* datalogger_create_c(DataLoggerBackend_c* backend);

// Shutdown and destroy a logger created by datalogger_create_c().
void datalogger_destroy_c(DataLogger_c* logger);

// Initialize the logger from a C-compatible configuration object.
int datalogger_init_c(DataLogger_c* logger, const DataLoggerConfig_c* config);

// Register every loaded schema table for automatic multi-table writes.
int datalogger_auto_register_tables_c(DataLogger_c* logger);

// Resolve one loaded table name to a reusable table handle.
DataLoggerTableHandle_c datalogger_register_table_c(DataLogger_c* logger, const char* tableName);

// Write one caller-owned struct row to every auto-registered table.
int datalogger_auto_write_c(DataLogger_c* logger, int64_t timestampMs, const void* structPtr);

// Write one caller-owned struct row through a registered table handle.
int datalogger_write_c(DataLogger_c* logger, DataLoggerTableHandle_c table, int64_t timestampMs, const void* structPtr);

// Flush all non-empty table buffers in schema order.
int datalogger_flush_c(DataLogger_c* logger);

// Flush one table buffer while preserving rows if backend insertion fails.
int datalogger_flush_table_c(DataLogger_c* logger, DataLoggerTableHandle_c table);

// Flush remaining rows and clear runtime state when the flush succeeds.
void datalogger_shutdown_c(DataLogger_c* logger);

// Copy the last error string into the caller buffer and return the required byte count.
size_t datalogger_get_error_string_c(const DataLogger_c* logger, char* buffer, size_t bufferSize);

#ifdef __cplusplus
}
#endif
