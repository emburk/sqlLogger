#pragma once

#include <string>

namespace DataLoggerCore
{
enum class ErrorCode
{
    None,
    InvalidConfig,
    SchemaLoadFailed,
    SchemaValidationFailed,
    InvalidTableHandle,
    DecodeFailed
};

struct DataLoggerError
{
    ErrorCode code = ErrorCode::None;
    std::string message;
};
}
