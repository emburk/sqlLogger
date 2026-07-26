#pragma once

#include <cstddef>
#include <string>

namespace DataLoggerCore
{
enum class ExistingTablePolicy
{
    Drop,
    RenameWithTimestampSuffix,
    ContinueCurrentTable
};

enum class SqlServerIndexMode
{
    RowstoreTimestampOnly,
    RowstoreWithNonclusteredColumnstore
};

struct DataLoggerConfig
{
    std::string connectionString;
    std::string schemaDirectory;
    std::string sqlSchemaName = "dbo";

    std::size_t batchSizeRows = 100;

    ExistingTablePolicy existingTablePolicy = ExistingTablePolicy::RenameWithTimestampSuffix;
    SqlServerIndexMode sqlServerIndexMode = SqlServerIndexMode::RowstoreTimestampOnly;

    // Print line-oriented initialization details after DB setup completes.
    bool printInfoFlag = true;
    // Print recorded logger/backend errors while still keeping lastError().
    bool printErrorFlag = true;
};
}
