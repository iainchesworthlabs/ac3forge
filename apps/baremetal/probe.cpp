// The minimum-footprint decoder probe (roadmap PF7): ac3::forge_minimal
// decoding real bitstreams on a target with no operating system, no
// filesystem and no C++ exceptions, and reporting what that cost.
//
// It is not a demo and not a unit test. It answers the three questions the
// profile exists to answer, in a place where an answer cannot be fudged by
// the host environment:
//
//   1. Does the decode-only archive LINK at all with the encoder, the
//      containers, the I/O layer and the direct-form transform tables absent?
//      A missing symbol here is a fact about the source list in
//      src/forge/minimal.cmake, and --gc-sections means an unreachable
//      function cannot paper over one.
//
//   2. Does it produce the right audio? Every frame of every fixture in
//      apps/baremetal/fixture.hpp is decoded and each channel's RMS compared
//      against what the same library produced on the host.
//
//   3. What does it actually cost? Peak heap in bytes, allocation counts split
//      between the first frame and the steady state, and the static working
//      set. These are the numbers docs/performance-trend.md's footprint table
//      carries, printed by the thing being measured rather than estimated.
//
// The output is machine-readable (`key=value` lines) so the CI leg can gate on
// it; tools/checks/footprint_report.py parses the same lines.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <vector>

#include "ac3/core/eac3_tools.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/oba/oamd.hpp"
#include "ac3/spatial/spatial.hpp"

#include "fixture.hpp"
#include "probe.hpp"
#include "render_fixture.hpp"
#include "stage_timers.hpp"

namespace {

// --- heap accounting -------------------------------------------------------
// Global replacement, so every allocation the archive makes is seen and not
// only the ones this file makes. The counters are the point of the exercise:
// PF7's requirement is "no heap traffic in the decode loop", and the honest
// way to report progress against it is a number, per frame, that a CI leg can
// hold to a ceiling.
std::size_t g_alloc_calls = 0;
std::size_t g_free_calls = 0;
std::size_t g_live_bytes = 0;
std::size_t g_peak_bytes = 0;
// The same high-water mark, restarted at the head of each fixture's decode:
// the run's peak says what the whole probe needed, this says which fixture
// needed it, which is the question a part with a different budget asks.
std::size_t g_fixture_peak_bytes = 0;

// --- where the peak actually is --------------------------------------------
// A peak-heap number says how much, never what. That is fine while the number
// only has to be under a ceiling, and useless the moment somebody has to make
// it smaller - which is exactly the position the ESP32-S3 port is in, with
// 270,886 bytes of peak against 160,764 bytes of free internal SRAM.
//
// So: live bytes by allocation size, in power-of-two buckets, snapshotted at
// the instant the peak is set. Maintained incrementally (one add per new, one
// subtract per delete) rather than by walking a list of live blocks, so it
// costs a shift and an add on each side and cannot itself allocate.
//
// Bucket i holds allocations of 2^i .. 2^(i+1)-1 bytes. 32 buckets covers
// every size a 32-bit size_t can express.
constexpr std::size_t kBuckets = 32;
std::array<std::size_t, kBuckets> g_live_by_bucket{};
std::array<std::size_t, kBuckets> g_peak_by_bucket{};
std::array<std::size_t, kBuckets> g_live_count_by_bucket{};
std::array<std::size_t, kBuckets> g_peak_count_by_bucket{};
std::size_t g_largest_alloc = 0;

// Exact sizes of the big allocations, not just their bucket.
//
// The buckets answer "how much, in what size range"; they do not answer "which
// buffer", and a 112,640-byte bucket holding eight allocations could be eight
// of one thing or four each of two. Distinguishing those decides which member
// to change, so the exact sizes are worth the eighteen words of storage.
//
// Only allocations at or above the threshold are tracked - the small ones are
// the per-block churn PF7's gap is about, and their sizes are not the question.
constexpr std::size_t kLargeAllocBytes = 8192;
constexpr std::size_t kLargeSlots = 12;
std::array<std::size_t, kLargeSlots> g_large_size{};
std::array<std::size_t, kLargeSlots> g_large_live{};
std::array<std::size_t, kLargeSlots> g_large_peak{};

void note_large_alloc(std::size_t size, bool freeing) {
    if (size < kLargeAllocBytes) {
        return;
    }
    for (std::size_t i = 0; i < kLargeSlots; ++i) {
        if (g_large_size[i] == 0) {
            g_large_size[i] = size;
        }
        if (g_large_size[i] == size) {
            if (freeing) {
                --g_large_live[i];
            } else {
                ++g_large_live[i];
                if (g_large_live[i] > g_large_peak[i]) {
                    g_large_peak[i] = g_large_live[i];
                }
            }
            return;
        }
    }
}

// Every allocation ever made, by size bucket - a COUNT, not a live total, and
// never decremented. The other two bucket arrays answer "how much is resident"
// (at the peak, and at the end); this one answers "how much traffic", which is
// the question PF7's open gap is actually about. A buffer allocated and freed
// every frame never appears in a live figure at all and is exactly the thing
// worth finding.
//
// Size is what identifies the buffer. There is no call site here - a probe
// built -fno-exceptions -Os on a target with no unwind tables cannot walk a
// stack - so the bucket is the handle: match a per-frame count against the
// dimensions in the source and the candidate is usually unique. See
// docs/building.md's gap note for the ones already attributed this way.
std::array<std::size_t, kBuckets> g_churn_count_by_bucket{};

std::size_t size_bucket(std::size_t size) {
    std::size_t bucket = 0;
    while (bucket + 1 < kBuckets && (std::size_t{1} << (bucket + 1)) <= size) {
        ++bucket;
    }
    return bucket;
}

// Two words of bookkeeping per block so operator delete knows the size even
// when the sized form is not the one called.
constexpr std::size_t kHeaderBytes = sizeof(std::size_t) < alignof(std::max_align_t)
                                         ? alignof(std::max_align_t)
                                         : sizeof(std::size_t);

}  // namespace

void* operator new(std::size_t size) {
    // Cannot throw: this profile compiles with -fno-exceptions, so
    // std::bad_alloc is not available to report failure with. A bare-metal
    // decoder that runs out of heap has nothing useful to do anyway - saying
    // so on the console and stopping beats returning null into code that was
    // written to trust operator new.
    void* raw = std::malloc(size + kHeaderBytes);
    if (raw == nullptr) {
        std::printf("result=fail reason=out_of_memory bytes=%lu\n",
                    static_cast<unsigned long>(size));
        std::exit(1);
    }
    *static_cast<std::size_t*>(raw) = size;
    ++g_alloc_calls;
    g_live_bytes += size;
    const std::size_t bucket = size_bucket(size);
    g_live_by_bucket[bucket] += size;
    ++g_live_count_by_bucket[bucket];
    ++g_churn_count_by_bucket[bucket];
    if (size > g_largest_alloc) {
        g_largest_alloc = size;
    }
    note_large_alloc(size, false);
    if (g_live_bytes > g_peak_bytes) {
        g_peak_bytes = g_live_bytes;
        g_peak_by_bucket = g_live_by_bucket;
        g_peak_count_by_bucket = g_live_count_by_bucket;
    }
    if (g_live_bytes > g_fixture_peak_bytes) {
        g_fixture_peak_bytes = g_live_bytes;
    }
    return static_cast<std::byte*>(raw) + kHeaderBytes;
}

void* operator new[](std::size_t size) { return ::operator new(size); }

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
    note_large_alloc(size, true);
    ++g_free_calls;
    std::free(raw);
}

void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

namespace {

// --- no caller-owned PCM ---------------------------------------------------
// The probe decodes through the *_by_block forms: each block of a frame or
// access unit arrives as views onto the decoder's own storage and the levels
// accumulate in place, so the probe holds no PCM at all. It used to hold a
// frame's worth for the *_into forms - 49,152 bytes of .bss at eight channels,
// 73,728 once the 7.1.4 fixture needed twelve - which is exactly the frame an
// integrator's DMA ring no longer needs either, and on an ESP32-S3 it was the
// difference between 280,792 and 257,572 bytes free before a decode began.
// The host suite proves the *_into forms sample for sample against these.
//
// Sixteen is §E3.8.2's cap on a rendered programme, and bounds only the level
// accumulator below; the decoders size their own storage to the stream.
constexpr std::size_t kMaxChannels = 16;

// Every fixture of both generations is checked against kMaxChannels below,
// once, over the two tables - see every_fixture_fits(). One assertion per
// fixture would be a second place to remember when adding one, which is the
// seam those tables exist to remove; there used to be one here for the single
// AC-3 stream, and it is gone because AC-3 has a table now too.

// Sum of squares per channel across every frame, so the RMS at the end is the
// whole fixture's - exactly what tools/generators/gen_baremetal_fixture.py
// computed on the host.
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
        const double rms =
            std::sqrt(sum_squares[channel] / static_cast<double>(counts[channel]));
        return static_cast<std::int32_t>(rms * 1e6 + 0.5);
    }
};

// Every delivered sample's bit pattern, in delivery order, through FNV-1a:
// printed beside the levels as <codec>.pcm_hash. For the fixed-point tier
// (planning/arithmetic-tiers.md) the decode is integer arithmetic and this
// value is the same on every leg - the host, the Cortex-M3, a RISC-V part -
// which is the tier's own gate; for the floating tiers it varies with the
// compiler and is informative only. Runs inside the sinks, whose time the
// rows above take back out of the decode figure.
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

void report_hash(const char* codec, const PcmHash& hash) {
    std::printf("%s.pcm_hash=%08lx%08lx\n", codec,
                static_cast<unsigned long>(hash.state >> 32),
                static_cast<unsigned long>(hash.state & 0xFFFFFFFFULL));
}

// What this part can give the decode, in bytes, or zero for "whatever it
// asks for". A target with less than a fixture needs skips that fixture and
// says so, rather than aborting the run on an allocation nobody can satisfy -
// which is what an ESP32-C3 does on the 7.1.4 fixture: 249,180 bytes free, a
// 238,094-byte peak, and a 6,144-byte request that fails anyway.
//
// A TOTAL is a proxy for what actually decides this, and an optimistic one.
// Both ESP32 parts' heaps are regioned - the C3 reports 249,180 free with a
// largest block of 114,688 - so a peak that packs into the flat newlib heap of
// the arm-none-eabi leg can still fail there. The budget a target sets should
// therefore be what it was OBSERVED to manage, not what its allocator reports
// free.
#ifndef AC3FORGE_PROBE_HEAP_BUDGET_BYTES
#define AC3FORGE_PROBE_HEAP_BUDGET_BYTES 0
#endif
constexpr std::size_t kHeapBudgetBytes = AC3FORGE_PROBE_HEAP_BUDGET_BYTES;

// True when this fixture asks for more than the part has. `needed` is the peak
// the fixture is measured to reach - the same figure the probe prints as
// <codec>.peak_bytes after every fixture that runs, so a stale entry here is
// visible beside the real one rather than only in this table.
bool over_budget(const char* codec, std::size_t needed) {
    if (kHeapBudgetBytes == 0 || needed <= kHeapBudgetBytes) {
        return false;
    }
    std::printf("%s.skipped=heap_budget needed=%lu budget=%lu\n", codec,
                static_cast<unsigned long>(needed),
                static_cast<unsigned long>(kHeapBudgetBytes));
    return true;
}

bool g_failed = false;

void fail(const char* what, long got, long expected) {
    std::printf("check=%s status=fail got=%ld expected=%ld\n", what, got, expected);
    g_failed = true;
}

// The same, for a check a fixture owns rather than the run as a whole. The
// codec prefix is what says WHICH fixture, now that three of them share one
// decode function: "eac3.frames" would name the first of them for a failure in
// any of the three.
void fail(const char* codec, const char* what, long got, long expected) {
    std::printf("check=%s.%s status=fail got=%ld expected=%ld\n", codec, what, got,
                expected);
    g_failed = true;
}

// 5% of the expected value, floored so a near-silent channel is not held to an
// impossible absolute bound. Generous on purpose: this checks that the decode
// is RIGHT, not that two floating-point implementations agree bit for bit -
// the fixture's own levels came from a host build with a different compiler,
// a different libm and (on the Cortex-M3 target) software floating point.
bool level_matches(std::int32_t got, std::int32_t expected) {
    const std::int32_t slack = expected / 20 + 200;
    return got >= expected - slack && got <= expected + slack;
}

void report_levels(const char* codec, const LevelAccumulator& levels,
                   std::span<const std::int32_t> expected) {
    for (std::size_t ch = 0; ch < expected.size(); ++ch) {
        const std::int32_t got = levels.rms_scaled(ch);
        std::printf("%s.rms[%u]=%ld expected=%ld\n", codec, static_cast<unsigned>(ch),
                    static_cast<long>(got), static_cast<long>(expected[ch]));
        if (!level_matches(got, expected[ch])) {
            fail(codec, "rms", got, expected[ch]);
        }
    }
}

struct Churn {
    // Snapshot of g_churn_count_by_bucket at the end of the FIRST frame, and
    // the running total after the last. The difference over the frames between
    // them is the steady state - first-frame allocations are one-off setup
    // (a decoder sizing its state to the stream it just saw) and averaging
    // them in would make every fixture look worse than it steadily is, which
    // is the number that has to reach zero.
    std::array<std::size_t, kBuckets> bucket_at_first{};
    std::array<std::size_t, kBuckets> bucket_at_last{};
    std::size_t first_frame_allocs = 0;
    std::size_t steady_allocs = 0;
    int frames = 0;
    // Time inside decode_frame_into / decode_access_unit_into only. The level
    // accumulation and the allocation bookkeeping around it are the PROBE's
    // cost, not the decoder's, and folding them in would flatter or slander the
    // decoder depending on how expensive they happen to be on a given target.
    std::uint64_t decode_us = 0;
};

// One line per size bucket that saw any steady-state traffic, as a rate per
// frame. Printed after the summary line rather than on it: the summary is what
// the runner scripts gate on and it should stay one line per fixture, while
// this is for a reader working out which buffer to move off the heap.
void report_churn_buckets(const char* codec, const Churn& churn) {
    const int steady_frames = churn.frames - 1;
    if (steady_frames <= 0) {
        return;
    }
    for (std::size_t bucket = 0; bucket < kBuckets; ++bucket) {
        const std::size_t traffic =
            churn.bucket_at_last[bucket] - churn.bucket_at_first[bucket];
        if (traffic == 0) {
            continue;
        }
        // Tenths, because several of these are below one per frame - a buffer
        // allocated once per BLOCK of six is 0.17 - and an integer rate would
        // round every one of them to zero and hide it.
        const std::size_t per_frame_tenths =
            (traffic * 10) / static_cast<std::size_t>(steady_frames);
        std::printf("%s.churn_bucket[%lu]=%lu.%lu count=%lu\n", codec,
                    static_cast<unsigned long>(std::size_t{1} << bucket),
                    static_cast<unsigned long>(per_frame_tenths / 10),
                    static_cast<unsigned long>(per_frame_tenths % 10),
                    static_cast<unsigned long>(traffic));
    }
}

void report_churn(const char* codec, const Churn& churn) {
    const int steady_frames = churn.frames - 1;
    std::printf("%s.frames=%d %s.first_frame_allocs=%lu %s.steady_allocs=%lu "
                "%s.steady_allocs_per_frame=%lu\n",
                codec, churn.frames, codec,
                static_cast<unsigned long>(churn.first_frame_allocs), codec,
                static_cast<unsigned long>(churn.steady_allocs), codec,
                static_cast<unsigned long>(steady_frames > 0
                                               ? churn.steady_allocs /
                                                     static_cast<std::size_t>(steady_frames)
                                               : 0));
    // Its own line rather than a field of the summary above, which the runner
    // scripts parse one key at a time and which should stay one line per
    // fixture. Bytes live at the highest point of this fixture's decode,
    // whatever earlier fixtures left resident.
    std::printf("%s.peak_bytes=%lu\n", codec, static_cast<unsigned long>(g_fixture_peak_bytes));
}

// A frame is 1536 samples at 48 kHz - 32 ms of audio. Real time means decoding
// one in less than that, so the ratio is the headline: below 1.0 the target
// keeps up, above 1.0 it does not. Reported as a permille integer because
// newlib-nano's printf has no floating-point support unless -u _printf_float is
// linked in, and a probe whose subject is footprint should not drag that in
// just to print a number (the same reason fixture.hpp stores RMS scaled by 1e6).
constexpr std::uint64_t kFrameDurationUs = 32000;

void report_timing(const char* codec, const Churn& churn) {
    if (churn.frames <= 0) {
        return;
    }
    const std::uint64_t per_frame = churn.decode_us / static_cast<std::uint64_t>(churn.frames);
    const std::uint64_t permille = (churn.decode_us * 1000) /
                                   (kFrameDurationUs * static_cast<std::uint64_t>(churn.frames));
    std::printf("%s.decode_us=%lu %s.us_per_frame=%lu %s.realtime_permille=%lu\n", codec,
                static_cast<unsigned long>(churn.decode_us), codec,
                static_cast<unsigned long>(per_frame), codec,
                static_cast<unsigned long>(permille));
}

// Every AC-3 fixture goes through this one function, exactly as every E-AC-3
// fixture goes through decode_eac3 below. What differs between them is the
// layout and the tools the ENCODER chose, which is a property of the bitstream
// rather than of the call: decode_frame_into's contract is the same for all of
// them, and a per-fixture copy of this loop would only give three places for a
// check to be dropped from.
int decode_ac3(const char* codec, std::span<const std::uint8_t> bytes,
               std::span<const std::int32_t> expected, const ac3::OutputConfig& output) {
    const std::span<const std::byte> stream{
        reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()};
    const auto frames = ac3::split_frames(stream);
    if (!frames) {
        std::printf("check=%s.split status=fail error=%d\n", codec,
                    static_cast<int>(frames.error()));
        return 1;
    }

    ac3::DecoderConfig config;
    config.output = output;
    ac3::FrameDecoder decoder{config};
    LevelAccumulator levels;
    PcmHash hash;
    Churn churn;
    churn.frames = static_cast<int>(frames->size());
    g_fixture_peak_bytes = g_live_bytes;
    ac3probe::reset_stages();
    std::size_t before = g_alloc_calls;
    int index = 0;
    int channels = 0;
    for (const auto frame : *frames) {
        // The block form: the decoder hands each block over from its own
        // storage and the levels accumulate in place, so the probe holds no
        // PCM at all. The sink's own work runs inside the decode call; it is
        // timed there and taken back out, so what is reported is the
        // decoder's cost rather than the probe's.
        std::uint64_t sink_us = 0;
        int delivered = 0;
        const auto sink = [&](const ac3::PcmBlock& block) {
            const std::uint64_t entered_us = ac3probe::now_us();
            for (std::size_t ch = 0; ch < block.channels.size() && ch < kMaxChannels; ++ch) {
                levels.add(ch, block.channels[ch]);
                hash.add(block.channels[ch]);
            }
            delivered = static_cast<int>(block.channels.size());
            sink_us += ac3probe::now_us() - entered_us;
        };
        const std::uint64_t started_us = ac3probe::now_us();
        const auto decoded = decoder.decode_frame_by_block(frame, sink);
        const std::uint64_t elapsed_us = ac3probe::now_us() - started_us;
        churn.decode_us += elapsed_us > sink_us ? elapsed_us - sink_us : 0;
        if (!decoded) {
            std::printf("check=%s.decode status=fail frame=%d error=%d\n", codec, index,
                        static_cast<int>(decoded.error()));
            return 1;
        }
        channels = delivered;
        if (index == 0) {
            churn.first_frame_allocs = g_alloc_calls - before;
            churn.bucket_at_first = g_churn_count_by_bucket;
        } else {
            churn.steady_allocs += g_alloc_calls - before;
        }
        churn.bucket_at_last = g_churn_count_by_bucket;
        before = g_alloc_calls;
        ++index;
    }

    if (churn.frames != ac3probe::kFrames) {
        fail(codec, "frames", churn.frames, ac3probe::kFrames);
    }
    if (channels != static_cast<int>(expected.size())) {
        fail(codec, "channels", channels, static_cast<long>(expected.size()));
    }
    report_levels(codec, levels, expected);
    report_hash(codec, hash);
    report_churn(codec, churn);
    report_churn_buckets(codec, churn);
    report_timing(codec, churn);
    // Where the time above went, when the library was built to say
    // (AC3FORGE_STAGE_TIMERS); silent otherwise.
    ac3probe::report_stages(codec, churn.frames);
    return 0;
}

// Every E-AC-3 fixture goes through this one function. What differs between
// them is the tool set and the layout the ENCODER chose, which is a property
// of the bitstream rather than of the call: decode_access_unit_into's contract
// is the same for all of them, and a per-fixture copy of this loop would only
// give three places for a check to be dropped from.
//
// `codec` prefixes every line this emits, so each fixture's levels, churn and
// timing stay separable in the output the runner scripts gate on.
int decode_eac3(const char* codec, std::span<const std::uint8_t> bytes,
                std::span<const std::int32_t> expected, bool bed_only,
                ac3::oba::joc::Domain domain, const ac3::OutputConfig& output) {
    const std::span<const std::byte> stream{
        reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()};
    const auto units = ac3::split_access_units(stream);
    if (!units) {
        std::printf("check=%s.split status=fail error=%d\n", codec,
                    static_cast<int>(units.error()));
        return 1;
    }

    ac3::DecoderConfig config;
    config.output = output;
    config.joc_domain = domain;
    config.skip_object_reconstruction = bed_only;
    ac3::Eac3Decoder decoder{config};
    LevelAccumulator levels;
    PcmHash hash;
    Churn churn;
    churn.frames = static_cast<int>(units->size());
    g_fixture_peak_bytes = g_live_bytes;
    ac3probe::reset_stages();
    std::size_t before = g_alloc_calls;
    int index = 0;
    int channels = 0;
    for (const auto unit : *units) {
        // The block form, as for AC-3 above: views onto the decoder's own
        // substream vectors, no PCM held here, the sink's time taken back out.
        std::uint64_t sink_us = 0;
        int delivered = 0;
        const auto sink = [&](const ac3::PcmBlock& block) {
            const std::uint64_t entered_us = ac3probe::now_us();
            for (std::size_t ch = 0; ch < block.channels.size() && ch < kMaxChannels; ++ch) {
                levels.add(ch, block.channels[ch]);
                hash.add(block.channels[ch]);
            }
            delivered = static_cast<int>(block.channels.size());
            sink_us += ac3probe::now_us() - entered_us;
        };
        const std::uint64_t started_us = ac3probe::now_us();
        const auto decoded = decoder.decode_access_unit_by_block(unit, sink);
        const std::uint64_t elapsed_us = ac3probe::now_us() - started_us;
        churn.decode_us += elapsed_us > sink_us ? elapsed_us - sink_us : 0;
        if (!decoded) {
            std::printf("check=%s.decode status=fail unit=%d error=%d\n", codec, index,
                        static_cast<int>(decoded.error()));
            return 1;
        }
        // std::nullopt is the §3.7 hold-back, not an error, and the sink is
        // not called for it. No fixture here selects transient pre-noise
        // processing - neither "all" nor "cpl+ecpl" includes tpn - so none of
        // them takes this branch today; handled anyway so a fixture that DOES
        // use it fails on levels rather than on a silent miscount.
        if (decoded->has_value()) {
            channels = delivered;
        }
        if (index == 0) {
            churn.first_frame_allocs = g_alloc_calls - before;
            churn.bucket_at_first = g_churn_count_by_bucket;
        } else {
            churn.steady_allocs += g_alloc_calls - before;
        }
        churn.bucket_at_last = g_churn_count_by_bucket;
        before = g_alloc_calls;
        ++index;
    }

    if (churn.frames != ac3probe::kFrames) {
        fail(codec, "frames", churn.frames, ac3probe::kFrames);
    }
    if (channels != static_cast<int>(expected.size())) {
        fail(codec, "channels", channels, static_cast<long>(expected.size()));
    }
    report_levels(codec, levels, expected);
    report_hash(codec, hash);
    report_churn(codec, churn);
    report_churn_buckets(codec, churn);
    report_timing(codec, churn);
    // Where the time above went, when the library was built to say
    // (AC3FORGE_STAGE_TIMERS); silent otherwise.
    ac3probe::report_stages(codec, churn.frames);
    return 0;
}

// --- objects onto loudspeakers ---------------------------------------------
// The objects row proves the objects come back; this proves they can be
// PLACED, on the target, which is what a part driving a 7.1.4 DAC has to do
// with them. The layout is 7.1.4 - the twelve the eac3_714 fixture decodes
// for - and the render is the one ac3cli's `qc objects=714` performs: every
// full-bandwidth target starts silent and each object's own recovered audio
// is summed into it by the object's own OAMD position, through
// ac3::spatial::pan_direction, the same height-aware geometry the encoder
// panned with; the bed's LFE passes through as the twelfth slot. The bed's
// other five channels are NOT added: for a dynamic-object-only programme the
// bed IS the objects' 5.1 fold, and adding it would render every object
// twice.
//
// Through the block form, decode_access_unit_by_block, whose PcmBlock carries
// the objects beside the bed: a view per object onto the unit's own
// reconstruction, cut to the block, with the metadata that places them. So
// nothing is copied - not the bed, not the objects - and the output is a
// block per target (g_render_block, static: twelve channels of one
// 256-sample block), which is what a player holds too. The pan is
// trigonometry in double, once per object per unit, on the unit's first
// block; the per-sample sums are float, on the FPU.
//
// The reference levels are the host shape's own - see render_fixture.hpp for
// why this row, alone among the decode rows, is a regression reference.
constexpr std::size_t kRenderBlock = 256;
constexpr std::size_t kRenderSlots = 12;
constexpr std::size_t kMaxObjects = 16;
std::array<std::array<float, kRenderBlock>, kRenderSlots> g_render_block{};
// Each object's gain onto each slot, refreshed on a unit's first block. Static
// rather than a local of render_eac3 for the reason the PCM block used to be:
// 1,536 bytes of it on the main task's stack left the ESP32-S3 48 bytes above
// the runner's floor.
std::array<std::array<double, kRenderSlots>, kMaxObjects> g_render_gains{};

int render_eac3(const char* codec, std::span<const std::uint8_t> bytes,
                std::span<const std::int32_t> expected, ac3::oba::joc::Domain domain) {
    using ac3::eac3::chanmap::Location;
    const std::span<const std::byte> stream{
        reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()};
    const auto units = ac3::split_access_units(stream);
    if (!units) {
        std::printf("check=%s.split status=fail error=%d\n", codec,
                    static_cast<int>(units.error()));
        return 1;
    }

    // 7.1.4 in Table E2.5 order. pan_targets drops the LFE from the panned
    // set; it is carried as the last slot below.
    constexpr auto kTargetMap = static_cast<std::uint16_t>(
        ac3::eac3::chanmap::acmod_map(ac3::Acmod::k3_2, true) | ac3::eac3::chanmap::k71Rear |
        ac3::eac3::chanmap::kTopQuad);
    constexpr auto kTargetLayout = ac3::eac3::chanmap::expand(kTargetMap);
    std::array<Location, kRenderSlots> target_locations{};
    std::size_t target_count = 0;
    for (const Location location : kTargetLayout) {
        if (target_count < target_locations.size()) {
            target_locations[target_count++] = location;
        }
    }
    const auto targets = ac3::spatial::pan_targets(
        std::span<const Location>(target_locations.data(), target_count));
    const std::size_t panned = targets.directions.size();
    if (panned + 1 != kRenderSlots) {
        fail(codec, "targets", static_cast<long>(panned + 1), static_cast<long>(kRenderSlots));
        return 1;
    }

    ac3::DecoderConfig config;
    config.joc_domain = domain;
    ac3::Eac3Decoder decoder{config};
    LevelAccumulator levels;
    PcmHash hash;
    // The bed's own slots, so the LFE can be picked out of them once the
    // layout is known - the block carries the samples in the layout's order
    // but not the layout, which the call returns afterwards.
    LevelAccumulator bed_levels;
    ac3::eac3::chanmap::Layout layout{};
    Churn churn;
    churn.frames = static_cast<int>(units->size());
    g_fixture_peak_bytes = g_live_bytes;
    ac3probe::reset_stages();
    std::size_t before = g_alloc_calls;
    std::uint64_t render_us = 0;
    int index = 0;
    int channels = 0;
    auto& gains = g_render_gains;
    std::size_t object_count = 0;
    for (const auto unit : *units) {
        std::uint64_t sink_us = 0;
        std::uint64_t levels_us = 0;
        bool delivered = false;
        const auto sink = [&](const ac3::PcmBlock& block) {
            const std::uint64_t entered_us = ac3probe::now_us();
            if (block.index == 0) {
                // Each object's gains onto the panned targets, once per unit.
                const auto objects = block.object_metadata != nullptr
                                         ? ac3::oba::describe_objects(*block.object_metadata)
                                         : std::vector<ac3::oba::DisplayObject>{};
                object_count = std::min({objects.size(), block.objects.size(), kMaxObjects});
                for (std::size_t i = 0; i < object_count; ++i) {
                    gains[i].fill(0.0);
                    if (!objects[i].active) {
                        continue;
                    }
                    const auto direction = ac3::spatial::position_direction(
                        objects[i].position.x, objects[i].position.y, objects[i].position.z);
                    ac3::spatial::pan_direction(direction, targets.directions,
                                                std::span<double>(gains[i].data(), panned));
                    const double linear = std::pow(10.0, objects[i].gain_db / 20.0);
                    for (std::size_t t = 0; t < panned; ++t) {
                        gains[i][t] *= linear;
                    }
                }
            }
            const std::size_t n =
                block.channels.empty() ? 0 : std::min(block.channels.front().size(), kRenderBlock);
            for (std::size_t t = 0; t < panned; ++t) {
                g_render_block[t].fill(0.0F);
            }
            for (std::size_t i = 0; i < object_count && i < block.objects.size(); ++i) {
                const auto audio = block.objects[i];
                if (audio.size() < n) {
                    continue;
                }
                for (std::size_t t = 0; t < panned; ++t) {
                    if (gains[i][t] <= 0.0) {
                        continue;
                    }
                    const auto g = static_cast<float>(gains[i][t]);
                    auto& slot = g_render_block[t];
                    for (std::size_t k = 0; k < n; ++k) {
                        slot[k] += g * audio[k];
                    }
                }
            }
            delivered = true;
            // The probe's own level sums are double arithmetic - software on
            // the part - and no part of the render; taken back out of both
            // times, as the sinks' time is in the rows above.
            const std::uint64_t levels_started_us = ac3probe::now_us();
            for (std::size_t t = 0; t < panned; ++t) {
                levels.add(t, std::span<const float>(g_render_block[t].data(), n));
                hash.add(std::span<const float>(g_render_block[t].data(), n));
            }
            for (std::size_t ch = 0; ch < block.channels.size() && ch < kMaxChannels; ++ch) {
                bed_levels.add(ch, block.channels[ch]);
            }
            levels_us += ac3probe::now_us() - levels_started_us;
            sink_us += ac3probe::now_us() - entered_us;
        };
        const std::uint64_t started_us = ac3probe::now_us();
        const auto decoded = decoder.decode_access_unit_by_block(unit, sink);
        const std::uint64_t elapsed_us = ac3probe::now_us() - started_us;
        if (!decoded) {
            std::printf("check=%s.decode status=fail unit=%d error=%d\n", codec, index,
                        static_cast<int>(decoded.error()));
            return 1;
        }
        // The row's time is decode AND render, less the level sums; the
        // render's own share is kept apart for its own line.
        churn.decode_us += elapsed_us > levels_us ? elapsed_us - levels_us : 0;
        render_us += sink_us > levels_us ? sink_us - levels_us : 0;
        // std::nullopt is the §3.7 hold-back, as in decode_eac3 above.
        if (decoded->has_value() && delivered) {
            layout = (*decoded)->layout;
            channels = static_cast<int>(kRenderSlots);
        }
        if (index == 0) {
            churn.first_frame_allocs = g_alloc_calls - before;
            churn.bucket_at_first = g_churn_count_by_bucket;
        } else {
            churn.steady_allocs += g_alloc_calls - before;
        }
        churn.bucket_at_last = g_churn_count_by_bucket;
        before = g_alloc_calls;
        ++index;
    }

    // The bed's LFE, passed through as the twelfth slot.
    const int lfe = layout.index_of(Location::kLfe);
    if (lfe >= 0 && static_cast<std::size_t>(lfe) < kMaxChannels) {
        levels.sum_squares[panned] = bed_levels.sum_squares[static_cast<std::size_t>(lfe)];
        levels.counts[panned] = bed_levels.counts[static_cast<std::size_t>(lfe)];
    }

    if (churn.frames != ac3probe::kFrames) {
        fail(codec, "frames", churn.frames, ac3probe::kFrames);
    }
    if (channels != static_cast<int>(expected.size())) {
        fail(codec, "channels", channels, static_cast<long>(expected.size()));
    }
    report_levels(codec, levels, expected);
    report_hash(codec, hash);
    report_churn(codec, churn);
    report_churn_buckets(codec, churn);
    report_timing(codec, churn);
    const auto frames = static_cast<std::uint64_t>(churn.frames > 0 ? churn.frames : 1);
    std::printf("%s.render_us=%lu %s.render_us_per_frame=%lu\n", codec,
                static_cast<unsigned long>(render_us), codec,
                static_cast<unsigned long>(render_us / frames));
    ac3probe::report_stages(codec, churn.frames);
    return 0;
}

// The AC-3 fixtures, in the order the probe decodes them. A table for the same
// reason the E-AC-3 one below is: adding a configuration should be a row here
// and a stream in tools/generators/gen_baremetal_fixture.py, not a fourth copy
// of a decode loop.
//
// It was a single hardcoded decode of the 5.1 stream until these rows arrived,
// which left two AC-3 paths linked into every build of this profile and
// executed by none of them - §7.5.4 rematrixing, which exists in 2/0 and no
// other layout, and the UNCOUPLED path, because the one fixture there was
// passed `couple`. That is the same shape of gap enhanced coupling had on the
// E-AC-3 side, found the same way: by asking what the fixtures do not reach
// rather than by anything failing.
struct Ac3Fixture {
    const char* codec;
    std::span<const std::uint8_t> stream;
    std::span<const std::int32_t> rms;
    // The peak heap this fixture is measured to reach. The same on every leg:
    // the allocator counts bytes, and x86-64, Thumb-2 and RV32IMC allocate the
    // same ones. Read only by over_budget() above.
    std::size_t peak_bytes;
    // DecoderConfig::output. As coded for every row but the fold, which is
    // the §7.8 stage a stereo player runs every frame.
    ac3::OutputConfig output{};
};

constexpr std::array<Ac3Fixture, 4> kAc3Fixtures{{
    {"ac3", ac3probe::kAc3Stream, ac3probe::kAc3Rms, 56685},
    // The same stream folded to Lo/Ro in line mode (§7.8.1 with §5.4.2.8's
    // dialnorm normalisation): what i2s_player does to every frame on the
    // way to a stereo DAC, and the output stage's first row on any target.
    // Levels are ac3cli's for the same options (tools/generators/
    // gen_baremetal_fixture.py's decode-variant rows), two channels.
    {"ac3_fold", ac3probe::kAc3Stream, ac3probe::kAc3FoldRms, 68973,
     {.target = ac3::DownmixTarget::kLoRo, .mode = ac3::OperatingMode::kLine}},
    // 2/0. §7.5.4 rematrixing lives in this layout alone, and it is a different
    // code path from the eac3_stereo row's - Annex E carries its own
    // rematrixing syntax - so that fixture does not stand in for this one.
    // Also the first AC-3 fixture whose channel count is not six.
    {"ac3_stereo", ac3probe::kAc3StereoStream, ac3probe::kAc3StereoRms, 49328},
    // 1/0. The narrowest programme the syntax has: one full-bandwidth channel,
    // no LFE, no coupling possible (§7.4 needs two channels to share a band
    // between) and no downmix to apply. Every per-channel loop in the decoder
    // runs exactly once here, which is the value 6 cannot catch an off-by-one
    // in.
    {"ac3_mono", ac3probe::kAc3MonoStream, ac3probe::kAc3MonoRms, 47608},
}};

// The E-AC-3 fixtures, in the order the probe decodes them. Adding one is a
// row here and a stream in tools/generators/gen_baremetal_fixture.py's own
// table; nothing else in this file changes, and neither runner script names a
// fixture - both gate every `<name>.steady_allocs_per_frame` line the probe
// prints.
struct Eac3Fixture {
    const char* codec;
    std::span<const std::uint8_t> stream;
    std::span<const std::int32_t> rms;
    // The peak heap this fixture is measured to reach. The same on every leg:
    // the allocator counts bytes, and x86-64, Thumb-2 and RV32IMC allocate the
    // same ones. Read only by over_budget() above.
    std::size_t peak_bytes;
    // DecoderConfig::skip_object_reconstruction. Only the Atmos fixture sets
    // it, and it is the whole reason that fixture can be here: see its row.
    bool bed_only = false;
    // DecoderConfig::joc_domain. Only the object row sets it; see there.
    ac3::oba::joc::Domain joc_domain = ac3::oba::joc::Domain::kQmf;
    // DecoderConfig::output. As coded for every row but the fold.
    ac3::OutputConfig output{};
    // Render the objects onto 7.1.4 (render_eac3) instead of accumulating
    // the decoded channels' levels. Only the render row sets it.
    bool render = false;
};

constexpr std::array<Eac3Fixture, 8> kEac3Fixtures{{
    {"eac3", ac3probe::kEac3Stream, ac3probe::kEac3Rms, 175674},
    // §E3.5's alternate coupling mode. `tools=all` does not select it
    // (plan::parse_tools maps "all" to cpl+spx+aht), so without this row
    // ecpl_channel_spectrum - and the 512-point DFT
    // src/forge/src/core/fft.cpp is in the minimal source list for - are
    // linked into every build of this profile and executed by none of them.
    {"eac3_ecpl", ac3probe::kEac3EcplStream, ac3probe::kEac3EcplRms, 159141},
    // An Atmos stream decoded for its BED. §6 object reconstruction allocates
    // an oba::joc::ReconstructionState - 147,504 bytes in one block, plus a
    // QmfState and its filterbanks - which is more than the largest free run
    // this decode leaves on an ESP32-S3, so a full decode of this stream dies
    // in operator new partway through. The bed does not: it is ordinary
    // E-AC-3, and this row is what proves that on the target rather than in a
    // paragraph. Levels are the bed's, which is what ac3cli decode writes for
    // an Atmos stream too, so the host reference needed no special case.
    {"eac3_atmos_bed", ac3probe::kEac3AtmosBedStream, ac3probe::kEac3AtmosBedRms, 125383,
     true},
    // The Atmos bitstream again, this time reconstructing its objects. Two
    // rows off one stream: it is already linked in, so the second path costs
    // nothing in image size, and what differs is a decoder setting.
    //
    // kMdctBand rather than the kQmf default, which is what makes it fit -
    // kQmf allocates a QmfState and two filterbanks on top and peaks at
    // 449,826 bytes. This is the configuration an embedded integrator would
    // use, not the reference one.
    //
    // It runs AFTER the enhanced-coupling row on purpose. That ordering used
    // to fail outright - ecpl leaves its thread_local spectrum scratch behind
    // (23,552 bytes on this profile, 32,768 in double; it was 34,232 with the
    // bin-angle vector that is a stack array now) on a target whose thread
    // never exits, and object reconstruction then had nowhere to go. release_ecpl_scratch() below is what makes the
    // order stop mattering, so this row sits where it would naturally rather
    // than where it happens to pass.
    {"eac3_atmos_objects", ac3probe::kEac3AtmosBedStream, ac3probe::kEac3AtmosBedRms, 211851,
     false, ac3::oba::joc::Domain::kMdctBand},
    // 2/0, and Annex E's own rematrixing syntax - the E-AC-3 half of what the
    // ac3_stereo row covers for AC-3. Also the first E-AC-3 fixture whose
    // channel count is not six, so the layout-driven half of the level check is
    // exercised rather than merely written.
    {"eac3_stereo", ac3probe::kEac3StereoStream, ac3probe::kEac3StereoRms, 144278},
    // 7.1.4: a 5.1 bed and two dependent substreams (k71Rear and kTopQuad),
    // the widest programme the encoder makes and the first fixture with more
    // channels than one substream can carry. The access unit's assembly -
    // locations unioned across substreams, a dependent's surrounds replacing
    // the bed's - runs here and nowhere else in this table, and twelve
    // channels of output is what a part driving a 7.1.4 DAC over TDM pays
    // for, in this probe's own PCM block as on the part.
    {"eac3_714", ac3probe::kEac3714Stream, ac3probe::kEac3714Rms, 238094},
    // The 5.1 stream folded to Lo/Ro in line mode - the E-AC-3 half of the
    // ac3_fold row, through the access-unit form's own output path.
    {"eac3_fold", ac3probe::kEac3Stream, ac3probe::kEac3FoldRms, 225038, false,
     ac3::oba::joc::Domain::kQmf,
     {.target = ac3::DownmixTarget::kLoRo, .mode = ac3::OperatingMode::kLine}},
    // Objects reconstructed (kMdctBand, as the objects row) and then PLACED
    // onto 7.1.4 by their own positions - see render_eac3, and
    // render_fixture.hpp for what the levels are worth. Its own stream: the
    // same source as the Atmos rows with three objects raised to the ceiling
    // and one half way (tools/generators/atmos_height_scene.txt), because the
    // Atmos rows' objects all sit on the listener plane and a render of them
    // would leave the four height targets silent and untested.
    {"eac3_atmos_render", ac3probe::kEac3AtmosHeightStream, ac3probe::kEac3AtmosRenderRms,
     212221, false, ac3::oba::joc::Domain::kMdctBand, {}, true},
}};

// What the per-fixture static_asserts above used to say, said once. Regenerate
// fixture.hpp with a layout wider than the PCM block and the build stops here,
// instead of decode_frame_into or decode_access_unit_into writing past the end
// of a span.
//
// Both tables, in one function. Two of these - one per generation - would be a
// second place to remember, which is the seam these tables exist to remove.
consteval bool every_fixture_fits() {
    for (const auto& fixture : kAc3Fixtures) {
        if (fixture.rms.size() > kMaxChannels) {
            return false;
        }
    }
    for (const auto& fixture : kEac3Fixtures) {
        if (fixture.rms.size() > kMaxChannels) {
            return false;
        }
    }
    return true;
}

static_assert(every_fixture_fits(),
              "a fixture has more channels than the probe's PCM block holds - raise "
              "kMaxChannels");

// The profile's one behavioural difference, checked rather than asserted in a
// comment: asking for the direct-form transform this build does not carry is
// refused, and refused with a code that says so, instead of being quietly
// served by the fast path.
void check_reference_transform_refused() {
    const std::span<const std::byte> stream{
        reinterpret_cast<const std::byte*>(ac3probe::kAc3Stream.data()),
        ac3probe::kAc3Stream.size()};
    const auto frames = ac3::split_frames(stream);
    if (!frames || frames->empty()) {
        fail("reference.setup", 0, 1);
        return;
    }
    ac3::FrameDecoder decoder{{.fast_imdct = false}};
    // The sink is never reached: the refusal is what is being checked.
    const auto discard = [](const ac3::PcmBlock&) {};
    const auto decoded = decoder.decode_frame_by_block(frames->front(), discard);
    const bool refused =
        !decoded && decoded.error() == ac3::DecodeError::kUnsupported;
    std::printf("check=reference_transform_refused status=%s\n", refused ? "pass" : "fail");
    if (!refused) {
        g_failed = true;
    }
}

}  // namespace

int ac3probe::run() {
    // Nothing here reads ac3/internal/profile.hpp, deliberately: that header
    // states what the build INTENDED, and a probe reporting its own intentions
    // back would prove nothing. Every claim below is observed - the levels
    // from a real decode, the churn from real allocations, and the absence of
    // the direct-form transform from the API actually refusing to use it.
    std::printf("profile=minimal-decoder\n");
    // static.pcm_bytes is 0 by construction now - the probe reads the decoders'
    // blocks in place (see kMaxChannels above) - and stays on the line so the
    // runner scripts and footprint_report.py see the key they always did.
    std::printf("static.pcm_bytes=%lu static.frame_decoder_bytes=%lu "
                "static.eac3_decoder_bytes=%lu\n",
                static_cast<unsigned long>(0),
                static_cast<unsigned long>(sizeof(ac3::FrameDecoder)),
                static_cast<unsigned long>(sizeof(ac3::Eac3Decoder)));

    // Measured before any fixture so it cannot be confused with one, and
    // printed either way: a reader of the log then knows whether the
    // stage[...] lines that follow are missing because nothing was timed or
    // because the build could not time anything.
    std::printf("stage.pair_cost_ns=%lu\n",
                static_cast<unsigned long>(ac3probe::stage_pair_cost_ns()));

    for (const auto& fixture : kAc3Fixtures) {
        if (over_budget(fixture.codec, fixture.peak_bytes)) {
            continue;
        }
        if (decode_ac3(fixture.codec, fixture.stream, fixture.rms, fixture.output) != 0) {
            std::printf("result=fail\n");
            return 1;
        }
    }
    for (const auto& fixture : kEac3Fixtures) {
        if (over_budget(fixture.codec, fixture.peak_bytes)) {
            continue;
        }
        const int status =
            fixture.render
                ? render_eac3(fixture.codec, fixture.stream, fixture.rms, fixture.joc_domain)
                : decode_eac3(fixture.codec, fixture.stream, fixture.rms, fixture.bed_only,
                              fixture.joc_domain, fixture.output);
        if (status != 0) {
            std::printf("result=fail\n");
            return 1;
        }
        // Hand back what enhanced coupling cached, if this fixture used it.
        // Its scratch is thread_local and this thread never exits, so without
        // this it stays resident for the rest of the run - not a leak, but
        // 23,552 bytes on this profile that the next fixture cannot have. It is what stopped
        // object reconstruction fitting on an ESP32-S3 whenever it ran after
        // the ecpl row, and calling it here is what lets these rows sit in
        // any order.
        //
        // Every fixture, not just the coupled one: nothing outside the
        // decoder can tell which streams used which tools, and this costs a
        // null check where the scratch was never built.
        ac3::eac3::release_ecpl_scratch();
    }
    check_reference_transform_refused();

    // Whether this build's library called the stage timers at all - see
    // stage_timers.hpp. A plain build says "off" here and prints no stage
    // lines; it is not an error, it is the ordinary footprint measurement.
    std::printf("stage_timers=%s\n", ac3probe::stages_active() ? "on" : "off");

    // Every decoder this run made is out of scope by now, so whatever is still
    // live is held by something with process lifetime inside the library rather
    // than by a frame that forgot to free. RETAINED, not leaked: the two are
    // different news and only one of them grows. The per-size breakdown below
    // says which buffer, because a total on its own cannot be acted on.
    std::printf("heap.peak_bytes=%lu heap.allocs=%lu heap.frees=%lu heap.retained_bytes=%lu\n",
                static_cast<unsigned long>(g_peak_bytes),
                static_cast<unsigned long>(g_alloc_calls),
                static_cast<unsigned long>(g_free_calls),
                static_cast<unsigned long>(g_live_bytes));
    for (std::size_t bucket = 0; bucket < kBuckets; ++bucket) {
        if (g_live_by_bucket[bucket] == 0) {
            continue;
        }
        std::printf("heap.retained_bucket[%lu]=%lu count=%lu\n",
                    static_cast<unsigned long>(std::size_t{1} << bucket),
                    static_cast<unsigned long>(g_live_by_bucket[bucket]),
                    static_cast<unsigned long>(g_live_count_by_bucket[bucket]));
    }
    std::printf("heap.largest_alloc_bytes=%lu\n",
                static_cast<unsigned long>(g_largest_alloc));
    for (std::size_t bucket = 0; bucket < kBuckets; ++bucket) {
        if (g_peak_by_bucket[bucket] == 0) {
            continue;
        }
        std::printf("heap.peak_bucket[%lu]=%lu count=%lu\n",
                    static_cast<unsigned long>(std::size_t{1} << bucket),
                    static_cast<unsigned long>(g_peak_by_bucket[bucket]),
                    static_cast<unsigned long>(g_peak_count_by_bucket[bucket]));
    }
    for (std::size_t i = 0; i < kLargeSlots && g_large_size[i] != 0; ++i) {
        std::printf("heap.large[%lu]=%lu peak_live=%lu total=%lu\n",
                    static_cast<unsigned long>(i),
                    static_cast<unsigned long>(g_large_size[i]),
                    static_cast<unsigned long>(g_large_peak[i]),
                    static_cast<unsigned long>(g_large_size[i] * g_large_peak[i]));
    }
    // Not failed here. Retained bytes are a FOOTPRINT number, and every other
    // footprint ceiling in this profile lives in the runner scripts where it can
    // be overridden and read next to the rest - see
    // AC3FORGE_MAX_RETAINED_BYTES in tools/checks/run_baremetal_probe.sh. What
    // this function fails on is correctness: levels, frame and channel counts,
    // and the direct-form transform being refused.

    std::printf("result=%s\n", g_failed ? "fail" : "pass");
    return g_failed ? 1 : 0;
}
