// The heap-region pair for a platform whose heap is one pool: the host's and newlib's on
// the mps2-an385. The probe's own counters already say everything there is to say.

#include "probe.hpp"

namespace iclforge_probe {

void heap_regions_begin() {}

void heap_regions_end(const char* /*fixture*/) {}

}  // namespace iclforge_probe
