#pragma once

#include "DataLogger/DataLogger.h"
#include "DataLogger/DataLoggerConfig.h"
#include "DataLogger/Error.h"
#include "DataLogger/IDBBackend.h"
#include "DataLogger/TableHandle.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace AsyncLogger
{
enum class AsyncOverflowPolicy
{
    DropNewest,
    DropOldest
};

enum class AsyncWorkerPriority
{
    Normal,
    BelowNormal,
    Lowest
};

enum class AsyncDataLoggerStatus
{
    Ok,
    Error,
    QueueFull,
    InvalidArgument,
    NotInitialized,
    AlreadyStarted,
    PayloadTooLarge
};

struct AsyncDataLoggerConfig
{
    std::size_t queueCapacity = 512;
    std::size_t maxPayloadBytes = 0;
    AsyncOverflowPolicy overflowPolicy = AsyncOverflowPolicy::DropNewest;
    AsyncWorkerPriority workerPriority = AsyncWorkerPriority::BelowNormal;
    bool flushOnStop = true;
    bool autoRegisterTablesOnStart = false;
};

struct AsyncDataLoggerStats
{
    std::uint64_t pushedSamples = 0;
    std::uint64_t droppedSamples = 0;
    std::uint64_t poppedSamples = 0;
    std::uint64_t autoWriteOps = 0;
    std::uint64_t writeOps = 0;
    std::uint64_t flushRequests = 0;
    std::uint64_t workerErrors = 0;
    std::uint64_t maxQueueDepth = 0;
    std::uint64_t currentQueueDepth = 0;
    std::uint64_t tryPushMaxNs = 0;
    std::uint64_t workerLoopMaxNs = 0;
    std::uint64_t flushMaxNs = 0;
};

class AsyncDataLogger
{
public:
    // Own the backend used by the wrapped DataLogger and async worker.
    explicit AsyncDataLogger(std::unique_ptr<DataLoggerCore::IDBBackend> backend);
    // Stop async work and release the wrapped synchronous logger.
    ~AsyncDataLogger();

    AsyncDataLogger(const AsyncDataLogger&) = delete;
    AsyncDataLogger& operator=(const AsyncDataLogger&) = delete;

    // Initialize the wrapped DataLogger and store async queue configuration.
    bool initialize(const DataLoggerCore::DataLoggerConfig& dataConfig,
                    const AsyncDataLoggerConfig& asyncConfig);

    // Resolve a table name to a reusable handle before async writes begin.
    bool registerTable(const std::string& tableName,
                       DataLoggerCore::TableHandle* outHandle);
    // Register every loaded table for tryAutoWrite calls.
    bool autoRegisterTables();

    // Start the worker thread that owns write and flush calls after startup.
    bool start();

    // Queue an automatic multi-table payload copy without blocking the caller.
    bool tryAutoWrite(std::int64_t timestampMs,
                      const void* structPtr,
                      std::size_t structSize) noexcept;

    // Queue one table-specific payload copy without blocking the caller.
    bool tryWrite(DataLoggerCore::TableHandle table,
                  std::int64_t timestampMs,
                  const void* structPtr,
                  std::size_t structSize) noexcept;

    // Queue an all-table flush request for the worker thread.
    bool requestFlush() noexcept;
    // Queue a table-specific flush request for the worker thread.
    bool requestFlush(DataLoggerCore::TableHandle table) noexcept;

    // Stop accepting work, drain queued operations, and join the worker.
    void stop();
    // Stop accepting work and synchronously flush the wrapped logger.
    bool stopAndFlush();

    // Return async counters collected by producer and worker paths.
    AsyncDataLoggerStats stats() const noexcept;
    // Return the last lightweight status without allocating in producer paths.
    AsyncDataLoggerStatus lastStatus() const noexcept;
    // Return the most recent async wrapper or wrapped logger error.
    const DataLoggerCore::DataLoggerError& lastError() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}
