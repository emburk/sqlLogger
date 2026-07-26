#include "SqlServerBackend/SqlServerOdbcBackend.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <sql.h>
#include <sqlext.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace SqlServerBackend
{
namespace
{
using DataLoggerCore::BackendError;
using DataLoggerCore::ColumnBatch;
using DataLoggerCore::ColumnStorage;
using DataLoggerCore::DataType;
using DataLoggerCore::ExistingTablePolicy;
using DataLoggerCore::OdbcDiagnostic;
using DataLoggerCore::SchemaRegistry;
using DataLoggerCore::SqlServerIndexMode;
using DataLoggerCore::TableSchema;

constexpr SQLLEN kNotNullIndicator = 0;

// Test ODBC return codes while accepting success-with-info as a successful call.
bool isOdbcSuccess(SQLRETURN result)
{
    return result == SQL_SUCCESS || result == SQL_SUCCESS_WITH_INFO;
}

// Quote a validated SQL Server identifier using bracket syntax.
std::string quoteIdentifier(const std::string& identifier)
{
    return "[" + identifier + "]";
}

// Build a schema-qualified SQL Server table name from validated identifiers.
std::string qualifiedTableName(const std::string& schemaName, const std::string& tableName)
{
    return quoteIdentifier(schemaName) + "." + quoteIdentifier(tableName);
}

// Escape single quotes for use inside T-SQL string literals.
std::string quoteSqlString(const std::string& value)
{
    std::string quoted = "N'";
    for (char character : value)
    {
        if (character == '\'')
        {
            quoted += "''";
        }
        else
        {
            quoted += character;
        }
    }
    quoted += "'";
    return quoted;
}

// Map logger datatypes to SQL Server column types.
std::string sqlTypeFor(DataType datatype)
{
    switch (datatype)
    {
    case DataType::Int8:
        return "SMALLINT";
    case DataType::UInt8:
        return "TINYINT";
    case DataType::Int16:
        return "SMALLINT";
    case DataType::UInt16:
        return "INT";
    case DataType::Int32:
        return "INT";
    case DataType::UInt32:
        return "BIGINT";
    case DataType::Int64:
        return "BIGINT";
    case DataType::UInt64:
        return "DECIMAL(20,0)";
    case DataType::Float:
        return "REAL";
    case DataType::Double:
        return "FLOAT(53)";
    }

    return "INT";
}

// Create the sortable suffix required by the rename-existing-table policy.
std::string currentTimestampSuffix()
{
    const std::time_t now = std::time(nullptr);
    std::tm localTime{};
    localtime_s(&localTime, &now);

    std::ostringstream stream;
    stream << std::put_time(&localTime, "%Y%m%d-%H%M%S");
    return stream.str();
}

// Convert a uint64 value into the little-endian integer bytes used by SQL_NUMERIC_STRUCT.
SQL_NUMERIC_STRUCT toNumericStruct(std::uint64_t value)
{
    SQL_NUMERIC_STRUCT numeric{};
    numeric.precision = 20;
    numeric.scale = 0;
    numeric.sign = 1;

    for (std::size_t i = 0; i < sizeof(value) && i < sizeof(numeric.val); ++i)
    {
        numeric.val[i] = static_cast<SQLCHAR>((value >> (i * 8)) & 0xFFU);
    }

    return numeric;
}

class OdbcHandle
{
public:
    // Create an empty handle wrapper for the requested ODBC handle type.
    explicit OdbcHandle(SQLSMALLINT handleType)
        : handleType_(handleType)
    {
    }

    // Free the wrapped ODBC handle when ownership leaves scope.
    ~OdbcHandle()
    {
        reset();
    }

    OdbcHandle(const OdbcHandle&) = delete;
    OdbcHandle& operator=(const OdbcHandle&) = delete;

    // Allocate a handle using the supplied parent handle.
    bool allocate(SQLHANDLE parent)
    {
        reset();
        return isOdbcSuccess(SQLAllocHandle(handleType_, parent, &handle_));
    }

    // Take ownership of an already allocated handle.
    void attach(SQLHANDLE handle)
    {
        reset();
        handle_ = handle;
    }

    // Free any currently owned handle and return to the empty state.
    void reset()
    {
        if (handle_ != SQL_NULL_HANDLE)
        {
            SQLFreeHandle(handleType_, handle_);
            handle_ = SQL_NULL_HANDLE;
        }
    }

    // Return the raw ODBC handle for API calls.
    SQLHANDLE get() const
    {
        return handle_;
    }

private:
    SQLSMALLINT handleType_ = SQL_HANDLE_ENV;
    SQLHANDLE handle_ = SQL_NULL_HANDLE;
};

struct BoundColumn
{
    DataType datatype = DataType::Int8;
    std::vector<SQLLEN> indicators;
    std::vector<SQL_NUMERIC_STRUCT> numericValues;
};

struct StatementInfo
{
    OdbcHandle statement;
    std::size_t capacityRows = 0;
    bool parametersBound = false;
    const ColumnBatch* boundBatch = nullptr;
    std::vector<SQLLEN> timestampIndicators;
    std::vector<BoundColumn> payloadColumns;
    std::vector<SQLUSMALLINT> rowStatuses;
    SQLULEN processedCount = 0;

    // Statements own a prepared ODBC handle plus reusable parameter-array buffers.
    StatementInfo()
        : statement(SQL_HANDLE_STMT)
    {
    }
};

struct ExistingColumnInfo
{
    std::string name;
    std::string dataType;
    int numericPrecision = 0;
    int numericScale = 0;
    bool nullable = true;
};

// Return a lowercase copy of SQL Server metadata type names for comparison.
std::string lowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

// Return the C type used for binding one payload column.
SQLSMALLINT cTypeFor(DataType datatype)
{
    switch (datatype)
    {
    case DataType::Int8:
    case DataType::Int16:
        return SQL_C_SSHORT;
    case DataType::UInt8:
        return SQL_C_UTINYINT;
    case DataType::UInt16:
    case DataType::Int32:
        return SQL_C_SLONG;
    case DataType::UInt32:
    case DataType::Int64:
        return SQL_C_SBIGINT;
    case DataType::UInt64:
        return SQL_C_NUMERIC;
    case DataType::Float:
        return SQL_C_FLOAT;
    case DataType::Double:
        return SQL_C_DOUBLE;
    }

    return SQL_C_SLONG;
}

// Return the SQL type used for binding one payload parameter.
SQLSMALLINT sqlParameterTypeFor(DataType datatype)
{
    switch (datatype)
    {
    case DataType::UInt64:
        return SQL_DECIMAL;
    case DataType::Float:
        return SQL_REAL;
    case DataType::Double:
        return SQL_DOUBLE;
    case DataType::UInt8:
        return SQL_TINYINT;
    case DataType::Int8:
    case DataType::Int16:
        return SQL_SMALLINT;
    case DataType::UInt16:
    case DataType::Int32:
        return SQL_INTEGER;
    case DataType::UInt32:
    case DataType::Int64:
        return SQL_BIGINT;
    }

    return SQL_INTEGER;
}

// Return the SQL column size expected by ODBC for parameter metadata.
SQLULEN columnSizeFor(DataType datatype)
{
    switch (datatype)
    {
    case DataType::UInt64:
        return 20;
    case DataType::Double:
        return 53;
    default:
        return 0;
    }
}

// Fill the reusable DECIMAL conversion buffer used by full-range uint64 columns.
void fillNumericValues(BoundColumn& boundColumn,
                       const ColumnStorage& sourceColumn,
                       std::size_t rowCount)
{
    for (std::size_t rowIndex = 0; rowIndex < rowCount; ++rowIndex)
    {
        boundColumn.numericValues[rowIndex] = toNumericStruct(sourceColumn.uint64Values[rowIndex]);
    }
}

// Return the active data pointer, binding directly from the column batch when possible.
SQLPOINTER dataPointerFor(const ColumnStorage& sourceColumn, BoundColumn& boundColumn)
{
    switch (sourceColumn.datatype)
    {
    case DataType::Int8:
    case DataType::Int16:
        return const_cast<std::int16_t*>(sourceColumn.int16Values.data());
    case DataType::UInt8:
        return const_cast<std::uint8_t*>(sourceColumn.uint8Values.data());
    case DataType::UInt16:
    case DataType::Int32:
        return const_cast<std::int32_t*>(sourceColumn.int32Values.data());
    case DataType::UInt32:
    case DataType::Int64:
        return const_cast<std::int64_t*>(sourceColumn.int64Values.data());
    case DataType::UInt64:
        return boundColumn.numericValues.data();
    case DataType::Float:
        return const_cast<float*>(sourceColumn.floatValues.data());
    case DataType::Double:
        return const_cast<double*>(sourceColumn.doubleValues.data());
    }

    return nullptr;
}

// Return the byte size of one active bound-column element.
SQLLEN bufferLengthFor(DataType datatype)
{
    switch (datatype)
    {
    case DataType::Int8:
    case DataType::Int16:
        return sizeof(std::int16_t);
    case DataType::UInt8:
        return sizeof(std::uint8_t);
    case DataType::UInt16:
    case DataType::Int32:
        return sizeof(std::int32_t);
    case DataType::UInt32:
    case DataType::Int64:
        return sizeof(std::int64_t);
    case DataType::UInt64:
        return sizeof(SQL_NUMERIC_STRUCT);
    case DataType::Float:
        return sizeof(float);
    case DataType::Double:
        return sizeof(double);
    }

    return 0;
}
}

class SqlServerOdbcBackend::Impl
{
public:
    // Create empty ODBC environment and connection handle wrappers.
    Impl()
        : environment_(SQL_HANDLE_ENV),
          connection_(SQL_HANDLE_DBC)
    {
    }

    // Disconnect from SQL Server before handle wrappers release their resources.
    ~Impl()
    {
        statements_.clear();
        if (connected_)
        {
            SQLDisconnect(connection_.get());
        }
    }

    // Open the ODBC environment, set ODBC 3, and connect with SQLDriverConnect.
    bool connect(const std::string& connectionString)
    {
        clearError();
        statements_.clear();

        if (connected_)
        {
            SQLDisconnect(connection_.get());
            connected_ = false;
        }

        connection_.reset();
        environment_.reset();

        if (!environment_.allocate(SQL_NULL_HANDLE))
        {
            setError("Unable to allocate ODBC environment handle.");
            return false;
        }

        SQLRETURN result = SQLSetEnvAttr(environment_.get(), SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0);
        if (!isOdbcSuccess(result))
        {
            setError("Unable to set ODBC environment version.", SQL_HANDLE_ENV, environment_.get());
            return false;
        }

        if (!connection_.allocate(environment_.get()))
        {
            setError("Unable to allocate ODBC connection handle.", SQL_HANDLE_ENV, environment_.get());
            return false;
        }

        SQLCHAR output[1024] = {};
        SQLSMALLINT outputLength = 0;
        result = SQLDriverConnectA(connection_.get(),
                                   nullptr,
                                   reinterpret_cast<SQLCHAR*>(const_cast<char*>(connectionString.c_str())),
                                   SQL_NTS,
                                   output,
                                   static_cast<SQLSMALLINT>(sizeof(output)),
                                   &outputLength,
                                   SQL_DRIVER_NOPROMPT);

        if (!isOdbcSuccess(result))
        {
            setError("Unable to connect to SQL Server through ODBC.", SQL_HANDLE_DBC, connection_.get());
            return false;
        }

        connected_ = true;
        return true;
    }

    // Prepare SQL tables from the schema registry and add timestamp indexes.
    bool initializeTables(const SchemaRegistry& registry,
                          const std::string& sqlSchemaName,
                          ExistingTablePolicy policy,
                          SqlServerIndexMode indexMode)
    {
        clearError();

        if (!connected_)
        {
            setError("Cannot initialize tables before connecting to SQL Server.");
            return false;
        }

        const std::string suffix = policy == ExistingTablePolicy::RenameWithTimestampSuffix ? currentTimestampSuffix() : "";
        for (const TableSchema& table : registry.tables)
        {
            if (policy == ExistingTablePolicy::Drop)
            {
                if (!dropExistingTable(sqlSchemaName, table.tableName))
                {
                    return false;
                }
            }
            else if (policy == ExistingTablePolicy::RenameWithTimestampSuffix)
            {
                if (!renameExistingTable(sqlSchemaName, table.tableName, suffix))
                {
                    return false;
                }
            }
            else if (policy == ExistingTablePolicy::ContinueCurrentTable)
            {
                bool exists = false;
                if (!tableExists(sqlSchemaName, table.tableName, exists))
                {
                    return false;
                }

                if (exists)
                {
                    if (!validateExistingTable(sqlSchemaName, table))
                    {
                        return false;
                    }

                    if (!createTimestampIndex(sqlSchemaName, table) ||
                        !createColumnstoreIndexIfRequested(sqlSchemaName, table, indexMode))
                    {
                        return false;
                    }

                    continue;
                }
            }

            if (!createTable(sqlSchemaName, table) ||
                !createTimestampIndex(sqlSchemaName, table) ||
                !createColumnstoreIndexIfRequested(sqlSchemaName, table, indexMode))
            {
                return false;
            }
        }

        return true;
    }

    // Prepare and retain one INSERT statement handle for every table schema.
    bool prepareInsertStatements(const SchemaRegistry& registry,
                                 const std::string& sqlSchemaName,
                                 std::size_t batchSizeRows)
    {
        clearError();
        statements_.clear();

        for (const TableSchema& table : registry.tables)
        {
            auto info = std::make_unique<StatementInfo>();
            if (!info->statement.allocate(connection_.get()))
            {
                setError("Unable to allocate ODBC statement for table '" + table.tableName + "'.", SQL_HANDLE_DBC, connection_.get());
                return false;
            }

            const std::string sql = buildInsertSql(sqlSchemaName, table);
            SQLRETURN result = SQLPrepareA(info->statement.get(),
                                           reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql.c_str())),
                                           SQL_NTS);
            if (!isOdbcSuccess(result))
            {
                setError("Unable to prepare insert statement for table '" + table.tableName + "'.", SQL_HANDLE_STMT, info->statement.get());
                return false;
            }

            initializeStatementBuffers(*info, table, batchSizeRows);
            statements_[table.tableName] = std::move(info);
        }

        return true;
    }

    // Execute a predecoded column batch in one transaction.
    bool insertBatch(const TableSchema& table, const ColumnBatch& batch)
    {
        clearError();

        if (batch.rowCount == 0)
        {
            return true;
        }

        auto statement = statements_.find(table.tableName);
        if (statement == statements_.end())
        {
            setError("No prepared insert statement exists for table '" + table.tableName + "'.");
            return false;
        }

        StatementInfo& info = *statement->second;
        if (!validateBatch(table, batch, info))
        {
            return false;
        }

        SQLHSTMT statementHandle = info.statement.get();
        if (!ensureParametersBound(statementHandle, table, batch, info))
        {
            return false;
        }

        if (!prepareBatchExecution(statementHandle, table, batch, info))
        {
            return false;
        }

        SQLUINTEGER previousAutocommit = SQL_AUTOCOMMIT_ON;
        if (!beginTransaction(previousAutocommit))
        {
            return false;
        }

        SQLRETURN result = SQLExecute(statementHandle);
        if (!isOdbcSuccess(result))
        {
            setError("ODBC batch execution failed for table '" + table.tableName + "'.", SQL_HANDLE_STMT, statementHandle);
            SQLEndTran(SQL_HANDLE_DBC, connection_.get(), SQL_ROLLBACK);
            restoreAutocommit(previousAutocommit);
            return false;
        }

        result = SQLEndTran(SQL_HANDLE_DBC, connection_.get(), SQL_COMMIT);
        if (!isOdbcSuccess(result))
        {
            setError("ODBC commit failed for table '" + table.tableName + "'.", SQL_HANDLE_DBC, connection_.get());
            restoreAutocommit(previousAutocommit);
            return false;
        }

        restoreAutocommit(previousAutocommit);
        return true;
    }

    // Return the last stored backend error.
    BackendError lastError() const
    {
        return lastError_;
    }

private:
    // Clear the last backend error before starting a public operation.
    void clearError()
    {
        lastError_ = BackendError{};
    }

    // Store an error message without ODBC diagnostics.
    void setError(const std::string& message)
    {
        lastError_.message = message;
    }

    // Store an error message and collect diagnostics from the supplied handle.
    void setError(const std::string& message, SQLSMALLINT handleType, SQLHANDLE handle)
    {
        lastError_.message = message;
        lastError_.diagnostics = collectDiagnostics(handleType, handle);
    }

    // Allocate reusable parameter-array side buffers for one prepared table.
    void initializeStatementBuffers(StatementInfo& info,
                                    const TableSchema& table,
                                    std::size_t batchSizeRows)
    {
        info.capacityRows = batchSizeRows;
        info.parametersBound = false;
        info.boundBatch = nullptr;
        info.processedCount = 0;
        info.timestampIndicators.assign(batchSizeRows, kNotNullIndicator);
        info.rowStatuses.assign(batchSizeRows, static_cast<SQLUSMALLINT>(SQL_PARAM_SUCCESS));
        info.payloadColumns.clear();
        info.payloadColumns.resize(table.expandedColumns.size());

        for (std::size_t columnIndex = 0; columnIndex < table.expandedColumns.size(); ++columnIndex)
        {
            BoundColumn& column = info.payloadColumns[columnIndex];
            column.datatype = table.expandedColumns[columnIndex].datatype;
            column.indicators.assign(batchSizeRows, kNotNullIndicator);
            if (column.datatype == DataType::UInt64)
            {
                column.numericValues.resize(batchSizeRows);
            }
        }
    }

    // Read all ODBC diagnostic records for a failed handle operation.
    std::vector<OdbcDiagnostic> collectDiagnostics(SQLSMALLINT handleType, SQLHANDLE handle) const
    {
        std::vector<OdbcDiagnostic> diagnostics;

        for (SQLSMALLINT record = 1;; ++record)
        {
            SQLCHAR state[6] = {};
            SQLINTEGER nativeError = 0;
            SQLCHAR message[SQL_MAX_MESSAGE_LENGTH] = {};
            SQLSMALLINT messageLength = 0;

            SQLRETURN result = SQLGetDiagRecA(handleType,
                                              handle,
                                              record,
                                              state,
                                              &nativeError,
                                              message,
                                              static_cast<SQLSMALLINT>(sizeof(message)),
                                              &messageLength);
            if (result == SQL_NO_DATA)
            {
                break;
            }

            if (!isOdbcSuccess(result))
            {
                break;
            }

            OdbcDiagnostic diagnostic;
            diagnostic.sqlState = reinterpret_cast<const char*>(state);
            diagnostic.nativeError = static_cast<int>(nativeError);
            diagnostic.message = reinterpret_cast<const char*>(message);
            diagnostics.push_back(diagnostic);
        }

        return diagnostics;
    }

    // Execute one direct SQL command and collect statement diagnostics on failure.
    bool executeDirect(const std::string& sql, const std::string& context)
    {
        OdbcHandle statement(SQL_HANDLE_STMT);
        if (!statement.allocate(connection_.get()))
        {
            setError("Unable to allocate statement for " + context + ".", SQL_HANDLE_DBC, connection_.get());
            return false;
        }

        SQLRETURN result = SQLExecDirectA(statement.get(),
                                          reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql.c_str())),
                                          SQL_NTS);
        if (!isOdbcSuccess(result))
        {
            setError("SQL execution failed while " + context + ".", SQL_HANDLE_STMT, statement.get());
            return false;
        }

        return true;
    }

    // Execute a scalar integer SQL query and report a clear backend error on failure.
    bool executeIntegerScalar(const std::string& sql,
                              const std::string& context,
                              int& value)
    {
        OdbcHandle statement(SQL_HANDLE_STMT);
        if (!statement.allocate(connection_.get()))
        {
            setError("Unable to allocate statement for " + context + ".", SQL_HANDLE_DBC, connection_.get());
            return false;
        }

        SQLRETURN result = SQLExecDirectA(statement.get(),
                                          reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql.c_str())),
                                          SQL_NTS);
        if (!isOdbcSuccess(result))
        {
            setError("SQL execution failed while " + context + ".", SQL_HANDLE_STMT, statement.get());
            return false;
        }

        result = SQLFetch(statement.get());
        if (result == SQL_NO_DATA)
        {
            setError("SQL query returned no rows while " + context + ".");
            return false;
        }

        if (!isOdbcSuccess(result))
        {
            setError("SQL fetch failed while " + context + ".", SQL_HANDLE_STMT, statement.get());
            return false;
        }

        SQLLEN indicator = 0;
        result = SQLGetData(statement.get(), 1, SQL_C_SLONG, &value, sizeof(value), &indicator);
        if (!isOdbcSuccess(result) || indicator == SQL_NULL_DATA)
        {
            setError("SQL scalar read failed while " + context + ".", SQL_HANDLE_STMT, statement.get());
            return false;
        }

        return true;
    }

    // Check whether the target user table already exists in SQL Server.
    bool tableExists(const std::string& sqlSchemaName,
                     const std::string& tableName,
                     bool& exists)
    {
        const std::string qualifiedName = qualifiedTableName(sqlSchemaName, tableName);
        const std::string sql =
            "SELECT CASE WHEN OBJECT_ID(" + quoteSqlString(qualifiedName) + ", N'U') IS NULL THEN 0 ELSE 1 END;";
        int result = 0;
        if (!executeIntegerScalar(sql, "checking table existence for '" + tableName + "'", result))
        {
            return false;
        }

        exists = result != 0;
        return true;
    }

    // Drop an existing table if one is present.
    bool dropExistingTable(const std::string& sqlSchemaName, const std::string& tableName)
    {
        const std::string qualifiedName = qualifiedTableName(sqlSchemaName, tableName);
        const std::string sql =
            "IF OBJECT_ID(" + quoteSqlString(qualifiedName) + ", N'U') IS NOT NULL DROP TABLE " + qualifiedName + ";";
        return executeDirect(sql, "dropping existing table '" + tableName + "'");
    }

    // Rename an existing table with a timestamp suffix, appending a number on collision.
    bool renameExistingTable(const std::string& sqlSchemaName,
                             const std::string& tableName,
                             const std::string& suffix)
    {
        const std::string oldQualifiedName = qualifiedTableName(sqlSchemaName, tableName);
        const std::string baseNewName = tableName + "_" + suffix;
        std::string sql =
            "IF OBJECT_ID(" + quoteSqlString(oldQualifiedName) + ", N'U') IS NOT NULL\n"
            "BEGIN\n"
            "    DECLARE @newName sysname = N'" + baseNewName + "';\n"
            "    DECLARE @candidate sysname = @newName;\n"
            "    DECLARE @i int = 1;\n"
            "    WHILE OBJECT_ID(" + quoteSqlString(sqlSchemaName + ".") + " + QUOTENAME(@candidate), N'U') IS NOT NULL\n"
            "    BEGIN\n"
            "        SET @candidate = @newName + N'_' + CONVERT(nvarchar(12), @i);\n"
            "        SET @i = @i + 1;\n"
            "    END;\n"
            "    EXEC sp_rename " + quoteSqlString(sqlSchemaName + "." + tableName) + ", @candidate;\n"
            "END;";
        return executeDirect(sql, "renaming existing table '" + tableName + "'");
    }

    // Load SQL Server column metadata in ordinal order for schema compatibility checks.
    bool loadExistingColumns(const std::string& sqlSchemaName,
                             const std::string& tableName,
                             std::vector<ExistingColumnInfo>& columns)
    {
        columns.clear();

        OdbcHandle statement(SQL_HANDLE_STMT);
        if (!statement.allocate(connection_.get()))
        {
            setError("Unable to allocate statement for loading metadata for table '" + tableName + "'.", SQL_HANDLE_DBC, connection_.get());
            return false;
        }

        const std::string sql =
            "SELECT COLUMN_NAME, DATA_TYPE, "
            "COALESCE(CAST(NUMERIC_PRECISION AS int), 0), "
            "COALESCE(CAST(NUMERIC_SCALE AS int), 0), "
            "IS_NULLABLE "
            "FROM INFORMATION_SCHEMA.COLUMNS "
            "WHERE TABLE_SCHEMA = " + quoteSqlString(sqlSchemaName) +
            " AND TABLE_NAME = " + quoteSqlString(tableName) +
            " ORDER BY ORDINAL_POSITION;";

        SQLRETURN result = SQLExecDirectA(statement.get(),
                                          reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql.c_str())),
                                          SQL_NTS);
        if (!isOdbcSuccess(result))
        {
            setError("SQL execution failed while loading metadata for table '" + tableName + "'.", SQL_HANDLE_STMT, statement.get());
            return false;
        }

        while ((result = SQLFetch(statement.get())) != SQL_NO_DATA)
        {
            if (!isOdbcSuccess(result))
            {
                setError("SQL fetch failed while loading metadata for table '" + tableName + "'.", SQL_HANDLE_STMT, statement.get());
                return false;
            }

            char columnName[256] = {};
            char dataType[128] = {};
            char isNullable[8] = {};
            SQLINTEGER precision = 0;
            SQLINTEGER scale = 0;
            SQLLEN indicator = 0;

            if (!readTextColumn(statement.get(), 1, columnName, sizeof(columnName), indicator, tableName) ||
                !readTextColumn(statement.get(), 2, dataType, sizeof(dataType), indicator, tableName) ||
                !readIntegerColumn(statement.get(), 3, precision, tableName) ||
                !readIntegerColumn(statement.get(), 4, scale, tableName) ||
                !readTextColumn(statement.get(), 5, isNullable, sizeof(isNullable), indicator, tableName))
            {
                return false;
            }

            ExistingColumnInfo column;
            column.name = columnName;
            column.dataType = lowerCopy(dataType);
            column.numericPrecision = static_cast<int>(precision);
            column.numericScale = static_cast<int>(scale);
            column.nullable = std::string(isNullable) == "YES";
            columns.push_back(column);
        }

        return true;
    }

    // Read one text metadata column into a fixed caller-provided buffer.
    bool readTextColumn(SQLHSTMT statement,
                        SQLUSMALLINT column,
                        char* buffer,
                        SQLLEN bufferSize,
                        SQLLEN& indicator,
                        const std::string& tableName)
    {
        SQLRETURN result = SQLGetData(statement, column, SQL_C_CHAR, buffer, bufferSize, &indicator);
        if (!isOdbcSuccess(result) || indicator == SQL_NULL_DATA)
        {
            setError("SQL metadata read failed for table '" + tableName + "'.", SQL_HANDLE_STMT, statement);
            return false;
        }

        return true;
    }

    // Read one integer metadata column into the caller-provided output value.
    bool readIntegerColumn(SQLHSTMT statement,
                           SQLUSMALLINT column,
                           SQLINTEGER& value,
                           const std::string& tableName)
    {
        SQLLEN indicator = 0;
        SQLRETURN result = SQLGetData(statement, column, SQL_C_SLONG, &value, sizeof(value), &indicator);
        if (!isOdbcSuccess(result) || indicator == SQL_NULL_DATA)
        {
            setError("SQL numeric metadata read failed for table '" + tableName + "'.", SQL_HANDLE_STMT, statement);
            return false;
        }

        return true;
    }

    // Verify an existing SQL table exactly matches the logger-generated schema.
    bool validateExistingTable(const std::string& sqlSchemaName, const TableSchema& table)
    {
        std::vector<ExistingColumnInfo> columns;
        if (!loadExistingColumns(sqlSchemaName, table.tableName, columns))
        {
            return false;
        }

        const std::size_t expectedColumnCount = table.expandedColumns.size() + 1;
        if (columns.size() != expectedColumnCount)
        {
            setError("Existing table '" + table.tableName + "' has " + std::to_string(columns.size()) +
                     " columns, but the CSV schema expects " + std::to_string(expectedColumnCount) + ".");
            return false;
        }

        if (!matchesTimestampColumn(columns[0]))
        {
            setError("Existing table '" + table.tableName + "' does not have a non-null BIGINT timestamp_ms first column.");
            return false;
        }

        for (std::size_t i = 0; i < table.expandedColumns.size(); ++i)
        {
            const ExistingColumnInfo& actual = columns[i + 1];
            const DataLoggerCore::ExpandedColumnSchema& expected = table.expandedColumns[i];
            if (actual.name != expected.sqlName || actual.nullable || !matchesDataColumn(actual, expected.datatype))
            {
                setError("Existing table '" + table.tableName + "' column '" + actual.name +
                         "' does not match CSV column '" + expected.sqlName + "'.");
                return false;
            }
        }

        return true;
    }

    // Check the generated timestamp column contract for continue-current-table mode.
    bool matchesTimestampColumn(const ExistingColumnInfo& column) const
    {
        return column.name == "timestamp_ms" && column.dataType == "bigint" && !column.nullable;
    }

    // Check one existing SQL column against the storage type generated for a CSV field.
    bool matchesDataColumn(const ExistingColumnInfo& column, DataType datatype) const
    {
        switch (datatype)
        {
        case DataType::Int8:
        case DataType::Int16:
            return column.dataType == "smallint";
        case DataType::UInt8:
            return column.dataType == "tinyint";
        case DataType::UInt16:
        case DataType::Int32:
            return column.dataType == "int";
        case DataType::UInt32:
        case DataType::Int64:
            return column.dataType == "bigint";
        case DataType::UInt64:
            return column.dataType == "decimal" && column.numericPrecision == 20 && column.numericScale == 0;
        case DataType::Float:
            return column.dataType == "real";
        case DataType::Double:
            return column.dataType == "float" && column.numericPrecision == 53;
        }

        return false;
    }

    // Create one SQL Server table from a validated table schema.
    bool createTable(const std::string& sqlSchemaName, const TableSchema& table)
    {
        std::ostringstream sql;
        sql << "CREATE TABLE " << qualifiedTableName(sqlSchemaName, table.tableName) << "\n"
            << "(\n"
            << "    [timestamp_ms] BIGINT NOT NULL";

        for (const auto& column : table.expandedColumns)
        {
            sql << ",\n    " << quoteIdentifier(column.sqlName) << " " << sqlTypeFor(column.datatype) << " NOT NULL";
        }

        sql << "\n);";
        return executeDirect(sql.str(), "creating table '" + table.tableName + "'");
    }

    // Create the Grafana/time-series timestamp index for one table.
    bool createTimestampIndex(const std::string& sqlSchemaName, const TableSchema& table)
    {
        const std::string indexName = "IX_" + table.tableName + "_timestamp_ms";
        const std::string sql =
            "IF NOT EXISTS (SELECT 1 FROM sys.indexes WHERE name = " + quoteSqlString(indexName) +
            " AND object_id = OBJECT_ID(" + quoteSqlString(qualifiedTableName(sqlSchemaName, table.tableName)) + ", N'U'))\n"
            "BEGIN\n"
            "    CREATE INDEX " + quoteIdentifier(indexName) + " ON " +
            qualifiedTableName(sqlSchemaName, table.tableName) + " ([timestamp_ms]);\n"
            "END;";
        return executeDirect(sql, "creating timestamp index for table '" + table.tableName + "'");
    }

    // Create an optional nonclustered columnstore index for analytical scans.
    bool createColumnstoreIndexIfRequested(const std::string& sqlSchemaName,
                                           const TableSchema& table,
                                           SqlServerIndexMode indexMode)
    {
        if (indexMode == SqlServerIndexMode::RowstoreTimestampOnly)
        {
            return true;
        }

        if (indexMode != SqlServerIndexMode::RowstoreWithNonclusteredColumnstore)
        {
            setError("Unsupported SQL Server index mode for table '" + table.tableName + "'.");
            return false;
        }

        const std::string indexName = "NCCI_" + table.tableName + "_telemetry";
        std::ostringstream columns;
        columns << "[timestamp_ms]";
        for (const auto& column : table.expandedColumns)
        {
            columns << ", " << quoteIdentifier(column.sqlName);
        }

        const std::string sql =
            "IF NOT EXISTS (SELECT 1 FROM sys.indexes WHERE name = " + quoteSqlString(indexName) +
            " AND object_id = OBJECT_ID(" + quoteSqlString(qualifiedTableName(sqlSchemaName, table.tableName)) + ", N'U'))\n"
            "BEGIN\n"
            "    CREATE NONCLUSTERED COLUMNSTORE INDEX " + quoteIdentifier(indexName) + " ON " +
            qualifiedTableName(sqlSchemaName, table.tableName) + " (" + columns.str() + ");\n"
            "END;";
        return executeDirect(sql, "creating columnstore index for table '" + table.tableName + "'");
    }

    // Build one parameterized INSERT statement for a validated table schema.
    std::string buildInsertSql(const std::string& sqlSchemaName, const TableSchema& table) const
    {
        std::ostringstream columns;
        std::ostringstream values;

        columns << "[timestamp_ms]";
        values << "?";

        for (const auto& column : table.expandedColumns)
        {
            columns << ", " << quoteIdentifier(column.sqlName);
            values << ", ?";
        }

        std::ostringstream sql;
        sql << "INSERT INTO " << qualifiedTableName(sqlSchemaName, table.tableName)
            << " (" << columns.str() << ") VALUES (" << values.str() << ");";
        return sql.str();
    }

    // Ensure the typed batch matches the prepared table and fixed capacity.
    bool validateBatch(const TableSchema& table, const ColumnBatch& batch, const StatementInfo& info)
    {
        if (batch.columns.size() != table.expandedColumns.size())
        {
            setError("Column batch for table '" + table.tableName + "' does not match the expanded schema width.");
            return false;
        }

        if (batch.rowCount > batch.rowCapacity || batch.rowCount > info.capacityRows)
        {
            setError("Column batch for table '" + table.tableName + "' exceeds the prepared batch capacity.");
            return false;
        }

        if (batch.timestamps.size() < batch.rowCapacity)
        {
            setError("Timestamp storage for table '" + table.tableName + "' is smaller than the batch capacity.");
            return false;
        }

        for (std::size_t columnIndex = 0; columnIndex < batch.columns.size(); ++columnIndex)
        {
            const ColumnStorage& column = batch.columns[columnIndex];
            if (column.datatype != table.expandedColumns[columnIndex].datatype ||
                DataLoggerCore::columnStorageSize(column) < batch.rowCapacity)
            {
                setError("Column storage for '" + table.expandedColumns[columnIndex].sqlName +
                         "' in table '" + table.tableName + "' is not initialized correctly.");
                return false;
            }
        }

        return true;
    }

    // Bind timestamp plus payload arrays once to the stable preallocated batch storage.
    bool ensureParametersBound(SQLHSTMT statementHandle,
                               const TableSchema& table,
                               const ColumnBatch& batch,
                               StatementInfo& info)
    {
        if (info.parametersBound && info.boundBatch == &batch)
        {
            return true;
        }

        SQLFreeStmt(statementHandle, SQL_RESET_PARAMS);
        SQLRETURN result = SQLBindParameter(statementHandle,
                                            1,
                                            SQL_PARAM_INPUT,
                                            SQL_C_SBIGINT,
                                            SQL_BIGINT,
                                            0,
                                            0,
                                            const_cast<std::int64_t*>(batch.timestamps.data()),
                                            sizeof(std::int64_t),
                                            info.timestampIndicators.data());
        if (!isOdbcSuccess(result))
        {
            setError("Unable to bind timestamp parameter for table '" + table.tableName + "'.", SQL_HANDLE_STMT, statementHandle);
            return false;
        }

        for (std::size_t columnIndex = 0; columnIndex < info.payloadColumns.size(); ++columnIndex)
        {
            BoundColumn& column = info.payloadColumns[columnIndex];
            const ColumnStorage& sourceColumn = batch.columns[columnIndex];
            result = SQLBindParameter(statementHandle,
                                      static_cast<SQLUSMALLINT>(columnIndex + 2),
                                      SQL_PARAM_INPUT,
                                      cTypeFor(column.datatype),
                                      sqlParameterTypeFor(column.datatype),
                                      columnSizeFor(column.datatype),
                                      0,
                                      dataPointerFor(sourceColumn, column),
                                      bufferLengthFor(column.datatype),
                                      column.indicators.data());
            if (!isOdbcSuccess(result))
            {
                setError("Unable to bind payload parameter '" + table.expandedColumns[columnIndex].sqlName +
                             "' for table '" + table.tableName + "'.",
                         SQL_HANDLE_STMT,
                         statementHandle);
                return false;
            }

            if (column.datatype == DataType::UInt64 &&
                !configureNumericDescriptor(statementHandle, static_cast<SQLSMALLINT>(columnIndex + 2), table, columnIndex))
            {
                return false;
            }
        }

        info.parametersBound = true;
        info.boundBatch = &batch;
        return true;
    }

    // Set per-execution ODBC attributes and refresh reusable conversion buffers.
    bool prepareBatchExecution(SQLHSTMT statementHandle,
                               const TableSchema& table,
                               const ColumnBatch& batch,
                               StatementInfo& info)
    {
        const SQLULEN rowCount = static_cast<SQLULEN>(batch.rowCount);
        info.processedCount = 0;
        std::fill(info.rowStatuses.begin(),
                  info.rowStatuses.begin() + batch.rowCount,
                  static_cast<SQLUSMALLINT>(SQL_PARAM_SUCCESS));

        for (std::size_t columnIndex = 0; columnIndex < info.payloadColumns.size(); ++columnIndex)
        {
            if (info.payloadColumns[columnIndex].datatype == DataType::UInt64)
            {
                fillNumericValues(info.payloadColumns[columnIndex], batch.columns[columnIndex], batch.rowCount);
            }
        }

        if (!setStatementAttribute(statementHandle, SQL_ATTR_PARAM_BIND_TYPE, SQL_PARAM_BIND_BY_COLUMN) ||
            !setStatementAttribute(statementHandle, SQL_ATTR_PARAMSET_SIZE, rowCount) ||
            !setStatementPointerAttribute(statementHandle, SQL_ATTR_PARAMS_PROCESSED_PTR, &info.processedCount) ||
            !setStatementPointerAttribute(statementHandle, SQL_ATTR_PARAM_STATUS_PTR, info.rowStatuses.data()))
        {
            setError("Unable to configure ODBC parameter array attributes for table '" + table.tableName + "'.",
                     SQL_HANDLE_STMT,
                     statementHandle);
            return false;
        }

        return true;
    }

    // Set precision and scale metadata required for SQL_C_NUMERIC parameter binding.
    bool configureNumericDescriptor(SQLHSTMT statementHandle,
                                    SQLSMALLINT parameterNumber,
                                    const TableSchema& table,
                                    std::size_t columnIndex)
    {
        SQLHDESC descriptor = SQL_NULL_HDESC;
        SQLRETURN result = SQLGetStmtAttr(statementHandle, SQL_ATTR_APP_PARAM_DESC, &descriptor, 0, nullptr);
        if (!isOdbcSuccess(result))
        {
            setError("Unable to read numeric parameter descriptor for table '" + table.tableName + "'.",
                     SQL_HANDLE_STMT,
                     statementHandle);
            return false;
        }

        result = SQLSetDescField(descriptor,
                                 parameterNumber,
                                 SQL_DESC_PRECISION,
                                 reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(20)),
                                 0);
        if (!isOdbcSuccess(result))
        {
            setError("Unable to set numeric precision for column '" + table.expandedColumns[columnIndex].sqlName + "'.",
                     SQL_HANDLE_STMT,
                     statementHandle);
            return false;
        }

        result = SQLSetDescField(descriptor,
                                 parameterNumber,
                                 SQL_DESC_SCALE,
                                 reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(0)),
                                 0);
        if (!isOdbcSuccess(result))
        {
            setError("Unable to set numeric scale for column '" + table.expandedColumns[columnIndex].sqlName + "'.",
                     SQL_HANDLE_STMT,
                     statementHandle);
            return false;
        }

        return true;
    }

    // Set an ODBC statement attribute represented as an integer-sized value.
    bool setStatementAttribute(SQLHSTMT statementHandle, SQLINTEGER attribute, SQLULEN value)
    {
        SQLRETURN result = SQLSetStmtAttr(statementHandle, attribute, reinterpret_cast<SQLPOINTER>(value), 0);
        return isOdbcSuccess(result);
    }

    // Set an ODBC statement attribute represented by a real pointer.
    bool setStatementPointerAttribute(SQLHSTMT statementHandle, SQLINTEGER attribute, SQLPOINTER value)
    {
        SQLRETURN result = SQLSetStmtAttr(statementHandle, attribute, value, 0);
        return isOdbcSuccess(result);
    }

    // Disable autocommit and remember the previous connection setting.
    bool beginTransaction(SQLUINTEGER& previousAutocommit)
    {
        SQLINTEGER stringLength = 0;
        SQLRETURN result = SQLGetConnectAttr(connection_.get(),
                                             SQL_ATTR_AUTOCOMMIT,
                                             &previousAutocommit,
                                             0,
                                             &stringLength);
        if (!isOdbcSuccess(result))
        {
            setError("Unable to read ODBC autocommit setting.", SQL_HANDLE_DBC, connection_.get());
            return false;
        }

        result = SQLSetConnectAttr(connection_.get(), SQL_ATTR_AUTOCOMMIT, reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_OFF), 0);
        if (!isOdbcSuccess(result))
        {
            setError("Unable to disable ODBC autocommit for batch transaction.", SQL_HANDLE_DBC, connection_.get());
            return false;
        }

        return true;
    }

    // Restore the connection autocommit setting after a batch transaction.
    bool restoreAutocommit(SQLUINTEGER previousAutocommit)
    {
        SQLRETURN result = SQLSetConnectAttr(connection_.get(),
                                             SQL_ATTR_AUTOCOMMIT,
                                             reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(previousAutocommit)),
                                             0);
        if (!isOdbcSuccess(result))
        {
            setError("Unable to restore ODBC autocommit setting.", SQL_HANDLE_DBC, connection_.get());
            return false;
        }

        return true;
    }

    OdbcHandle environment_;
    OdbcHandle connection_;
    bool connected_ = false;
    BackendError lastError_;
    std::map<std::string, std::unique_ptr<StatementInfo>> statements_;
};

// Construct the concrete SQL Server backend and its hidden ODBC state.
SqlServerOdbcBackend::SqlServerOdbcBackend()
    : impl_(std::make_unique<Impl>())
{
}

// Release prepared statements, connection, and environment handles through RAII.
SqlServerOdbcBackend::~SqlServerOdbcBackend() = default;

// Open the SQL Server ODBC connection using the caller-provided connection string.
bool SqlServerOdbcBackend::connect(const std::string& connectionString)
{
    return impl_->connect(connectionString);
}

// Apply existing-table policy, prepare SQL tables, and create timestamp indexes.
bool SqlServerOdbcBackend::initializeTables(const SchemaRegistry& registry,
                                            const std::string& sqlSchemaName,
                                            ExistingTablePolicy policy,
                                            SqlServerIndexMode indexMode)
{
    return impl_->initializeTables(registry, sqlSchemaName, policy, indexMode);
}

// Prepare one reusable parameterized INSERT statement and reusable buffers per table.
bool SqlServerOdbcBackend::prepareInsertStatements(const SchemaRegistry& registry,
                                                   const std::string& sqlSchemaName,
                                                   std::size_t batchSizeRows)
{
    return impl_->prepareInsertStatements(registry, sqlSchemaName, batchSizeRows);
}

// Insert one decoded column batch using ODBC column-wise parameter arrays.
bool SqlServerOdbcBackend::insertBatch(const TableSchema& table, const ColumnBatch& batch)
{
    return impl_->insertBatch(table, batch);
}

// Return the most recent backend error and collected ODBC diagnostics.
BackendError SqlServerOdbcBackend::lastError() const
{
    return impl_->lastError();
}
}
