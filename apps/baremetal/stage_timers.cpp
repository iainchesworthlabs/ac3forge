// Where a frame's microseconds go, stage by stage - the application half of
// the AC3FORGE_STAGE_TIMERS zone backend. See stage_timers.hpp for the
// contract and src/forge/src/internal/profiling/stage_timers/ for the other
// half.
//
// The design is set by what the target can afford: a fixed table, no
// allocation on any path (operator new is the thing being measured, and the
// probe's own counters would see it), integer arithmetic only, and a clock
// read that is the platform's own ac3probe::now_us().
//
// Two figures per zone. INCLUSIVE is wall time between enter and leave;
// SELF is that minus the inclusive time of every zone opened inside it,
// which is the one that says where the work is: eac3_parse_block's inclusive
// time is nearly the whole of pass one, while its self time is only what
// pass one does OUTSIDE exponents, bit allocation and mantissas.

#include "stage_timers.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "probe.hpp"

// The library's side of the contract, declared here rather than by including
// its private header: src/forge/src/internal/ is not on the probe's include
// path, deliberately, and two declarations of the same two functions are the
// whole interface.
namespace ac3::internal::profiling {
void zone_enter(const char* name);
void zone_leave();
}  // namespace ac3::internal::profiling

namespace {

// Zones are the library's marker names - about two dozen exist across both
// decoders and the encoders - and a decode nests them at most five deep
// (access unit > substream > block > stage > kernel). Both bounds are checked
// rather than trusted: an overflow is counted and reported, never written
// past the end of an array.
constexpr std::size_t kMaxZones = 32;
constexpr std::size_t kMaxDepth = 16;
// A name reaches here as a pointer to a string literal, and the same text
// can be a different literal in a different translation unit. Pointers are
// what make the lookup cheap; the text is what makes two of them the same
// zone. So a first sighting of a pointer resolves it by text once, and every
// later sighting is a pointer compare.
constexpr std::size_t kMaxAliases = 64;

struct Zone {
    const char* name = nullptr;
    std::uint64_t inclusive_us = 0;
    std::uint64_t self_us = 0;
    std::uint32_t calls = 0;
};

struct Alias {
    const char* text = nullptr;
    std::size_t zone = 0;
};

struct Open {
    std::size_t zone = 0;  // kMaxZones when the table was full
    std::uint64_t started_us = 0;
    std::uint64_t children_us = 0;
};

std::array<Zone, kMaxZones> g_zones{};
std::size_t g_zone_count = 0;
std::array<Alias, kMaxAliases> g_aliases{};
std::size_t g_alias_count = 0;
std::array<Open, kMaxDepth> g_stack{};
std::size_t g_depth = 0;
// Inclusive time of every zone that had no zone open around it: the
// outermost markers, which for a decode is the frame or access-unit zone.
std::uint64_t g_root_us = 0;
std::uint32_t g_enters = 0;
std::uint32_t g_overflows = 0;
bool g_ever_active = false;

std::size_t resolve(const char* name) {
    for (std::size_t i = 0; i < g_alias_count; ++i) {
        if (g_aliases[i].text == name) {
            return g_aliases[i].zone;
        }
    }
    std::size_t zone = g_zone_count;
    for (std::size_t i = 0; i < g_zone_count; ++i) {
        if (std::strcmp(g_zones[i].name, name) == 0) {
            zone = i;
            break;
        }
    }
    if (zone == g_zone_count) {
        if (g_zone_count == kMaxZones) {
            ++g_overflows;
            return kMaxZones;
        }
        g_zones[g_zone_count].name = name;
        ++g_zone_count;
    }
    if (g_alias_count < kMaxAliases) {
        g_aliases[g_alias_count] = Alias{name, zone};
        ++g_alias_count;
    }
    return zone;
}

}  // namespace

namespace ac3::internal::profiling {

void zone_enter(const char* name) {
    ++g_enters;
    g_ever_active = true;
    // Depth is counted past the array so the matching leave still pairs up;
    // only the frame itself is dropped.
    if (g_depth >= kMaxDepth) {
        ++g_overflows;
        ++g_depth;
        return;
    }
    // The lookup runs before the clock is read, so its cost lands on the
    // PARENT's self time rather than inflating this zone's inclusive one.
    const std::size_t zone = resolve(name);
    g_stack[g_depth] = Open{zone, ac3probe::now_us(), 0};
    ++g_depth;
}

void zone_leave() {
    // The clock first, before any bookkeeping, for the same reason as above:
    // what follows belongs to whoever is still open.
    const std::uint64_t now = ac3probe::now_us();
    if (g_depth == 0) {
        ++g_overflows;
        return;
    }
    --g_depth;
    if (g_depth >= kMaxDepth) {
        return;
    }
    const Open& open = g_stack[g_depth];
    const std::uint64_t elapsed = now - open.started_us;
    if (open.zone < kMaxZones) {
        Zone& zone = g_zones[open.zone];
        zone.inclusive_us += elapsed;
        zone.self_us += elapsed - open.children_us;
        ++zone.calls;
    }
    if (g_depth == 0) {
        g_root_us += elapsed;
    } else {
        g_stack[g_depth - 1].children_us += elapsed;
    }
}

}  // namespace ac3::internal::profiling

namespace ac3probe {

void reset_stages() {
    for (std::size_t i = 0; i < g_zone_count; ++i) {
        g_zones[i].inclusive_us = 0;
        g_zones[i].self_us = 0;
        g_zones[i].calls = 0;
    }
    g_depth = 0;
    g_root_us = 0;
    g_enters = 0;
    g_overflows = 0;
}

void report_stages(const char* codec, int frames) {
    if (g_enters == 0 || frames <= 0) {
        return;
    }
    const auto per_frame = [frames](std::uint64_t total) {
        return static_cast<unsigned long>(total / static_cast<std::uint64_t>(frames));
    };
    // Calls per frame in tenths: a zone that runs once per BLOCK of six is
    // 6.0, and one that runs only on the frames that use its tool is a
    // fraction an integer would round to nothing.
    const auto calls_per_frame_tenths = [frames](std::uint32_t calls) {
        return (static_cast<std::uint64_t>(calls) * 10) / static_cast<std::uint64_t>(frames);
    };
    // In the order the zones were first entered, which for a decode is the
    // order the stages run in - more useful to read than sorted by cost, and
    // a reader sorting by cost has the numbers to do it.
    for (std::size_t i = 0; i < g_zone_count; ++i) {
        const Zone& zone = g_zones[i];
        if (zone.calls == 0) {
            continue;
        }
        const std::uint64_t tenths = calls_per_frame_tenths(zone.calls);
        std::printf("%s.stage[%s].us_per_frame=%lu self_us_per_frame=%lu calls_per_frame=%lu.%lu\n",
                    codec, zone.name, per_frame(zone.inclusive_us), per_frame(zone.self_us),
                    static_cast<unsigned long>(tenths / 10),
                    static_cast<unsigned long>(tenths % 10));
    }
    // Against <codec>.us_per_frame: the difference is what runs outside every
    // marker - the probe's own call into the library, the output stage, and
    // whatever nobody has put a zone around yet.
    std::printf("%s.stage_root_us_per_frame=%lu %s.stage_enters_per_frame=%lu\n", codec,
                per_frame(g_root_us), codec, per_frame(g_enters));
    if (g_overflows != 0) {
        std::printf("%s.stage_overflows=%lu\n", codec, static_cast<unsigned long>(g_overflows));
    }
}

bool stages_active() { return g_ever_active; }

std::uint64_t stage_pair_cost_ns() {
    // Measured, not assumed: a thousand empty enter/leave pairs on a
    // throwaway zone, against the same clock the zones use. Both the lookup
    // (a pointer compare on every pass after the first) and the two clock
    // reads are inside the window, which is exactly what a pair costs the
    // zone enclosing it.
    constexpr std::uint32_t kPairs = 1000;
    static constexpr char kProbeZone[] = "stage_timer_self_test";
    const std::uint64_t before = now_us();
    for (std::uint32_t i = 0; i < kPairs; ++i) {
        ac3::internal::profiling::zone_enter(kProbeZone);
        ac3::internal::profiling::zone_leave();
    }
    const std::uint64_t elapsed_us = now_us() - before;
    // The self test's own zone must not appear in a fixture's report, and its
    // enters must not make stages_active() true on a build that never calls
    // the library's markers. Undo both.
    reset_stages();
    g_ever_active = false;
    if (g_zone_count > 0 && g_zones[g_zone_count - 1].name == kProbeZone) {
        --g_zone_count;
        if (g_alias_count > 0 && g_aliases[g_alias_count - 1].text == kProbeZone) {
            --g_alias_count;
        }
    }
    return (elapsed_us * 1000) / kPairs;
}

}  // namespace ac3probe
