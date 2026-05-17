#include "DataLogger/DataLogger.h"

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
    return imu.isValid() ? 0 : 1;
}
