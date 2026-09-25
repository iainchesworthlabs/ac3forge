#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fmt/base.h>
#include <fmt/format.h>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../ac4_channels.hpp"
#include "../exit_codes.hpp"
#include "../support.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/meta/loudness.hpp"
#include "ac4/ac4.hpp"
#include "ac4enc/encoder.hpp"
#include "encode.hpp"
#include "mp4/mp4.hpp"

// ac4-encode: WAV to AC-4 through ac4::Encoder (src/ac4enc), as a raw stream
// of sync frames with the CRC of TS 103 190-2 Annex G, or in an MP4 file with
// Annex E's 'ac-4' sample entry when the output is named .mp4, .m4a or .mov.
// What the encoder writes so far: mono, stereo, 5.0 and 5.1, and with
// experimental=7x-... 7.0 and 7.1, at 48 kHz at every frame rate of Part 1
// Table 83 or at 44.1 kHz in 2 048-sample frames, the SIMPLE, ASPX or A-CPL
// codec modes, at a constant, average or variable rate, with the loudness,
// DRC, downmix and dialogue enhancement metadata the options configure. The
// WAV file's channels are taken in the order `decode` writes them
// (ac4_channels.hpp).

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

// Part 1 Table 83's frame rates at 48 kHz as frame-rate= spells them.
// clang-format off
constexpr std::array<std::string_view, 13> kFrameRates = {
    "23.976", "24", "25", "29.97", "30", "47.95", "48",
    "50", "59.94", "60", "100", "119.88", "120"};
// clang-format on

[[nodiscard]] std::string_view rate_mode_name(ac4::RateMode mode) {
    switch (mode) {
        case ac4::RateMode::kAverage:
            return "average";
        case ac4::RateMode::kVariable:
            return "variable";
        case ac4::RateMode::kConstant:
            break;
    }
    return "constant";
}

// Matched as 'remux' matches them: std::filesystem::path::extension(), case
// kept.
[[nodiscard]] bool names_mp4(std::string_view out_path) {
    constexpr std::array<std::string_view, 3> kMp4Exts{".mp4", ".m4a", ".mov"};
    const std::string ext = std::filesystem::path{std::string{out_path}}.extension().string();
    return std::ranges::any_of(kMp4Exts,
                               [&](std::string_view candidate) { return ext == candidate; });
}

// The encoder's input channels, in ac4::Decoder's order, for a WAV file of
// `count` channels and the 7.X element's additional pair; empty for a count
// the encoder does not take with that pair.
[[nodiscard]] std::vector<ac4::Speaker> input_speakers(std::size_t count,
                                                       ac4::AdditionalPair pair) {
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

// BS.1770's measurements of the programme, for dialnorm=auto and loudness=:
// the integrated loudness, the loudness range, the true peak, and the highest
// momentary and short-term loudness, read every 100 ms, the step at which the
// meter's 400 ms blocks overlap.
struct Measured {
    double integrated = 0.0;
    std::optional<double> range;
    std::optional<double> true_peak;
    std::optional<double> max_momentary;
    std::optional<double> max_short_term;
};

// Over the channels the encoder takes, `channels` in its order: the loudness
// over the 5.1 or 5.0 bed of a 7.X layout, as encode measures E-AC-3's, and
// the true peak over every channel. Nothing where no block passes the
// absolute gate.
[[nodiscard]] std::optional<Measured> measure_programme(
    std::span<const std::span<const float>> channels, std::uint32_t sample_rate) {
    const std::size_t count = channels.size();
    const std::size_t bed = count > 6 ? count - 2 : count;
    const bool lfe = bed == 6;
    const auto acmod =
        bed == 1 ? ac3::Acmod::k1_0 : (bed == 2 ? ac3::Acmod::k2_0 : ac3::Acmod::k3_2);
    const auto rate = sample_rate == 48000 ? ac3::SampleRate::k48000 : ac3::SampleRate::k44100;
    ac3::meta::LoudnessMeter meter{rate, acmod, lfe};
    // The meter takes AC-3's coded order, L C R Ls Rs and the LFE last, and
    // the encoder's order for the bed is a 5.1 WAV file's, whose permutation
    // ac3_layout_for gives.
    std::vector<std::size_t> order(bed);
    if (const auto layout = ac3::io::ac3_layout_for(bed);
        layout && layout->wav_index.size() == bed) {
        order.assign(layout->wav_index.begin(), layout->wav_index.end());
    } else {
        for (std::size_t k = 0; k < bed; ++k) {
            order[k] = k;
        }
    }
    // The 7.X pair's true peak, from a stereo meter of its own.
    std::optional<ac3::meta::LoudnessMeter> pair_meter;
    if (count > bed) {
        pair_meter.emplace(rate, ac3::Acmod::k2_0, false);
    }
    Measured out;
    const std::size_t length = channels.empty() ? 0 : channels.front().size();
    const std::size_t step = sample_rate / 10;
    std::vector<std::span<const float>> views(bed);
    std::vector<std::span<const float>> pair_views(count - bed);
    for (std::size_t at = 0; at < length; at += step) {
        const std::size_t n = std::min(step, length - at);
        for (std::size_t k = 0; k < bed; ++k) {
            views[k] = channels[order[k]].subspan(at, n);
        }
        meter.push(views);
        for (std::size_t k = bed; k < count; ++k) {
            pair_views[k - bed] = channels[k].subspan(at, n);
        }
        if (pair_meter) {
            pair_meter->push(pair_views);
        }
        const auto keep_max = [](std::optional<double>& max, std::optional<double> value) {
            if (value && (!max || *value > *max)) {
                max = value;
            }
        };
        keep_max(out.max_momentary, meter.momentary_lkfs());
        keep_max(out.max_short_term, meter.short_term_lkfs());
    }
    const auto integrated = meter.integrated_lkfs();
    if (!integrated) {
        return std::nullopt;
    }
    out.integrated = *integrated;
    out.range = meter.loudness_range();
    out.true_peak = meter.true_peak_dbtp();
    if (pair_meter) {
        if (const auto pair_peak = pair_meter->true_peak_dbtp();
            pair_peak && (!out.true_peak || *pair_peak > *out.true_peak)) {
            out.true_peak = pair_peak;
        }
    }
    return out;
}

// dialogue-channels=, "l,r,c" or any of the three.
[[nodiscard]] bool names_channel(std::string_view list, std::string_view channel) {
    while (!list.empty()) {
        const std::size_t comma = list.find(',');
        if (list.substr(0, comma) == channel) {
            return true;
        }
        list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
    }
    return false;
}

}  // namespace

int run_ac4_encode(std::string_view in_path, std::string_view out_path, std::uint32_t bitrate,
                   const ac3cli::Options& meta) {
    const Options::Ac4Encode& opts = meta.ac4enc;
    // AC-3's and E-AC-3's own metadata has no AC-4 counterpart, so asking for
    // it is refused rather than dropped.
    if (meta.p.heavy.has_value() || meta.p.heavy2.has_value() || meta.p.drc2.has_value() ||
        meta.p.mixmeta || meta.p.infomdat || meta.p.annexd || meta.dialnorm2_given) {
        fmt::println(stderr,
                     "error: heavy, heavy2, drc2=, dialnorm2=, infomdat, annexd and E-AC-3's "
                     "mixing metadata have no AC-4 counterpart; ac4-encode takes AC-4's own "
                     "(ac3cli help ac4-encode)");
        return kExitUsage;
    }
    const bool drc_named =
        opts.drc.has_value() || std::ranges::any_of(opts.drc_modes, [](const auto& profile) {
            return profile.has_value();
        });
    if (opts.drc_gains && !drc_named) {
        fmt::println(
            stderr,
            "error: experimental=drc-gains-{} sends the gains of a DRC profile: name one with drc=",
            *opts.drc_gains);
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
        fmt::println(
            stderr,
            "error: {}: AC-4 encoding takes mono, stereo, 5.0 and 5.1, and 7.0 and 7.1 with "
            "experimental=7x-back, 7x-wide or 7x-top-front; the source has {} channels{}",
            in_path, wav->channels.size(),
            pair != ac4::AdditionalPair::kNone ? " and a 7.X pair was named" : "");
        return kExitInput;
    }
    if (wav->sample_rate != 48000 && wav->sample_rate != 44100) {
        fmt::println(stderr, "error: {}: AC-4 encoding takes 48 or 44.1 kHz; the source is {} Hz",
                     in_path, wav->sample_rate);
        return kExitInput;
    }
    if (wav->sample_rate == 44100 && opts.frame_rate_index != 13) {
        fmt::println(
            stderr,
            "error: {}: at 44.1 kHz AC-4 has the native frame rate alone (Part 1 Table 83); "
            "frame-rate= names another",
            in_path);
        return kExitUsage;
    }
    const bool multichannel = speakers.size() >= 5;
    const bool has_lfe = std::ranges::find(speakers, ac4::Speaker::kLfe) != speakers.end();
    const bool downmix_named = opts.loro_centre_db || opts.loro_surround_db ||
                               opts.ltrt_centre_db || opts.ltrt_surround_db || opts.lfe_db ||
                               opts.preferred_downmix || opts.loro_correction_db ||
                               opts.ltrt_correction_db;
    if (downmix_named && !multichannel) {
        fmt::println(
            stderr,
            "error: the downmix options describe the stereo downmix of 5.0, 5.1, 7.0 and 7.1; the "
            "source is {}",
            layout_name(speakers.size(), pair));
        return kExitUsage;
    }
    if (opts.lfe_db && !has_lfe) {
        fmt::println(stderr, "error: lfemix= is the LFE's gain into the downmix, and {} has no LFE",
                     layout_name(speakers.size(), pair));
        return kExitUsage;
    }

    ac4::EncoderConfig config;
    config.channels = static_cast<int>(speakers.size());
    config.sample_rate_hz = static_cast<int>(wav->sample_rate);
    config.frame_rate_index = opts.frame_rate_index;
    config.bitrate_kbps = static_cast<int>(bitrate);
    config.rate_mode = opts.rate_mode;
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
    if (opts.iframe_interval) {
        config.iframe_interval = *opts.iframe_interval;
    }
    config.iframes = opts.iframes;
    if (opts.fragment_seconds) {
        // A fragment at every multiple of the duration, through the decoded
        // output's length.
        const double step = *opts.fragment_seconds * static_cast<double>(wav->sample_rate);
        const double end =
            static_cast<double>(wav->frame_count()) + 2.0 * static_cast<double>(wav->sample_rate);
        for (double at = step; at < end; at += step) {
            config.fragment_starts.push_back(static_cast<std::int64_t>(std::llround(at)));
        }
    }
    config.experimental.aspx_balance = meta.ac4_experimental_balance;
    config.experimental.aspx_varvar = meta.ac4_experimental_varvar;
    config.experimental.aspx_interleave = meta.ac4_experimental_interleave;
    config.experimental.coding_configs = meta.ac4_experimental_coding_configs;
    config.experimental.acpl = meta.ac4_experimental_acpl;
    config.experimental.seven_x = pair;
    config.experimental.drc_gains = opts.drc_gains.has_value();

    if (drc_named) {
        // Table 161's four modes on drc='s profile, and a mode named on a
        // profile of its own takes its curve, or repeats the first mode with
        // the same.
        ac4::DrcConfig drc;
        drc.profile = opts.drc.value_or(drc.profile);
        for (int id = 0; id < 4; ++id) {
            ac4::DrcModeConfig mode{.id = id,
                                    .output_level_from_db = 0,
                                    .output_level_to_db = 0,
                                    .profile = std::nullopt,
                                    .repeat_of = std::nullopt,
                                    .gains_config = opts.drc_gains};
            const std::optional<ac4::DrcProfile>& own =
                opts.drc_modes[static_cast<std::size_t>(id)];
            if (own && *own != drc.profile) {
                for (int earlier = 0; earlier < id; ++earlier) {
                    if (opts.drc_modes[static_cast<std::size_t>(earlier)] == own) {
                        mode.repeat_of = earlier;
                        break;
                    }
                }
                if (!mode.repeat_of) {
                    mode.profile = own;
                }
            }
            drc.modes.push_back(mode);
        }
        config.drc = drc;
    }
    if (downmix_named) {
        ac4::DownmixConfig downmix;
        downmix.loro_centre_db = opts.loro_centre_db.value_or(downmix.loro_centre_db);
        downmix.loro_surround_db = opts.loro_surround_db.value_or(downmix.loro_surround_db);
        downmix.ltrt_centre_db = opts.ltrt_centre_db;
        downmix.ltrt_surround_db = opts.ltrt_surround_db;
        downmix.lfe_db = opts.lfe_db;
        downmix.preferred = opts.preferred_downmix.value_or(downmix.preferred);
        downmix.loro_correction_db2 = opts.loro_correction_db;
        downmix.ltrt_correction_db2 = opts.ltrt_correction_db;
        config.downmix = downmix;
    }
    const bool stem = !opts.dialogue_stem.empty();
    if (opts.dialogue_channels || stem) {
        ac4::DialogueConfig dialogue;
        dialogue.method = opts.dialogue_method;
        dialogue.source = stem ? ac4::DialogueSource::kStem : ac4::DialogueSource::kMarkedChannels;
        dialogue.max_gain_db = opts.dialogue_max_gain_db;
        if (opts.dialogue_channels) {
            dialogue.left = names_channel(*opts.dialogue_channels, "l");
            dialogue.right = names_channel(*opts.dialogue_channels, "r");
            dialogue.centre = names_channel(*opts.dialogue_channels, "c");
        } else {
            // A stem's parameters go to each of L, R and C the layout has.
            dialogue.left = speakers.size() > 1;
            dialogue.right = speakers.size() > 1;
            dialogue.centre = speakers.size() != 2;
        }
        config.dialogue = dialogue;
    }
    // The configuration is checked before loudness= reads the whole file,
    // with loudness values in place: they cost the same bits whatever they
    // are.
    const auto refuse_config = [] {
        fmt::println(stderr,
                     "error: {}: the codec mode, the rate and the options must be ones the "
                     "encoder takes together (ac3cli help ac4-encode)",
                     ac4::describe(ac4::EncodeError::kInvalidConfig));
        return kExitUsage;
    };
    ac4::EncoderConfig sized = config;
    if (opts.loudness) {
        ac4::FurtherLoudness loudness;
        loudness.practice = *opts.loudness;
        loudness.integrated_lkfs = -23.0;
        loudness.loudness_range_lu = 0.0;
        loudness.max_true_peak_dbtp = 0.0;
        loudness.max_momentary_lufs = -23.0;
        loudness.max_short_term_lufs = -23.0;
        sized.loudness = loudness;
    }
    if (!ac4::Encoder::create(sized).has_value()) {
        return refuse_config();
    }
    // Each of the encoder's channels' place in the WAV file.
    const std::vector<std::size_t> wav_order = ac4_order(std::span{speakers}, ac4_wav_rank);
    std::vector<std::size_t> wav_index(speakers.size());
    for (std::size_t w = 0; w < wav_order.size(); ++w) {
        wav_index[wav_order[w]] = w;
    }
    std::vector<std::span<const float>> views;
    for (const std::size_t w : wav_index) {
        views.emplace_back(wav->channels[w]);
    }
    // dialogue-stem=: the dialogue in the programme's channels, sample for
    // sample.
    ac3::io::WavData stem_wav;
    std::vector<std::span<const float>> stem_views;
    if (stem) {
        auto read = read_wav_arg(opts.dialogue_stem);
        if (!read.has_value()) {
            fmt::println(stderr, "error: {}: {}", opts.dialogue_stem,
                         ac3::io::describe(read.error()));
            return kExitInput;
        }
        if (read->channels.size() != wav->channels.size() ||
            read->sample_rate != wav->sample_rate || read->frame_count() != wav->frame_count()) {
            fmt::println(stderr,
                         "error: {}: a dialogue stem has the programme's channels, rate and "
                         "length: {} channels at {} Hz, {} samples",
                         opts.dialogue_stem, wav->channels.size(), wav->sample_rate,
                         wav->frame_count());
            return kExitInput;
        }
        stem_wav = std::move(*read);
        for (const std::size_t w : wav_index) {
            stem_views.emplace_back(stem_wav.channels[w]);
        }
    }

    // dialnorm=, 0 to 31.75 dB below full scale in steps of 0.25; auto, or
    // loudness= without it, takes the integrated loudness to the step.
    const auto status = status_stream(out_path);
    double dialnorm = opts.dialnorm_db.value_or(31.0);
    const bool measure_dialnorm =
        meta.p.measure_dialnorm || (opts.loudness && !meta.dialnorm_given);
    if (measure_dialnorm || opts.loudness) {
        const auto measured = measure_programme(views, wav->sample_rate);
        if (!measured) {
            fmt::println(
                stderr,
                "error: no audio above the -70 LKFS absolute gate; pass dialnorm=<0..31.75> and "
                "leave loudness= out");
            return kExitRuntime;
        }
        if (measure_dialnorm) {
            dialnorm = std::clamp(std::round(-measured->integrated * 4.0) / 4.0, 0.0, 31.75);
        }
        status_println(status, "measured {:.2f} LKFS (BS.1770-4, gated) -> dialnorm -{:g} dB",
                       measured->integrated, dialnorm);
        if (opts.loudness) {
            // Each within what its code holds: -102.4 to +102.3, the range 0
            // to 102.3 LU (Part 1 clauses 4.3.12.3.8 to 4.3.12.3.30).
            const auto held = [](std::optional<double> value, double low) {
                return value ? std::optional<double>{std::clamp(*value, low, 102.3)} : value;
            };
            ac4::FurtherLoudness loudness;
            loudness.practice = *opts.loudness;
            loudness.integrated_lkfs = held(measured->integrated, -102.4);
            loudness.loudness_range_lu = held(measured->range, 0.0);
            loudness.max_true_peak_dbtp = held(measured->true_peak, -102.4);
            loudness.max_momentary_lufs = held(measured->max_momentary, -102.4);
            loudness.max_short_term_lufs = held(measured->max_short_term, -102.4);
            config.loudness = loudness;
            const auto show = [](std::optional<double> value) {
                return value ? fmt::format("{:.1f}", *value) : std::string{"none"};
            };
            status_println(
                status,
                "          loudness range {} LU, true peak {} dBTP, highest momentary {} LUFS and "
                "short-term {} LUFS",
                show(measured->range), show(measured->true_peak), show(measured->max_momentary),
                show(measured->max_short_term));
        }
    }
    config.dialnorm_db = -dialnorm;

    // syntax-trace=: every record the encoder writes, as ac4_syntax.py's
    // `trace` writes what it reads. A frame's first record is its substream
    // 0's first element, which is where the frame count moves on.
    std::ofstream trace_file;
    long long trace_frame = -1;
    const auto trace = [&trace_file, &trace_frame](const ac4::SyntaxRecord& r) {
        if (r.substream == 0 && r.bit_offset == 0) {
            ++trace_frame;
        }
        trace_file << trace_frame << '\t' << r.substream << '\t' << r.bit_offset << '\t' << r.bits
                   << '\t' << r.value << '\t' << r.name << '\n';
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
        return refuse_config();
    }
    auto frames = stem ? encoder->encode(views, stem_views) : encoder->encode(views);
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
        // Part 2 Annex E: each frame a sample, the I-frames its sync samples,
        // timed as Table E.1 says.
        std::vector<std::span<const std::byte>> samples;
        mp4::MuxOptions options;
        samples.reserve(frames->size());
        options.sync_samples.reserve(frames->size());
        for (const ac4::EncodedFrame& frame : *frames) {
            samples.emplace_back(frame.raw_ac4_frame);
            options.sync_samples.push_back(frame.iframe);
        }
        const ac4::Toc& toc = encoder->toc();
        const auto timing = ac4::media_timing(toc);
        if (!timing) {
            fmt::println(stderr, "error: frame_rate_index {} has no MP4 timing (Part 2 Table E.1)",
                         toc.frame_rate_index);
            return kExitOutput;
        }
        const mp4::AudioTrack track{.codec_id = std::string{mp4::kCodecAc4},
                                    .sample_rate = static_cast<std::uint32_t>(toc.sample_rate_hz),
                                    .channels = 2,  // TS 103 190-2 E.4.5: "should be set to 2"
                                    .samples_per_frame = timing->sample_delta,
                                    .codec_config = ac4::build_dac4(toc),
                                    .rfc6381 = ac4::rfc6381_codec_string(toc),
                                    .timescale = timing->timescale};
        rfc6381 = track.rfc6381;
        auto muxed = mp4::mux(track, samples, options);
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
    status_println(
        status, "encoded {} AC-4 frames -> {} ({} Hz, {}, {} kbps, dialnorm -{:g} dB{})",
        frames->size(), out_path, config.sample_rate_hz, layout_name(speakers.size(), pair),
        bitrate, dialnorm,
        to_mp4 ? fmt::format(", MP4, codecs {}", rfc6381) : std::string{", raw with CRC"});
    const bool native = config.frame_rate_index == 13;
    status_println(
        status,
        "          {} mode, {}, {} rate; the decoder's output lags the input by {} samples{}",
        mode_name(encoder->codec_mode()),
        native
            ? std::string{"2 048-sample frames"}
            : fmt::format("{} fps", kFrameRates[static_cast<std::size_t>(config.frame_rate_index)]),
        rate_mode_name(config.rate_mode),
        encoder->delay_samples() + encoder->decoder_delay_samples(),
        native ? "" : ", to the nearest sample");
    return kExitOk;
}

}  // namespace ac3cli::commands
