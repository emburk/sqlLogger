#include "DataLogger/SchemaLoaderCsv.h"

#include "DataLogger/SchemaValidator.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace DataLoggerCore
{
namespace
{
struct CsvRecord
{
    std::vector<std::string> fields;
    std::size_t lineNumber = 0;
};

std::string trim(const std::string& text)
{
    std::size_t first = 0;
    while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first])))
    {
        ++first;
    }

    std::size_t last = text.size();
    while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1])))
    {
        --last;
    }

    return text.substr(first, last - first);
}

bool parseCsvLine(const std::string& line,
                  std::vector<std::string>& fields,
                  std::string& errorMessage)
{
    fields.clear();

    std::string field;
    bool inQuotes = false;

    for (std::size_t i = 0; i < line.size(); ++i)
    {
        const char character = line[i];
        if (inQuotes)
        {
            if (character == '"')
            {
                if (i + 1 < line.size() && line[i + 1] == '"')
                {
                    field.push_back('"');
                    ++i;
                }
                else
                {
                    inQuotes = false;
                }
            }
            else
            {
                field.push_back(character);
            }
        }
        else
        {
            if (character == ',')
            {
                fields.push_back(trim(field));
                field.clear();
            }
            else if (character == '"')
            {
                if (!trim(field).empty())
                {
                    errorMessage = "quote must appear at the start of a CSV field";
                    return false;
                }
                inQuotes = true;
            }
            else
            {
                field.push_back(character);
            }
        }
    }

    if (inQuotes)
    {
        errorMessage = "unterminated quoted CSV field";
        return false;
    }

    fields.push_back(trim(field));
    return true;
}

bool readCsvRecords(const std::filesystem::path& path,
                    std::vector<CsvRecord>& records,
                    DataLoggerError& error)
{
    std::ifstream input(path);
    if (!input)
    {
        error = { ErrorCode::SchemaLoadFailed, "Unable to open schema file '" + path.string() + "'." };
        return false;
    }

    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(input, line))
    {
        ++lineNumber;

        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }

        const std::string stripped = trim(line);
        if (stripped.empty() || stripped.front() == '#')
        {
            continue;
        }

        CsvRecord record;
        record.lineNumber = lineNumber;

        std::string csvError;
        if (!parseCsvLine(line, record.fields, csvError))
        {
            error = { ErrorCode::SchemaLoadFailed,
                      "CSV parse error in '" + path.string() + "' at line " + std::to_string(lineNumber) +
                          ": " + csvError + "." };
            return false;
        }

        records.push_back(record);
    }

    return true;
}

bool parseUnsignedSize(const std::string& text,
                       std::size_t& value)
{
    if (text.empty())
    {
        return false;
    }

    for (char character : text)
    {
        if (!std::isdigit(static_cast<unsigned char>(character)))
        {
            return false;
        }
    }

    std::istringstream stream(text);
    stream >> value;
    return !stream.fail() && stream.eof();
}

bool requireHeader(const std::map<std::string, std::size_t>& header,
                   const std::string& name,
                   const std::filesystem::path& path,
                   DataLoggerError& error)
{
    if (header.find(name) == header.end())
    {
        error = { ErrorCode::SchemaLoadFailed, "Schema file '" + path.string() + "' is missing required column '" + name + "'." };
        return false;
    }

    return true;
}

bool loadSchemaFile(const std::filesystem::path& path,
                    TableSchema& table,
                    DataLoggerError& error)
{
    std::vector<CsvRecord> records;
    if (!readCsvRecords(path, records, error))
    {
        return false;
    }

    if (records.empty())
    {
        error = { ErrorCode::SchemaLoadFailed, "Schema file '" + path.string() + "' is empty." };
        return false;
    }

    table = TableSchema{};
    table.tableName = path.stem().string();

    const CsvRecord& headerRecord = records.front();
    std::map<std::string, std::size_t> header;
    for (std::size_t i = 0; i < headerRecord.fields.size(); ++i)
    {
        const std::string name = headerRecord.fields[i];
        if (header.find(name) != header.end())
        {
            error = { ErrorCode::SchemaLoadFailed, "Duplicate CSV header '" + name + "' in schema file '" + path.string() + "'." };
            return false;
        }

        header[name] = i;
    }

    const std::vector<std::string> allowedHeaders = {
        "column_name",
        "offset",
        "datatype",
        "size",
        "length",
        "unit",
        "description"
    };

    for (const auto& entry : header)
    {
        if (std::find(allowedHeaders.begin(), allowedHeaders.end(), entry.first) == allowedHeaders.end())
        {
            error = { ErrorCode::SchemaLoadFailed,
                      "Unknown CSV header '" + entry.first + "' in schema file '" + path.string() + "'." };
            return false;
        }
    }

    if (!requireHeader(header, "column_name", path, error) ||
        !requireHeader(header, "offset", path, error) ||
        !requireHeader(header, "datatype", path, error) ||
        !requireHeader(header, "size", path, error) ||
        !requireHeader(header, "length", path, error))
    {
        return false;
    }

    for (std::size_t i = 1; i < records.size(); ++i)
    {
        const CsvRecord& record = records[i];
        if (record.fields.size() != headerRecord.fields.size())
        {
            error = { ErrorCode::SchemaLoadFailed,
                      "Schema file '" + path.string() + "' line " + std::to_string(record.lineNumber) +
                          " has " + std::to_string(record.fields.size()) + " fields, expected " +
                          std::to_string(headerRecord.fields.size()) + "." };
            return false;
        }

        ColumnSchema column;
        column.baseName = record.fields[header["column_name"]];

        if (!parseUnsignedSize(record.fields[header["offset"]], column.offset))
        {
            error = { ErrorCode::SchemaLoadFailed,
                      "Invalid offset for column '" + column.baseName + "' in schema file '" + path.string() +
                          "' line " + std::to_string(record.lineNumber) + "." };
            return false;
        }

        if (!parseDataType(record.fields[header["datatype"]], column.datatype))
        {
            error = { ErrorCode::SchemaLoadFailed,
                      "Unsupported datatype '" + record.fields[header["datatype"]] + "' for column '" +
                          column.baseName + "' in schema file '" + path.string() + "' line " +
                          std::to_string(record.lineNumber) + "." };
            return false;
        }

        if (!parseUnsignedSize(record.fields[header["size"]], column.size))
        {
            error = { ErrorCode::SchemaLoadFailed,
                      "Invalid size for column '" + column.baseName + "' in schema file '" + path.string() +
                          "' line " + std::to_string(record.lineNumber) + "." };
            return false;
        }

        if (!parseUnsignedSize(record.fields[header["length"]], column.length))
        {
            error = { ErrorCode::SchemaLoadFailed,
                      "Invalid length for column '" + column.baseName + "' in schema file '" + path.string() +
                          "' line " + std::to_string(record.lineNumber) + "." };
            return false;
        }

        const auto unit = header.find("unit");
        if (unit != header.end())
        {
            column.unit = record.fields[unit->second];
        }

        const auto description = header.find("description");
        if (description != header.end())
        {
            column.description = record.fields[description->second];
        }

        table.columns.push_back(column);
    }

    return true;
}
}

bool loadSchemaDirectory(const std::string& schemaDirectory,
                         SchemaRegistry& registry,
                         DataLoggerError& error)
{
    registry = SchemaRegistry{};

    const std::filesystem::path directory(schemaDirectory);
    std::error_code existsError;
    if (!std::filesystem::exists(directory, existsError) || existsError)
    {
        error = { ErrorCode::SchemaLoadFailed, "Schema directory '" + schemaDirectory + "' does not exist." };
        return false;
    }

    std::error_code directoryError;
    if (!std::filesystem::is_directory(directory, directoryError) || directoryError)
    {
        error = { ErrorCode::SchemaLoadFailed, "Schema path '" + schemaDirectory + "' is not a directory." };
        return false;
    }

    std::vector<std::filesystem::path> csvFiles;
    for (const auto& entry : std::filesystem::directory_iterator(directory))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".csv")
        {
            csvFiles.push_back(entry.path());
        }
    }

    std::sort(csvFiles.begin(), csvFiles.end());

    for (const std::filesystem::path& csvFile : csvFiles)
    {
        TableSchema table;
        if (!loadSchemaFile(csvFile, table, error))
        {
            return false;
        }

        registry.tables.push_back(table);
    }

    return validateAndExpandSchemaRegistry(registry, error);
}
}
