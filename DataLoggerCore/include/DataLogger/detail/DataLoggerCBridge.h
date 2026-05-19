#pragma once

#ifndef __cplusplus
#error "DataLoggerCBridge.h is a private C++ bridge header and cannot be included from C."
#endif

#include "DataLogger/IDBBackend.h"

#include <memory>
#include <utility>

struct DataLoggerBackend_c
{
    // Own one concrete backend through the core backend interface.
    explicit DataLoggerBackend_c(std::unique_ptr<DataLoggerCore::IDBBackend> backendValue)
        : backend(std::move(backendValue))
    {
    }

    std::unique_ptr<DataLoggerCore::IDBBackend> backend;
};
