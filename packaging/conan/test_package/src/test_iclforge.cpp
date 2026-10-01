#include <fmt/base.h>

#include "iclforge/ac3/version.hpp"

int main() {
    fmt::println("{}", iclforge::ac3::version_details());
    return 0;
}
