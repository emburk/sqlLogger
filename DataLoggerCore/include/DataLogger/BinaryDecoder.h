#pragma once

#include "DataLogger/Error.h"
#include "DataLogger/Schema.h"

#include <cstdint>

namespace DataLoggerCore
{
// Decode one caller-owned struct into the next preallocated typed batch slot.
bool decodeIntoColumnBatch(const TableSchema& table,
                           std::int64_t timestampMs,
                           const void* structPtr,
                           ColumnBatch& batch,
                           DataLoggerError& error);
}
