#pragma once

// Zone markers for the AC-4 kernels, stage-timer variant: selected by
// AC3FORGE_STAGE_TIMERS (src/ac4core/CMakeLists.txt), which the minimum-footprint
// profile has and no other build does. See the none/ sibling for the default and
// for why a directory, not a preprocessor test, chooses between them.
//
// The markers are routed to the same two functions iclforge::ac3's own zones are:
// zone_enter() at a marker and zone_leave() at the end of its scope, which the
// APPLICATION supplies. apps/baremetal/stage_timers.cpp is the one
// implementation - a stack of open zones, a per-name accumulator and the report
// esp-idf/iclforge/examples/hearth_sink prints beside each play - so an AC-4
// play's `play.stage[<zone>]` lines come out of the same code as an AC-3 play's,
// and a library built with this variant links only where that or another
// implementation of the two functions is present, which is the intended failure.
// The functions are declared here rather than by including iclforge::ac3's header:
// this library links nothing of iclforge::ac3's, and two declarations of the same
// two functions are the whole interface.
//
// Cost when selected: two calls and two clock reads for each marker that runs.
// The markers are on the calls that do the transform and filter work of a frame
// (a QMF bank's process(), a block's inverse transform, the high-frequency
// regenerator, the decorrelators, the sample rate converter), each of which
// runs some hundreds of times a frame at most.

namespace iclforge::internal::profiling {
void zone_enter(const char* name);
void zone_leave();
}  // namespace iclforge::internal::profiling

namespace iclforge::ac4::detail::profiling {

// Enters on construction and leaves on destruction, as ZoneScope in iclforge::ac3's
// variant does.
class ZoneScope {
   public:
    explicit ZoneScope(const char* name) { ::iclforge::internal::profiling::zone_enter(name); }
    ~ZoneScope() { ::iclforge::internal::profiling::zone_leave(); }
    ZoneScope(const ZoneScope&) = delete;
    ZoneScope& operator=(const ZoneScope&) = delete;
    ZoneScope(ZoneScope&&) = delete;
    ZoneScope& operator=(ZoneScope&&) = delete;
};

}  // namespace iclforge::ac4::detail::profiling

// Two-step expansion so __LINE__ is substituted before the paste, giving each
// marker in a function its own local.
#define AC4_PROFILING_ZONE_NAME_2(prefix, line) prefix##line
#define AC4_PROFILING_ZONE_NAME(prefix, line) AC4_PROFILING_ZONE_NAME_2(prefix, line)

#define AC4_ZONE_SCOPED_N(name) \
    ::iclforge::ac4::detail::profiling::ZoneScope AC4_PROFILING_ZONE_NAME(ac4_zone_, __LINE__){name}
