#include "AsyncLogger/AsyncDataLogger.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace AsyncLogger
{
namespace
{
using Clock = std::chrono::steady_clock;

enum class AsyncLogOperation
{
    AutoWrite,
    Write,
    FlushAll,
    FlushTable
};

enum class SlotState
{
    Empty,
    Full,
    Reading
};

struct AsyncLogSlot
{
    // Start every slot empty so initialization can wire payload storage directly.
    AsyncLogSlot()
        : state(SlotState::Empty)
    {
    }

    std::atomic<SlotState> state;
    AsyncLogOperation operation = AsyncLogOperation::AutoWrite;
    DataLoggerCore::TableHandle table = DataLoggerCore::TableHandle::invalid();
    std::int64_t timestampMs = 0;
    std::size_t payloadSize = 0;
    std::byte* payloadStorage = nullptr;
};

struct WorkerItem
{
    AsyncLogOperation operation = AsyncLogOperation::AutoWrite;
    DataLoggerCore::TableHandle table = DataLoggerCore::TableHandle::invalid();
    std::int64_t timestampMs = 0;
    std::size_t payloadSize = 0;
    const void* payload = nullptr;
};

struct AtomicStats
{
    std::atomic<std::uint64_t> pushedSamples{ 0 };
    std::atomic<std::uint64_t> droppedSamples{ 0 };
    std::atomic<std::uint64_t> poppedSamples{ 0 };
    std::atomic<std::uint64_t> autoWriteOps{ 0 };
    std::atomic<std::uint64_t> writeOps{ 0 };
    std::atomic<std::uint64_t> flushRequests{ 0 };
    std::atomic<std::uint64_t> workerErrors{ 0 };
    std::atomic<std::uint64_t> maxQueueDepth{ 0 };
    std::atomic<std::uint64_t> currentQueueDepth{ 0 };
    std::atomic<std::uint64_t> tryPushMaxNs{ 0 };
    std::atomic<std::uint64_t> workerLoopMaxNs{ 0 };
    std::atomic<std::uint64_t> flushMaxNs{ 0 };

    // Reset all counters before a new initialization becomes observable.
    void reset() noexcept
    {
        pushedSamples.store(0, std::memory_order_relaxed);
        droppedSamples.store(0, std::memory_order_relaxed);
        poppedSamples.store(0, std::memory_order_relaxed);
        autoWriteOps.store(0, std::memory_order_relaxed);
        writeOps.store(0, std::memory_order_relaxed);
        flushRequests.store(0, std::memory_order_relaxed);
        workerErrors.store(0, std::memory_order_relaxed);
        maxQueueDepth.store(0, std::memory_order_relaxed);
        currentQueueDepth.store(0, std::memory_order_relaxed);
        tryPushMaxNs.store(0, std::memory_order_relaxed);
        workerLoopMaxNs.store(0, std::memory_order_relaxed);
        flushMaxNs.store(0, std::memory_order_relaxed);
    }

    // Copy atomic counters into the plain public stats structure.
    AsyncDataLoggerStats snapshot() const noexcept
    {
        AsyncDataLoggerStats output;
        output.pushedSamples = pushedSamples.load(std::memory_order_relaxed);
        output.droppedSamples = droppedSamples.load(std::memory_order_relaxed);
        output.poppedSamples = poppedSamples.load(std::memory_order_relaxed);
        output.autoWriteOps = autoWriteOps.load(std::memory_order_relaxed);
        output.writeOps = writeOps.load(std::memory_order_relaxed);
        output.flushRequests = flushRequests.load(std::memory_order_relaxed);
        output.workerErrors = workerErrors.load(std::memory_order_relaxed);
        output.maxQueueDepth = maxQueueDepth.load(std::memory_order_relaxed);
        output.currentQueueDepth = currentQueueDepth.load(std::memory_order_relaxed);
        output.tryPushMaxNs = tryPushMaxNs.load(std::memory_order_relaxed);
        output.workerLoopMaxNs = workerLoopMaxNs.load(std::memory_order_relaxed);
        output.flushMaxNs = flushMaxNs.load(std::memory_order_relaxed);
        return output;
    }
};

// Check whether an operation carries a telemetry payload sample.
bool isDataOperation(AsyncLogOperation operation) noexcept
{
    return operation == AsyncLogOperation::AutoWrite || operation == AsyncLogOperation::Write;
}

// Return elapsed nanoseconds for lightweight max-duration counters.
std::uint64_t elapsedNanoseconds(Clock::time_point started) noexcept
{
    const auto elapsed = Clock::now() - started;
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
}

// Atomically keep the largest observed duration or depth value.
void storeMax(std::atomic<std::uint64_t>& target, std::uint64_t value) noexcept
{
    std::uint64_t current = target.load(std::memory_order_relaxed);
    while (value > current &&
           !target.compare_exchange_weak(current, value, std::memory_order_relaxed, std::memory_order_relaxed))
    {
    }
}

// Round queue capacity upward so ring indexes can use a mask.
bool nextPowerOfTwo(std::size_t requested, std::size_t& output) noexcept
{
    if (requested == 0)
    {
        return false;
    }

    std::size_t value = 1;
    while (value < requested)
    {
        if (value > (std::numeric_limits<std::size_t>::max)() / 2)
        {
            return false;
        }

        value *= 2;
    }

    output = value;
    return true;
}

// Convert the portable async priority enum to the allowed Windows priority value.
int nativeThreadPriority(AsyncWorkerPriority priority) noexcept
{
    switch (priority)
    {
    case AsyncWorkerPriority::Normal:
        return THREAD_PRIORITY_NORMAL;
    case AsyncWorkerPriority::BelowNormal:
        return THREAD_PRIORITY_BELOW_NORMAL;
    case AsyncWorkerPriority::Lowest:
        return THREAD_PRIORITY_LOWEST;
    }

    return THREAD_PRIORITY_BELOW_NORMAL;
}

// Validate async queue settings before any producer path can observe them.
bool validateAsyncConfig(const AsyncDataLoggerConfig& config,
                         DataLoggerCore::DataLoggerError& error)
{
    if (config.queueCapacity == 0)
    {
        error = { DataLoggerCore::ErrorCode::InvalidConfig, "Async queue capacity must be at least 1." };
        return false;
    }

    if (config.maxPayloadBytes == 0)
    {
        error = { DataLoggerCore::ErrorCode::InvalidConfig, "Async max payload bytes must be at least 1." };
        return false;
    }

    return true;
}
}

class AsyncDataLogger::Impl
{
public:
    // Keep the existing synchronous logger owned behind the async wrapper boundary.
    explicit Impl(std::unique_ptr<DataLoggerCore::IDBBackend> backend)
        : logger(std::move(backend))
    {
    }

    // Stop the worker before DataLogger destruction can attempt a final flush.
    ~Impl()
    {
        stopWorker(false);
    }

    // Allocate the fixed-size ring slots and contiguous payload storage.
    bool allocateQueue(const AsyncDataLoggerConfig& asyncConfig)
    {
        std::size_t roundedCapacity = 0;
        if (!nextPowerOfTwo(asyncConfig.queueCapacity, roundedCapacity))
        {
            setError(AsyncDataLoggerStatus::InvalidArgument, "Async queue capacity is too large.");
            return false;
        }

        if (asyncConfig.maxPayloadBytes > (std::numeric_limits<std::size_t>::max)() / roundedCapacity)
        {
            setError(AsyncDataLoggerStatus::InvalidArgument, "Async payload storage size is too large.");
            return false;
        }

        queueCapacity = roundedCapacity;
        queueMask = roundedCapacity - 1;
        queueSlots.reset(new AsyncLogSlot[queueCapacity]);
        payloadStorage.resize(queueCapacity * asyncConfig.maxPayloadBytes);
        workerPayload.resize(asyncConfig.maxPayloadBytes);

        for (std::size_t i = 0; i < queueCapacity; ++i)
        {
            queueSlots[i].state.store(SlotState::Empty, std::memory_order_relaxed);
            queueSlots[i].payloadStorage = payloadStorage.data() + (i * asyncConfig.maxPayloadBytes);
        }

        head.store(0, std::memory_order_relaxed);
        tail.store(0, std::memory_order_relaxed);
        stats.currentQueueDepth.store(0, std::memory_order_relaxed);
        stats.maxQueueDepth.store(0, std::memory_order_relaxed);
        return true;
    }

    // Clear wrapper error text and mark the latest status as successful.
    void clearError()
    {
        {
            std::lock_guard<std::mutex> lock(errorMutex);
            lastError = {};
        }

        lastStatus.store(AsyncDataLoggerStatus::Ok, std::memory_order_relaxed);
    }

    // Store a control-path error message and a lightweight status code.
    void setError(AsyncDataLoggerStatus status, const std::string& message)
    {
        {
            std::lock_guard<std::mutex> lock(errorMutex);
            lastError = { DataLoggerCore::ErrorCode::InvalidConfig, message };
        }

        lastStatus.store(status, std::memory_order_relaxed);
    }

    // Store an existing DataLogger error as the async wrapper's current error.
    void setLoggerError(AsyncDataLoggerStatus status)
    {
        {
            std::lock_guard<std::mutex> lock(errorMutex);
            lastError = logger.lastError();
        }

        lastStatus.store(status, std::memory_order_relaxed);
    }

    // Record a non-fatal worker warning without making future stop calls fail.
    void setWorkerWarning(const std::string& message)
    {
        stats.workerErrors.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(errorMutex);
        lastError = { DataLoggerCore::ErrorCode::InvalidConfig, message };
    }

    // Record a worker-side logger failure after DataLogger has preserved details.
    void setWorkerFailure()
    {
        stats.workerErrors.fetch_add(1, std::memory_order_relaxed);
        workerFailed.store(true, std::memory_order_relaxed);
        setLoggerError(AsyncDataLoggerStatus::Error);
    }

    // Return a copy-backed reference for the legacy lastError() style.
    const DataLoggerCore::DataLoggerError& lastErrorRef() const
    {
        std::lock_guard<std::mutex> lock(errorMutex);
        lastErrorCache = lastError;
        return lastErrorCache;
    }

    // Compute the current queued item count from monotonic head and tail counters.
    std::uint64_t currentDepth() const noexcept
    {
        const std::uint64_t currentHead = head.load(std::memory_order_acquire);
        const std::uint64_t currentTail = tail.load(std::memory_order_acquire);
        return currentHead >= currentTail ? currentHead - currentTail : 0;
    }

    // Advance tail over the oldest full slot so a new sample can replace it.
    bool dropOldest(std::uint64_t observedTail) noexcept
    {
        AsyncLogSlot& slot = queueSlots[static_cast<std::size_t>(observedTail) & queueMask];
        SlotState expected = SlotState::Full;
        if (!slot.state.compare_exchange_strong(expected,
                                                SlotState::Empty,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire))
        {
            return false;
        }

        tail.compare_exchange_strong(observedTail,
                                     observedTail + 1,
                                     std::memory_order_acq_rel,
                                     std::memory_order_acquire);
        stats.droppedSamples.fetch_add(1, std::memory_order_relaxed);
        stats.currentQueueDepth.store(currentDepth(), std::memory_order_relaxed);
        return true;
    }

    // Enqueue a payload or control operation without locks or blocking.
    bool tryPush(AsyncLogOperation operation,
                 DataLoggerCore::TableHandle table,
                 std::int64_t timestampMs,
                 const void* structPtr,
                 std::size_t structSize) noexcept
    {
        const auto startedAt = Clock::now();

        if (!initialized.load(std::memory_order_acquire) ||
            !started.load(std::memory_order_acquire) ||
            stopRequested.load(std::memory_order_acquire))
        {
            lastStatus.store(AsyncDataLoggerStatus::NotInitialized, std::memory_order_relaxed);
            storeMax(stats.tryPushMaxNs, elapsedNanoseconds(startedAt));
            return false;
        }

        if (isDataOperation(operation))
        {
            if (structPtr == nullptr || structSize == 0)
            {
                lastStatus.store(AsyncDataLoggerStatus::InvalidArgument, std::memory_order_relaxed);
                storeMax(stats.tryPushMaxNs, elapsedNanoseconds(startedAt));
                return false;
            }

            if (structSize > config.maxPayloadBytes)
            {
                stats.droppedSamples.fetch_add(1, std::memory_order_relaxed);
                lastStatus.store(AsyncDataLoggerStatus::PayloadTooLarge, std::memory_order_relaxed);
                storeMax(stats.tryPushMaxNs, elapsedNanoseconds(startedAt));
                return false;
            }
        }

        std::uint64_t currentHead = head.load(std::memory_order_relaxed);
        std::uint64_t currentTail = tail.load(std::memory_order_acquire);
        if (currentHead - currentTail >= queueCapacity)
        {
            const bool canDropOldest = isDataOperation(operation) &&
                                       config.overflowPolicy == AsyncOverflowPolicy::DropOldest &&
                                       dropOldest(currentTail);
            if (!canDropOldest)
            {
                if (isDataOperation(operation))
                {
                    stats.droppedSamples.fetch_add(1, std::memory_order_relaxed);
                }

                lastStatus.store(AsyncDataLoggerStatus::QueueFull, std::memory_order_relaxed);
                storeMax(stats.tryPushMaxNs, elapsedNanoseconds(startedAt));
                return false;
            }

            currentHead = head.load(std::memory_order_relaxed);
        }

        AsyncLogSlot& slot = queueSlots[static_cast<std::size_t>(currentHead) & queueMask];
        if (slot.state.load(std::memory_order_acquire) != SlotState::Empty)
        {
            if (isDataOperation(operation))
            {
                stats.droppedSamples.fetch_add(1, std::memory_order_relaxed);
            }

            lastStatus.store(AsyncDataLoggerStatus::QueueFull, std::memory_order_relaxed);
            storeMax(stats.tryPushMaxNs, elapsedNanoseconds(startedAt));
            return false;
        }

        slot.operation = operation;
        slot.table = table;
        slot.timestampMs = timestampMs;
        slot.payloadSize = isDataOperation(operation) ? structSize : 0;
        if (isDataOperation(operation))
        {
            std::memcpy(slot.payloadStorage, structPtr, structSize);
        }

        slot.state.store(SlotState::Full, std::memory_order_release);
        head.store(currentHead + 1, std::memory_order_release);

        const std::uint64_t depth = currentDepth();
        stats.currentQueueDepth.store(depth, std::memory_order_relaxed);
        storeMax(stats.maxQueueDepth, depth);
        if (isDataOperation(operation))
        {
            stats.pushedSamples.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            stats.flushRequests.fetch_add(1, std::memory_order_relaxed);
        }

        lastStatus.store(AsyncDataLoggerStatus::Ok, std::memory_order_relaxed);
        storeMax(stats.tryPushMaxNs, elapsedNanoseconds(startedAt));
        return true;
    }

    // Pop one full slot into worker-owned storage and release the queue slot.
    bool pop(WorkerItem& item) noexcept
    {
        std::uint64_t currentTail = tail.load(std::memory_order_relaxed);
        const std::uint64_t currentHead = head.load(std::memory_order_acquire);
        if (currentTail >= currentHead)
        {
            stats.currentQueueDepth.store(0, std::memory_order_relaxed);
            return false;
        }

        AsyncLogSlot& slot = queueSlots[static_cast<std::size_t>(currentTail) & queueMask];
        SlotState expected = SlotState::Full;
        if (!slot.state.compare_exchange_strong(expected,
                                                SlotState::Reading,
                                                std::memory_order_acquire,
                                                std::memory_order_acquire))
        {
            if (expected == SlotState::Empty)
            {
                const std::uint64_t desiredTail = currentTail + 1;
                tail.compare_exchange_strong(currentTail,
                                             desiredTail,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire);
            }

            stats.currentQueueDepth.store(currentDepth(), std::memory_order_relaxed);
            return false;
        }

        item.operation = slot.operation;
        item.table = slot.table;
        item.timestampMs = slot.timestampMs;
        item.payloadSize = slot.payloadSize;
        item.payload = nullptr;
        if (isDataOperation(item.operation) && item.payloadSize > 0)
        {
            std::memcpy(workerPayload.data(), slot.payloadStorage, item.payloadSize);
            item.payload = workerPayload.data();
        }

        slot.state.store(SlotState::Empty, std::memory_order_release);
        tail.store(currentTail + 1, std::memory_order_release);
        stats.currentQueueDepth.store(currentDepth(), std::memory_order_relaxed);
        return true;
    }

    // Apply the configured non-realtime Windows worker priority.
    void applyWorkerPriority()
    {
        if (!SetThreadPriority(GetCurrentThread(), nativeThreadPriority(config.workerPriority)))
        {
            setWorkerWarning("Unable to set AsyncDataLogger worker thread priority.");
        }
    }

    // Run one queued operation on the worker-owned DataLogger instance.
    void processItem(const WorkerItem& item)
    {
        switch (item.operation)
        {
        case AsyncLogOperation::AutoWrite:
            stats.poppedSamples.fetch_add(1, std::memory_order_relaxed);
            stats.autoWriteOps.fetch_add(1, std::memory_order_relaxed);
            if (!logger.autoWrite(item.timestampMs, item.payload))
            {
                setWorkerFailure();
            }
            break;
        case AsyncLogOperation::Write:
            stats.poppedSamples.fetch_add(1, std::memory_order_relaxed);
            stats.writeOps.fetch_add(1, std::memory_order_relaxed);
            if (!logger.write(item.table, item.timestampMs, item.payload))
            {
                setWorkerFailure();
            }
            break;
        case AsyncLogOperation::FlushAll:
            flushAllFromWorker();
            break;
        case AsyncLogOperation::FlushTable:
            flushTableFromWorker(item.table);
            break;
        }
    }

    // Flush all table buffers on the worker thread and update flush timing stats.
    bool flushAllFromWorker()
    {
        const auto startedAt = Clock::now();
        const bool ok = logger.flush();
        storeMax(stats.flushMaxNs, elapsedNanoseconds(startedAt));
        if (!ok)
        {
            setWorkerFailure();
        }

        return ok;
    }

    // Flush one table buffer on the worker thread and update flush timing stats.
    bool flushTableFromWorker(DataLoggerCore::TableHandle table)
    {
        const auto startedAt = Clock::now();
        const bool ok = logger.flush(table);
        storeMax(stats.flushMaxNs, elapsedNanoseconds(startedAt));
        if (!ok)
        {
            setWorkerFailure();
        }

        return ok;
    }

    // Drain queued operations until stop is requested and no queued work remains.
    void workerLoop()
    {
        applyWorkerPriority();

        while (true)
        {
            WorkerItem item;
            if (pop(item))
            {
                const auto startedAt = Clock::now();
                processItem(item);
                storeMax(stats.workerLoopMaxNs, elapsedNanoseconds(startedAt));
                continue;
            }

            if (stopRequested.load(std::memory_order_acquire) && currentDepth() == 0)
            {
                break;
            }

            std::this_thread::yield();
        }

        if (initialized.load(std::memory_order_acquire) &&
            (config.flushOnStop || finalFlushRequested.load(std::memory_order_acquire)))
        {
            flushAllFromWorker();
        }

        started.store(false, std::memory_order_release);
    }

    // Request worker shutdown, join it, and optionally force a final flush.
    bool stopWorker(bool forceFlush)
    {
        if (!started.load(std::memory_order_acquire))
        {
            return !workerFailed.load(std::memory_order_relaxed);
        }

        if (forceFlush)
        {
            finalFlushRequested.store(true, std::memory_order_release);
        }

        stopRequested.store(true, std::memory_order_release);
        if (worker.joinable())
        {
            worker.join();
        }

        return !workerFailed.load(std::memory_order_relaxed);
    }

    DataLoggerCore::DataLogger logger;
    AsyncDataLoggerConfig config;
    std::unique_ptr<AsyncLogSlot[]> queueSlots;
    std::vector<std::byte> payloadStorage;
    std::vector<std::byte> workerPayload;
    std::size_t queueCapacity = 0;
    std::size_t queueMask = 0;
    std::atomic<std::uint64_t> head{ 0 };
    std::atomic<std::uint64_t> tail{ 0 };
    AtomicStats stats;
    std::atomic<AsyncDataLoggerStatus> lastStatus{ AsyncDataLoggerStatus::NotInitialized };
    mutable std::mutex errorMutex;
    DataLoggerCore::DataLoggerError lastError;
    mutable DataLoggerCore::DataLoggerError lastErrorCache;
    std::thread worker;
    std::atomic<bool> initialized{ false };
    std::atomic<bool> started{ false };
    std::atomic<bool> stopRequested{ false };
    std::atomic<bool> finalFlushRequested{ false };
    std::atomic<bool> workerFailed{ false };
    bool autoRegistered = false;
};

// Create the async wrapper around the same backend-injection model as DataLogger.
AsyncDataLogger::AsyncDataLogger(std::unique_ptr<DataLoggerCore::IDBBackend> backend)
    : impl_(std::make_unique<Impl>(std::move(backend)))
{
}

// Stop worker activity before releasing the wrapped synchronous logger.
AsyncDataLogger::~AsyncDataLogger() = default;

// Initialize DataLogger, preallocate the queue, and keep async producer paths allocation-free.
bool AsyncDataLogger::initialize(const DataLoggerCore::DataLoggerConfig& dataConfig,
                                 const AsyncDataLoggerConfig& asyncConfig)
{
    if (impl_->started.load(std::memory_order_acquire))
    {
        impl_->setError(AsyncDataLoggerStatus::AlreadyStarted, "Cannot initialize AsyncDataLogger while it is started.");
        return false;
    }

    impl_->clearError();
    impl_->initialized.store(false, std::memory_order_release);
    impl_->stopRequested.store(false, std::memory_order_release);
    impl_->finalFlushRequested.store(false, std::memory_order_release);
    impl_->workerFailed.store(false, std::memory_order_release);
    impl_->stats.reset();
    impl_->autoRegistered = false;

    DataLoggerCore::DataLoggerError configError;
    if (!validateAsyncConfig(asyncConfig, configError))
    {
        impl_->setError(AsyncDataLoggerStatus::InvalidArgument, configError.message);
        return false;
    }

    try
    {
        if (!impl_->allocateQueue(asyncConfig))
        {
            return false;
        }
    }
    catch (const std::bad_alloc&)
    {
        impl_->setError(AsyncDataLoggerStatus::Error, "Unable to allocate AsyncDataLogger queue storage.");
        return false;
    }

    if (!impl_->logger.initialize(dataConfig))
    {
        impl_->setLoggerError(AsyncDataLoggerStatus::Error);
        return false;
    }

    impl_->config = asyncConfig;
    impl_->initialized.store(true, std::memory_order_release);
    impl_->lastStatus.store(AsyncDataLoggerStatus::Ok, std::memory_order_relaxed);
    return true;
}

// Register one table before the worker thread starts using DataLogger.
bool AsyncDataLogger::registerTable(const std::string& tableName,
                                    DataLoggerCore::TableHandle* outHandle)
{
    impl_->clearError();

    if (outHandle == nullptr)
    {
        impl_->setError(AsyncDataLoggerStatus::InvalidArgument, "Output table handle pointer must not be null.");
        return false;
    }

    if (!impl_->initialized.load(std::memory_order_acquire))
    {
        *outHandle = DataLoggerCore::TableHandle::invalid();
        impl_->setError(AsyncDataLoggerStatus::NotInitialized, "AsyncDataLogger must be initialized before registering tables.");
        return false;
    }

    if (impl_->started.load(std::memory_order_acquire))
    {
        *outHandle = DataLoggerCore::TableHandle::invalid();
        impl_->setError(AsyncDataLoggerStatus::AlreadyStarted, "Register tables before starting AsyncDataLogger.");
        return false;
    }

    *outHandle = impl_->logger.registerTable(tableName);
    if (!outHandle->isValid())
    {
        impl_->setLoggerError(AsyncDataLoggerStatus::Error);
        return false;
    }

    return true;
}

// Register every loaded schema table before the worker thread starts using DataLogger.
bool AsyncDataLogger::autoRegisterTables()
{
    impl_->clearError();

    if (!impl_->initialized.load(std::memory_order_acquire))
    {
        impl_->setError(AsyncDataLoggerStatus::NotInitialized, "AsyncDataLogger must be initialized before automatic table registration.");
        return false;
    }

    if (impl_->started.load(std::memory_order_acquire))
    {
        impl_->setError(AsyncDataLoggerStatus::AlreadyStarted, "Auto-register tables before starting AsyncDataLogger.");
        return false;
    }

    if (!impl_->logger.autoRegisterTables())
    {
        impl_->setLoggerError(AsyncDataLoggerStatus::Error);
        return false;
    }

    impl_->autoRegistered = true;
    return true;
}

// Start the single consumer worker that owns write/flush calls after this point.
bool AsyncDataLogger::start()
{
    impl_->clearError();

    if (!impl_->initialized.load(std::memory_order_acquire))
    {
        impl_->setError(AsyncDataLoggerStatus::NotInitialized, "AsyncDataLogger must be initialized before start().");
        return false;
    }

    if (impl_->started.load(std::memory_order_acquire))
    {
        impl_->setError(AsyncDataLoggerStatus::AlreadyStarted, "AsyncDataLogger is already started.");
        return false;
    }

    if (impl_->config.autoRegisterTablesOnStart && !impl_->autoRegistered)
    {
        if (!impl_->logger.autoRegisterTables())
        {
            impl_->setLoggerError(AsyncDataLoggerStatus::Error);
            return false;
        }

        impl_->autoRegistered = true;
    }

    impl_->stopRequested.store(false, std::memory_order_release);
    impl_->finalFlushRequested.store(false, std::memory_order_release);
    impl_->workerFailed.store(false, std::memory_order_release);
    impl_->started.store(true, std::memory_order_release);

    try
    {
        impl_->worker = std::thread([impl = impl_.get()]() {
            impl->workerLoop();
        });
    }
    catch (const std::system_error&)
    {
        impl_->started.store(false, std::memory_order_release);
        impl_->setError(AsyncDataLoggerStatus::Error, "Unable to start AsyncDataLogger worker thread.");
        return false;
    }

    return true;
}

// Queue an automatic multi-table payload copy without touching DataLogger or SQL.
bool AsyncDataLogger::tryAutoWrite(std::int64_t timestampMs,
                                   const void* structPtr,
                                   std::size_t structSize) noexcept
{
    return impl_->tryPush(AsyncLogOperation::AutoWrite,
                          DataLoggerCore::TableHandle::invalid(),
                          timestampMs,
                          structPtr,
                          structSize);
}

// Queue a table-specific payload copy without touching DataLogger or SQL.
bool AsyncDataLogger::tryWrite(DataLoggerCore::TableHandle table,
                               std::int64_t timestampMs,
                               const void* structPtr,
                               std::size_t structSize) noexcept
{
    return impl_->tryPush(AsyncLogOperation::Write, table, timestampMs, structPtr, structSize);
}

// Queue an all-table flush request for the worker thread.
bool AsyncDataLogger::requestFlush() noexcept
{
    return impl_->tryPush(AsyncLogOperation::FlushAll,
                          DataLoggerCore::TableHandle::invalid(),
                          0,
                          nullptr,
                          0);
}

// Queue a table-specific flush request for the worker thread.
bool AsyncDataLogger::requestFlush(DataLoggerCore::TableHandle table) noexcept
{
    return impl_->tryPush(AsyncLogOperation::FlushTable, table, 0, nullptr, 0);
}

// Stop the worker after it drains currently queued operations.
void AsyncDataLogger::stop()
{
    impl_->stopWorker(false);
}

// Stop the worker after draining queued operations and force a final flush.
bool AsyncDataLogger::stopAndFlush()
{
    impl_->clearError();

    if (!impl_->initialized.load(std::memory_order_acquire))
    {
        impl_->setError(AsyncDataLoggerStatus::NotInitialized, "AsyncDataLogger must be initialized before stopAndFlush().");
        return false;
    }

    if (!impl_->started.load(std::memory_order_acquire))
    {
        const bool ok = impl_->flushAllFromWorker();
        impl_->lastStatus.store(ok ? AsyncDataLoggerStatus::Ok : AsyncDataLoggerStatus::Error, std::memory_order_relaxed);
        return ok;
    }

    const bool ok = impl_->stopWorker(true);
    impl_->lastStatus.store(ok ? AsyncDataLoggerStatus::Ok : AsyncDataLoggerStatus::Error, std::memory_order_relaxed);
    return ok;
}

// Return a copy of the current async counters.
AsyncDataLoggerStats AsyncDataLogger::stats() const noexcept
{
    return impl_->stats.snapshot();
}

// Return the last lightweight status code for C ABI mapping.
AsyncDataLoggerStatus AsyncDataLogger::lastStatus() const noexcept
{
    return impl_->lastStatus.load(std::memory_order_relaxed);
}

// Return the async wrapper error through the existing reference-returning style.
const DataLoggerCore::DataLoggerError& AsyncDataLogger::lastError() const
{
    return impl_->lastErrorRef();
}
}
