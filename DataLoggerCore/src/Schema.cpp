#include "DataLogger/Schema.h"

namespace DataLoggerCore
{
// Map CSV datatype tokens to the internal enum; accepted names stay lowercase by design.
bool parseDataType(const std::string& text, DataType& datatype)
{
    if (text == "int8")
    {
        datatype = DataType::Int8;
        return true;
    }
    if (text == "uint8")
    {
        datatype = DataType::UInt8;
        return true;
    }
    if (text == "int16")
    {
        datatype = DataType::Int16;
        return true;
    }
    if (text == "uint16")
    {
        datatype = DataType::UInt16;
        return true;
    }
    if (text == "int32")
    {
        datatype = DataType::Int32;
        return true;
    }
    if (text == "uint32")
    {
        datatype = DataType::UInt32;
        return true;
    }
    if (text == "int64")
    {
        datatype = DataType::Int64;
        return true;
    }
    if (text == "uint64")
    {
        datatype = DataType::UInt64;
        return true;
    }
    if (text == "float")
    {
        datatype = DataType::Float;
        return true;
    }
    if (text == "double")
    {
        datatype = DataType::Double;
        return true;
    }

    return false;
}

// Return the byte size required by each supported fixed-width numeric payload type.
std::size_t expectedDataTypeSize(DataType datatype)
{
    switch (datatype)
    {
    case DataType::Int8:
    case DataType::UInt8:
        return 1;
    case DataType::Int16:
    case DataType::UInt16:
        return 2;
    case DataType::Int32:
    case DataType::UInt32:
    case DataType::Float:
        return 4;
    case DataType::Int64:
    case DataType::UInt64:
    case DataType::Double:
        return 8;
    }

    return 0;
}

// Preallocate only the vector matching this column's SQL-ready storage type.
void initializeColumnStorage(ColumnStorage& storage, DataType datatype, std::size_t rowCapacity)
{
    storage = ColumnStorage{};
    storage.datatype = datatype;

    switch (datatype)
    {
    case DataType::Int8:
    case DataType::Int16:
        storage.int16Values.resize(rowCapacity);
        break;
    case DataType::UInt8:
        storage.uint8Values.resize(rowCapacity);
        break;
    case DataType::UInt16:
    case DataType::Int32:
        storage.int32Values.resize(rowCapacity);
        break;
    case DataType::UInt32:
    case DataType::Int64:
        storage.int64Values.resize(rowCapacity);
        break;
    case DataType::UInt64:
        storage.uint64Values.resize(rowCapacity);
        break;
    case DataType::Float:
        storage.floatValues.resize(rowCapacity);
        break;
    case DataType::Double:
        storage.doubleValues.resize(rowCapacity);
        break;
    }
}

// Preallocate the complete table batch so write() only fills existing slots.
void initializeColumnBatch(const TableSchema& table, std::size_t rowCapacity, ColumnBatch& batch)
{
    batch = ColumnBatch{};
    batch.schema = &table;
    batch.rowCapacity = rowCapacity;
    batch.timestamps.resize(rowCapacity);
    batch.columns.resize(table.expandedColumns.size());

    for (std::size_t i = 0; i < table.expandedColumns.size(); ++i)
    {
        initializeColumnStorage(batch.columns[i], table.expandedColumns[i].datatype, rowCapacity);
    }
}

// Return the preallocated row capacity of the active typed vector.
std::size_t columnStorageSize(const ColumnStorage& storage)
{
    switch (storage.datatype)
    {
    case DataType::Int8:
    case DataType::Int16:
        return storage.int16Values.size();
    case DataType::UInt8:
        return storage.uint8Values.size();
    case DataType::UInt16:
    case DataType::Int32:
        return storage.int32Values.size();
    case DataType::UInt32:
    case DataType::Int64:
        return storage.int64Values.size();
    case DataType::UInt64:
        return storage.uint64Values.size();
    case DataType::Float:
        return storage.floatValues.size();
    case DataType::Double:
        return storage.doubleValues.size();
    }

    return 0;
}
}
