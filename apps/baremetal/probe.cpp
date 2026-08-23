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
//   2. Does it produce the right audio? Every frame of both fixtures is
//      decoded and each channel's RMS compared against what the same library
//      produced on the host (apps/baremetal/fixture.hpp).
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
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"

#include "fixture.hpp"

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
    if (g_live_bytes > g_peak_bytes) {
        g_peak_bytes = g_live_bytes;
    }
    return static_cast<std::byte*>(raw) + kHeaderBytes;
}

void* operator new[](std::size_t size) { return ::operator new(size); }

void operator delete(void* p) noexcept {
    if (p == nullptr) {
        return;
    }
    void* raw = static_cast<std::byte*>(p) - kHeaderBytes;
    g_live_bytes -= *static_cast<std::size_t*>(raw);
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
// sized once, reused every frame. Sixteen channels covers §E3.8.2's cap.
constexpr std::size_t kMaxChannels = 16;
std::array<std::array<float, ac3::kSamplesPerFrame>, kMaxChannels> g_pcm{};
std::array<std::span<float>, kMaxChannels> g_pcm_spans{};

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
            fail("rms", got, expected[ch]);
        }
    }
}

struct Churn {
    std::size_t first_frame_allocs = 0;
    std::size_t steady_allocs = 0;
    int frames = 0;
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
        const auto decoded = decoder.decode_frame_into(frame, g_pcm_spans);
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
    return 0;
}

int decode_eac3() {
    const std::span<const std::byte> stream{
        reinterpret_cast<const std::byte*>(ac3probe::kEac3Stream.data()),
        ac3probe::kEac3Stream.size()};
    const auto units = ac3::split_access_units(stream);
    if (!units) {
        std::printf("check=eac3.split status=fail error=%d\n", static_cast<int>(units.error()));
        return 1;
    }

    ac3::Eac3Decoder decoder;
    LevelAccumulator levels;
    Churn churn;
    churn.frames = static_cast<int>(units->size());
    std::size_t before = g_alloc_calls;
    int index = 0;
    int channels = 0;
    for (const auto unit : *units) {
        const auto decoded = decoder.decode_access_unit_into(unit, g_pcm_spans);
        if (!decoded) {
            std::printf("check=eac3.decode status=fail unit=%d error=%d\n", index,
                        static_cast<int>(decoded.error()));
            return 1;
        }
        // std::nullopt is the §3.7 hold-back, not an error. tools=all does not
        // turn that tool on, so this fixture never takes the branch - handled
        // anyway so a regenerated fixture that DOES use it fails on levels
        // rather than on a silent miscount.
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
        fail("eac3.frames", churn.frames, ac3probe::kFrames);
    }
    if (channels != static_cast<int>(ac3probe::kEac3Rms.size())) {
        fail("eac3.channels", channels, static_cast<long>(ac3probe::kEac3Rms.size()));
    }
    report_levels("eac3", levels, ac3probe::kEac3Rms);
    report_churn("eac3", churn);
    return 0;
}

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

int main() {
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

    if (decode_ac3() != 0 || decode_eac3() != 0) {
        std::printf("result=fail\n");
        return 1;
    }
    check_reference_transform_refused();

    std::printf("heap.peak_bytes=%lu heap.allocs=%lu heap.frees=%lu heap.leaked_bytes=%lu\n",
                static_cast<unsigned long>(g_peak_bytes),
                static_cast<unsigned long>(g_alloc_calls),
                static_cast<unsigned long>(g_free_calls),
                static_cast<unsigned long>(g_live_bytes));
    if (g_live_bytes != 0) {
        fail("heap.leaked", static_cast<long>(g_live_bytes), 0);
    }

    std::printf("result=%s\n", g_failed ? "fail" : "pass");
    return g_failed ? 1 : 0;
}
