// The AC-4 decoder probe (planning/ac4.md, D14a): iclforge::ac4dec decoding committed AC-4
// streams on a target with no operating system, no filesystem and no C++ exceptions, and
// reporting what that cost. The AC-4 half of what probe.cpp is for AC-3 and E-AC-3, and
// like probe.cpp it is neither a demo nor a unit test.
//
// It answers the same three questions, in a place where an answer cannot be fudged by
// the host environment:
//
//   1. Does the decoder LINK at all with the encoder, the containers and the I/O layer
//      absent, and build without exceptions or RTTI? The AC-4 libraries carry no throw,
//      try or catch of their own (std::expected is the error mechanism throughout), and
//      -fno-exceptions is what asserts it.
//
//   2. Does it produce the right audio? Every frame of every fixture in ac4_fixture.hpp is
//      decoded, and each channel's RMS is compared with what the same library wrote on the
//      host, in double.
//
//   3. What does it cost? Peak heap in bytes, allocations per frame split between the first
//      frame and the steady state, the stack a decode used, and (on the instruction-counting
//      leg) instructions per frame.
//
// The output is machine-readable key=value lines, in the keys probe.cpp uses where the
// meaning is the same, so tools/checks/run_baremetal_probe.sh reads both.
//
// The stack is read by painting: before a fixture's decode the probe fills a window of the
// stack below its own frame with a pattern, and after it looks for the lowest word that no
// longer holds the pattern. Nothing that prints runs between the two, since a printf on
// newlib alone would use a kilobyte or more of the window and be counted as the decoder's.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>

#include "iclforge/ac4/ac4.hpp"
#include "iclforge/ac4dec/decoder.hpp"

#include "ac4_fixture.hpp"
#include "probe.hpp"
#include "stage_timers.hpp"

namespace {

// --- heap accounting -------------------------------------------------------
// Global replacement, so every allocation the archives make is seen and not only the ones
// this file makes. See probe.cpp for why the counters are per frame.
std::size_t g_alloc_calls = 0;
std::size_t g_free_calls = 0;
std::size_t g_live_bytes = 0;
std::size_t g_peak_bytes = 0;
// Every byte ever requested, never decremented: the traffic, where the live figures say
// what is resident.
std::size_t g_alloc_bytes_total = 0;
// The peak restarted at the head of each fixture's decode: the run's peak says what the
// whole probe needed, this says which fixture needed it.
std::size_t g_fixture_peak_bytes = 0;
std::size_t g_largest_alloc = 0;

// Live bytes by allocation size, in power-of-two buckets, snapshotted at the instant of the
// run's peak (bucket i holds sizes 2^i to 2^(i+1)-1): the answer to "what is in it" that a
// peak on its own cannot give.
constexpr std::size_t kBuckets = 32;
std::array<std::size_t, kBuckets> g_live_by_bucket{};
std::array<std::size_t, kBuckets> g_peak_by_bucket{};
std::array<std::size_t, kBuckets> g_live_count_by_bucket{};
std::array<std::size_t, kBuckets> g_peak_count_by_bucket{};

// The same snapshot at each fixture's own peak, with the frame being decoded and the innermost
// stage open then (only a build with ICLFORGE_STAGE_TIMERS has stages): what the run's single
// snapshot cannot say for the fixtures that are not the largest.
std::array<std::size_t, kBuckets> g_fixture_peak_by_bucket{};
std::array<std::size_t, kBuckets> g_fixture_peak_count_by_bucket{};
int g_frame_index = -1;
int g_fixture_peak_frame = -1;
const char* g_fixture_peak_stage = nullptr;

std::size_t size_bucket(std::size_t size) {
    std::size_t bucket = 0;
    while (bucket + 1 < kBuckets && (std::size_t{1} << (bucket + 1)) <= size) {
        ++bucket;
    }
    return bucket;
}

// Room for the size in front of every block, so that operator delete knows it whichever
// form is called.
constexpr std::size_t kHeaderBytes = sizeof(std::size_t) < alignof(std::max_align_t)
                                         ? alignof(std::max_align_t)
                                         : sizeof(std::size_t);

}  // namespace

void* operator new(std::size_t size) {
    // -fno-exceptions: there is no std::bad_alloc to report failure with, and a decoder
    // that runs out of heap has nothing useful to do but say so.
    void* raw = std::malloc(size + kHeaderBytes);
    if (raw == nullptr) {
        std::printf("result=fail reason=out_of_memory bytes=%lu\n",
                    static_cast<unsigned long>(size));
        std::exit(1);
    }
    *static_cast<std::size_t*>(raw) = size;
    ++g_alloc_calls;
    g_alloc_bytes_total += size;
    g_live_bytes += size;
    const std::size_t bucket = size_bucket(size);
    g_live_by_bucket[bucket] += size;
    ++g_live_count_by_bucket[bucket];
    g_largest_alloc = std::max(g_largest_alloc, size);
    if (g_live_bytes > g_peak_bytes) {
        g_peak_bytes = g_live_bytes;
        g_peak_by_bucket = g_live_by_bucket;
        g_peak_count_by_bucket = g_live_count_by_bucket;
    }
    if (g_live_bytes > g_fixture_peak_bytes) {
        g_fixture_peak_bytes = g_live_bytes;
        g_fixture_peak_by_bucket = g_live_by_bucket;
        g_fixture_peak_count_by_bucket = g_live_count_by_bucket;
        g_fixture_peak_frame = g_frame_index;
        g_fixture_peak_stage = iclforge_probe::current_stage();
    }
    return static_cast<std::byte*>(raw) + kHeaderBytes;
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void operator delete(void* p) noexcept {
    if (p == nullptr) {
        return;
    }
    void* raw = static_cast<std::byte*>(p) - kHeaderBytes;
    const std::size_t size = *static_cast<std::size_t*>(raw);
    const std::size_t bucket = size_bucket(size);
    g_live_bytes -= size;
    g_live_by_bucket[bucket] -= size;
    --g_live_count_by_bucket[bucket];
    ++g_free_calls;
    std::free(raw);
}

void operator delete[](void* p) noexcept {
    ::operator delete(p);
}
void operator delete(void* p, std::size_t) noexcept {
    ::operator delete(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    ::operator delete(p);
}

namespace {

// --- the stack -------------------------------------------------------------
// The window painted below the probe's frame, in bytes (ICLFORGE_PROBE_STACK_WINDOW_BYTES,
// from CMake, never defaulted here: the target decides how much stack it can spare).
// Painted a slice at a time by recursion, one array of kSliceBytes a level, so that no
// single stack object is larger than the 4 KiB the library's own code is held to.
constexpr std::size_t kStackWindowBytes = ICLFORGE_PROBE_STACK_WINDOW_BYTES;
constexpr std::size_t kSliceBytes = 1024;
constexpr std::size_t kSliceWords = kSliceBytes / sizeof(std::uint32_t);
constexpr std::size_t kSlices = kStackWindowBytes / kSliceBytes;
constexpr std::uint32_t kPaint = 0xA5C3'5A3CU;

static_assert(kSlices >= 1, "ICLFORGE_PROBE_STACK_WINDOW_BYTES holds no slice");

// Where each level of paint() put its array. Level 0 is the shallowest, nearest the frame
// that called it, at the highest address; the last level is the deepest.
std::array<volatile std::uint32_t*, kSlices> g_slice{};
volatile std::uint32_t g_paint_witness = 0;

// noinline and a read after the recursive call, so that no level is turned into a tail call
// that would reuse the frame of the one before: each level must keep its own array.
[[gnu::noinline]] void paint(std::size_t level) {
    volatile std::uint32_t slice[kSliceWords];
    for (std::size_t i = 0; i < kSliceWords; ++i) {
        slice[i] = kPaint;
    }
    g_slice[level] = slice;
    if (level + 1 < kSlices) {
        paint(level + 1);
    }
    g_paint_witness = g_paint_witness + slice[0];
}

struct StackUse {
    std::size_t bytes = 0;     // from the top of the window to the lowest word changed
    bool window_full = false;  // the deepest slice was touched: bytes is a floor
};

// The lowest changed word of the window, counted from the top of its first slice.
StackUse read_stack() {
    StackUse use;
    const std::uintptr_t top =
        reinterpret_cast<std::uintptr_t>(g_slice[0]) + kSliceBytes;  // NOLINT(*reinterpret-cast)
    for (std::size_t level = kSlices; level-- > 0;) {
        const volatile std::uint32_t* slice = g_slice[level];
        for (std::size_t i = 0; i < kSliceWords; ++i) {
            if (slice[i] != kPaint) {
                use.bytes =
                    top - (reinterpret_cast<std::uintptr_t>(slice) +  // NOLINT(*reinterpret-cast)
                           i * sizeof(std::uint32_t));
                use.window_full = level + 1 == kSlices;
                return use;
            }
        }
    }
    return use;
}

// --- checks ----------------------------------------------------------------
constexpr std::size_t kMaxChannels = 16;

// Sum of squares per channel across every frame, so the RMS at the end is the whole
// fixture's, as tools/generators/gen_baremetal_ac4_fixture.py computed it on the host.
struct LevelAccumulator {
    std::array<double, kMaxChannels> sum_squares{};
    std::array<std::size_t, kMaxChannels> counts{};

    void add(std::size_t channel, std::span<const float> pcm) {
        for (const float sample : pcm) {
            sum_squares[channel] += static_cast<double>(sample) * static_cast<double>(sample);
        }
        counts[channel] += pcm.size();
    }

    [[nodiscard]] std::int32_t rms_scaled(std::size_t channel) const {
        if (counts[channel] == 0) {
            return 0;
        }
        const double rms = std::sqrt(sum_squares[channel] / static_cast<double>(counts[channel]));
        return static_cast<std::int32_t>(rms * 1e6 + 0.5);
    }
};

// Every delivered sample's bit pattern, in delivery order, through FNV-1a, printed as
// <fixture>.pcm_hash. In the float tier the value is the same on every leg that computes in
// IEEE single without fused multiply-add; in the fixed-point tier it is the same on every leg.
// check_probe_hashes.py holds one leg to another where that is the claim.
struct PcmHash {
    std::uint64_t state = 14695981039346656037ULL;

    void add(std::span<const float> pcm) {
        for (const float sample : pcm) {
            const auto bits = std::bit_cast<std::uint32_t>(sample);
            for (int shift = 0; shift < 32; shift += 8) {
                state ^= (bits >> shift) & 0xFFU;
                state *= 1099511628211ULL;
            }
        }
    }
};

// What this part can give the decode in bytes, or zero for whatever it asks for: a fixture
// whose peak is above it is named and skipped. From CMake, as in probe.cpp.
constexpr std::size_t kHeapBudgetBytes = ICLFORGE_PROBE_HEAP_BUDGET_BYTES;

bool g_failed = false;

void fail(const char* fixture, const char* what, long got, long expected) {
    std::printf("check=%s.%s status=fail got=%ld expected=%ld\n", fixture, what, got, expected);
    g_failed = true;
}

// 5% of the expected value with a floor of 2 in the scaled unit (2e-6 of full scale): this
// checks that the decode is RIGHT, not that two floating-point implementations agree bit for
// bit, but the streams have levels down to 36 in that unit, so the floor is the smallest that
// float's rounding stays inside. The fixed-point tier's levels match the same expected values.
bool level_matches(std::int32_t got, std::int32_t expected) {
    const std::int32_t slack = expected / 20 + 2;
    return got >= expected - slack && got <= expected + slack;
}

struct Fixture {
    const char* name;
    std::span<const std::uint8_t> stream;
    int frames;
    std::span<const std::int32_t> rms;
    // The peak this fixture is measured to reach, for the budget check.
    std::size_t peak_bytes;
};

// The decoder's frame at frame_rate_index 13: 2048 samples at 48 kHz, 42.666 ms.
constexpr std::uint64_t kFrameDurationUs =
    static_cast<std::uint64_t>(iclforge_probe::kAc4SamplesPerFrame) * 1000000ULL /
    static_cast<std::uint64_t>(iclforge_probe::kAc4SampleRateHz);

struct Measured {
    std::size_t first_frame_allocs = 0;
    std::size_t first_frame_bytes = 0;
    std::size_t steady_allocs = 0;
    std::size_t steady_bytes = 0;
    // Time inside decode_by_block() and flush() only: the level accumulation and the hash in
    // the sink are the probe's cost, not the decoder's, and are taken back out.
    std::uint64_t decode_us = 0;
    std::uint64_t sink_us = 0;
    StackUse stack;
};

// The peak of the stack over the whole run, for the line at the end.
std::size_t g_stack_peak_bytes = 0;

int decode_fixture(const Fixture& fixture) {
    const std::span<const std::byte> bytes{
        reinterpret_cast<const std::byte*>(fixture.stream.data()), fixture.stream.size()};
    const iclforge::ac4::ScanResult scanned = iclforge::ac4::scan(bytes);
    if (scanned.stopped_at.has_value() ||
        static_cast<int>(scanned.frames.size()) != fixture.frames) {
        fail(fixture.name, "sync_frames", static_cast<long>(scanned.frames.size()), fixture.frames);
        return 1;
    }
    if (fixture.rms.size() > kMaxChannels) {
        fail(fixture.name, "channels", static_cast<long>(fixture.rms.size()),
             static_cast<long>(kMaxChannels));
        return 1;
    }

    LevelAccumulator levels;
    PcmHash hash;
    Measured measured;
    std::size_t delivered = 0;
    std::size_t sink_channels = 0;
    bool frames_ok = true;
    g_fixture_peak_bytes = g_live_bytes;
    g_fixture_peak_by_bucket = g_live_by_bucket;
    g_fixture_peak_count_by_bucket = g_live_count_by_bucket;
    g_frame_index = -1;
    g_fixture_peak_frame = -1;
    g_fixture_peak_stage = nullptr;

    {
        iclforge::ac4::Decoder decoder;
        const auto sink = [&](const iclforge::ac4::PcmBlock& block) {
            const std::uint64_t t0 = iclforge_probe::now_us();
            sink_channels = block.channels.size();
            for (std::size_t channel = 0; channel < block.channels.size() && channel < kMaxChannels;
                 ++channel) {
                levels.add(channel, block.channels[channel]);
                hash.add(block.channels[channel]);
            }
            delivered += block.samples;
            measured.sink_us += iclforge_probe::now_us() - t0;
        };

        // Nothing that prints between here and read_stack() below.
        paint(0);
        int index = 0;
        for (const iclforge::ac4::SyncFrame& frame : scanned.frames) {
            const std::size_t allocs_before = g_alloc_calls;
            const std::size_t bytes_before = g_alloc_bytes_total;
            const std::uint64_t sink_before = measured.sink_us;
            const std::uint64_t t0 = iclforge_probe::now_us();
            g_frame_index = index;
            const auto decoded = decoder.decode_by_block(frame.raw_ac4_frame, sink);
            const std::uint64_t t1 = iclforge_probe::now_us();
            measured.decode_us += (t1 - t0) - (measured.sink_us - sink_before);
            const std::size_t allocs = g_alloc_calls - allocs_before;
            const std::size_t allocated = g_alloc_bytes_total - bytes_before;
            if (index == 0) {
                measured.first_frame_allocs = allocs;
                measured.first_frame_bytes = allocated;
            } else {
                measured.steady_allocs += allocs;
                measured.steady_bytes += allocated;
            }
            if (!decoded.has_value() || !decoded->has_value() ||
                (*decoded)->samples !=
                    static_cast<std::size_t>(iclforge_probe::kAc4SamplesPerFrame) ||
                (*decoded)->sample_rate_hz != iclforge_probe::kAc4SampleRateHz ||
                (*decoded)->speakers.size() != fixture.rms.size()) {
                frames_ok = false;
            }
            ++index;
        }
        const std::uint64_t t0 = iclforge_probe::now_us();
        const std::uint64_t sink_before = measured.sink_us;
        g_frame_index = index;
        (void)decoder.flush(sink);
        measured.decode_us += (iclforge_probe::now_us() - t0) - (measured.sink_us - sink_before);
        measured.stack = read_stack();
    }

    if (!frames_ok) {
        fail(fixture.name, "frame", 0, 1);
    }
    if (sink_channels != fixture.rms.size()) {
        fail(fixture.name, "channels", static_cast<long>(sink_channels),
             static_cast<long>(fixture.rms.size()));
    }
    const std::size_t expected_samples =
        static_cast<std::size_t>(fixture.frames) *
        static_cast<std::size_t>(iclforge_probe::kAc4SamplesPerFrame);
    // `delivered` counts each block once, not once a channel.
    if (delivered != expected_samples) {
        fail(fixture.name, "samples", static_cast<long>(delivered),
             static_cast<long>(expected_samples));
    }
    if (measured.stack.window_full) {
        fail(fixture.name, "stack_window", static_cast<long>(kStackWindowBytes), 0);
    }
    g_stack_peak_bytes = std::max(g_stack_peak_bytes, measured.stack.bytes);

    const int steady_frames = fixture.frames - 1;
    const auto per_frame = [&](std::size_t total) {
        return static_cast<unsigned long>(
            steady_frames > 0 ? total / static_cast<std::size_t>(steady_frames) : 0);
    };
    std::printf(
        "%s.frames=%d %s.first_frame_allocs=%lu %s.steady_allocs=%lu "
        "%s.steady_allocs_per_frame=%lu\n",
        fixture.name, fixture.frames, fixture.name,
        static_cast<unsigned long>(measured.first_frame_allocs), fixture.name,
        static_cast<unsigned long>(measured.steady_allocs), fixture.name,
        per_frame(measured.steady_allocs));
    std::printf("%s.first_frame_bytes=%lu %s.steady_bytes_per_frame=%lu\n", fixture.name,
                static_cast<unsigned long>(measured.first_frame_bytes), fixture.name,
                per_frame(measured.steady_bytes));
    std::printf("%s.peak_bytes=%lu %s.stack_bytes=%lu\n", fixture.name,
                static_cast<unsigned long>(g_fixture_peak_bytes), fixture.name,
                static_cast<unsigned long>(measured.stack.bytes));
    // The frame is the index of the decode_by_block call the peak fell in, the fixture's
    // frame count for the flush; the stage is "-" outside every marker or without them.
    std::printf("%s.peak_frame=%d %s.peak_stage=%s\n", fixture.name, g_fixture_peak_frame,
                fixture.name, g_fixture_peak_stage != nullptr ? g_fixture_peak_stage : "-");
    for (std::size_t bucket = 0; bucket < kBuckets; ++bucket) {
        if (g_fixture_peak_by_bucket[bucket] == 0) {
            continue;
        }
        std::printf("%s.peak_live[%lu]=%lu count=%lu\n", fixture.name,
                    static_cast<unsigned long>(std::size_t{1} << bucket),
                    static_cast<unsigned long>(g_fixture_peak_by_bucket[bucket]),
                    static_cast<unsigned long>(g_fixture_peak_count_by_bucket[bucket]));
    }
    const std::uint64_t per_frame_us =
        measured.decode_us / static_cast<std::uint64_t>(fixture.frames);
    const std::uint64_t permille = (measured.decode_us * 1000ULL) /
                                   (kFrameDurationUs * static_cast<std::uint64_t>(fixture.frames));
    std::printf("%s.decode_us=%lu %s.us_per_frame=%lu %s.realtime_permille=%lu\n", fixture.name,
                static_cast<unsigned long>(measured.decode_us), fixture.name,
                static_cast<unsigned long>(per_frame_us), fixture.name,
                static_cast<unsigned long>(permille));
    for (std::size_t channel = 0; channel < fixture.rms.size(); ++channel) {
        const std::int32_t got = levels.rms_scaled(channel);
        std::printf("%s.rms[%u]=%ld expected=%ld\n", fixture.name, static_cast<unsigned>(channel),
                    static_cast<long>(got), static_cast<long>(fixture.rms[channel]));
        if (!level_matches(got, fixture.rms[channel])) {
            fail(fixture.name, "rms", got, fixture.rms[channel]);
        }
    }
    std::printf("%s.pcm_hash=%08lx%08lx\n", fixture.name,
                static_cast<unsigned long>(hash.state >> 32),
                static_cast<unsigned long>(hash.state & 0xFFFFFFFFULL));
    return 0;
}

// Every fixture ac4_fixture.hpp carries. peak_bytes is what the fixture is measured to reach
// (its own <fixture>.peak_bytes line), for a part whose budget it may exceed.
constexpr std::array kFixtures = {
    Fixture{"ac4_20_music", iclforge_probe::kAc420MusicStream, iclforge_probe::kAc420MusicFrames,
            iclforge_probe::kAc420MusicRms, 0},
    Fixture{"ac4_20_acpl", iclforge_probe::kAc420AcplStream, iclforge_probe::kAc420AcplFrames,
            iclforge_probe::kAc420AcplRms, 0},
    Fixture{"ac4_51_music", iclforge_probe::kAc451MusicStream, iclforge_probe::kAc451MusicFrames,
            iclforge_probe::kAc451MusicRms, 0},
    Fixture{"ac4_51_acpl", iclforge_probe::kAc451AcplStream, iclforge_probe::kAc451AcplFrames,
            iclforge_probe::kAc451AcplRms, 0},
    Fixture{"ac4_514_tones", iclforge_probe::kAc4514TonesStream, iclforge_probe::kAc4514TonesFrames,
            iclforge_probe::kAc4514TonesRms, 0},
    Fixture{"ac4_20_companding", iclforge_probe::kAc420CompandingStream,
            iclforge_probe::kAc420CompandingFrames, iclforge_probe::kAc420CompandingRms, 0},
};

bool over_budget(const Fixture& fixture) {
    if (kHeapBudgetBytes == 0 || fixture.peak_bytes <= kHeapBudgetBytes) {
        return false;
    }
    std::printf("%s.skipped=heap_budget needed=%lu budget=%lu\n", fixture.name,
                static_cast<unsigned long>(fixture.peak_bytes),
                static_cast<unsigned long>(kHeapBudgetBytes));
    return true;
}

}  // namespace

int iclforge_probe::run() {
    std::printf("profile=ac4-decoder\n");
    std::printf("static.decoder_bytes=%lu stack.window_bytes=%lu\n",
                static_cast<unsigned long>(sizeof(iclforge::ac4::Decoder)),
                static_cast<unsigned long>(kStackWindowBytes));

    for (const Fixture& fixture : kFixtures) {
        if (over_budget(fixture)) {
            continue;
        }
        if (decode_fixture(fixture) != 0) {
            std::printf("result=fail\n");
            return 1;
        }
    }

    // Every decoder this run made is out of scope by now, so whatever is still live is held
    // by something with process lifetime rather than by a frame that forgot to free.
    std::printf("heap.peak_bytes=%lu heap.allocs=%lu heap.frees=%lu heap.retained_bytes=%lu\n",
                static_cast<unsigned long>(g_peak_bytes), static_cast<unsigned long>(g_alloc_calls),
                static_cast<unsigned long>(g_free_calls), static_cast<unsigned long>(g_live_bytes));
    for (std::size_t bucket = 0; bucket < kBuckets; ++bucket) {
        if (g_live_by_bucket[bucket] == 0) {
            continue;
        }
        std::printf("heap.retained_bucket[%lu]=%lu count=%lu\n",
                    static_cast<unsigned long>(std::size_t{1} << bucket),
                    static_cast<unsigned long>(g_live_by_bucket[bucket]),
                    static_cast<unsigned long>(g_live_count_by_bucket[bucket]));
    }
    std::printf("heap.largest_alloc_bytes=%lu\n", static_cast<unsigned long>(g_largest_alloc));
    for (std::size_t bucket = 0; bucket < kBuckets; ++bucket) {
        if (g_peak_by_bucket[bucket] == 0) {
            continue;
        }
        std::printf("heap.peak_bucket[%lu]=%lu count=%lu\n",
                    static_cast<unsigned long>(std::size_t{1} << bucket),
                    static_cast<unsigned long>(g_peak_by_bucket[bucket]),
                    static_cast<unsigned long>(g_peak_count_by_bucket[bucket]));
    }
    std::printf("stack.peak_bytes=%lu\n", static_cast<unsigned long>(g_stack_peak_bytes));
    std::printf("result=%s\n", g_failed ? "fail" : "pass");
    return g_failed ? 1 : 0;
}
