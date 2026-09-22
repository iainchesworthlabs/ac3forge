#include "truehd.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fmt/base.h>
#include <fstream>
#include <iterator>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ac3/io/wav.hpp"
#include "ac3/mlp/atmos.hpp"
#include "ac3/mlp/block.hpp"
#include "ac3/mlp/mlp_tables.hpp"
#include "ac3/mlp/stream.hpp"
#include "ac3/oba/atmos.hpp"
#include "ac3/oba/oamd.hpp"
#include "ac3/oba/scene.hpp"
#include "../exit_codes.hpp"
#include "../support.hpp"

namespace ac3cli::commands {

namespace {

std::optional<ac3::mlp::SampleRate> mlp_sample_rate(std::uint32_t hz) {
    switch (hz) {
        case 48000: return ac3::mlp::SampleRate::k48000;
        case 96000: return ac3::mlp::SampleRate::k96000;
        case 192000: return ac3::mlp::SampleRate::k192000;
        case 44100: return ac3::mlp::SampleRate::k44100;
        case 88200: return ac3::mlp::SampleRate::k88200;
        case 176400: return ac3::mlp::SampleRate::k176400;
        default: return std::nullopt;
    }
}

}  // namespace

int run_truehd_encode(std::string_view in_path, std::string_view out_path) {
    const auto wav = ac3::io::read_wav_pcm(std::string{in_path});
    if (!wav) {
        fmt::println(stderr, "error: {}: {}", in_path, ac3::io::describe(wav.error()));
        return kExitInput;
    }
    const auto rate = mlp_sample_rate(wav->sample_rate);
    if (!rate) {
        fmt::println(stderr,
                     "error: {} Hz is not an MLP sample rate "
                     "(48000/96000/192000 or 44100/88200/176400)",
                     wav->sample_rate);
        return kExitInput;
    }
    const auto channel_count = wav->channels.size();
    if (channel_count > static_cast<std::size_t>(ac3::mlp::kMaxBlockChannels)) {
        fmt::println(stderr, "error: {} channels (this encoder carries up to {})", channel_count,
                     ac3::mlp::kMaxBlockChannels);
        return kExitUsage;
    }
    const auto frame = static_cast<std::size_t>(ac3::mlp::samples_per_access_unit(*rate));
    const std::size_t source_frames = wav->frame_count();
    if (source_frames == 0) {
        fmt::println(stderr, "error: {} holds no samples", in_path);
        return kExitInput;
    }
    // An access unit is a fixed number of samples (40 at 48 kHz), so the tail
    // is filled with silence rather than dropped. The fill count rides in the
    // final access unit's zero_samples terminator field (§4.6.4), so a decoder
    // trims it back off and returns the source's exact length.
    const std::size_t padded = (source_frames + frame - 1) / frame * frame;
    std::vector<std::vector<std::int32_t>> channels = wav->channels;
    for (auto& channel : channels) {
        channel.resize(padded, 0);
    }

    ac3::mlp::StreamConfig config;
    config.sample_rate = *rate;
    config.wordlength = wav->bits;
    config.channels = static_cast<int>(channel_count);
    config.automatic = true;

    // Two passes: §4.2.6 defines peak_data_rate as the maximum effective rate
    // over the WHOLE stream, which a single pass can't know when it writes the
    // first major sync. The field is fixed-width, so a second pass with the
    // measured value produces identical access units apart from the field
    // (and the CRC covering it).
    const auto encode_all = [&](ac3::mlp::StreamEncoder& encoder, std::ofstream* out) {
        std::size_t written = 0;
        for (std::size_t at = 0; at < padded; at += frame) {
            std::vector<std::span<const std::int32_t>> spans;
            spans.reserve(channel_count);
            for (const auto& channel : channels) {
                spans.emplace_back(std::span<const std::int32_t>{channel}.subspan(at, frame));
            }
            const std::span<const std::span<const std::int32_t>> view{spans};
            const bool last = at + frame >= padded;
            const auto unit =
                last ? encoder.encode_access_unit(
                           view, ac3::mlp::EndOfStream{static_cast<int>(padded - source_frames)})
                     : encoder.encode_access_unit(view);
            if (out != nullptr) {
                out->write(reinterpret_cast<const char*>(unit.data()),
                           static_cast<std::streamsize>(unit.size()));
            }
            written += unit.size();
        }
        return written;
    };

    ac3::mlp::StreamEncoder measuring(config);
    (void)encode_all(measuring, nullptr);
    config.peak_data_rate_16ths = measuring.measured_peak_data_rate_16ths();

    std::ofstream out{std::string{out_path}, std::ios::binary};
    if (!out) {
        fmt::println(stderr, "error: cannot open {}", out_path);
        return kExitOutput;
    }
    ac3::mlp::StreamEncoder encoder(config);
    const std::size_t written = encode_all(encoder, &out);
    if (!out) {
        fmt::println(stderr, "error: writing {} failed", out_path);
        return kExitOutput;
    }
    const std::size_t pcm_bytes =
        padded * channel_count * static_cast<std::size_t>(wav->bits) / 8;
    fmt::println("wrote {} access units to {}", padded / frame, out_path);
    fmt::println("{} PCM bytes -> {} MLP bytes ({:.1f}% of source, {}-bit, {} ch, {} Hz)",
                 pcm_bytes, written, 100.0 * static_cast<double>(written) / pcm_bytes, wav->bits,
                 channel_count, wav->sample_rate);
    // peak_data_rate is 1/16 bit per sample period; x rate / 16 is bit/s.
    fmt::println("peak data rate {:.1f} kbit/s (measured; FBA channel ceiling {} kbit/s)",
                 static_cast<double>(config.peak_data_rate_16ths) * wav->sample_rate / 16000.0,
                 ac3::mlp::kPeakDataRateBitsPerSecond / 1000);
    if (padded != source_frames) {
        fmt::println("note: final access unit filled with {} silent samples "
                     "(recorded in-stream; decode trims them)",
                     padded - source_frames);
    }
    if (encoder.rate_violations() != 0) {
        fmt::println(stderr,
                     "warning: {} access units exceed the 18 Mbit/s FBA channel - a "
                     "spec-minimum decoder FIFO could underrun",
                     encoder.rate_violations());
    }
    return kExitOk;
}

int run_truehd_decode(std::string_view in_path, std::string_view out_path) {
    const auto stream = read_all(in_path);
    if (stream.empty()) {
        fmt::println(stderr, "error: cannot read {}", in_path);
        return kExitInput;
    }
    std::span<const std::byte> data{stream};

    // §5's file format allows a 16-byte SMPTE ST 339 timestamp before the
    // stream proper. The stream itself must open with a major sync, whose
    // format_sync sits 4 bytes into the first access unit - so look for it
    // there, and failing that, one header-skip later.
    const auto format_sync_at = [&data](std::size_t at) {
        if (data.size() < at + 8) {
            return false;
        }
        std::uint32_t word = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            word = (word << 8) | std::to_integer<std::uint32_t>(data[at + 4 + i]);
        }
        return word == ac3::mlp::kFormatSync;
    };
    if (!format_sync_at(0) && format_sync_at(16)) {
        data = data.subspan(16);
    }

    ac3::mlp::StreamDecoder decoder;
    std::vector<std::vector<std::int32_t>> channels;
    std::size_t units = 0;
    while (!data.empty()) {
        // mlp_sync's length field frames the stream: the low 12 bits of the
        // first 16 bits are the access unit's length in 16-bit words.
        if (data.size() < 8) {
            fmt::println(stderr, "error: {}: {} trailing bytes after access unit {}", in_path,
                         data.size(), units);
            return kExitInput;
        }
        const std::size_t length_words =
            (std::to_integer<std::size_t>(data[0]) & 0x0F) << 8 |
            std::to_integer<std::size_t>(data[1]);
        const std::size_t unit_bytes = length_words * 2;
        if (unit_bytes < 8 || unit_bytes > data.size()) {
            fmt::println(stderr, "error: {}: access unit {} declares {} bytes, {} remain",
                         in_path, units, unit_bytes, data.size());
            return kExitInput;
        }
        std::vector<std::vector<std::int32_t>> unit_channels;
        if (!decoder.decode_access_unit(data.first(unit_bytes), unit_channels)) {
            fmt::println(stderr, "error: {}: access unit {} failed to decode", in_path, units);
            return kExitInput;
        }
        if (channels.empty()) {
            channels.resize(unit_channels.size());
        }
        for (std::size_t ch = 0; ch < unit_channels.size(); ++ch) {
            channels[ch].insert(channels[ch].end(), unit_channels[ch].begin(),
                                unit_channels[ch].end());
        }
        data = data.subspan(unit_bytes);
        ++units;
    }
    if (channels.empty()) {
        fmt::println(stderr, "error: {} holds no access units", in_path);
        return kExitInput;
    }

    // §4.6.4: the final access unit's zero_samples field records how much
    // silence the encoder appended to fill it - trim it back off so the
    // output is length-exact against the source, not just content-exact.
    const auto trim = static_cast<std::size_t>(
        decoder.end_of_stream() ? decoder.zero_samples_appended() : 0);
    if (trim > 0 && trim <= channels.front().size()) {
        for (auto& channel : channels) {
            channel.resize(channel.size() - trim);
        }
    }

    const auto rate_hz = ac3::mlp::sample_rate_hz(decoder.sample_rate());
    const int bits = decoder.wordlength() <= 16 ? 16 : 24;
    if (const auto result = ac3::io::write_wav_pcm(std::string{out_path}, channels, rate_hz, bits);
        !result) {
        fmt::println(stderr, "error: {}: {}", out_path, ac3::io::describe(result.error()));
        return kExitOutput;
    }
    fmt::println("decoded {} access units: {} samples, {}-bit, {} ch, {} Hz -> {}", units,
                 channels.front().size(), decoder.wordlength(), channels.size(), rate_hz,
                 out_path);
    if (trim > 0) {
        fmt::println("trimmed {} encoder-fill samples recorded by the stream's terminator", trim);
    }
    if (!decoder.end_of_stream()) {
        fmt::println("note: stream carries no end-of-stream terminator (truncated capture?)");
    }
    return kExitOk;
}

int run_truehd_atmos(std::string_view in_path, std::string_view out_path,
                     std::uint32_t objects, std::string_view paths_path) {
    const auto wav = ac3::io::read_wav_pcm(std::string{in_path});
    if (!wav) {
        fmt::println(stderr, "error: {}: {}", in_path, ac3::io::describe(wav.error()));
        return kExitInput;
    }
    const auto rate = mlp_sample_rate(wav->sample_rate);
    if (!rate) {
        fmt::println(stderr,
                     "error: {} Hz is not an MLP sample rate "
                     "(48000/96000/192000 or 44100/88200/176400)",
                     wav->sample_rate);
        return kExitInput;
    }
    const std::size_t src_channels = wav->channels.size();
    // One object per source channel unless told otherwise; the presentation
    // ceiling is the codec's own 16 channels (§4.4.3).
    const auto count =
        objects == 0 ? src_channels : std::min<std::size_t>(objects, src_channels);
    if (count < 1 || count > 16) {
        fmt::println(stderr, "error: 1 to 16 objects (§4.4.3's presentation ceiling); "
                             "this file has {} channels",
                     src_channels);
        return kExitUsage;
    }
    const auto frame = static_cast<std::size_t>(ac3::mlp::samples_per_access_unit(*rate));
    const std::size_t source_frames = wav->frame_count();
    if (source_frames == 0) {
        fmt::println(stderr, "error: {} holds no samples", in_path);
        return kExitInput;
    }
    const std::size_t padded = (source_frames + frame - 1) / frame * frame;
    std::vector<std::vector<std::int32_t>> channels(wav->channels.begin(),
                                                    wav->channels.begin() +
                                                        static_cast<std::ptrdiff_t>(count));
    for (auto& channel : channels) {
        channel.resize(padded, 0);
    }

    // Default placement: objects fan out evenly around the room at ear
    // height, full level - discrete lossless channels carry their authored
    // levels, so unlike atmos-encode's bed there is no summation to
    // compensate for.
    std::vector<ac3::oba::ObjectPlacement> placement(count);
    for (std::size_t i = 0; i < count; ++i) {
        const double radians =
            2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(count);
        placement[i] = {.position = {.x = 0.5 - 0.5 * std::sin(radians),
                                     .y = 0.5 - 0.5 * std::cos(radians),
                                     .z = 0.0},
                        .gain = 1.0,
                        .lfe_send = 0.0};
    }

    // A scene file (the keyframe grammar atmos-path takes, or the JSON
    // object-scene form - see ac3/oba/scene.hpp; object index == WAV channel
    // index) drives motion instead of the static fan-out above. An index the
    // file skips holds the fan-out placement it already had, never moving.
    std::optional<ac3::oba::ObjectScene> scene;
    if (!paths_path.empty()) {
        std::ifstream in{std::string{paths_path}, std::ios::binary};
        if (!in) {
            fmt::println(stderr, "error: cannot open {}", paths_path);
            return kExitInput;
        }
        const std::string text{std::istreambuf_iterator<char>{in},
                               std::istreambuf_iterator<char>{}};
        auto contents = ac3::oba::read_scene(text);
        if (!contents) {
            // Line 0 means the format had no line to point at (a JSON-level
            // complaint about the scene as a whole).
            if (contents.error().line != 0) {
                fmt::println(stderr, "error: {}:{}: {}", paths_path, contents.error().line,
                             contents.error().message);
            } else {
                fmt::println(stderr, "error: {}: {}", paths_path, contents.error().message);
            }
            return kExitInput;
        }
        contents->objects.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            if (contents->objects[i].automation.empty()) {
                contents->objects[i].automation.push_back({.time_s = 0.0,
                                                            .position = placement[i].position,
                                                            .gain = placement[i].gain,
                                                            .lfe_send = 0.0});
            }
        }
        auto created =
            ac3::oba::ObjectScene::create(std::move(contents->objects), contents->orientation);
        if (!created) {
            fmt::println(stderr, "error: {}: {}", paths_path, created.error().message);
            return kExitInput;
        }
        scene = std::move(*created);
    }

    // The per-frame object list handed to the encoder: positions from the
    // (possibly moving) scene, evaluated at each frame's END time (the
    // atmos-path/GUI convention); OAMD object gain from the scene's linear
    // gain (0 dB at unity - the audio itself already carries its authored
    // level, this is renderer trim only).
    const auto objects_at = [&](std::size_t end_sample) {
        std::vector<ac3::oba::DynamicObject> out(count);
        const auto placed =
            scene ? scene->evaluate(static_cast<double>(end_sample) /
                                    static_cast<double>(wav->sample_rate))
                  : placement;
        for (std::size_t i = 0; i < count; ++i) {
            out[i].position = placed[i].position;
            const double gain = placed[i].gain;
            out[i].gain_db =
                gain == 1.0 ? 0.0
                            : std::clamp(20.0 * std::log10(std::max(gain, 1e-3)), -49.0, 15.0);
        }
        return out;
    };

    ac3::mlp::AtmosConfig config;
    config.sample_rate = *rate;
    config.wordlength = wav->bits;
    config.bed = 0;  // objects only - every channel is a dynamic object
    config.dynamic_objects = static_cast<int>(count);

    // The same two-pass peak_data_rate treatment as truehd-encode.
    const auto encode_all = [&](ac3::mlp::AtmosEncoder& encoder, std::ofstream* out) {
        std::size_t written = 0;
        for (std::size_t at = 0; at < padded; at += frame) {
            std::vector<std::span<const std::int32_t>> spans;
            spans.reserve(count);
            for (const auto& channel : channels) {
                spans.emplace_back(std::span<const std::int32_t>{channel}.subspan(at, frame));
            }
            const std::span<const std::span<const std::int32_t>> view{spans};
            const auto frame_objects = objects_at(at + frame);
            const bool last = at + frame >= padded;
            const auto unit =
                last ? encoder.encode_access_unit(
                           view, frame_objects,
                           ac3::mlp::EndOfStream{static_cast<int>(padded - source_frames)})
                     : encoder.encode_access_unit(view, frame_objects);
            if (out != nullptr) {
                out->write(reinterpret_cast<const char*>(unit.data()),
                           static_cast<std::streamsize>(unit.size()));
            }
            written += unit.size();
        }
        return written;
    };

    ac3::mlp::AtmosEncoder measuring(config);
    (void)encode_all(measuring, nullptr);
    config.peak_data_rate_16ths = measuring.measured_peak_data_rate_16ths();

    std::ofstream out{std::string{out_path}, std::ios::binary};
    if (!out) {
        fmt::println(stderr, "error: cannot open {}", out_path);
        return kExitOutput;
    }
    ac3::mlp::AtmosEncoder encoder(config);
    const std::size_t written = encode_all(encoder, &out);
    if (!out) {
        fmt::println(stderr, "error: writing {} failed", out_path);
        return kExitOutput;
    }
    const std::size_t pcm_bytes = padded * count * static_cast<std::size_t>(wav->bits) / 8;
    fmt::println("wrote {} access units to {}: {} dynamic objects as discrete lossless channels",
                 padded / frame, out_path, count);
    fmt::println("{} PCM bytes -> {} MLP bytes ({:.1f}% of source, {}-bit, {} Hz, "
                 "OAMD every {} access units)",
                 pcm_bytes, written, 100.0 * static_cast<double>(written) / pcm_bytes, wav->bits,
                 wav->sample_rate, config.metadata_interval);
    fmt::println("peak data rate {:.1f} kbit/s (measured; FBA channel ceiling {} kbit/s)",
                 static_cast<double>(config.peak_data_rate_16ths) * wav->sample_rate / 16000.0,
                 ac3::mlp::kPeakDataRateBitsPerSecond / 1000);
    if (count < src_channels) {
        fmt::println("note: only the first {} of {} source channels are carried", count,
                     src_channels);
    }
    if (padded != source_frames) {
        fmt::println("note: final access unit filled with {} silent samples "
                     "(recorded in-stream; decode trims them)",
                     padded - source_frames);
    }
    if (encoder.rate_violations() != 0) {
        fmt::println(stderr,
                     "warning: {} access units exceed the 18 Mbit/s FBA channel - a "
                     "spec-minimum decoder FIFO could underrun",
                     encoder.rate_violations());
    }
    return kExitOk;
}

}  // namespace ac3cli::commands
