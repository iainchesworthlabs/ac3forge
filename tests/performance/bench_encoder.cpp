// Performance-trend data producer: the OTHER half of the performance suite,
// alongside test_performance.cpp's hard real-time gate (ac3perf). That gate
// answers "did the codec stay faster than real time" (pass/fail); this
// answers "how much faster, and is that number drifting" - a trend signal,
// not a threshold, the same relationship docs/quality-trend.md's gold-
// reference gate has to tools/ci/append_quality_history.py.
//
// Not a Catch2 binary on purpose: nothing here asserts anything. It runs
// each configuration for a fixed frame count and writes one JSON record per
// configuration to --json-out, for tools/ci/append_performance_history.py to
// append to the performance-history data (docs/performance-trend.md) the
// same way compare_wav.py's --json-out feeds append_quality_history.py.
//
// Every workload is fed real programme material through real_audio.hpp - see
// that header for why a 440 Hz tone, which is what this bench ran on until
// roadmap PF1, is a different workload rather than a cheaper one.
//
// The workload set covers both directions of both generations, plus the
// object path: three encoders (AC-3, E-AC-3 with its tools on `auto`, and
// Atmos/JOC) and the three decoders that read what they produce. Before PF1
// the E-AC-3 encoder - the largest single source file in the codec - and
// every decode path had no ms/frame series at all, so a regression in any of
// them was invisible to this page. The decode series are timed off streams
// the encode series in the same run just produced, which is also what keeps
// them honest: a decode number is only meaningful against a stream whose
// tools and rate are known.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/oba/atmos.hpp"
#include "real_audio.hpp"

namespace {

constexpr int kFrames = 200;
constexpr double kSampleRate = 48000.0;
// The object count the Atmos encode and decode workloads share.
constexpr int kObjects = 4;

double real_time_budget_ms(int frames) {
    return 1000.0 * static_cast<double>(frames) * ac3::kSamplesPerFrame / kSampleRate;
}

struct Result {
    std::string name;
    int frames = 0;
    double total_ms = 0.0;
    double ms_per_frame = 0.0;
};

class Timer {
public:
    void start() { start_ = std::chrono::steady_clock::now(); }

    [[nodiscard]] Result stop(std::string name, int frames) const {
        const auto elapsed_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_)
                .count();
        return {.name = std::move(name), .frames = frames, .total_ms = elapsed_ms,
                .ms_per_frame = elapsed_ms / frames};
    }

private:
    std::chrono::steady_clock::time_point start_{};
};

[[noreturn]] void fail(const char* workload, const char* what) {
    std::fprintf(stderr, "%s: %s failed\n", workload, what);
    std::exit(1);
}

// --- encoder construction --------------------------------------------------
// Shared by the timed runs below and by the untimed setup pass that builds
// the decode workloads' source streams, so a decode series is always read
// against exactly the configuration whose encode series sits above it.

ac3::FrameEncoder make_ac3_51(bool fast_mdct) {
    return ac3::FrameEncoder{
        {.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true, .fast_mdct = fast_mdct}};
}

// E-AC-3 with auto_tools: the landscape configuration, not a corner. `auto`
// picks the Annex E tool set from the per-channel rate (see FrameConfig's own
// comment on why neither "all on" nor "all off" is a sensible default), so
// this is the code path a stream produced by this encoder normally takes -
// and the one whose cost a rate change silently moves.
ac3::eac3::FrameEncoder make_eac3_auto(ac3::Acmod acmod, bool lfe, std::uint32_t bitrate_kbps) {
    return ac3::eac3::FrameEncoder{
        {.bitrate_kbps = bitrate_kbps, .acmod = acmod, .lfe = lfe, .auto_tools = true}};
}

std::vector<ac3::oba::ObjectPlacement> object_placement() {
    std::vector<ac3::oba::ObjectPlacement> placement(static_cast<std::size_t>(kObjects));
    for (int obj = 0; obj < kObjects; ++obj) {
        placement[static_cast<std::size_t>(obj)] = {
            .position = {.x = 0.2 + 0.2 * obj, .y = 0.5, .z = 0.0}, .gain = 1.0};
    }
    return placement;
}

// --- encode workloads ------------------------------------------------------
// Nothing is captured out of a timed loop: appending each frame to a buffer
// would put the harness's own memcpy inside an encode series' number, and
// only inside whichever variant happened to be the capture source. The decode
// workloads' streams come from the untimed setup pass instead.

Result bench_ac3_51(perf::FrameSource& source, bool fast_mdct) {
    auto encoder = make_ac3_51(fast_mdct);
    const char* name = fast_mdct ? "plain_51_fast_mdct" : "plain_51";

    Timer timer;
    timer.start();
    for (int frame = 0; frame < kFrames; ++frame) {
        const auto result = encoder.encode_frame(source.frame(static_cast<std::size_t>(frame)));
        if (!result) {
            fail(name, "encode_frame");
        }
    }
    return timer.stop(name, kFrames);
}

Result bench_eac3_auto(std::string name, perf::FrameSource& source, ac3::Acmod acmod, bool lfe,
                       std::uint32_t bitrate_kbps) {
    auto encoder = make_eac3_auto(acmod, lfe, bitrate_kbps);

    Timer timer;
    timer.start();
    for (int frame = 0; frame < kFrames; ++frame) {
        const auto result = encoder.encode_frame(source.frame(static_cast<std::size_t>(frame)));
        if (!result) {
            fail(name.c_str(), "encode_frame");
        }
    }
    return timer.stop(std::move(name), kFrames);
}

Result bench_atmos_4obj(perf::FrameSource& source, bool fast_mdct) {
    ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = 448, .fast_mdct = fast_mdct}, kObjects};
    const char* name = fast_mdct ? "atmos_4obj_fast_mdct" : "atmos_4obj";
    const auto placement = object_placement();

    Timer timer;
    timer.start();
    for (int frame = 0; frame < kFrames; ++frame) {
        const auto result =
            encoder.encode_frame(source.frame(static_cast<std::size_t>(frame)), placement);
        if (!result) {
            fail(name, "encode_frame");
        }
    }
    return timer.stop(name, kFrames);
}

// --- decode workload sources (untimed) -------------------------------------

std::vector<std::byte> encode_ac3_stream(perf::FrameSource& source) {
    auto encoder = make_ac3_51(/*fast_mdct=*/true);
    std::vector<std::byte> stream;
    stream.reserve(static_cast<std::size_t>(kFrames) * 2048);
    for (int frame = 0; frame < kFrames; ++frame) {
        const auto result = encoder.encode_frame(source.frame(static_cast<std::size_t>(frame)));
        if (!result) {
            fail("ac3_51_decode", "building its source stream");
        }
        stream.insert(stream.end(), result->begin(), result->end());
    }
    return stream;
}

std::vector<std::byte> encode_eac3_stream(perf::FrameSource& source) {
    auto encoder = make_eac3_auto(ac3::Acmod::k3_2, /*lfe=*/true, 448);
    std::vector<std::byte> stream;
    stream.reserve(static_cast<std::size_t>(kFrames) * 2048);
    for (int frame = 0; frame < kFrames; ++frame) {
        const auto result = encoder.encode_frame(source.frame(static_cast<std::size_t>(frame)));
        if (!result) {
            fail("eac3_51_decode", "building its source stream");
        }
        stream.insert(stream.end(), result->begin(), result->end());
    }
    return stream;
}

std::vector<std::byte> encode_atmos_stream(perf::FrameSource& source) {
    ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = 448, .fast_mdct = true}, kObjects};
    const auto placement = object_placement();
    std::vector<std::byte> stream;
    stream.reserve(static_cast<std::size_t>(kFrames) * 4096);
    for (int frame = 0; frame < kFrames; ++frame) {
        const auto result =
            encoder.encode_frame(source.frame(static_cast<std::size_t>(frame)), placement);
        if (!result) {
            fail("atmos_4obj_decode", "building its source stream");
        }
        stream.insert(stream.end(), result->bytes.begin(), result->bytes.end());
    }
    return stream;
}

// --- decode workloads ------------------------------------------------------
// Frame splitting is done before the clock starts: it is a scan over the
// stream's sync words, not decoding, and leaving it inside would put a cost
// that scales with the buffer rather than with the frame into a per-frame
// number.

Result bench_ac3_decode(std::span<const std::byte> stream) {
    const auto frames = ac3::split_frames(stream);
    if (!frames || frames->empty()) {
        fail("ac3_51_decode", "split_frames");
    }
    ac3::FrameDecoder decoder{};

    Timer timer;
    timer.start();
    for (const auto& frame : *frames) {
        const auto result = decoder.decode_frame(frame);
        if (!result) {
            fail("ac3_51_decode", "decode_frame");
        }
    }
    return timer.stop("ac3_51_decode", static_cast<int>(frames->size()));
}

Result bench_eac3_decode(std::string name, std::span<const std::byte> stream) {
    const auto units = ac3::split_access_units(stream);
    if (!units || units->empty()) {
        fail(name.c_str(), "split_access_units");
    }
    ac3::Eac3Decoder decoder{};

    Timer timer;
    timer.start();
    for (const auto& unit : *units) {
        const auto result = decoder.decode_access_unit(unit);
        if (!result) {
            fail(name.c_str(), "decode_access_unit");
        }
    }
    // Drained inside the timed span: the last access unit's samples only
    // leave the decoder here, so a flush left outside would hand the series
    // one frame's work for free.
    (void)decoder.flush();
    return timer.stop(std::move(name), static_cast<int>(units->size()));
}

void write_json(const std::vector<Result>& results, const std::string& path) {
    std::ofstream out(path);
    out << "{\n  \"real_time_budget_ms_per_frame\": " << real_time_budget_ms(1)
        << ",\n  \"results\": [\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        out << "    {\"name\": \"" << r.name << "\", \"frames\": " << r.frames
            << ", \"total_ms\": " << r.total_ms << ", \"ms_per_frame\": " << r.ms_per_frame
            << "}" << (i + 1 < results.size() ? "," : "") << "\n";
    }
    out << "  ]\n}\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string json_out;
    std::string wav_path = perf::kReference51Wav;
    // --only <name>, repeatable: run just these workloads. CI never passes it
    // (a trend run has to produce every series), but re-measuring one series
    // by hand should not cost the other eight - on a machine shared with
    // other work, a short run that can be repeated many times is worth far
    // more than one long one.
    std::vector<std::string> only;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--json-out" && i + 1 < argc) {
            json_out = argv[++i];
        } else if (arg == "--wav" && i + 1 < argc) {
            wav_path = argv[++i];
        } else if (arg == "--only" && i + 1 < argc) {
            only.emplace_back(argv[++i]);
        }
    }
    const auto wanted = [&only](std::string_view name) {
        return only.empty() || std::ranges::find(only, name) != only.end();
    };

    const ac3::io::WavData audio =
        perf::load_real_audio(wav_path, 6, static_cast<std::size_t>(ac3::kSamplesPerFrame));
    perf::FrameSource six_channel{audio, perf::kFiveOneChannels};
    perf::FrameSource two_channel{audio, perf::kStereoChannels};
    perf::FrameSource four_object{audio, perf::kFourObjectChannels};

    // Setup, before anything is timed - and only for the decode workloads
    // actually selected, so --only on an encode series does not pay for three
    // encode passes it will not read.
    std::vector<std::byte> ac3_stream;
    std::vector<std::byte> eac3_stream;
    std::vector<std::byte> atmos_stream;
    if (wanted("ac3_51_decode")) {
        ac3_stream = encode_ac3_stream(six_channel);
    }
    if (wanted("eac3_51_decode")) {
        eac3_stream = encode_eac3_stream(six_channel);
    }
    if (wanted("atmos_4obj_decode")) {
        atmos_stream = encode_atmos_stream(four_object);
    }

    std::vector<Result> results;
    if (wanted("plain_51")) {
        results.push_back(bench_ac3_51(six_channel, /*fast_mdct=*/false));
    }
    if (wanted("plain_51_fast_mdct")) {
        results.push_back(bench_ac3_51(six_channel, /*fast_mdct=*/true));
    }
    if (wanted("eac3_51_auto")) {
        results.push_back(
            bench_eac3_auto("eac3_51_auto", six_channel, ac3::Acmod::k3_2, /*lfe=*/true, 448));
    }
    if (wanted("eac3_stereo_auto")) {
        results.push_back(bench_eac3_auto("eac3_stereo_auto", two_channel, ac3::Acmod::k2_0,
                                          /*lfe=*/false, 192));
    }
    if (wanted("atmos_4obj")) {
        results.push_back(bench_atmos_4obj(four_object, /*fast_mdct=*/false));
    }
    if (wanted("atmos_4obj_fast_mdct")) {
        results.push_back(bench_atmos_4obj(four_object, /*fast_mdct=*/true));
    }
    if (wanted("ac3_51_decode")) {
        results.push_back(bench_ac3_decode(ac3_stream));
    }
    if (wanted("eac3_51_decode")) {
        results.push_back(bench_eac3_decode("eac3_51_decode", eac3_stream));
    }
    if (wanted("atmos_4obj_decode")) {
        results.push_back(bench_eac3_decode("atmos_4obj_decode", atmos_stream));
    }
    if (results.empty()) {
        std::fprintf(stderr, "ac3bench: --only matched no workload\n");
        return 1;
    }

    for (const auto& r : results) {
        std::printf("%-20s %5d frames  %9.3f ms total  %6.3f ms/frame  (budget %.3f ms/frame)\n",
                    r.name.c_str(), r.frames, r.total_ms, r.ms_per_frame, real_time_budget_ms(1));
    }

    if (!json_out.empty()) {
        write_json(results, json_out);
        std::printf("wrote %s\n", json_out.c_str());
    }

    return 0;
}
