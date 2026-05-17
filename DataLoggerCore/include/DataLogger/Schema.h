#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace DataLoggerCore
{
enum class DataType
{
    Int8,
    UInt8,
    Int16,
    UInt16,
    Int32,
    UInt32,
    Int64,
    UInt64,
    Float,
    Double
};

struct ColumnSchema
{
    std::string baseName;
    std::string unit;
    std::string description;

    std::size_t offset = 0;
    DataType datatype = DataType::Int8;
    std::size_t size = 0;
    std::size_t length = 1;
};

struct ExpandedColumnSchema
{
    std::string sqlName;
    std::size_t offset = 0;
    DataType datatype = DataType::Int8;
    std::size_t size = 0;
};

struct TableSchema
{
    std::string tableName;
    std::vector<ColumnSchema> columns;
    std::vector<ExpandedColumnSchema> expandedColumns;
};

struct SchemaRegistry
{
    std::vector<TableSchema> tables;
};

using FieldValue = std::variant<
    std::int8_t,
    std::uint8_t,
    std::int16_t,
    std::uint16_t,
    std::int32_t,
    std::uint32_t,
    std::int64_t,
    std::uint64_t,
    float,
    double>;

struct DecodedRow
{
    std::int64_t timestampMs = 0;
    std::vector<FieldValue> values;
};

// Parse a lowercase CSV datatype token into the internal enum.
bool parseDataType(const std::string& text, DataType& datatype);
// Return the expected byte size for one supported numeric datatype.
std::size_t expectedDataTypeSize(DataType datatype);
}
