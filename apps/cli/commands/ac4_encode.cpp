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

#include "../ac4_channels.hpp"
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
// What the encoder writes so far: mono, stereo, 5.0 and 5.1, and with
// experimental=7x-... 7.0 and 7.1, at 48 or 44.1 kHz, frame_rate_index 13, the
// SIMPLE, ASPX or A-CPL codec modes, a constant bit rate. The WAV file's
// channels are taken in the order `decode` writes them (ac4_channels.hpp).

namespace ac3cli::commands {
namespace {

// The codec mode as Part 1 Table 95 names it.
[[nodiscard]] std::string_view mode_name(ac4::CodecMode mode) {
    switch (mode) {
        case ac4::CodecMode::kAspx:
            return "ASPX";
        case ac4::CodecMode::kAspxAcpl1:
            return "ASPX_ACPL_1";
        case ac4::CodecMode::kAspxAcpl2:
            return "ASPX_ACPL_2";
        case ac4::CodecMode::kAspxAcpl3:
            return "ASPX_ACPL_3";
        case ac4::CodecMode::kAuto:
        case ac4::CodecMode::kSimple:
            break;
    }
    return "SIMPLE";
}

// Matched as 'remux' matches them: std::filesystem::path::extension(), case
// kept.
[[nodiscard]] bool names_mp4(std::string_view out_path) {
    constexpr std::array<std::string_view, 3> kMp4Exts{".mp4", ".m4a", ".mov"};
    const std::string ext = std::filesystem::path{std::string{out_path}}.extension().string();
    return std::ranges::any_of(kMp4Exts, [&](std::string_view candidate) { return ext == candidate; });
}

// The encoder's input channels, in ac4::Decoder's order, for a WAV file of
// `count` channels and the 7.X element's additional pair; empty for a count
// the encoder does not take with that pair.
[[nodiscard]] std::vector<ac4::Speaker> input_speakers(std::size_t count, ac4::AdditionalPair pair) {
    using S = ac4::Speaker;
    const bool seven = count == 7 || count == 8;
    if (seven != (pair != ac4::AdditionalPair::kNone)) {
        return {};
    }
    switch (count) {
        case 1:
            return {S::kCentre};
        case 2:
            return {S::kLeft, S::kRight};
        case 5:
        case 6:
        case 7:
        case 8:
            break;
        default:
            return {};
    }
    std::vector<S> out = {S::kLeft, S::kRight, S::kCentre};
    if (count % 2 == 0) {
        out.push_back(S::kLfe);
    }
    out.push_back(S::kLeftSurround);
    out.push_back(S::kRightSurround);
    if (pair == ac4::AdditionalPair::kBack) {
        out.insert(out.end(), {S::kLeftBack, S::kRightBack});
    } else if (pair == ac4::AdditionalPair::kWide) {
        out.insert(out.end(), {S::kLeftWide, S::kRightWide});
    } else if (pair == ac4::AdditionalPair::kTopFront) {
        out.insert(out.end(), {S::kTopFrontLeft, S::kTopFrontRight});
    }
    return out;
}

[[nodiscard]] std::string_view layout_name(std::size_t count, ac4::AdditionalPair pair) {
    switch (count) {
        case 1:
            return "mono";
        case 2:
            return "stereo";
        case 5:
            return "5.0";
        case 6:
            return "5.1";
        default:
            break;
    }
    const bool lfe = count == 8;
    switch (pair) {
        case ac4::AdditionalPair::kBack:
            return lfe ? "7.1, 3/4/0" : "7.0, 3/4/0";
        case ac4::AdditionalPair::kWide:
            return lfe ? "7.1, 5/2/0" : "7.0, 5/2/0";
        default:
            return lfe ? "7.1, 3/2/2" : "7.0, 3/2/2";
    }
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
    ac4::AdditionalPair pair = ac4::AdditionalPair::kNone;
    if (meta.ac4_experimental_seven_x == "back") {
        pair = ac4::AdditionalPair::kBack;
    } else if (meta.ac4_experimental_seven_x == "wide") {
        pair = ac4::AdditionalPair::kWide;
    } else if (meta.ac4_experimental_seven_x == "top-front") {
        pair = ac4::AdditionalPair::kTopFront;
    }
    const std::vector<ac4::Speaker> speakers = input_speakers(wav->channels.size(), pair);
    if (speakers.empty()) {
        fmt::println(stderr,
                     "error: {}: AC-4 encoding takes mono, stereo, 5.0 and 5.1, and 7.0 and 7.1 with "
                     "experimental=7x-back, 7x-wide or 7x-top-front; the source has {} channels{}",
                     in_path, wav->channels.size(), pair != ac4::AdditionalPair::kNone ? " and a 7.X pair was named" : "");
        return kExitInput;
    }
    if (wav->sample_rate != 48000 && wav->sample_rate != 44100) {
        fmt::println(stderr, "error: {}: AC-4 encoding takes 48 or 44.1 kHz; the source is {} Hz", in_path,
                     wav->sample_rate);
        return kExitInput;
    }

    ac4::EncoderConfig config;
    config.channels = static_cast<int>(speakers.size());
    config.sample_rate_hz = static_cast<int>(wav->sample_rate);
    config.bitrate_kbps = static_cast<int>(bitrate);
    if (meta.ac4_codec_mode == "simple") {
        config.codec_mode = ac4::CodecMode::kSimple;
    } else if (meta.ac4_codec_mode == "aspx") {
        config.codec_mode = ac4::CodecMode::kAspx;
    } else if (meta.ac4_codec_mode == "aspx-acpl-1") {
        config.codec_mode = ac4::CodecMode::kAspxAcpl1;
    } else if (meta.ac4_codec_mode == "aspx-acpl-2") {
        config.codec_mode = ac4::CodecMode::kAspxAcpl2;
    } else if (meta.ac4_codec_mode == "aspx-acpl-3") {
        config.codec_mode = ac4::CodecMode::kAspxAcpl3;
    }
    config.experimental.aspx_balance = meta.ac4_experimental_balance;
    config.experimental.aspx_varvar = meta.ac4_experimental_varvar;
    config.experimental.aspx_interleave = meta.ac4_experimental_interleave;
    config.experimental.coding_configs = meta.ac4_experimental_coding_configs;
    config.experimental.acpl = meta.ac4_experimental_acpl;
    config.experimental.seven_x = pair;
    // The rate is checked before dialnorm=auto reads the whole file.
    if (!ac4::Encoder::create(config).has_value()) {
        fmt::println(stderr, "error: {}", ac4::describe(ac4::EncodeError::kInvalidConfig));
        return kExitUsage;
    }
    // Each of the encoder's channels' place in the WAV file.
    const std::vector<std::size_t> wav_order = ac4_order(std::span{speakers}, ac4_wav_rank);
    std::vector<std::size_t> wav_index(speakers.size());
    for (std::size_t w = 0; w < wav_order.size(); ++w) {
        wav_index[wav_order[w]] = w;
    }

    // dialnorm= as for encode: 1 to 31, the dialogue level in -dB; auto
    // measures it with BS.1770 over the whole file: over the 5.1 or 5.0 bed of
    // a 7.X layout, as encode measures E-AC-3's.
    int dialnorm = meta.p.dialnorm;
    if (meta.p.measure_dialnorm) {
        const auto rate = wav->sample_rate == 48000 ? ac3::SampleRate::k48000 : ac3::SampleRate::k44100;
        const bool lfe = speakers.size() % 2 == 0 && speakers.size() > 2;
        const auto acmod = speakers.size() == 1 ? ac3::Acmod::k1_0
                                                : (speakers.size() == 2 ? ac3::Acmod::k2_0 : ac3::Acmod::k3_2);
        ac3::io::WavData bed;
        const ac3::io::WavData* measure = &*wav;
        if (speakers.size() > 6) {
            // The bed in 5.1's or 5.0's WAV order: L R C, the LFE, Ls Rs.
            bed.sample_rate = wav->sample_rate;
            for (std::size_t k = 0; k < (lfe ? 6U : 5U); ++k) {
                bed.channels.push_back(wav->channels[wav_index[k]]);
            }
            measure = &bed;
        }
        const auto measured = measured_dialnorm(*measure, rate, acmod, lfe, status_stream(out_path));
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
    for (const std::size_t w : wav_index) {
        views.emplace_back(wav->channels[w]);
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
                   out_path, config.sample_rate_hz, layout_name(speakers.size(), pair), bitrate, dialnorm,
                   to_mp4 ? fmt::format(", MP4, codecs {}", rfc6381) : std::string{", raw with CRC"});
    // A decoder's own delay at frame_rate_index 13: d_pcm (Part 1 Table 188),
    // the QMF banks' 577 samples and six QMF slots (5.7.1).
    constexpr int kDecoderDelay = 352 + 577 + 6 * 64;
    status_println(status, "          {} mode at frame_rate_index 13; the decoder's output lags the input by {} "
                           "samples",
                   mode_name(encoder->codec_mode()),
                   encoder->delay_samples() + kDecoderDelay);
    return kExitOk;
}

}  // namespace ac3cli::commands
