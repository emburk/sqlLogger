#pragma once

#include <cstddef>
#include <limits>

namespace DataLoggerCore
{
class TableHandle
{
public:
    static TableHandle invalid();

    TableHandle() = default;
    explicit TableHandle(std::size_t index);

    bool isValid() const;
    std::size_t index() const;

private:
    static constexpr std::size_t InvalidIndex = (std::numeric_limits<std::size_t>::max)();

    std::size_t index_ = InvalidIndex;
};
}
