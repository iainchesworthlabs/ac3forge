#include <fmt/format.h>

#include "ac3/version.hpp"

int main() {
    fmt::println("{}", ac3::version_details());
    return 0;
}
