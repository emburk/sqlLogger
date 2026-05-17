#include "DataLogger/TableHandle.h"

namespace DataLoggerCore
{
// Construct the sentinel handle used for failed registration and invalid inputs.
TableHandle TableHandle::invalid()
{
    return TableHandle();
}

// Wrap the schema registry index in a named type so handles are not raw size_t values.
TableHandle::TableHandle(std::size_t index)
    : index_(index)
{
}

// A handle is syntactically valid if it is not the reserved sentinel value.
bool TableHandle::isValid() const
{
    return index_ != InvalidIndex;
}

// Expose the registry index to core code that already validated the handle.
std::size_t TableHandle::index() const
{
    return index_;
}
}
