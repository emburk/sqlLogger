#include "DataLogger/DataLogger.h"
#include "SqlServerBackend/SqlServerOdbcBackend.h"

#include <conio.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <io.h>
#include <memory>
#include <string>

// Include the large local payload only for explicit local stress runs.
#define LOCAL_TEST
#ifdef LOCAL_TEST
#include "local/payload_types.h"
#include "local/payloadInitialize.c"
#endif

namespace
{
#ifdef LOCAL_TEST
constexpr std::size_t kBatchSizeRows = 400;
constexpr int kWriteIterations = 10000;
constexpr std::int64_t kBaseTimestampMs = 1779062400000;
#else
constexpr std::size_t kBatchSizeRows = 2;
constexpr std::int64_t kFirstTimestampMs = 1779062400000;
constexpr std::int64_t kSecondTimestampMs = 1779062400001;

// Match the simple example struct layout to ExampleApp/schemas/imu_data.csv.
#pragma pack(push, 1)
struct ImuData
{
    std::int64_t unusedOrSequence = 0;
    float gyro[3] = {};
    float accel[3] = {};
    double temperature = 0.0;
    std::uint16_t status = 0;
};
#pragma pack(pop)
#endif

// Read the SQL Server ODBC connection string from the environment.
std::string readConnectionString()
{
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, "SQLLOGGER_CONNECTION_STRING") != 0 || value == nullptr)
    {
        return {};
    }

    std::string connectionString(value);
    std::free(value);
    return connectionString;
}

// Pause failed interactive runs so Visual Studio-launched consoles stay readable.
void waitForKeyPress()
{
    if (_isatty(_fileno(stdin)) == 0)
    {
        return;
    }

    std::cerr << "Press any key to exit..." << '\n';
    (void)_getch();
}

// Return the standard example failure code after the optional diagnostic pause.
int failAfterKeyPress()
{
    waitForKeyPress();
    return 1;
}

#ifdef LOCAL_TEST
// Write the same AO payload to every split table for a deterministic load test.
bool writeAoRows(DataLoggerCore::DataLogger& logger,
                 const payloadStruct_T& pl)
{
    for (int iteration = 0; iteration < kWriteIterations; ++iteration)
    {
        const std::int64_t timestampMs = kBaseTimestampMs + iteration;
        if (!logger.autoWrite(timestampMs, &pl))
        {
            return false;
        }
    }

    return true;
}
#else
// Build one deterministic simple sensor payload for the default example run.
ImuData makeSimpleSensorData()
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
bool writeSimpleSensorRows(DataLoggerCore::DataLogger& logger,
                           DataLoggerCore::TableHandle table,
                           const ImuData& sample)
{
    return logger.write(table, kFirstTimestampMs, &sample) &&
           logger.write(table, kSecondTimestampMs, &sample);
}
#endif
}

// Configure the real SQL Server backend and run the selected example payload.
int main()
{
#ifdef LOCAL_TEST
    payloadStruct_T ao;
    payload_initalizeStruct(&ao);
#endif

    const std::string connectionString = readConnectionString();
    if (connectionString.empty())
    {
        std::cerr << "Set SQLLOGGER_CONNECTION_STRING before running the example.\n";
        return failAfterKeyPress();
    }

    DataLoggerCore::DataLoggerConfig config;
    config.connectionString = connectionString;
#ifdef LOCAL_TEST
    config.schemaDirectory = "ExampleApp\\local\\schemas";
#else
    config.schemaDirectory = "ExampleApp\\schemas";
#endif
    config.batchSizeRows = kBatchSizeRows;
    config.existingTablePolicy = DataLoggerCore::ExistingTablePolicy::Drop;

    auto backend = std::make_unique<SqlServerBackend::SqlServerOdbcBackend>();
    DataLoggerCore::DataLogger logger(std::move(backend));
    if (!logger.initialize(config))
    {
        return failAfterKeyPress();
    }

#ifdef LOCAL_TEST
    if (!logger.autoRegisterTables())
    {
        return failAfterKeyPress();
    }

    if (!writeAoRows(logger, ao) || !logger.flush())
    {
        return failAfterKeyPress();
    }

    logger.shutdown();
    std::cout << "Inserted PL test rows for " << kWriteIterations << " iterations.\n";
#else
    const DataLoggerCore::TableHandle imu = logger.registerTable("imu_data");
    if (!imu.isValid())
    {
        return failAfterKeyPress();
    }

    const ImuData sample = makeSimpleSensorData();
    if (!writeSimpleSensorRows(logger, imu, sample) || !logger.flush())
    {
        return failAfterKeyPress();
    }

    logger.shutdown();
    std::cout << "Inserted simple sensor rows into imu_data.\n";
    waitForKeyPress();
#endif
    return 0;
}
