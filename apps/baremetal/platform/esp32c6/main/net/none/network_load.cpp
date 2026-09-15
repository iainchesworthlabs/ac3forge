// No network load: the probe alone, as on the C3 and the S3. See
// ../../network_load.hpp.

#include "network_load.hpp"

#include <cstdio>

namespace ac3probe {

bool network_start() {
    std::printf("network=none\n");
    return true;
}

void network_report() {}

}  // namespace ac3probe
