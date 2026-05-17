#include "DataLogger/BinaryDecoder.h"
#include "DataLogger/DataLogger.h"

#include <cstdint>
#include <variant>

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

int main()
{
    DataLoggerCore::DataLoggerConfig config;
    config.schemaDirectory = "ExampleApp\\schemas";

    DataLoggerCore::DataLogger logger;
    if (!logger.initialize(config))
    {
        return 1;
    }

    const DataLoggerCore::TableHandle imu = logger.registerTable("imu_data");
    if (!imu.isValid())
    {
        return 1;
    }

    const DataLoggerCore::TableSchema* schema = logger.tableSchema(imu);
    if (schema == nullptr)
    {
        return 1;
    }

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

    DataLoggerCore::DecodedRow row;
    DataLoggerCore::DataLoggerError error;
    if (!DataLoggerCore::decodeRow(*schema, 123456789, &sample, row, error))
    {
        return 1;
    }

    if (row.timestampMs != 123456789 || row.values.size() != 8)
    {
        return 1;
    }

    if (std::get<float>(row.values[0]) != 1.0F ||
        std::get<float>(row.values[1]) != 2.0F ||
        std::get<float>(row.values[2]) != 3.0F ||
        std::get<float>(row.values[3]) != 4.0F ||
        std::get<float>(row.values[4]) != 5.0F ||
        std::get<float>(row.values[5]) != 6.0F ||
        std::get<double>(row.values[6]) != 7.5 ||
        std::get<std::uint16_t>(row.values[7]) != 9)
    {
        return 1;
    }

    return 0;
}
