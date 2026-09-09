#pragma once

// Zone macros, stage-timer variant - the third answer to
// "ac3/internal/profiling.hpp", beside tracy_enabled/ and tracy_disabled/,
// and selected the same way: CMake puts this directory on the include path
// (src/forge/minimal.cmake, under AC3FORGE_STAGE_TIMERS) rather than any
// source asking with an #ifdef.
//
// What it is for. Tracy needs a host with a socket and a build with the
// Tracy client in it; a minimum-footprint decoder on a bare-metal part has
// neither, and the question that profile eventually has to answer - "where
// do the microseconds of one frame go on this silicon?" - cannot be answered
// from the emulator (docs/platforms/esp32.md's Timing section). So the same
// AC3_ZONE_SCOPED_N() markers the Tracy build uses are routed here to two
// plain functions the APPLICATION supplies: one at zone entry, one at exit.
// The library carries no clock, no table and no output of its own, because a
// clock is exactly the thing that differs per platform (esp_timer on
// ESP-IDF, semihosting under QEMU, std::chrono on a host) and the probe
// already owns that seam as ac3probe::now_us().
//
// apps/baremetal/stage_timers.cpp is the one implementation today: a stack
// of open zones and a per-name accumulator, reported per fixture as
// `<codec>.stage[<name>]` lines beside the decode timing the probe already
// prints. Any other application linking a library built with this variant
// must define the same two functions or fail to link - which is the intended
// failure, loud and at link time, rather than a zone quietly timing nothing.
//
// Cost when selected: two calls and two clock reads per zone, which the
// probe measures on itself and reports as stage.pair_cost_ns so a reader can
// subtract them. Cost when not selected: none, since tracy_disabled/ is then
// the directory on the path and every macro below expands to nothing there.

namespace ac3::internal::profiling {

// Supplied by the application. `name` is the string literal the zone was
// declared with, so its address identifies the zone as well as its text;
// the implementation may key on either.
void zone_enter(const char* name);
void zone_leave();

// Enters on construction, leaves on destruction - the lexically-scoped form
// every AC3_ZONE_SCOPED*() site relies on, matching ZoneScoped's semantics.
class ZoneScope {
   public:
    explicit ZoneScope(const char* name) { zone_enter(name); }
    ~ZoneScope() { zone_leave(); }
    ZoneScope(const ZoneScope&) = delete;
    ZoneScope& operator=(const ZoneScope&) = delete;
    ZoneScope(ZoneScope&&) = delete;
    ZoneScope& operator=(ZoneScope&&) = delete;
};

}  // namespace ac3::internal::profiling

// Two-step expansion so __LINE__ is substituted before the paste, giving
// each zone in a function its own local.
#define AC3_PROFILING_ZONE_NAME_2(prefix, line) prefix##line
#define AC3_PROFILING_ZONE_NAME(prefix, line) AC3_PROFILING_ZONE_NAME_2(prefix, line)

#define AC3_ZONE_SCOPED() \
    ::ac3::internal::profiling::ZoneScope AC3_PROFILING_ZONE_NAME(ac3_zone_, __LINE__){__func__}
#define AC3_ZONE_SCOPED_N(name) \
    ::ac3::internal::profiling::ZoneScope AC3_PROFILING_ZONE_NAME(ac3_zone_, __LINE__){name}
// The manual pair. `var` is Tracy's context handle and means nothing here:
// the stack in the application pairs each leave with the innermost open
// zone, which is what a correctly nested begin/end pair is.
#define AC3_ZONE_BEGIN(var, name) ::ac3::internal::profiling::zone_enter(name)
#define AC3_ZONE_END(var) ::ac3::internal::profiling::zone_leave()
// Frame marks are for Tracy's frame view; the probe already knows where its
// frames are, since it is the thing calling decode_frame_into.
#define AC3_FRAME_MARK()
