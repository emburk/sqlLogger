#pragma once

#include "DataLogger/Error.h"
#include "DataLogger/Schema.h"

namespace DataLoggerCore
{
constexpr std::size_t kSqlServerMaxColumnsPerTable = 1024;
constexpr std::size_t kSqlServerMaxParameters = 2100;

bool isValidSqlIdentifier(const std::string& identifier);
bool validateAndExpandSchemaRegistry(SchemaRegistry& registry, DataLoggerError& error);
}
