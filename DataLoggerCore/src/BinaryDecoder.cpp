#include "DataLogger/BinaryDecoder.h"

#include <cstring>

namespace DataLoggerCore
{
namespace
{
// Copy bytes into a typed value without aliasing or alignment assumptions.
template<typename T>
T readValue(const void* base, std::size_t offset)
{
    T value{};
    std::memcpy(&value, static_cast<const unsigned char*>(base) + offset, sizeof(T));
    return value;
}

// Store one decoded payload value into the matching typed column vector.
void decodeFieldIntoColumn(const void* base,
                           const ExpandedColumnSchema& column,
                           ColumnStorage& storage,
                           std::size_t rowIndex)
{
    switch (column.datatype)
    {
    case DataType::Int8:
        storage.int16Values[rowIndex] = static_cast<std::int16_t>(readValue<std::int8_t>(base, column.offset));
        break;
    case DataType::UInt8:
        storage.uint8Values[rowIndex] = readValue<std::uint8_t>(base, column.offset);
        break;
    case DataType::Int16:
        storage.int16Values[rowIndex] = readValue<std::int16_t>(base, column.offset);
        break;
    case DataType::UInt16:
        storage.int32Values[rowIndex] = static_cast<std::int32_t>(readValue<std::uint16_t>(base, column.offset));
        break;
    case DataType::Int32:
        storage.int32Values[rowIndex] = readValue<std::int32_t>(base, column.offset);
        break;
    case DataType::UInt32:
        storage.int64Values[rowIndex] = static_cast<std::int64_t>(readValue<std::uint32_t>(base, column.offset));
        break;
    case DataType::Int64:
        storage.int64Values[rowIndex] = readValue<std::int64_t>(base, column.offset);
        break;
    case DataType::UInt64:
        storage.uint64Values[rowIndex] = readValue<std::uint64_t>(base, column.offset);
        break;
    case DataType::Float:
        storage.floatValues[rowIndex] = readValue<float>(base, column.offset);
        break;
    case DataType::Double:
        storage.doubleValues[rowIndex] = readValue<double>(base, column.offset);
        break;
    }
}
}

// Decode a caller-owned struct into preallocated column vectors in schema order.
// The caller remains responsible for passing memory matching the CSV offsets.
bool decodeIntoColumnBatch(const TableSchema& table,
                           std::int64_t timestampMs,
                           const void* structPtr,
                           ColumnBatch& batch,
                           DataLoggerError& error)
{
    if (structPtr == nullptr)
    {
        error = { ErrorCode::DecodeFailed, "Cannot decode table '" + table.tableName + "' from a null struct pointer." };
        return false;
    }

    if (batch.rowCount >= batch.rowCapacity)
    {
        error = { ErrorCode::DecodeFailed, "Column batch for table '" + table.tableName + "' is full." };
        return false;
    }

    if (batch.timestamps.size() < batch.rowCapacity || batch.columns.size() != table.expandedColumns.size())
    {
        error = { ErrorCode::DecodeFailed, "Column batch for table '" + table.tableName + "' is not initialized for this schema." };
        return false;
    }

    const std::size_t rowIndex = batch.rowCount;
    batch.timestamps[rowIndex] = timestampMs;

    // Copy every payload value now so no caller pointer is retained after this function.
    for (std::size_t columnIndex = 0; columnIndex < table.expandedColumns.size(); ++columnIndex)
    {
        decodeFieldIntoColumn(structPtr, table.expandedColumns[columnIndex], batch.columns[columnIndex], rowIndex);
    }

    ++batch.rowCount;
    error = DataLoggerError{};
    return true;
}
}
