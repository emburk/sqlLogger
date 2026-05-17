#pragma once

#include "DataLogger/Error.h"
#include "DataLogger/Schema.h"

#include <string>

namespace DataLoggerCore
{
// Load every CSV schema from a directory and validate the resulting registry.
bool loadSchemaDirectory(const std::string& schemaDirectory,
                         SchemaRegistry& registry,
                         DataLoggerError& error);
}
