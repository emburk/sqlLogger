#include "DataLogger/BinaryDecoder.h"

#include <cstring>

namespace DataLoggerCore
{
namespace
{
template<typename T>
T readValue(const void* base, std::size_t offset)
{
    T value{};
    std::memcpy(&value, static_cast<const unsigned char*>(base) + offset, sizeof(T));
    return value;
}

FieldValue decodeField(const void* base, const ExpandedColumnSchema& column)
{
    switch (column.datatype)
    {
    case DataType::Int8:
        return readValue<std::int8_t>(base, column.offset);
    case DataType::UInt8:
        return readValue<std::uint8_t>(base, column.offset);
    case DataType::Int16:
        return readValue<std::int16_t>(base, column.offset);
    case DataType::UInt16:
        return readValue<std::uint16_t>(base, column.offset);
    case DataType::Int32:
        return readValue<std::int32_t>(base, column.offset);
    case DataType::UInt32:
        return readValue<std::uint32_t>(base, column.offset);
    case DataType::Int64:
        return readValue<std::int64_t>(base, column.offset);
    case DataType::UInt64:
        return readValue<std::uint64_t>(base, column.offset);
    case DataType::Float:
        return readValue<float>(base, column.offset);
    case DataType::Double:
        return readValue<double>(base, column.offset);
    }

    return std::int8_t{};
}
}

bool decodeRow(const TableSchema& table,
               std::int64_t timestampMs,
               const void* structPtr,
               DecodedRow& row,
               DataLoggerError& error)
{
    if (structPtr == nullptr)
    {
        error = { ErrorCode::DecodeFailed, "Cannot decode table '" + table.tableName + "' from a null struct pointer." };
        return false;
    }

    DecodedRow decoded;
    decoded.timestampMs = timestampMs;
    decoded.values.reserve(table.expandedColumns.size());

    for (const ExpandedColumnSchema& column : table.expandedColumns)
    {
        decoded.values.push_back(decodeField(structPtr, column));
    }

    row = decoded;
    error = DataLoggerError{};
    return true;
}
}
