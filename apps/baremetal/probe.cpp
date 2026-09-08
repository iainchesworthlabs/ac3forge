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

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>

#include "ac3/core/eac3_tools.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"

#include "fixture.hpp"
#include "probe.hpp"

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
    if (size > g_largest_alloc) {
        g_largest_alloc = size;
    }
    note_large_alloc(size, false);
    if (g_live_bytes > g_peak_bytes) {
        g_peak_bytes = g_live_bytes;
        g_peak_by_bucket = g_live_by_bucket;
        g_peak_count_by_bucket = g_live_count_by_bucket;
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

// --- caller-owned PCM ------------------------------------------------------
// The decode_frame_into / decode_access_unit_into forms write through spans
// the caller owns, which is what an embedded integrator has: one static block,
// sized once, reused every frame.
//
// Eight channels, not §E3.8.2's cap of sixteen. This block is the CALLER's, not
// the library's, and an integrator decoding 5.1 allocates six - so provisioning
// for a stream the fixture does not contain was inflating the probe's own .bss
// by 49,152 bytes and making the profile look more expensive than it is. Eight
// still covers 7.1, which is a layout that exists.
//
// The static_assert below is what keeps this honest rather than merely smaller:
// regenerate fixture.hpp with a wider layout and the build stops here, instead
// of the decode writing past the end of a span.
constexpr std::size_t kMaxChannels = 8;
std::array<std::array<float, ac3::kSamplesPerFrame>, kMaxChannels> g_pcm{};
std::array<std::span<float>, kMaxChannels> g_pcm_spans{};

static_assert(ac3probe::kAc3Rms.size() <= kMaxChannels,
              "the AC-3 fixture has more channels than the probe's PCM block holds - raise "
              "kMaxChannels");
// The E-AC-3 fixtures are checked the same way below, once, over their table -
// see kEac3Fixtures. One assertion per fixture would be a second place to
// remember when adding one, which is the seam this file is trying not to have.

void bind_pcm_spans() {
    for (std::size_t ch = 0; ch < kMaxChannels; ++ch) {
        g_pcm_spans[ch] = std::span<float>(g_pcm[ch]);
    }
}

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
    std::size_t first_frame_allocs = 0;
    std::size_t steady_allocs = 0;
    int frames = 0;
    // Time inside decode_frame_into / decode_access_unit_into only. The level
    // accumulation and the allocation bookkeeping around it are the PROBE's
    // cost, not the decoder's, and folding them in would flatter or slander the
    // decoder depending on how expensive they happen to be on a given target.
    std::uint64_t decode_us = 0;
};

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

int decode_ac3() {
    const std::span<const std::byte> stream{
        reinterpret_cast<const std::byte*>(ac3probe::kAc3Stream.data()),
        ac3probe::kAc3Stream.size()};
    const auto frames = ac3::split_frames(stream);
    if (!frames) {
        std::printf("check=ac3.split status=fail error=%d\n", static_cast<int>(frames.error()));
        return 1;
    }

    ac3::FrameDecoder decoder;
    LevelAccumulator levels;
    Churn churn;
    churn.frames = static_cast<int>(frames->size());
    std::size_t before = g_alloc_calls;
    int index = 0;
    int channels = 0;
    for (const auto frame : *frames) {
        const std::uint64_t started_us = ac3probe::now_us();
        const auto decoded = decoder.decode_frame_into(frame, g_pcm_spans);
        churn.decode_us += ac3probe::now_us() - started_us;
        if (!decoded) {
            std::printf("check=ac3.decode status=fail frame=%d error=%d\n", index,
                        static_cast<int>(decoded.error()));
            return 1;
        }
        channels = ac3::fullbw_channel_count(decoded->acmod) + (decoded->lfe ? 1 : 0);
        for (int ch = 0; ch < channels; ++ch) {
            levels.add(static_cast<std::size_t>(ch), g_pcm[static_cast<std::size_t>(ch)]);
        }
        if (index == 0) {
            churn.first_frame_allocs = g_alloc_calls - before;
        } else {
            churn.steady_allocs += g_alloc_calls - before;
        }
        before = g_alloc_calls;
        ++index;
    }

    if (churn.frames != ac3probe::kFrames) {
        fail("ac3.frames", churn.frames, ac3probe::kFrames);
    }
    if (channels != static_cast<int>(ac3probe::kAc3Rms.size())) {
        fail("ac3.channels", channels, static_cast<long>(ac3probe::kAc3Rms.size()));
    }
    report_levels("ac3", levels, ac3probe::kAc3Rms);
    report_churn("ac3", churn);
    report_timing("ac3", churn);
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
                ac3::oba::joc::Domain domain) {
    const std::span<const std::byte> stream{
        reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()};
    const auto units = ac3::split_access_units(stream);
    if (!units) {
        std::printf("check=%s.split status=fail error=%d\n", codec,
                    static_cast<int>(units.error()));
        return 1;
    }

    ac3::Eac3Decoder decoder{{.joc_domain = domain,
                             .skip_object_reconstruction = bed_only}};
    LevelAccumulator levels;
    Churn churn;
    churn.frames = static_cast<int>(units->size());
    std::size_t before = g_alloc_calls;
    int index = 0;
    int channels = 0;
    for (const auto unit : *units) {
        const std::uint64_t started_us = ac3probe::now_us();
        const auto decoded = decoder.decode_access_unit_into(unit, g_pcm_spans);
        churn.decode_us += ac3probe::now_us() - started_us;
        if (!decoded) {
            std::printf("check=%s.decode status=fail unit=%d error=%d\n", codec, index,
                        static_cast<int>(decoded.error()));
            return 1;
        }
        // std::nullopt is the §3.7 hold-back, not an error. No fixture here
        // selects transient pre-noise processing - neither "all" nor "cpl+ecpl"
        // includes tpn - so none of them takes this branch today; handled anyway
        // so a fixture that DOES use it fails on levels rather than on a silent
        // miscount.
        if (decoded->has_value()) {
            channels = static_cast<int>((*decoded)->layout.count);
            for (int ch = 0; ch < channels; ++ch) {
                levels.add(static_cast<std::size_t>(ch), g_pcm[static_cast<std::size_t>(ch)]);
            }
        }
        if (index == 0) {
            churn.first_frame_allocs = g_alloc_calls - before;
        } else {
            churn.steady_allocs += g_alloc_calls - before;
        }
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
    report_churn(codec, churn);
    report_timing(codec, churn);
    return 0;
}

// The E-AC-3 fixtures, in the order the probe decodes them. Adding one is a
// row here and a stream in tools/generators/gen_baremetal_fixture.py's own
// table; nothing else in this file changes, and neither runner script names a
// fixture - both gate every `<name>.steady_allocs_per_frame` line the probe
// prints.
struct Eac3Fixture {
    const char* codec;
    std::span<const std::uint8_t> stream;
    std::span<const std::int32_t> rms;
    // DecoderConfig::skip_object_reconstruction. Only the Atmos fixture sets
    // it, and it is the whole reason that fixture can be here: see its row.
    bool bed_only = false;
    // DecoderConfig::joc_domain. Only the object row sets it; see there.
    ac3::oba::joc::Domain joc_domain = ac3::oba::joc::Domain::kQmf;
};

constexpr std::array<Eac3Fixture, 5> kEac3Fixtures{{
    {"eac3", ac3probe::kEac3Stream, ac3probe::kEac3Rms},
    // §E3.5's alternate coupling mode. `tools=all` does not select it
    // (plan::parse_tools maps "all" to cpl+spx+aht), so without this row
    // ecpl_channel_spectrum - and the 512-point DFT
    // src/forge/src/core/fft.cpp is in the minimal source list for - are
    // linked into every build of this profile and executed by none of them.
    {"eac3_ecpl", ac3probe::kEac3EcplStream, ac3probe::kEac3EcplRms},
    // 2/0, the only layout §7.5.4 rematrixing exists in: no 5.1 fixture
    // reaches it whatever its tools are. Also the first fixture whose channel
    // count is not six, so the layout-driven half of the level check is
    // exercised rather than merely written.
    // An Atmos stream decoded for its BED. §6 object reconstruction allocates
    // an oba::joc::ReconstructionState - 147,504 bytes in one block, plus a
    // QmfState and its filterbanks - which is more than the largest free run
    // this decode leaves on an ESP32-S3, so a full decode of this stream dies
    // in operator new partway through. The bed does not: it is ordinary
    // E-AC-3, and this row is what proves that on the target rather than in a
    // paragraph. Levels are the bed's, which is what ac3cli decode writes for
    // an Atmos stream too, so the host reference needed no special case.
    {"eac3_atmos_bed", ac3probe::kEac3AtmosBedStream, ac3probe::kEac3AtmosBedRms, true},
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
    // to fail outright - ecpl leaves 34,232 bytes of thread_local scratch
    // behind on a target whose thread never exits, and object reconstruction
    // then had nowhere to go. release_ecpl_scratch() below is what makes the
    // order stop mattering, so this row sits where it would naturally rather
    // than where it happens to pass.
    {"eac3_atmos_objects", ac3probe::kEac3AtmosBedStream, ac3probe::kEac3AtmosBedRms,
     false, ac3::oba::joc::Domain::kMdctBand},
    {"eac3_stereo", ac3probe::kEac3StereoStream, ac3probe::kEac3StereoRms},
}};

// What the per-fixture static_asserts above used to say, said once. Regenerate
// fixture.hpp with a layout wider than the PCM block and the build stops here,
// instead of decode_access_unit_into writing past the end of a span.
consteval bool every_fixture_fits() {
    for (const auto& fixture : kEac3Fixtures) {
        if (fixture.rms.size() > kMaxChannels) {
            return false;
        }
    }
    return true;
}

static_assert(every_fixture_fits(),
              "an E-AC-3 fixture has more channels than the probe's PCM block holds - raise "
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
    const auto decoded = decoder.decode_frame_into(frames->front(), g_pcm_spans);
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
    std::printf("static.pcm_bytes=%lu static.frame_decoder_bytes=%lu "
                "static.eac3_decoder_bytes=%lu\n",
                static_cast<unsigned long>(sizeof(g_pcm)),
                static_cast<unsigned long>(sizeof(ac3::FrameDecoder)),
                static_cast<unsigned long>(sizeof(ac3::Eac3Decoder)));

    bind_pcm_spans();

    if (decode_ac3() != 0) {
        std::printf("result=fail\n");
        return 1;
    }
    for (const auto& fixture : kEac3Fixtures) {
        if (decode_eac3(fixture.codec, fixture.stream, fixture.rms,
                        fixture.bed_only, fixture.joc_domain) != 0) {
            std::printf("result=fail\n");
            return 1;
        }
        // Hand back what enhanced coupling cached, if this fixture used it.
        // Its scratch is thread_local and this thread never exits, so without
        // this it stays resident for the rest of the run - not a leak, but
        // 34,232 bytes the next fixture cannot have. It is what stopped
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
