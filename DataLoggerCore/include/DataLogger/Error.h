#pragma once

#include <string>
#include <vector>

namespace DataLoggerCore
{
enum class ErrorCode
{
    None,
    InvalidConfig,
    SchemaLoadFailed,
    SchemaValidationFailed,
    InvalidTableHandle,
    DecodeFailed,
    BackendConnectFailed,
    BackendTableInitFailed,
    BackendPrepareFailed,
    BackendInsertFailed
};

struct DataLoggerError
{
    ErrorCode code = ErrorCode::None;
    std::string message;
};

struct OdbcDiagnostic
{
    std::string sqlState;
    int nativeError = 0;
    std::string message;
};

struct BackendError
{
    std::string message;
    std::vector<OdbcDiagnostic> diagnostics;
};
}
