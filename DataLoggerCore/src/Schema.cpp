#include "DataLogger/Schema.h"

namespace DataLoggerCore
{
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
}
