#pragma once

#include "DataLogger/Error.h"
#include "DataLogger/Schema.h"

#include <cstdint>

namespace DataLoggerCore
{
// Decode one caller-owned struct into row-owned values using fixed schema offsets.
bool decodeRow(const TableSchema& table,
               std::int64_t timestampMs,
               const void* structPtr,
               DecodedRow& row,
               DataLoggerError& error);
}
