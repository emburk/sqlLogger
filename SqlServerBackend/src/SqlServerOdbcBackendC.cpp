#include "SqlServerBackend/SqlServerOdbcBackendC.h"

#include "DataLogger/detail/DataLoggerCBridge.h"
#include "SqlServerBackend/SqlServerOdbcBackend.h"

#include <memory>

// Create the concrete SQL Server ODBC backend for use with datalogger_create_c().
DataLoggerBackend_c* sqlserver_backend_create_c(void)
{
    try
    {
        auto backend = std::make_unique<SqlServerBackend::SqlServerOdbcBackend>();
        return new DataLoggerBackend_c(std::move(backend));
    }
    catch (...)
    {
        return nullptr;
    }
}

// Destroy a backend handle that has not been transferred to datalogger_create_c().
void sqlserver_backend_destroy_c(DataLoggerBackend_c* backend)
{
    delete backend;
}
