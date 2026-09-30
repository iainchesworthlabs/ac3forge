#pragma once

#include <memory>
#include <type_traits>

// `value = T{}` builds the T on the stack and copies it over `value`: for an
// SfData, 11.8 KB of stack for a reset, and planning/ac4.md's D14a memory rules
// allow no stack object over 4 KiB. This destroys the object and makes it again
// where it is, for the same result.

namespace iclforge::ac4::detail {

template <typename T>
void reset_in_place(T& value) noexcept(std::is_nothrow_default_constructible_v<T>) {
    std::destroy_at(std::addressof(value));
    std::construct_at(std::addressof(value));
}

}  // namespace iclforge::ac4::detail
