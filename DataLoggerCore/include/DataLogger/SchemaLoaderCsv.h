#pragma once

#include "DataLogger/Error.h"
#include "DataLogger/Schema.h"

#include <string>

namespace DataLoggerCore
{
bool loadSchemaDirectory(const std::string& schemaDirectory,
                         SchemaRegistry& registry,
                         DataLoggerError& error);
}
