#pragma once

#include <cstdint>

// The probe's half of the stage-timer zone backend
// (src/forge/src/internal/profiling/stage_timers/, selected by
// AC3FORGE_STAGE_TIMERS). The library's AC3_ZONE_SCOPED_N() markers call
// two functions this application defines - see stage_timers.cpp - and this
// header is what the probe itself reads the result back through.
//
// Compiled into every shape of the probe, whether or not the library was
// built to call it: when it was not, nothing calls zone_enter, the table
// stays empty, report_stages() prints nothing and --gc-sections drops the
// rest. So the ONLY difference between a timed build and a plain one is the
// library's include path, which is the property that keeps the two builds'
// decode arithmetic identical and their timings comparable.

namespace ac3probe {

// Zero every accumulator. Called before each fixture's decode loop so the
// report that follows describes that fixture alone.
void reset_stages();

// One line per zone that ran, prefixed by `codec` like every other line the
// probe prints for a fixture, with the per-frame inclusive time, the per-frame
// SELF time (inclusive minus the zones nested inside it) and the calls per
// frame. Then one line for the time inside outermost zones, which against the
// probe's own decode_us says how much of a decode the markers do not cover.
// Prints nothing at all when no zone ever ran.
void report_stages(const char* codec, int frames);

// Whether any zone has run since the program started: the probe prints this
// once so a reader of the log knows which kind of build produced it.
[[nodiscard]] bool stages_active();

// The cost of one enter/leave pair on this target, measured on itself over
// an empty zone, in nanoseconds. Landed on the enclosing zone's self time by
// construction, so a reader can subtract calls_per_frame times this from any
// parent's figure.
[[nodiscard]] std::uint64_t stage_pair_cost_ns();

}  // namespace ac3probe
