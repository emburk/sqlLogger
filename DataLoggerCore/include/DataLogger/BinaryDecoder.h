#pragma once

#include "DataLogger/Error.h"
#include "DataLogger/Schema.h"

#include <cstdint>

namespace DataLoggerCore
{
bool decodeRow(const TableSchema& table,
               std::int64_t timestampMs,
               const void* structPtr,
               DecodedRow& row,
               DataLoggerError& error);
}
