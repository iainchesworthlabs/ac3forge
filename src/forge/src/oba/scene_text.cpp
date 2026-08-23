#include "scene_text.hpp"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <ios>
#include <iomanip>
#include <locale>
#include <sstream>
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
    // correctly rounded value - refusing it would make write_double unable to
    // find any spelling of a subnormal that read_double takes back, and the
    // two have to agree for the round trip to hold at all.
    if (errno == ERANGE && !std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

std::string write_double(double value) {
    // Imbued with the classic locale rather than inheriting the global one:
    // a host application that has called setlocale to a comma-decimal locale
    // must not be able to make this library write a scene file no reader,
    // including this one, can take back.
    std::ostringstream out;
    out.imbue(std::locale::classic());
    for (int precision = 1; precision <= 17; ++precision) {
        out.str(std::string{});
        out.clear();
        out << std::defaultfloat << std::setprecision(precision) << value;
        std::string text = out.str();
        double back = 0.0;
        if (read_double(text, back) && back == value) {
            return text;
        }
    }
    // Seventeen significant digits round-trip every finite double, so the loop
    // above always returns for one. Anything reaching here is not finite -
    // which ObjectScene::create and the JSON reader both refuse, leaving only
    // the raw-object to_keyframe_text() overload as a way in. Print it rather
    // than pretending: a caller that hands this a NaN should see one.
    out.str(std::string{});
    out.clear();
    out << std::defaultfloat << std::setprecision(17) << value;
    return out.str();
}

}  // namespace ac3::oba
