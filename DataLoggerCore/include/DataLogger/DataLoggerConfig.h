#pragma once

#include <cstddef>
#include <string>

namespace DataLoggerCore
{
enum class ExistingTablePolicy
{
    Drop,
    RenameWithTimestampSuffix
};

struct DataLoggerConfig
{
    std::string connectionString;
    std::string schemaDirectory;
    std::string sqlSchemaName = "dbo";

    std::size_t batchSizeRows = 100;

    ExistingTablePolicy existingTablePolicy = ExistingTablePolicy::RenameWithTimestampSuffix;
};
}
