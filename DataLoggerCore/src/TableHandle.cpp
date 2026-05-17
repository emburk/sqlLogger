#include "DataLogger/TableHandle.h"

namespace DataLoggerCore
{
TableHandle TableHandle::invalid()
{
    return TableHandle();
}

TableHandle::TableHandle(std::size_t index)
    : index_(index)
{
}

bool TableHandle::isValid() const
{
    return index_ != InvalidIndex;
}

std::size_t TableHandle::index() const
{
    return index_;
}
}
