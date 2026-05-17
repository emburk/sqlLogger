#include "DataLogger/SchemaValidator.h"

#include <cctype>
#include <limits>
#include <set>
#include <string>

namespace DataLoggerCore
{
namespace
{
constexpr const char* kTimestampColumnName = "timestamp_ms";

bool willOverflowOffset(std::size_t offset, std::size_t size, std::size_t index)
{
    if (index == 0)
    {
        return false;
    }

    const std::size_t maxValue = (std::numeric_limits<std::size_t>::max)();
    return size > (maxValue - offset) / index;
}

bool validateAndExpandTable(TableSchema& table, DataLoggerError& error)
{
    if (!isValidSqlIdentifier(table.tableName))
    {
        error = { ErrorCode::SchemaValidationFailed,
                  "Invalid table name '" + table.tableName + "'. SQL identifiers must match [A-Za-z_][A-Za-z0-9_]*." };
        return false;
    }

    if (table.columns.empty())
    {
        error = { ErrorCode::SchemaValidationFailed,
                  "Schema for table '" + table.tableName + "' does not contain any payload columns." };
        return false;
    }

    table.expandedColumns.clear();
    std::set<std::string> expandedNames;

    for (const ColumnSchema& column : table.columns)
    {
        if (!isValidSqlIdentifier(column.baseName))
        {
            error = { ErrorCode::SchemaValidationFailed,
                      "Invalid column name '" + column.baseName + "' in table '" + table.tableName + "'." };
            return false;
        }

        if (column.length == 0)
        {
            error = { ErrorCode::SchemaValidationFailed,
                      "Column '" + column.baseName + "' in table '" + table.tableName + "' has length 0." };
            return false;
        }

        const std::size_t expectedSize = expectedDataTypeSize(column.datatype);
        if (column.size != expectedSize)
        {
            error = { ErrorCode::SchemaValidationFailed,
                      "Column '" + column.baseName + "' in table '" + table.tableName + "' has size " +
                          std::to_string(column.size) + ", expected " + std::to_string(expectedSize) + "." };
            return false;
        }

        for (std::size_t i = 0; i < column.length; ++i)
        {
            if (willOverflowOffset(column.offset, column.size, i))
            {
                error = { ErrorCode::SchemaValidationFailed,
                          "Column '" + column.baseName + "' in table '" + table.tableName +
                              "' overflows while expanding array offsets." };
                return false;
            }

            ExpandedColumnSchema expandedColumn;
            expandedColumn.sqlName = column.length == 1 ? column.baseName : column.baseName + "_" + std::to_string(i);
            expandedColumn.offset = column.offset + i * column.size;
            expandedColumn.datatype = column.datatype;
            expandedColumn.size = column.size;

            if (!isValidSqlIdentifier(expandedColumn.sqlName))
            {
                error = { ErrorCode::SchemaValidationFailed,
                          "Expanded column name '" + expandedColumn.sqlName + "' in table '" + table.tableName +
                              "' is not a valid SQL identifier." };
                return false;
            }

            if (expandedColumn.sqlName == kTimestampColumnName)
            {
                error = { ErrorCode::SchemaValidationFailed,
                          "Payload column '" + expandedColumn.sqlName + "' in table '" + table.tableName +
                              "' conflicts with reserved timestamp_ms column." };
                return false;
            }

            if (!expandedNames.insert(expandedColumn.sqlName).second)
            {
                error = { ErrorCode::SchemaValidationFailed,
                          "Duplicate expanded column '" + expandedColumn.sqlName + "' in table '" + table.tableName + "'." };
                return false;
            }

            table.expandedColumns.push_back(expandedColumn);
        }
    }

    const std::size_t sqlColumnCount = table.expandedColumns.size() + 1;
    if (sqlColumnCount > kSqlServerMaxColumnsPerTable)
    {
        error = { ErrorCode::SchemaValidationFailed,
                  "Table '" + table.tableName + "' expands to " + std::to_string(sqlColumnCount) +
                      " SQL columns including timestamp_ms. Split the schema into multiple CSV/table files." };
        return false;
    }

    if (sqlColumnCount > kSqlServerMaxParameters)
    {
        error = { ErrorCode::SchemaValidationFailed,
                  "Table '" + table.tableName + "' requires " + std::to_string(sqlColumnCount) +
                      " insert parameters. Split the schema into multiple CSV/table files." };
        return false;
    }

    return true;
}
}

bool isValidSqlIdentifier(const std::string& identifier)
{
    if (identifier.empty())
    {
        return false;
    }

    const unsigned char first = static_cast<unsigned char>(identifier.front());
    if (!(std::isalpha(first) || identifier.front() == '_'))
    {
        return false;
    }

    for (char character : identifier)
    {
        const unsigned char value = static_cast<unsigned char>(character);
        if (!(std::isalnum(value) || character == '_'))
        {
            return false;
        }
    }

    return true;
}

bool validateAndExpandSchemaRegistry(SchemaRegistry& registry, DataLoggerError& error)
{
    if (registry.tables.empty())
    {
        error = { ErrorCode::SchemaValidationFailed, "Schema directory did not contain any CSV schema files." };
        return false;
    }

    std::set<std::string> tableNames;
    for (TableSchema& table : registry.tables)
    {
        if (!tableNames.insert(table.tableName).second)
        {
            error = { ErrorCode::SchemaValidationFailed, "Duplicate table schema '" + table.tableName + "'." };
            return false;
        }

        if (!validateAndExpandTable(table, error))
        {
            return false;
        }
    }

    return true;
}
}
