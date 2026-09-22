#include "scene_text.hpp"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>

namespace ac3::oba {

bool read_double(std::string_view token, double& out) {
    if (token.empty()) {
        return false;
    }
    // strtod needs a NUL-terminated buffer; token is a view into someone
    // else's storage (a mapped file, a CLI argument) and need not be one.
    const std::string buffer(token);
    errno = 0;
    char* end = nullptr;
    const double value = std::strtod(buffer.c_str(), &end);
    if (end != buffer.c_str() + buffer.size()) {
        return false;
    }
    // ERANGE covers both ends and they are not the same thing. Overflow hands
    // back +/-HUGE_VAL and is a genuine refusal ("1e999" is not a number this
    // can carry). Underflow hands back a subnormal or zero, which IS the
    // correctly rounded value - refusing it would make fmt::format("{}", ...)
    // unable to find any spelling of a subnormal that this accepts back, and
    // the two have to agree for a round trip to hold at all.
    if (errno == ERANGE && !std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

}  // namespace ac3::oba
