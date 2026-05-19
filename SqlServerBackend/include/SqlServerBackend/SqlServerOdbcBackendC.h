#pragma once

#include "DataLogger/DataLoggerC.h"

#ifdef __cplusplus
extern "C" {
#endif

// Create the concrete SQL Server ODBC backend for use with datalogger_create_c().
DataLoggerBackend_c* sqlserver_backend_create_c(void);

// Destroy a backend handle that has not been transferred to datalogger_create_c().
void sqlserver_backend_destroy_c(DataLoggerBackend_c* backend);

#ifdef __cplusplus
}
#endif
