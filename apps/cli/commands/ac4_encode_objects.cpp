#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fmt/base.h>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "../exit_codes.hpp"
#include "../support.hpp"
#include "iclforge/ac3/io/wav.hpp"
#include "iclforge/ac4/ac4.hpp"
#include "iclforge/ac4enc/encoder.hpp"
#include "encode.hpp"

// ac4-encode with objects=<scene>, experimental: the WAV file's channels as
// the objects of one object substream (ac4::ObjectsConfig), in the library's
// own terms, which the applications' scene readers of phase I5 will give it.
// The scene file is text, one directive a line, '#' starting a comment:
//
//   coding ajoc|direct
//   downmix computed|5.0|5.1
//   downmix-signals <n>
//   decorrelation on|off
//   object <channel> dynamic <x> <y> <z> [<gain dB>]
//   object <channel> bed L|R|C|Ls|Rs|Lb|Rb|Tfl|Tfr|Tsl|Tsr|Tbl|Tbr|Lw|Rw [<gain dB>]
//   object <channel> lfe
//   update <channel> <sample> <ramp samples> <x> <y> <z> [<gain dB>]
//
// Channels count from 0; one not named is a dynamic object at the room's
// centre. The stream is written raw, as sync frames.

namespace ac3cli::commands {
namespace {

[[nodiscard]] std::optional<double> number(std::string_view token) {
    double value = 0.0;
    const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
    if (error != std::errc{} || end != token.data() + token.size() || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<long long> integer(std::string_view token) {
    long long value = 0;
    const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
    if (error != std::errc{} || end != token.data() + token.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<ac4::BedChannel> bed_channel(std::string_view name) {
    using ac4::BedChannel;
    constexpr std::array<std::pair<std::string_view, BedChannel>, 15> kNames = {{
        {"L", BedChannel::kLeft},
        {"R", BedChannel::kRight},
        {"C", BedChannel::kCentre},
        {"Ls", BedChannel::kLeftSurround},
        {"Rs", BedChannel::kRightSurround},
        {"Lb", BedChannel::kLeftBack},
        {"Rb", BedChannel::kRightBack},
        {"Tfl", BedChannel::kTopFrontLeft},
        {"Tfr", BedChannel::kTopFrontRight},
        {"Tsl", BedChannel::kTopSideLeft},
        {"Tsr", BedChannel::kTopSideRight},
        {"Tbl", BedChannel::kTopBackLeft},
        {"Tbr", BedChannel::kTopBackRight},
        {"Lw", BedChannel::kLeftWide},
        {"Rw", BedChannel::kRightWide},
    }};
    for (const auto& [n, channel] : kNames) {
        if (n == name) {
            return channel;
        }
    }
    return std::nullopt;
}

struct Scene {
    ac4::ObjectsConfig objects;
    std::vector<ac4::ObjectMetadataUpdate> updates;
};

// A position and an optional gain from `tokens` from `first`, into `p`.
[[nodiscard]] bool read_position(std::span<const std::string> tokens, std::size_t first,
                                 ac4::ObjectProperties& p) {
    if (tokens.size() < first + 3 || tokens.size() > first + 4) {
        return false;
    }
    for (std::size_t k = 0; k < 3; ++k) {
        const std::optional<double> v = number(tokens[first + k]);
        if (!v) {
            return false;
        }
        p.position[k] = *v;
    }
    if (tokens.size() == first + 4) {
        const std::optional<double> gain = number(tokens[first + 3]);
        if (!gain) {
            return false;
        }
        p.gain_db = *gain;
    }
    return true;
}

// The scene file for `channels` objects, or nothing with the line it stopped
// at printed.
[[nodiscard]] std::optional<Scene> read_scene(const std::string& path, std::size_t channels) {
    std::ifstream in(path);
    if (!in) {
        fmt::println(stderr, "error: cannot open the scene file {}", path);
        return std::nullopt;
    }
    Scene scene;
    scene.objects.objects.resize(channels);
    std::string line;
    int number_of_line = 0;
    while (std::getline(in, line)) {
        ++number_of_line;
        if (const std::size_t hash = line.find('#'); hash != std::string::npos) {
            line.erase(hash);
        }
        std::istringstream words(line);
        std::vector<std::string> t;
        for (std::string word; words >> word;) {
            t.push_back(word);
        }
        if (t.empty()) {
            continue;
        }
        const auto channel_of = [&](std::size_t at) -> std::optional<std::size_t> {
            const std::optional<long long> c = t.size() > at ? integer(t[at]) : std::nullopt;
            if (!c || *c < 0 || static_cast<std::size_t>(*c) >= channels) {
                return std::nullopt;
            }
            return static_cast<std::size_t>(*c);
        };
        bool ok = false;
        if (t[0] == "coding" && t.size() == 2) {
            ok = t[1] == "ajoc" || t[1] == "direct";
            scene.objects.coding = t[1] == "direct" ? ac4::ObjectCoding::kDirect : ac4::ObjectCoding::kAjoc;
        } else if (t[0] == "downmix" && t.size() == 2) {
            ok = t[1] == "computed" || t[1] == "5.0" || t[1] == "5.1";
            scene.objects.downmix = t[1] == "5.0"   ? ac4::AjocDownmix::kStatic50
                                    : t[1] == "5.1" ? ac4::AjocDownmix::kStatic51
                                                    : ac4::AjocDownmix::kComputed;
        } else if (t[0] == "downmix-signals" && t.size() == 2) {
            const std::optional<long long> n = integer(t[1]);
            ok = n.has_value() && *n > 0 && *n < 64;
            scene.objects.downmix_signals = static_cast<int>(n.value_or(1));
        } else if (t[0] == "decorrelation" && t.size() == 2) {
            ok = t[1] == "on" || t[1] == "off";
            scene.objects.decorrelation = t[1] == "on";
        } else if (t[0] == "object" && t.size() >= 3) {
            if (const std::optional<std::size_t> c = channel_of(1)) {
                ac4::ObjectConfig& o = scene.objects.objects[*c];
                if (t[2] == "dynamic") {
                    ok = read_position(t, 3, o.properties);
                } else if (t[2] == "bed" && (t.size() == 4 || t.size() == 5)) {
                    o.bed = bed_channel(t[3]);
                    const std::optional<double> gain = t.size() == 5 ? number(t[4]) : 0.0;
                    ok = o.bed.has_value() && gain.has_value();
                    o.properties.gain_db = gain.value_or(0.0);
                } else if (t[2] == "lfe" && t.size() == 3) {
                    o.lfe = true;
                    ok = true;
                }
            }
        } else if (t[0] == "update" && t.size() >= 7) {
            const std::optional<std::size_t> c = channel_of(1);
            const std::optional<long long> sample = integer(t[2]);
            const std::optional<long long> ramp = integer(t[3]);
            if (c && sample && ramp && *sample >= 0 && *ramp >= 0 && *ramp <= 2048) {
                ac4::ObjectMetadataUpdate u;
                u.object = static_cast<int>(*c);
                u.sample = *sample;
                u.ramp_samples = static_cast<int>(*ramp);
                ok = read_position(t, 4, u.properties);
                scene.updates.push_back(u);
            }
        }
        if (!ok) {
            fmt::println(stderr, "error: {}:{}: not a scene directive (ac3cli help ac4-encode)",
                         path, number_of_line);
            return std::nullopt;
        }
    }
    return scene;
}

}  // namespace

int run_ac4_encode_objects(std::string_view in_path, std::string_view out_path,
                           std::uint32_t bitrate, const ac3cli::Options& meta) {
    const Options::Ac4Encode& opts = meta.ac4enc;
    auto wav = read_wav_arg(in_path);
    if (!wav.has_value()) {
        fmt::println(stderr, "error: {}: {}", in_path, ac3::io::describe(wav.error()));
        return kExitInput;
    }
    const std::optional<Scene> scene = read_scene(meta.ac4_objects_path, wav->channels.size());
    if (!scene) {
        return kExitUsage;
    }
    ac4::EncoderConfig config;
    config.sample_rate_hz = static_cast<int>(wav->sample_rate);
    config.frame_rate_index = opts.frame_rate_index;
    config.bitrate_kbps = static_cast<int>(bitrate);
    config.rate_mode = opts.rate_mode;
    config.dialnorm_db = -opts.dialnorm_db.value_or(31.0);
    if (opts.iframe_interval) {
        config.iframe_interval = *opts.iframe_interval;
    }
    config.experimental.objects = meta.ac4_experimental_objects;
    ac4::SubstreamConfig substream;
    substream.objects = scene->objects;
    substream.codec_mode = meta.ac4_codec_mode == "simple" ? ac4::CodecMode::kSimple
                           : meta.ac4_codec_mode == "aspx" ? ac4::CodecMode::kAspx
                                                           : ac4::CodecMode::kAuto;
    config.substreams = {substream};

    // syntax-trace=, as ac4-encode writes it. A frame's records start with
    // the substream a reader reads first, a direct-coded group's OAMD
    // substream or substream 0, where the frame count moves on.
    std::ofstream trace_file;
    long long trace_frame = -1;
    int first_substream = -1;
    if (!meta.syntax_trace_path.empty()) {
        trace_file.open(std::filesystem::path{meta.syntax_trace_path}, std::ios::binary);
        if (!trace_file) {
            fmt::println(stderr, "error: cannot open {} for writing", meta.syntax_trace_path);
            return kExitOutput;
        }
        config.trace = [&trace_file, &trace_frame, &first_substream](const ac4::SyntaxRecord& r) {
            if (first_substream < 0) {
                first_substream = r.substream;
            }
            if (r.substream == first_substream && r.bit_offset == 0) {
                ++trace_frame;
            }
            trace_file << trace_frame << '\t' << r.substream << '\t' << r.bit_offset << '\t'
                       << r.bits << '\t' << r.value << '\t' << r.name << '\n';
        };
    }
    auto encoder = ac4::Encoder::create(config);
    if (!encoder.has_value()) {
        fmt::println(stderr, "error: the encoder refuses {} (ac3cli help ac4-encode)",
                     ac4::Encoder::refusal_reason(config));
        return kExitUsage;
    }
    std::vector<std::span<const float>> views(wav->channels.begin(), wav->channels.end());
    auto frames = encoder->encode(views, scene->updates);
    if (!frames.has_value()) {
        fmt::println(stderr, "error: {}: {}", in_path, ac4::describe(frames.error()));
        return kExitInput;
    }
    auto rest = encoder->flush();
    if (!rest.has_value()) {
        fmt::println(stderr, "error: {}", ac4::describe(rest.error()));
        return kExitInput;
    }
    frames->insert(frames->end(), rest->begin(), rest->end());
    std::vector<std::vector<std::byte>> bytes;
    for (const ac4::EncodedFrame& frame : *frames) {
        bytes.push_back(ac4::sync_frame(frame.raw_ac4_frame, opts.crc.value_or(true)));
    }
    if (!write_frames(out_path, bytes)) {
        return kExitOutput;
    }
    if (trace_file.is_open()) {
        trace_file.close();
        if (!trace_file) {
            fmt::println(stderr, "error: cannot write {}", meta.syntax_trace_path);
            return kExitOutput;
        }
    }
    const auto status = status_stream(out_path);
    status_println(status, "encoded {} AC-4 frames -> {} ({} Hz, {} objects, {} kbps)",
                   frames->size(), out_path, config.sample_rate_hz, wav->channels.size(), bitrate);
    status_println(status, "          the decoder's output lags the input by {} samples",
                   encoder->delay_samples() + encoder->decoder_delay_samples());
    return kExitOk;
}

}  // namespace ac3cli::commands
