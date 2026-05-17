#pragma once

#include "DataLogger/Error.h"
#include "DataLogger/Schema.h"

namespace DataLoggerCore
{
constexpr std::size_t kSqlServerMaxColumnsPerTable = 1024;
constexpr std::size_t kSqlServerMaxParameters = 2100;

// Validate conservative SQL identifier syntax before generated names are bracketed.
bool isValidSqlIdentifier(const std::string& identifier);
// Validate all tables and expand array fields into scalar SQL payload columns.
bool validateAndExpandSchemaRegistry(SchemaRegistry& registry, DataLoggerError& error);
}
