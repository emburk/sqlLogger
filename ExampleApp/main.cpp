#include "DataLogger/DataLogger.h"
#include "SqlServerBackend/SqlServerOdbcBackend.h"

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

namespace
{
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

// Print the logger's structured error in a compact example-friendly form.
int failWithLastError(const DataLoggerCore::DataLogger& logger)
{
    const DataLoggerCore::DataLoggerError& error = logger.lastError();
    if (!error.message.empty())
    {
        std::cerr << error.message << '\n';
    }
    return 1;
}
}

// The example struct is packed so its offsets intentionally match imu_data.csv.
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

// Configure the real SQL Server backend, write two rows, and flush them.
int main()
{
    const std::string connectionString = readConnectionString();
    if (connectionString.empty())
    {
        std::cerr << "Set SQLLOGGER_CONNECTION_STRING before running the example.\n";
        return 1;
    }

    DataLoggerCore::DataLoggerConfig config;
    config.connectionString = connectionString;
    config.schemaDirectory = "ExampleApp\\schemas";
    config.batchSizeRows = 2;

    auto backend = std::make_unique<SqlServerBackend::SqlServerOdbcBackend>();
    DataLoggerCore::DataLogger logger(std::move(backend));
    if (!logger.initialize(config))
    {
        return failWithLastError(logger);
    }

    const DataLoggerCore::TableHandle imu = logger.registerTable("imu_data");
    if (!imu.isValid())
    {
        return failWithLastError(logger);
    }

    ImuData sample;
    sample.unusedOrSequence = 1;
    sample.gyro[0] = 1.0F;
    sample.gyro[1] = 2.0F;
    sample.gyro[2] = 3.0F;
    sample.accel[0] = 4.0F;
    sample.accel[1] = 5.0F;
    sample.accel[2] = 6.0F;
    sample.temperature = 7.5;
    sample.status = 9;

    if (!logger.write(imu, 123456789, &sample) ||
        !logger.write(imu, 123456790, &sample) ||
        !logger.flush())
    {
        return failWithLastError(logger);
    }

    logger.shutdown();
    std::cout << "Inserted example imu_data rows.\n";
    return 0;
}
