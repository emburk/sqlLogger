#include "SqlServerBackend/SqlServerOdbcBackend.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <sql.h>
#include <sqlext.h>

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
using DataLoggerCore::DataType;
using DataLoggerCore::DecodedRow;
using DataLoggerCore::ExistingTablePolicy;
using DataLoggerCore::FieldValue;
using DataLoggerCore::OdbcDiagnostic;
using DataLoggerCore::SchemaRegistry;
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

// Return a typed value from the row variant; schema validation keeps this order correct.
template<typename T>
T fieldAs(const FieldValue& value)
{
    return std::get<T>(value);
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

struct StatementInfo
{
    OdbcHandle statement;

    // Statements own a prepared ODBC statement handle for one table.
    StatementInfo()
        : statement(SQL_HANDLE_STMT)
    {
    }
};

struct BoundColumn
{
    DataType datatype = DataType::Int8;
    std::vector<SQLLEN> indicators;
    std::vector<std::int16_t> int16Values;
    std::vector<std::uint8_t> uint8Values;
    std::vector<std::int32_t> int32Values;
    std::vector<std::int64_t> int64Values;
    std::vector<float> floatValues;
    std::vector<double> doubleValues;
    std::vector<SQL_NUMERIC_STRUCT> numericValues;
};

// Append one typed payload value into a column-wise ODBC buffer.
void appendPayloadValue(BoundColumn& column, const FieldValue& value)
{
    column.indicators.push_back(kNotNullIndicator);

    switch (column.datatype)
    {
    case DataType::Int8:
        column.int16Values.push_back(static_cast<std::int16_t>(fieldAs<std::int8_t>(value)));
        break;
    case DataType::UInt8:
        column.uint8Values.push_back(fieldAs<std::uint8_t>(value));
        break;
    case DataType::Int16:
        column.int16Values.push_back(fieldAs<std::int16_t>(value));
        break;
    case DataType::UInt16:
        column.int32Values.push_back(static_cast<std::int32_t>(fieldAs<std::uint16_t>(value)));
        break;
    case DataType::Int32:
        column.int32Values.push_back(fieldAs<std::int32_t>(value));
        break;
    case DataType::UInt32:
        column.int64Values.push_back(static_cast<std::int64_t>(fieldAs<std::uint32_t>(value)));
        break;
    case DataType::Int64:
        column.int64Values.push_back(fieldAs<std::int64_t>(value));
        break;
    case DataType::UInt64:
        column.numericValues.push_back(toNumericStruct(fieldAs<std::uint64_t>(value)));
        break;
    case DataType::Float:
        column.floatValues.push_back(fieldAs<float>(value));
        break;
    case DataType::Double:
        column.doubleValues.push_back(fieldAs<double>(value));
        break;
    }
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

// Return the active data pointer for a populated bound column.
SQLPOINTER dataPointerFor(BoundColumn& column)
{
    switch (column.datatype)
    {
    case DataType::Int8:
    case DataType::Int16:
        return column.int16Values.data();
    case DataType::UInt8:
        return column.uint8Values.data();
    case DataType::UInt16:
    case DataType::Int32:
        return column.int32Values.data();
    case DataType::UInt32:
    case DataType::Int64:
        return column.int64Values.data();
    case DataType::UInt64:
        return column.numericValues.data();
    case DataType::Float:
        return column.floatValues.data();
    case DataType::Double:
        return column.doubleValues.data();
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

    // Recreate SQL tables from the schema registry and add timestamp indexes.
    bool initializeTables(const SchemaRegistry& registry,
                          const std::string& sqlSchemaName,
                          ExistingTablePolicy policy)
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
            else if (!renameExistingTable(sqlSchemaName, table.tableName, suffix))
            {
                return false;
            }

            if (!createTable(sqlSchemaName, table) || !createTimestampIndex(sqlSchemaName, table))
            {
                return false;
            }
        }

        return true;
    }

    // Prepare and retain one INSERT statement handle for every table schema.
    bool prepareInsertStatements(const SchemaRegistry& registry, const std::string& sqlSchemaName)
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

            statements_[table.tableName] = std::move(info);
        }

        return true;
    }

    // Bind column-wise parameter arrays and execute the batch in one transaction.
    bool insertBatch(const TableSchema& table, const std::vector<DecodedRow>& rows)
    {
        clearError();

        if (rows.empty())
        {
            return true;
        }

        auto statement = statements_.find(table.tableName);
        if (statement == statements_.end())
        {
            setError("No prepared insert statement exists for table '" + table.tableName + "'.");
            return false;
        }

        if (!validateRows(table, rows))
        {
            return false;
        }

        OdbcBatchBuffers buffers = buildBatchBuffers(table, rows);
        SQLHSTMT statementHandle = statement->second->statement.get();

        SQLFreeStmt(statementHandle, SQL_RESET_PARAMS);
        SQLULEN rowCount = static_cast<SQLULEN>(rows.size());
        SQLULEN processedCount = 0;
        std::vector<SQLUSMALLINT> rowStatuses(rows.size(), SQL_PARAM_SUCCESS);

        if (!setStatementAttribute(statementHandle, SQL_ATTR_PARAM_BIND_TYPE, SQL_PARAM_BIND_BY_COLUMN) ||
            !setStatementAttribute(statementHandle, SQL_ATTR_PARAMSET_SIZE, rowCount) ||
            !setStatementAttribute(statementHandle, SQL_ATTR_PARAMS_PROCESSED_PTR, reinterpret_cast<SQLULEN>(&processedCount)) ||
            !setStatementAttribute(statementHandle, SQL_ATTR_PARAM_STATUS_PTR, reinterpret_cast<SQLULEN>(rowStatuses.data())))
        {
            setError("Unable to configure ODBC parameter array attributes.", SQL_HANDLE_STMT, statementHandle);
            return false;
        }

        if (!bindBatchParameters(statementHandle, table, buffers))
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
    struct OdbcBatchBuffers
    {
        std::vector<std::int64_t> timestamps;
        std::vector<SQLLEN> timestampIndicators;
        std::vector<BoundColumn> payloadColumns;
    };

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
            "CREATE INDEX " + quoteIdentifier(indexName) + " ON " +
            qualifiedTableName(sqlSchemaName, table.tableName) + " ([timestamp_ms]);";
        return executeDirect(sql, "creating timestamp index for table '" + table.tableName + "'");
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

    // Ensure decoded rows match the expected expanded payload width.
    bool validateRows(const TableSchema& table, const std::vector<DecodedRow>& rows)
    {
        for (std::size_t i = 0; i < rows.size(); ++i)
        {
            if (rows[i].values.size() != table.expandedColumns.size())
            {
                setError("Decoded row " + std::to_string(i) + " for table '" + table.tableName +
                         "' does not match the expanded schema width.");
                return false;
            }
        }

        return true;
    }

    // Convert row-oriented decoded rows into column-wise ODBC parameter buffers.
    OdbcBatchBuffers buildBatchBuffers(const TableSchema& table, const std::vector<DecodedRow>& rows) const
    {
        OdbcBatchBuffers buffers;
        buffers.timestamps.reserve(rows.size());
        buffers.timestampIndicators.reserve(rows.size());
        buffers.payloadColumns.resize(table.expandedColumns.size());

        for (std::size_t columnIndex = 0; columnIndex < table.expandedColumns.size(); ++columnIndex)
        {
            buffers.payloadColumns[columnIndex].datatype = table.expandedColumns[columnIndex].datatype;
            buffers.payloadColumns[columnIndex].indicators.reserve(rows.size());
        }

        for (const DecodedRow& row : rows)
        {
            buffers.timestamps.push_back(row.timestampMs);
            buffers.timestampIndicators.push_back(kNotNullIndicator);

            for (std::size_t columnIndex = 0; columnIndex < row.values.size(); ++columnIndex)
            {
                appendPayloadValue(buffers.payloadColumns[columnIndex], row.values[columnIndex]);
            }
        }

        return buffers;
    }

    // Bind timestamp plus payload columns as one ODBC parameter array.
    bool bindBatchParameters(SQLHSTMT statementHandle, const TableSchema& table, OdbcBatchBuffers& buffers)
    {
        SQLRETURN result = SQLBindParameter(statementHandle,
                                            1,
                                            SQL_PARAM_INPUT,
                                            SQL_C_SBIGINT,
                                            SQL_BIGINT,
                                            0,
                                            0,
                                            buffers.timestamps.data(),
                                            sizeof(std::int64_t),
                                            buffers.timestampIndicators.data());
        if (!isOdbcSuccess(result))
        {
            setError("Unable to bind timestamp parameter for table '" + table.tableName + "'.", SQL_HANDLE_STMT, statementHandle);
            return false;
        }

        for (std::size_t columnIndex = 0; columnIndex < buffers.payloadColumns.size(); ++columnIndex)
        {
            BoundColumn& column = buffers.payloadColumns[columnIndex];
            result = SQLBindParameter(statementHandle,
                                      static_cast<SQLUSMALLINT>(columnIndex + 2),
                                      SQL_PARAM_INPUT,
                                      cTypeFor(column.datatype),
                                      sqlParameterTypeFor(column.datatype),
                                      columnSizeFor(column.datatype),
                                      0,
                                      dataPointerFor(column),
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

// Apply existing-table policy, create fresh tables, and create timestamp indexes.
bool SqlServerOdbcBackend::initializeTables(const SchemaRegistry& registry,
                                            const std::string& sqlSchemaName,
                                            ExistingTablePolicy policy)
{
    return impl_->initializeTables(registry, sqlSchemaName, policy);
}

// Prepare one reusable parameterized INSERT statement per table.
bool SqlServerOdbcBackend::prepareInsertStatements(const SchemaRegistry& registry, const std::string& sqlSchemaName)
{
    return impl_->prepareInsertStatements(registry, sqlSchemaName);
}

// Insert one decoded row batch using ODBC column-wise parameter arrays.
bool SqlServerOdbcBackend::insertBatch(const TableSchema& table, const std::vector<DecodedRow>& rows)
{
    return impl_->insertBatch(table, rows);
}

// Return the most recent backend error and collected ODBC diagnostics.
BackendError SqlServerOdbcBackend::lastError() const
{
    return impl_->lastError();
}
}
