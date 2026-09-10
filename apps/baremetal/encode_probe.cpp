// The minimum-footprint ENCODER probe (roadmap PF7): ac3::forge_minimal
// encoding real audio on a target with no operating system, no filesystem and
// no C++ exceptions, and reporting what that cost.
//
// The mirror of probe.cpp, which does the same for the decode direction, and it
// answers the same three questions. Two things about it are different, and both
// follow from the direction rather than from taste:
//
//   1. The INPUT is synthesised, not linked in. A decoder's fixture is a
//      bitstream - 10,752 bytes for six frames of 5.1 AC-3, which is nothing.
//      An encoder's fixture is the PCM those frames came from: 221,184 bytes
//      for the same six frames, which is most of an ESP32-S3's internal SRAM
//      and more than the arm-none-eabi image ceiling has spare. So the signal
//      is computed here instead, from a formula, costing a few hundred bytes of
//      code and giving any number of frames.
//
//   2. The CHECK is a checksum of the encoded bytes rather than per-channel
//      levels. There is no decoder in this profile to reconstruct with - that
//      is the whole point of it - so what a run can say is that the same input
//      produced the same bitstream it produced on the host. See kExpected below
//      for what that does and does not establish.
//
// It also answers a fourth, since 2026-09-10: how long each frame took, on
// exactly probe.cpp's terms (<codec>.us_per_frame and realtime_permille
// against a 32 ms frame) and with exactly its caveats - see report_timing
// below for what the number is worth on each leg.
//
// The output is machine-readable (`key=value` lines) so the CI leg can gate on
// it; tools/checks/run_baremetal_probe.sh parses the same lines for either
// direction.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <span>
#include <utility>
#include <vector>

#include "ac3/core/eac3_tools.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"

#include "encode_fixture.hpp"
#include "probe.hpp"

// AC3FORGE_PROBE_SEVEN_ONE is 0 or 1 from CMake (apps/baremetal/CMakeLists.txt,
// platform/esp32s3/main/CMakeLists.txt); a build that predates the option
// gets the default the probe has always had.
#ifndef AC3FORGE_PROBE_SEVEN_ONE
#define AC3FORGE_PROBE_SEVEN_ONE 0
#endif

namespace {

// The 7.1 access-unit fixture is a SWITCH, not a default - see the block in
// run() and encode_fixture.hpp for the measurement that made it one.
constexpr bool kProbeSevenOne = AC3FORGE_PROBE_SEVEN_ONE != 0;

// --- heap accounting -------------------------------------------------------
// The same global replacement probe.cpp uses, and for the same reason: PF7's
// requirement is no heap traffic in the codec loop, and the honest way to
// report progress against it is a number per frame that a CI leg can hold.
//
// It matters more here. Both encoders return std::vector<std::byte> from
// encode_frame - there is no encode_frame_into to match the decoder's
// decode_frame_into - so an allocation per frame is in the API rather than
// merely in the implementation, and the counts below say what that costs.
std::size_t g_alloc_calls = 0;
std::size_t g_free_calls = 0;
std::size_t g_live_bytes = 0;
std::size_t g_peak_bytes = 0;
// The peak over the current fixture alone, reset by encode_all as a fixture
// starts: heap.peak_bytes says what the run needed at its worst, and this says
// which encoder needed it - the number a part with a different budget reads
// to know whether it can hold AC-3, E-AC-3, or each in turn.
std::size_t g_fixture_peak_bytes = 0;

constexpr std::size_t kHeaderBytes = sizeof(std::size_t) < alignof(std::max_align_t)
                                         ? alignof(std::max_align_t)
                                         : sizeof(std::size_t);

}  // namespace

void* operator new(std::size_t size) {
    // Cannot throw: -fno-exceptions, so std::bad_alloc is not available to
    // report failure with. Saying so on the console and stopping beats
    // returning null into code written to trust operator new.
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
    g_live_bytes -= *static_cast<std::size_t*>(raw);
    ++g_free_calls;
    std::free(raw);
}

void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

namespace {

bool g_failed = false;

// --- the signal ------------------------------------------------------------
// Six sines, one per 5.1 channel, at frequencies that are not harmonically
// related so no two channels share a partial and the coupling decision has
// something to actually decide. Deterministic to the last bit: the arithmetic
// is double throughout and the only rounding is the final narrowing to float,
// so every target that has IEEE doubles computes the identical samples.
//
// Not silence and not one tone, per CONTRIBUTING.md's rule that both make weak
// fixtures - silence encodes to nothing interesting and a single tone exercises
// one band. Not real programme material either, because that would have to be
// linked in, which is the whole problem this avoids.
constexpr int kChannels = 6;

// At namespace scope, not in run()'s frame. Six channels of 1,536 floats is
// 36,864 bytes, and the mps2-an385's stack is nowhere near that - declaring it
// as a local hard-faults the target before the first frame is encoded, which is
// exactly how this comment came to be written. probe.cpp keeps its own PCM
// block here for the same reason.
//
// It is also what an embedded integrator has: one block, sized once, reused
// every frame. The encoders read through spans over it.
std::array<std::array<float, ac3::kSamplesPerFrame>, kChannels> g_pcm{};
std::array<std::span<const float>, kChannels> g_views{};

void fill_signal(std::array<std::array<float, ac3::kSamplesPerFrame>, kChannels>& pcm,
                 int frame) {
    for (std::size_t ch = 0; ch < kChannels; ++ch) {
        // 220, 337, 554, 881, 1409, 2273 Hz - each roughly 1.6x the last and
        // none an integer multiple of another.
        constexpr std::array<double, kChannels> kHz{220.0, 337.0, 554.0,
                                                    881.0, 1409.0, 2273.0};
        const double hz = kHz[ch];
        for (std::size_t n = 0; n < ac3::kSamplesPerFrame; ++n) {
            // Sample index continues across frames, so successive frames are a
            // continuous signal rather than six copies of one - which is what
            // gives block switching and the exponent strategy something to
            // track.
            const double t =
                static_cast<double>(static_cast<std::size_t>(frame) * ac3::kSamplesPerFrame + n) /
                48000.0;
            pcm[ch][n] = static_cast<float>(0.25 * std::sin(2.0 * 3.14159265358979323846 * hz * t));
        }
    }
}

// FNV-1a over the encoded bytes. Not a cryptographic hash and does not need to
// be: it is comparing this run's output against the same library's output on
// the host, where the question is "did anything change at all", not "can an
// adversary find a collision". 64-bit, so an accidental collision across a
// change to the bitstream is not a thing that happens.
// Printed as two 32-bit halves, never with %llu. newlib-nano's printf has no
// long long conversion unless -u _printf_ll is linked in - the same limitation
// fixture.hpp records for _printf_float, and with the same answer: a probe
// whose subject is footprint should not drag in a wider printf to report a
// number. A %llu here does not fail gracefully either; it double-faults the
// mps2-an385 after the encode has already succeeded, which is a confusing place
// to look for an encoder bug.
constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void hash_bytes(std::uint64_t& h, std::span<const std::byte> data) {
    for (const std::byte b : data) {
        h ^= static_cast<std::uint64_t>(b);
        h *= kFnvPrime;
    }
}

void fail(const char* what, std::uint64_t got, std::uint64_t expected) {
    std::printf("check=%s status=fail got=%08lx%08lx expected=%08lx%08lx\n", what,
                static_cast<unsigned long>(got >> 32), static_cast<unsigned long>(got & 0xffffffffU),
                static_cast<unsigned long>(expected >> 32),
                static_cast<unsigned long>(expected & 0xffffffffU));
    g_failed = true;
}

struct EncodeResult {
    std::size_t bytes = 0;
    std::uint64_t hash = kFnvOffset;
    std::size_t first_frame_allocs = 0;
    std::size_t steady_allocs = 0;
    // Microseconds inside encode_frame, summed over every frame; the signal
    // synthesis is outside the span, so this is the encoder's alone.
    std::uint64_t encode_us = 0;
    // Live heap at its highest while this fixture's encoder existed.
    std::size_t peak_bytes = 0;
};

// A frame is 1536 samples at 48 kHz - 32 ms of audio - so real time means
// encoding one in less than that, and the ratio is the headline exactly as it
// is on the decode side: below 1.0 the target keeps up. A permille integer
// rather than a float because newlib-nano's printf has no floating-point
// support unless -u _printf_float is linked in, the same reason the hash
// above prints as two halves.
//
// What the number is worth depends on the leg, and probe.hpp says which: on a
// board it is the answer; under QEMU's semihosting clock it is the host's own
// time and describes nothing; under -icount (run_baremetal_probe.sh --encoder
// --icount) each microsecond is a thousand instructions, deterministic, and
// the runner holds it to a ceiling per fixture.
constexpr std::uint64_t kFrameDurationUs = 32000;

void report_timing(const char* codec, const EncodeResult& r) {
    constexpr auto kFrames = static_cast<std::uint64_t>(ac3probe::kEncodeFrames);
    const std::uint64_t per_frame = r.encode_us / kFrames;
    const std::uint64_t permille = (r.encode_us * 1000) / (kFrameDurationUs * kFrames);
    std::printf("%s.encode_us=%lu %s.us_per_frame=%lu %s.realtime_permille=%lu\n", codec,
                static_cast<unsigned long>(r.encode_us), codec,
                static_cast<unsigned long>(per_frame), codec,
                static_cast<unsigned long>(permille));
}

void report(const char* codec, const EncodeResult& r, std::size_t expected_bytes,
            std::uint64_t expected_hash) {
    const std::size_t steady_frames = ac3probe::kEncodeFrames - 1;
    std::printf("%s.bytes=%lu %s.hash=%08lx%08lx %s.first_frame_allocs=%lu "
                "%s.steady_allocs_per_frame=%lu\n",
                codec, static_cast<unsigned long>(r.bytes), codec,
                static_cast<unsigned long>(r.hash >> 32),
                static_cast<unsigned long>(r.hash & 0xffffffffU), codec,
                static_cast<unsigned long>(r.first_frame_allocs), codec,
                static_cast<unsigned long>(steady_frames > 0 ? r.steady_allocs / steady_frames
                                                             : 0));
    std::printf("%s.peak_bytes=%lu\n", codec, static_cast<unsigned long>(r.peak_bytes));
    report_timing(codec, r);
    if (r.bytes != expected_bytes) {
        fail("bytes", r.bytes, expected_bytes);
    }
    if (r.hash != expected_hash) {
        // A hash mismatch with the right byte count is the interesting case: the
        // encoder still produced a well-formed frame of the expected size and
        // put different bits in it. That is a real difference in the
        // arithmetic, not a configuration slip.
        fail("hash", r.hash, expected_hash);
    }
}

// The two encoder shapes answer the same question through different names:
// a FrameEncoder's encode_frame returns the syncframe's bytes, an
// AccessUnitEncoder's encode_access_unit returns an AccessUnit whose `bytes`
// is every substream in transmission order. Both are one span of bytes to
// count and hash, which is all encode_all wants.
template <typename Encoder>
auto encode_one(Encoder& encoder, std::span<const std::span<const float>> views) {
    return encoder.encode_frame(views);
}

[[maybe_unused]] std::expected<std::vector<std::byte>, ac3::FrameError> encode_one(
    ac3::eac3::AccessUnitEncoder& encoder, std::span<const std::span<const float>> views) {
    auto unit = encoder.encode_access_unit(views);
    if (!unit) {
        return std::unexpected(unit.error());
    }
    return std::move(unit->bytes);
}

// `views` is however many channels the encoder's own layout asks for, which is
// not always the six the PCM block holds - see the 2/0 fixture below.
template <typename Encoder>
EncodeResult encode_all(Encoder& encoder,
                        std::array<std::array<float, ac3::kSamplesPerFrame>, kChannels>& pcm,
                        std::span<const std::span<const float>> views) {
    EncodeResult result;
    // The fixture's peak starts from what is live now: this encoder, just
    // constructed, and nothing of the previous one, which its scope destroyed.
    g_fixture_peak_bytes = g_live_bytes;
    std::size_t before = g_alloc_calls;
    for (int frame = 0; frame < ac3probe::kEncodeFrames; ++frame) {
        fill_signal(pcm, frame);
        const std::uint64_t started = ac3probe::now_us();
        const auto encoded = encode_one(encoder, views);
        result.encode_us += ac3probe::now_us() - started;
        if (!encoded) {
            std::printf("check=encode status=fail frame=%d error=%d\n", frame,
                        static_cast<int>(encoded.error()));
            g_failed = true;
            result.peak_bytes = g_fixture_peak_bytes;
            return result;
        }
        result.bytes += encoded->size();
        hash_bytes(result.hash, *encoded);
        if (frame == 0) {
            result.first_frame_allocs = g_alloc_calls - before;
        } else {
            result.steady_allocs += g_alloc_calls - before;
        }
        before = g_alloc_calls;
    }
    result.peak_bytes = g_fixture_peak_bytes;
    return result;
}

}  // namespace

int ac3probe::run() {
    std::printf("profile=minimal-encoder\n");
    std::printf("static.pcm_bytes=%lu static.frame_encoder_bytes=%lu "
                "static.eac3_frame_encoder_bytes=%lu\n",
                static_cast<unsigned long>(sizeof(g_pcm)),
                static_cast<unsigned long>(sizeof(ac3::FrameEncoder)),
                static_cast<unsigned long>(sizeof(ac3::eac3::FrameEncoder)));

    for (std::size_t ch = 0; ch < kChannels; ++ch) {
        g_views[ch] = std::span<const float>(g_pcm[ch]);
    }

    // Scoped so each encoder is destroyed before the next is built. That is not
    // tidiness - it is the shape this profile exists to prove. Holding both at
    // once peaks at 440,420 bytes against an ESP32-S3's 277,400 free, and
    // holding one peaks at 201,770 or 243,770. Sequential is what fits, so
    // sequential is what the probe measures.
    {
        ac3::FrameEncoder encoder{
            {.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true}};
        const auto r = encode_all(encoder, g_pcm, g_views);
        report("ac3", r, ac3probe::kAc3Bytes, ac3probe::kAc3Hash);
    }
    const std::size_t peak_after_ac3 = g_peak_bytes;

    // The narrower shapes, one per codec: 2/0 is what a small part is most
    // likely to be asked to encode, and each reads two of the block's six
    // channels - the encoder's layout decides how many spans it takes.
    {
        ac3::FrameEncoder encoder{{.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0}};
        const auto r = encode_all(encoder, g_pcm, std::span{g_views}.first(2));
        report("ac3_stereo", r, ac3probe::kAc3StereoBytes, ac3probe::kAc3StereoHash);
    }

    {
        ac3::eac3::FrameEncoder encoder{
            {.bitrate_kbps = 384, .acmod = ac3::Acmod::k3_2, .lfe = true}};
        const auto r = encode_all(encoder, g_pcm, g_views);
        report("eac3", r, ac3probe::kEac3Bytes, ac3probe::kEac3Hash);
    }

    {
        ac3::eac3::FrameEncoder encoder{{.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0}};
        const auto r = encode_all(encoder, g_pcm, std::span{g_views}.first(2));
        report("eac3_stereo", r, ac3probe::kEac3StereoBytes, ac3probe::kEac3StereoHash);
    }

    // The 5.1 row above encodes with no tool at all - that is the default -
    // so until this row the encoder's coupling, spectral-extension and AHT
    // paths were linked into the profile and run by nothing in it. All three
    // at once, at 2/0 with the band edges pinned; encode_fixture.hpp says why
    // not 5.1 and why pinned, with the numbers.
    {
        ac3::eac3::FrameEncoder encoder{{.bitrate_kbps = 192,
                                         .acmod = ac3::Acmod::k2_0,
                                         .coupling = true,
                                         .cplbegf = 0,
                                         .spx = true,
                                         .spxbegf = 7,
                                         .aht = true}};
        const auto r = encode_all(encoder, g_pcm, std::span{g_views}.first(2));
        report("eac3_tools", r, ac3probe::kEac3ToolsBytes, ac3probe::kEac3ToolsHash);
    }

    // §E3.5 enhanced coupling, which nothing else here reaches. `coupling` and
    // `enhanced` are set explicitly rather than through auto_tools: that flag
    // overrides the individual ones and picks per rate, so asking it for
    // enhanced coupling is asking it for whatever it happens to choose - which
    // is not a fixture, it is a moving target.
    //
    // Both flags, because `enhanced` is only meaningful together with
    // `coupling` - §E3.5 is an alternate coupling mode, not an independent
    // tool.
    {
        ac3::eac3::FrameEncoder encoder{{.bitrate_kbps = 192,
                                         .acmod = ac3::Acmod::k2_0,
                                         .coupling = true,
                                         .enhanced = true}};
        const auto r = encode_all(encoder, g_pcm, std::span{g_views}.first(2));
        report("eac3_ecpl", r, ac3probe::kEac3EcplBytes, ac3probe::kEac3EcplHash);
    }
    // 7.1 as an ACCESS UNIT: an independent 5.1 substream and a dependent
    // carrying Ls, Rs, Lrs and Rrs (chanmap k71Rear), which is how Annex E
    // codes a layout wider than 5.1 (E3.8.2) and the shape tests/encoder/
    // test_eac3.cpp's seven_one() builds. Two FrameEncoders live at once
    // inside the AccessUnitEncoder, and that is the finding: on the host this
    // fixture takes the run's peak from about 223,000 bytes to 435,263, and
    // on an ESP32-S3 (QEMU, same memory map, 2026-09-10) it dies on a
    // 73,728-byte request with 303,656 bytes free and a largest block of
    // 241,664 before the run began. A 7.1 E-AC-3 encode does not fit this
    // part's internal SRAM; PSRAM is the question that remains, and QEMU
    // cannot ask it.
    //
    // Opt-in (-DAC3FORGE_PROBE_SEVEN_ONE=ON) for exactly that reason: a default
    // fixture that cannot pass on one leg is not a fixture, and the two legs'
    // heap ceilings are statements about what fits. Off, the block is
    // discarded and the image is the one the ceilings hold.
    //
    // Ten spans over six channels of PCM: the dependent's four alias the
    // bed's L, R, SL and SR. The encoder does not care that two substreams
    // see the same samples, and widening g_pcm to ten channels would add
    // 24 KB of .bss to an image whose whole subject is footprint.
    if constexpr (kProbeSevenOne) {
        ac3::eac3::AccessUnitEncoder encoder{
            {.independent = {.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true},
             .dependents = {{.bitrate_kbps = 224,
                             .acmod = ac3::Acmod::k2_2,
                             .chanmap = ac3::eac3::chanmap::k71Rear}}}};
        const std::array<std::span<const float>, 10> views = {
            g_views[0], g_views[1], g_views[2], g_views[3], g_views[4], g_views[5],
            g_views[3], g_views[4], g_views[0], g_views[1]};
        const auto r = encode_all(encoder, g_pcm, views);
        report("eac3_71", r, ac3probe::kEac3SevenOneBytes, ac3probe::kEac3SevenOneHash);
    }

    // Hand back what enhanced coupling cached, exactly as probe.cpp does after
    // each decode fixture and for the same reason: the scratch is thread_local
    // and this thread never exits, so without this its 34,208 bytes stay
    // resident for the rest of the run. Not a leak - bounded, paid once, and
    // the point of caching it - but the probe reports retained bytes and a
    // runner gates them at 1,024, so a fixture that leaves five figures behind
    // has to say whether that is a cache or a bug. It is a cache.
    //
    // The ENCODER shares eac3_tools with the decoder, which is why this call
    // means anything in an encode-only profile at all.
    ac3::eac3::release_ecpl_scratch();

    std::printf("heap.peak_bytes=%lu heap.peak_after_ac3_bytes=%lu heap.allocs=%lu "
                "heap.frees=%lu heap.retained_bytes=%lu\n",
                static_cast<unsigned long>(g_peak_bytes),
                static_cast<unsigned long>(peak_after_ac3),
                static_cast<unsigned long>(g_alloc_calls),
                static_cast<unsigned long>(g_free_calls),
                static_cast<unsigned long>(g_live_bytes));

    std::printf("result=%s\n", g_failed ? "fail" : "pass");
    return g_failed ? 1 : 0;
}
