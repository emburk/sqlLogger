#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
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

struct ColumnStorage
{
    DataType datatype = DataType::Int8;
    std::vector<std::int16_t> int16Values;
    std::vector<std::uint8_t> uint8Values;
    std::vector<std::int32_t> int32Values;
    std::vector<std::int64_t> int64Values;
    std::vector<std::uint64_t> uint64Values;
    std::vector<float> floatValues;
    std::vector<double> doubleValues;
};

struct ColumnBatch
{
    const TableSchema* schema = nullptr;
    std::size_t rowCount = 0;
    std::size_t rowCapacity = 0;
    std::vector<std::int64_t> timestamps;
    std::vector<ColumnStorage> columns;
};

// Parse a lowercase CSV datatype token into the internal enum.
bool parseDataType(const std::string& text, DataType& datatype);
// Return the expected byte size for one supported numeric datatype.
std::size_t expectedDataTypeSize(DataType datatype);
// Preallocate one typed column storage vector for a fixed batch capacity.
void initializeColumnStorage(ColumnStorage& storage, DataType datatype, std::size_t rowCapacity);
// Preallocate timestamp and typed payload vectors for one table batch.
void initializeColumnBatch(const TableSchema& table, std::size_t rowCapacity, ColumnBatch& batch);
// Return the active vector size for the storage type selected by datatype.
std::size_t columnStorageSize(const ColumnStorage& storage);
}
