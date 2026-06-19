#include "DataLogger/DataLoggerC.h"
#include "SqlServerBackend/SqlServerOdbcBackendC.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum
{
    kBatchSizeRows = 2
};

static const int64_t kFirstTimestampMs = 1779062400000LL;
static const int64_t kSecondTimestampMs = 1779062400001LL;

#pragma pack(push, 1)
typedef struct ImuData
{
    int64_t unusedOrSequence;
    float gyro[3];
    float accel[3];
    double temperature;
    uint16_t status;
} ImuData;
#pragma pack(pop)

// Read the SQL Server ODBC connection string from the process environment.
static char* readConnectionString(void)
{
    char* value = NULL;
    size_t length = 0;
    if (_dupenv_s(&value, &length, "SQLLOGGER_CONNECTION_STRING") != 0 || value == NULL)
    {
        return NULL;
    }

    return value;
}

// Print the current logger error through the caller-owned C buffer API.
static void printLoggerError(const DataLogger_c* logger, const char* prefix)
{
    const size_t requiredSize = datalogger_get_error_string_c(logger, NULL, 0);
    char* message = NULL;

    if (requiredSize > 0)
    {
        message = (char*)malloc(requiredSize);
    }

    if (message != NULL)
    {
        datalogger_get_error_string_c(logger, message, requiredSize);
        printf("%s: %s\n", prefix, message);
        free(message);
        return;
    }

    printf("%s\n", prefix);
}

// Build one deterministic simple sensor payload for the C library example.
static ImuData makeSimpleSensorData(void)
{
    ImuData sample;
    sample.unusedOrSequence = 42;
    sample.gyro[0] = 1.0F;
    sample.gyro[1] = 2.0F;
    sample.gyro[2] = 3.0F;
    sample.accel[0] = 4.0F;
    sample.accel[1] = 5.0F;
    sample.accel[2] = 6.0F;
    sample.temperature = 7.5;
    sample.status = 9;
    return sample;
}

// Write two simple sensor rows so the configured batch size triggers a flush.
static int writeSimpleSensorRows(DataLogger_c* logger,
                                 DataLoggerTableHandle_c table,
                                 const ImuData* sample)
{
    return datalogger_write_c(logger, table, kFirstTimestampMs, sample) &&
           datalogger_write_c(logger, table, kSecondTimestampMs, sample);
}

// Configure the real SQL Server backend and run the C facade example.
int main(void)
{
    int result = 1;
    char* connectionString = readConnectionString();
    DataLoggerBackend_c* backend = NULL;
    DataLogger_c* logger = NULL;
    DataLoggerConfig_c config;
    DataLoggerTableHandle_c imu;
    ImuData sample;

    if (connectionString == NULL)
    {
        printf("Set SQLLOGGER_CONNECTION_STRING before running the C example.\n");
        return 1;
    }

    backend = sqlserver_backend_create_c();
    if (backend == NULL)
    {
        printf("Unable to create SQL Server backend.\n");
        free(connectionString);
        return 1;
    }

    logger = datalogger_create_c(backend);
    backend = NULL;
    if (logger == NULL)
    {
        printf("Unable to create DataLogger.\n");
        free(connectionString);
        return 1;
    }

    datalogger_config_default_c(&config);
    config.connectionString = connectionString;
    config.schemaDirectory = "examples\\ExampleApp\\schemas";
    config.batchSizeRows = kBatchSizeRows;
    config.existingTablePolicy = DATALOGGER_EXISTING_TABLE_POLICY_RENAME_WITH_TIMESTAMP_SUFFIX;

    if (!datalogger_init_c(logger, &config))
    {
        printLoggerError(logger, "init error");
        goto cleanup;
    }

    imu = datalogger_register_table_c(logger, "imu_data");
    if (!datalogger_table_handle_is_valid_c(imu))
    {
        printLoggerError(logger, "register table error");
        goto cleanup;
    }

    sample = makeSimpleSensorData();
    if (!writeSimpleSensorRows(logger, imu, &sample) || !datalogger_flush_c(logger))
    {
        printLoggerError(logger, "write or flush error");
        goto cleanup;
    }

    datalogger_shutdown_c(logger);
    printf("Inserted simple sensor rows into imu_data through the C facade.\n");
    result = 0;

cleanup:
    datalogger_destroy_c(logger);
    sqlserver_backend_destroy_c(backend);
    free(connectionString);
    return result;
}
