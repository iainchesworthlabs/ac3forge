#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fmt/base.h>
#include <fmt/format.h>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../exit_codes.hpp"
#include "../support.hpp"
#include "ac3/io/wav.hpp"
#include "ac4/ac4.hpp"
#include "ac4enc/encoder.hpp"
#include "encode.hpp"
#include "mp4/mp4.hpp"

// ac4-encode: WAV to AC-4 through ac4::Encoder (src/ac4enc), as a raw stream
// of sync frames with the CRC of TS 103 190-2 Annex G, or in an MP4 file with
// Annex E's 'ac-4' sample entry when the output is named .mp4, .m4a or .mov.
// What the encoder writes so far: mono or stereo at 48 or 44.1 kHz,
// frame_rate_index 13, the SIMPLE or ASPX codec mode, a constant bit rate.

namespace ac3cli::commands {
namespace {

// Matched as 'remux' matches them: std::filesystem::path::extension(), case
// kept.
[[nodiscard]] bool names_mp4(std::string_view out_path) {
    constexpr std::array<std::string_view, 3> kMp4Exts{".mp4", ".m4a", ".mov"};
    const std::string ext = std::filesystem::path{std::string{out_path}}.extension().string();
    return std::ranges::any_of(kMp4Exts, [&](std::string_view candidate) { return ext == candidate; });
}

}  // namespace

int run_ac4_encode(std::string_view in_path, std::string_view out_path, std::uint32_t bitrate,
                   const ac3cli::Options& meta) {
    // AC-4 carries DRC and downmix data in metadata() elements this encoder
    // does not write yet, so asking for them is refused rather than dropped.
    if (meta.p.drc.has_value() || meta.p.heavy.has_value() || meta.p.mixmeta || meta.p.infomdat) {
        fmt::println(stderr,
                     "error: of the metadata options, ac4-encode writes dialnorm= alone so far; drc=, heavy, "
                     "mixmeta and infomdat are not written to AC-4 yet");
        return kExitUsage;
    }
    const auto wav = read_wav_arg(in_path);
    if (!wav.has_value()) {
        fmt::println(stderr, "error: {}: {}", in_path, ac3::io::describe(wav.error()));
        return kExitInput;
    }
    if (wav->channels.size() != 1 && wav->channels.size() != 2) {
        fmt::println(stderr, "error: {}: AC-4 encoding takes mono or stereo so far; the source has {} channels",
                     in_path, wav->channels.size());
        return kExitInput;
    }
    if (wav->sample_rate != 48000 && wav->sample_rate != 44100) {
        fmt::println(stderr, "error: {}: AC-4 encoding takes 48 or 44.1 kHz; the source is {} Hz", in_path,
                     wav->sample_rate);
        return kExitInput;
    }

    ac4::EncoderConfig config;
    config.channels = static_cast<int>(wav->channels.size());
    config.sample_rate_hz = static_cast<int>(wav->sample_rate);
    config.bitrate_kbps = static_cast<int>(bitrate);
    if (meta.ac4_codec_mode == "simple") {
        config.codec_mode = ac4::CodecMode::kSimple;
    } else if (meta.ac4_codec_mode == "aspx") {
        config.codec_mode = ac4::CodecMode::kAspx;
    }
    config.experimental.aspx_balance = meta.ac4_experimental_balance;
    config.experimental.aspx_varvar = meta.ac4_experimental_varvar;
    config.experimental.aspx_interleave = meta.ac4_experimental_interleave;
    // The rate is checked before dialnorm=auto reads the whole file.
    if (!ac4::Encoder::create(config).has_value()) {
        fmt::println(stderr, "error: {}", ac4::describe(ac4::EncodeError::kInvalidConfig));
        return kExitUsage;
    }
    // dialnorm= as for encode: 1 to 31, the dialogue level in -dB; auto
    // measures it with BS.1770 over the whole file.
    int dialnorm = meta.p.dialnorm;
    if (meta.p.measure_dialnorm) {
        const auto rate = wav->sample_rate == 48000 ? ac3::SampleRate::k48000 : ac3::SampleRate::k44100;
        const auto acmod = wav->channels.size() == 1 ? ac3::Acmod::k1_0 : ac3::Acmod::k2_0;
        const auto measured = measured_dialnorm(*wav, rate, acmod, false, status_stream(out_path));
        if (!measured.has_value()) {
            fmt::println(stderr, "error: no audio above the -70 LKFS absolute gate; pass dialnorm=<1..31> explicitly");
            return kExitRuntime;
        }
        dialnorm = *measured;
    }
    config.dialnorm_db = -static_cast<double>(dialnorm);

    // syntax-trace=: every record the encoder writes, as ac4_syntax.py's
    // `trace` writes what it reads. A frame's first record is its substream
    // 0's first element, which is where the frame count moves on.
    std::ofstream trace_file;
    long long trace_frame = -1;
    const auto trace = [&trace_file, &trace_frame](const ac4::SyntaxRecord& r) {
        if (r.substream == 0 && r.bit_offset == 0) {
            ++trace_frame;
        }
        trace_file << trace_frame << '\t' << r.substream << '\t' << r.bit_offset << '\t' << r.bits << '\t'
                   << r.value << '\t' << r.name << '\n';
    };
    if (!meta.syntax_trace_path.empty()) {
        trace_file.open(std::filesystem::path{meta.syntax_trace_path}, std::ios::binary);
        if (!trace_file) {
            fmt::println(stderr, "error: cannot open {} for writing", meta.syntax_trace_path);
            return kExitOutput;
        }
        config.trace = trace;
    }

    auto encoder = ac4::Encoder::create(config);
    if (!encoder.has_value()) {
        fmt::println(stderr, "error: {}", ac4::describe(encoder.error()));
        return kExitUsage;
    }
    std::vector<std::span<const float>> views;
    for (const std::vector<float>& channel : wav->channels) {
        views.emplace_back(channel);
    }
    auto frames = encoder->encode(views);
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
    const bool to_mp4 = names_mp4(out_path);
    std::string rfc6381;
    if (to_mp4) {
        std::vector<std::span<const std::byte>> samples;
        samples.reserve(frames->size());
        for (const ac4::EncodedFrame& frame : *frames) {
            samples.emplace_back(frame.raw_ac4_frame);
        }
        const ac4::Toc& toc = encoder->toc();
        const auto samples_per_frame = ac4::samples_per_frame(toc);
        const mp4::AudioTrack track{
            .codec_id = std::string{mp4::kCodecAc4},
            .sample_rate = static_cast<std::uint32_t>(toc.sample_rate_hz),
            .channels = 2,  // TS 103 190-2 E.4.5: "should be set to 2"
            .samples_per_frame = samples_per_frame.value_or(2048),
            .codec_config = ac4::build_dac4(toc),
            .rfc6381 = ac4::rfc6381_codec_string(toc)};
        rfc6381 = track.rfc6381;
        auto muxed = mp4::mux(track, samples);
        if (!muxed.has_value()) {
            fmt::println(stderr, "error: {}", mp4::describe(muxed.error()));
            return kExitOutput;
        }
        bytes.push_back(std::move(*muxed));
    } else {
        bytes.reserve(frames->size());
        for (const ac4::EncodedFrame& frame : *frames) {
            bytes.push_back(ac4::sync_frame(frame.raw_ac4_frame, true));
        }
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
    status_println(status, "encoded {} AC-4 frames -> {} ({} Hz, {}, {} kbps, dialnorm -{} dB{})", frames->size(),
                   out_path, config.sample_rate_hz, config.channels == 2 ? "stereo" : "mono", bitrate, dialnorm,
                   to_mp4 ? fmt::format(", MP4, codecs {}", rfc6381) : std::string{", raw with CRC"});
    // A decoder's own delay at frame_rate_index 13: d_pcm (Part 1 Table 188),
    // the QMF banks' 577 samples and six QMF slots (5.7.1).
    constexpr int kDecoderDelay = 352 + 577 + 6 * 64;
    status_println(status, "          {} mode at frame_rate_index 13; the decoder's output lags the input by {} "
                           "samples",
                   encoder->codec_mode() == ac4::CodecMode::kAspx ? "ASPX" : "SIMPLE",
                   encoder->delay_samples() + kDecoderDelay);
    return kExitOk;
}

}  // namespace ac3cli::commands
