#pragma once

#include <cstddef>
#include <limits>

namespace DataLoggerCore
{
class TableHandle
{
public:
    // Return the sentinel handle used after failed registration or invalid input.
    static TableHandle invalid();

    TableHandle() = default;
    // Wrap a schema registry index in a small named handle type.
    explicit TableHandle(std::size_t index);

    // Report whether this handle is not the reserved sentinel value.
    bool isValid() const;
    // Expose the underlying index after the caller has validated the handle.
    std::size_t index() const;

private:
    static constexpr std::size_t InvalidIndex = (std::numeric_limits<std::size_t>::max)();

    std::size_t index_ = InvalidIndex;
};
}
