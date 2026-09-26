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
// of sync frames with the CRC of TS 103 190-2 Annex G (without it where
// crc=off), or in an MP4 file with Annex E's 'ac-4' sample entry when the
// output is named .mp4, .m4a or .mov. What the encoder writes: mono, stereo,
// 5.0 and 5.1, and with experimental=7x-... 7.0 and 7.1 and with
// experimental=three-zero 3.0, at 48 kHz at every frame rate of Part 1 Table
// 83 or at 44.1 kHz in 2 048-sample frames, the SIMPLE, ASPX or A-CPL codec
// modes, at a constant, average or variable rate, with the loudness, DRC,
// downmix and dialogue enhancement metadata the options configure; and, with
// substreamN= and presentationN=, several substreams, each an input of its
// own or a hybrid dialogue enhancement's waveform, and the presentations of
// Part 2 Table 53 made of them. Each WAV file's channels are taken in the
// order `decode` writes them (ac4_channels.hpp).

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
// `count` channels, the 7.X element's additional pair, which seven or eight
// channels need and the other counts leave to another substream, and whether
// the 3.0 element is asked for; empty for a count the encoder does not take so.
[[nodiscard]] std::vector<ac4::Speaker> input_speakers(std::size_t count, ac4::AdditionalPair pair,
                                                       bool three_zero) {
    using S = ac4::Speaker;
    const bool seven = count == 7 || count == 8;
    if (seven && pair == ac4::AdditionalPair::kNone) {
        return {};
    }
    switch (count) {
        case 1:
            return {S::kCentre};
        case 2:
            return {S::kLeft, S::kRight};
        case 3:
            if (three_zero) {
                return {S::kLeft, S::kRight, S::kCentre};
            }
            return {};
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
    if (!seven) {
        return out;
    }
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
        case 3:
            return "3.0";
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
    const auto acmod = bed == 1   ? ac3::Acmod::k1_0
                       : bed == 2 ? ac3::Acmod::k2_0
                                  : (bed == 3 ? ac3::Acmod::k3_0 : ac3::Acmod::k3_2);
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

// codec-mode='s values as ac4::CodecMode, kAuto for "auto" and none.
[[nodiscard]] ac4::CodecMode codec_mode_of(std::string_view name) {
    if (name == "simple") {
        return ac4::CodecMode::kSimple;
    }
    if (name == "aspx") {
        return ac4::CodecMode::kAspx;
    }
    if (name == "aspx-acpl-1") {
        return ac4::CodecMode::kAspxAcpl1;
    }
    if (name == "aspx-acpl-2") {
        return ac4::CodecMode::kAspxAcpl2;
    }
    if (name == "aspx-acpl-3") {
        return ac4::CodecMode::kAspxAcpl3;
    }
    return ac4::CodecMode::kAuto;
}

// One input of the stream: a substream's WAV file, its channels in the
// encoder's order, and its dialogue stem where it has one.
struct Input {
    ac3::io::WavData wav;
    std::vector<ac4::Speaker> speakers;
    std::vector<std::size_t> wav_index;  // the WAV file's channel of each encoder channel
    ac3::io::WavData stem;
    bool has_stem = false;
};

// Reads substream `number`'s WAV file and its dialogue stem, and checks them
// against what the encoder takes; nothing, the message printed, where they are
// not.
[[nodiscard]] std::optional<Input> read_input(std::string_view path, std::size_t number,
                                              const Options::Ac4Encode::Dialogue& dialogue,
                                              ac4::AdditionalPair pair, bool three_zero) {
    auto wav = read_wav_arg(path);
    if (!wav.has_value()) {
        fmt::println(stderr, "error: {}: {}", path, ac3::io::describe(wav.error()));
        return std::nullopt;
    }
    Input input;
    input.speakers = input_speakers(wav->channels.size(), pair, three_zero);
    if (input.speakers.empty()) {
        fmt::println(stderr,
                     "error: {}: AC-4 encoding takes mono, stereo, 5.0 and 5.1, 7.0 and 7.1 with "
                     "experimental=7x-back, 7x-wide or 7x-top-front, and 3.0 with "
                     "experimental=three-zero; substream {} has {} channels",
                     path, number, wav->channels.size());
        return std::nullopt;
    }
    if (wav->sample_rate != 48000 && wav->sample_rate != 44100) {
        fmt::println(stderr, "error: {}: AC-4 encoding takes 48 or 44.1 kHz; the source is {} Hz",
                     path, wav->sample_rate);
        return std::nullopt;
    }
    const std::vector<std::size_t> wav_order = ac4_order(std::span{input.speakers}, ac4_wav_rank);
    input.wav_index.resize(input.speakers.size());
    for (std::size_t w = 0; w < wav_order.size(); ++w) {
        input.wav_index[wav_order[w]] = w;
    }
    // dialogue-stem=: the dialogue in the programme's channels, sample for
    // sample.
    if (!dialogue.stem.empty()) {
        auto stem = read_wav_arg(dialogue.stem);
        if (!stem.has_value()) {
            fmt::println(stderr, "error: {}: {}", dialogue.stem, ac3::io::describe(stem.error()));
            return std::nullopt;
        }
        if (stem->channels.size() != wav->channels.size() ||
            stem->sample_rate != wav->sample_rate || stem->frame_count() != wav->frame_count()) {
            fmt::println(stderr,
                         "error: {}: a dialogue stem has the programme's channels, rate and "
                         "length: {} channels at {} Hz, {} samples",
                         dialogue.stem, wav->channels.size(), wav->sample_rate, wav->frame_count());
            return std::nullopt;
        }
        input.stem = std::move(*stem);
        input.has_stem = true;
    }
    input.wav = std::move(*wav);
    return input;
}

// A substream's dialogue enhancement, from its dialogue-...= options, for a
// substream of `channels` input channels; nothing where none is asked for.
[[nodiscard]] std::optional<ac4::DialogueConfig> dialogue_config(
    const Options::Ac4Encode::Dialogue& options, std::size_t channels) {
    const bool stem = !options.stem.empty();
    if (!options.channels && !stem) {
        return std::nullopt;
    }
    ac4::DialogueConfig dialogue;
    dialogue.method = options.method;
    dialogue.source = stem ? ac4::DialogueSource::kStem : ac4::DialogueSource::kMarkedChannels;
    dialogue.max_gain_db = options.max_gain_db;
    if (options.channels) {
        dialogue.left = names_channel(*options.channels, "l");
        dialogue.right = names_channel(*options.channels, "r");
        dialogue.centre = names_channel(*options.channels, "c");
    } else {
        // A stem's parameters go to each of L, R and C the layout has.
        dialogue.left = channels > 1;
        dialogue.right = channels > 1;
        dialogue.centre = channels != 2;
    }
    if (options.hybrid_share) {
        dialogue.hybrid = true;
        dialogue.waveform_share = *options.hybrid_share;
    }
    return dialogue;
}

// Whether a substream's options ask for dialogue enhancement.
[[nodiscard]] bool asks_dialogue(const Options::Ac4Encode::Dialogue& d) {
    return d.channels.has_value() || !d.stem.empty() || d.hybrid_share.has_value();
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
    const bool to_mp4 = names_mp4(out_path);
    if (opts.crc && to_mp4) {
        fmt::println(stderr,
                     "error: crc= is a raw stream's sync frames' CRC; an MP4 sample is the raw "
                     "frame alone, with no sync word and no CRC");
        return kExitUsage;
    }

    // The substreams, numbered from 1 without a gap: 1 the positional input,
    // each further one an input of its own or the waveform of a substream's
    // hybrid dialogue enhancement, which takes none.
    const std::vector<Options::Ac4Encode::Substream>& substreams = opts.substreams;
    const std::size_t count = substreams.size();
    for (std::size_t n = 0; n < count; ++n) {
        const Options::Ac4Encode::Substream& s = substreams[n];
        const std::size_t number = n + 1;
        if (n == 0 && s.enhances) {
            fmt::println(stderr,
                         "error: substream1-enhances=: substream 1, the positional input, is a "
                         "programme, not a dialogue enhancement substream");
            return kExitUsage;
        }
        if (n > 0 && s.path.empty() && !s.enhances) {
            fmt::println(stderr,
                         "error: substream {0} has neither an input (substream{0}=) nor a "
                         "substream it enhances (substream{0}-enhances=): substreams are numbered "
                         "from 1 without a gap",
                         number);
            return kExitUsage;
        }
        if (n > 0 && !s.path.empty() && s.enhances) {
            fmt::println(stderr,
                         "error: substream{0}= and substream{0}-enhances= both: a dialogue "
                         "enhancement substream takes no input of its own",
                         number);
            return kExitUsage;
        }
        if (s.enhances && static_cast<std::size_t>(*s.enhances) > count) {
            fmt::println(stderr,
                         "error: substream{}-enhances={} names a substream the stream lacks",
                         number, *s.enhances);
            return kExitUsage;
        }
        if (s.enhances && asks_dialogue(s.dialogue)) {
            fmt::println(stderr,
                         "error: substream {} is a dialogue enhancement substream, the waveform of "
                         "another's, and has no dialogue enhancement of its own",
                         number);
            return kExitUsage;
        }
        if (s.dialogue.hybrid_share && !s.dialogue.channels && s.dialogue.stem.empty()) {
            fmt::println(stderr,
                         "error: {}dialogue-hybrid= is a hybrid method of the dialogue "
                         "enhancement dialogue-channels= or dialogue-stem= configures",
                         n == 0 ? std::string{} : fmt::format("substream{}-", number));
            return kExitUsage;
        }
    }
    for (std::size_t n = 0; n < opts.presentations.size(); ++n) {
        const Options::Ac4Encode::Presentation& p = opts.presentations[n];
        if (!p.named) {
            fmt::println(stderr,
                         "error: presentation {} is missing: presentations are numbered from 1 "
                         "without a gap",
                         n + 1);
            return kExitUsage;
        }
        for (const int s : p.substreams) {
            if (static_cast<std::size_t>(s) > count) {
                fmt::println(stderr,
                             "error: presentation{} names substream {}, which the stream lacks",
                             n + 1, s);
                return kExitUsage;
            }
        }
        if (p.substreams.empty() && p.config != 6) {
            fmt::println(stderr,
                         "error: presentation {0} plays no substream: list them with "
                         "presentation{0}=, or give presentation{0}-config=6 for EMDF payloads "
                         "alone",
                         n + 1);
            return kExitUsage;
        }
    }
    const Options::Ac4Encode::Substream& first = substreams.front();
    // The configuration's substreams form: several substreams, or values of
    // the first that only ac4::SubstreamConfig carries.
    const bool substream_form = count > 1 || first.content || !first.language.empty() ||
                                first.bitrate_kbps || first.max_dialogue_gain_db ||
                                !first.pan_degrees.empty() || !first.emdf.empty();
    if (count > 1 && (meta.p.measure_dialnorm || opts.loudness)) {
        fmt::println(stderr,
                     "error: dialnorm=auto and loudness= measure one programme, and this stream "
                     "has several substreams: give dialnorm=, and presentationN-dialnorm= where "
                     "a presentation's differs");
        return kExitUsage;
    }

    ac4::AdditionalPair pair = ac4::AdditionalPair::kNone;
    if (meta.ac4_experimental_seven_x == "back") {
        pair = ac4::AdditionalPair::kBack;
    } else if (meta.ac4_experimental_seven_x == "wide") {
        pair = ac4::AdditionalPair::kWide;
    } else if (meta.ac4_experimental_seven_x == "top-front") {
        pair = ac4::AdditionalPair::kTopFront;
    }
    // Every substream's input, a dialogue enhancement substream's none.
    std::vector<std::optional<Input>> inputs(count);
    for (std::size_t n = 0; n < count; ++n) {
        if (n == 0 || !substreams[n].path.empty()) {
            inputs[n] = read_input(n == 0 ? in_path : std::string_view{substreams[n].path}, n + 1,
                                   substreams[n].dialogue, pair, meta.ac4_experimental_three_zero);
            if (!inputs[n]) {
                return kExitInput;
            }
        }
    }
    const Input& main = *inputs.front();
    const std::vector<ac4::Speaker>& speakers = main.speakers;
    for (std::size_t n = 1; n < count; ++n) {
        if (inputs[n] && (inputs[n]->wav.sample_rate != main.wav.sample_rate ||
                          inputs[n]->wav.frame_count() != main.wav.frame_count())) {
            fmt::println(stderr,
                         "error: {}: every substream's input has the positional input's rate and "
                         "length: {} Hz, {} samples",
                         substreams[n].path, main.wav.sample_rate, main.wav.frame_count());
            return kExitInput;
        }
    }
    if (main.wav.sample_rate == 44100 && opts.frame_rate_index != 13) {
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
    // With several substreams the downmix goes to the presentations of 5.X
    // and 7.X, and the encoder says where there is none.
    if (count == 1 && downmix_named && !multichannel) {
        fmt::println(
            stderr,
            "error: the downmix options describe the stereo downmix of 5.0, 5.1, 7.0 and 7.1; the "
            "source is {}",
            layout_name(speakers.size(), pair));
        return kExitUsage;
    }
    if (count == 1 && opts.lfe_db && !has_lfe) {
        fmt::println(stderr, "error: lfemix= is the LFE's gain into the downmix, and {} has no LFE",
                     layout_name(speakers.size(), pair));
        return kExitUsage;
    }

    ac4::EncoderConfig config;
    config.channels = static_cast<int>(speakers.size());
    config.sample_rate_hz = static_cast<int>(main.wav.sample_rate);
    config.frame_rate_index = opts.frame_rate_index;
    config.bitrate_kbps = static_cast<int>(bitrate);
    config.rate_mode = opts.rate_mode;
    config.codec_mode = codec_mode_of(meta.ac4_codec_mode);
    if (opts.iframe_interval) {
        config.iframe_interval = *opts.iframe_interval;
    }
    config.iframes = opts.iframes;
    if (opts.fragment_seconds) {
        // A fragment at every multiple of the duration, through the decoded
        // output's length.
        const double step = *opts.fragment_seconds * static_cast<double>(main.wav.sample_rate);
        const double end = static_cast<double>(main.wav.frame_count()) +
                           2.0 * static_cast<double>(main.wav.sample_rate);
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
    config.experimental.three_zero = meta.ac4_experimental_three_zero;

    if (drc_named) {
        // Table 161's four modes on drc='s profile, and a mode named on a
        // profile of its own takes its curve, or repeats the first mode with
        // the same.
        ac4::DrcConfig drc;
        drc.profile = opts.drc.value_or(drc.profile);
        for (int id = 0; id < 4; ++id) {
            ac4::DrcModeConfig mode{.id = id, .gains_config = opts.drc_gains};
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
    if (!substream_form) {
        config.dialogue = dialogue_config(first.dialogue, speakers.size());
    } else {
        for (std::size_t n = 0; n < count; ++n) {
            const Options::Ac4Encode::Substream& s = substreams[n];
            ac4::SubstreamConfig substream;
            substream.channels = inputs[n] ? static_cast<int>(inputs[n]->speakers.size()) : 0;
            substream.bitrate_kbps = s.bitrate_kbps;
            substream.codec_mode = codec_mode_of(n == 0 ? meta.ac4_codec_mode : s.codec_mode);
            substream.content = s.content;
            substream.language = s.language;
            if (inputs[n]) {
                substream.dialogue = dialogue_config(s.dialogue, inputs[n]->speakers.size());
            }
            if (s.max_dialogue_gain_db || !s.pan_degrees.empty()) {
                substream.dialogue_mix = ac4::DialogueMix{.max_gain_db = s.max_dialogue_gain_db,
                                                          .pan_degrees = s.pan_degrees};
            }
            if (s.enhances) {
                substream.enhances = *s.enhances - 1;
            }
            substream.emdf = s.emdf;
            config.substreams.push_back(std::move(substream));
        }
    }
    for (const Options::Ac4Encode::Presentation& p : opts.presentations) {
        ac4::PresentationConfig presentation;
        presentation.config = p.config;
        for (const int s : p.substreams) {
            presentation.substreams.push_back(s - 1);
        }
        presentation.presentation_id = p.id;
        presentation.md_compat = p.md_compat;
        presentation.enabled = p.enabled;
        presentation.pre_virtualized = p.pre_virtualized;
        presentation.name = p.name;
        presentation.dialnorm_db = p.dialnorm_db;
        presentation.gains_db = p.gains_db;
        if (p.main_db || p.main_centre_db || p.main_front_db || p.associated_pan) {
            presentation.associated = ac4::AssociatedMix{.main_db = p.main_db,
                                                         .main_centre_db = p.main_centre_db,
                                                         .main_front_db = p.main_front_db,
                                                         .pan_degrees = p.associated_pan};
        }
        presentation.emdf = p.emdf;
        config.presentations.push_back(std::move(presentation));
    }
    // The configuration is checked before loudness= reads the whole file,
    // with loudness values in place: they cost the same bits whatever they
    // are.
    const auto refuse_config = [](const ac4::EncoderConfig& refused) {
        fmt::println(stderr, "error: the encoder refuses {} (ac3cli help ac4-encode)",
                     ac4::Encoder::refusal_reason(refused));
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
        return refuse_config(sized);
    }
    // encode() takes every substream's channels one substream after the
    // other, and with a stem the dialogue in each: a substream without one
    // gives silence there, which the encoder does not read.
    const bool stems = std::ranges::any_of(
        inputs, [](const std::optional<Input>& input) { return input && input->has_stem; });
    const std::vector<float> silence(stems ? main.wav.frame_count() : 0, 0.0F);
    std::vector<std::span<const float>> views;
    std::vector<std::span<const float>> stem_views;
    for (const std::optional<Input>& input : inputs) {
        if (!input) {
            continue;
        }
        for (const std::size_t w : input->wav_index) {
            views.emplace_back(input->wav.channels[w]);
            if (stems) {
                stem_views.emplace_back(input->has_stem
                                            ? std::span<const float>{input->stem.channels[w]}
                                            : std::span<const float>{silence});
            }
        }
    }
    const std::span<const std::span<const float>> main_views =
        std::span{views}.first(speakers.size());

    // dialnorm=, 0 to 31.75 dB below full scale in steps of 0.25; auto, or
    // loudness= without it, takes the integrated loudness to the step.
    const auto status = status_stream(out_path);
    double dialnorm = opts.dialnorm_db.value_or(31.0);
    const bool measure_dialnorm =
        meta.p.measure_dialnorm || (opts.loudness && !meta.dialnorm_given);
    if (measure_dialnorm || opts.loudness) {
        const auto measured = measure_programme(main_views, main.wav.sample_rate);
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
        return refuse_config(config);
    }
    auto frames = stems ? encoder->encode(views, stem_views) : encoder->encode(views);
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

    const ac4::Toc& toc = encoder->toc();
    std::vector<std::vector<std::byte>> bytes;
    std::string rfc6381;
    if (to_mp4) {
        // Part 2 Annex E: each frame a sample, the I-frames its sync samples,
        // timed as Table E.1 says.
        std::vector<std::span<const std::byte>> samples;
        mp4::MuxOptions options;
        samples.reserve(frames->size());
        // Sized, not reserved: GCC 16's -Wnull-dereference flags vector<bool>::reserve on an
        // empty vector.
        options.sync_samples = std::vector<bool>(frames->size());
        for (std::size_t i = 0; i < frames->size(); ++i) {
            samples.emplace_back((*frames)[i].raw_ac4_frame);
            options.sync_samples[i] = (*frames)[i].iframe;
        }
        const auto timing = ac4::media_timing(toc);
        if (!timing) {
            fmt::println(stderr, "error: frame_rate_index {} has no MP4 timing (Part 2 Table E.1)",
                         toc.frame_rate_index);
            return kExitOutput;
        }
        std::vector<std::byte> dac4 = ac4::build_dac4(toc);
        if (dac4.empty()) {
            fmt::println(stderr,
                         "error: the MP4 sample entry's dac4 cannot describe {}; write a raw .ac4 "
                         "instead",
                         ac4::dac4_refusal(toc));
            return kExitUsage;
        }
        const mp4::AudioTrack track{.codec_id = std::string{mp4::kCodecAc4},
                                    .sample_rate = static_cast<std::uint32_t>(toc.sample_rate_hz),
                                    .channels = 2,  // TS 103 190-2 E.4.5: "should be set to 2"
                                    .samples_per_frame = timing->sample_delta,
                                    .codec_config = std::move(dac4),
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
        const bool crc = opts.crc.value_or(true);
        bytes.reserve(frames->size());
        for (const ac4::EncodedFrame& frame : *frames) {
            bytes.push_back(ac4::sync_frame(frame.raw_ac4_frame, crc));
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
    const std::string shape = count > 1 ? fmt::format("{} substreams", count)
                                        : std::string{layout_name(speakers.size(), pair)};
    const std::string presentations = toc.n_presentations > 1
                                          ? fmt::format(", {} presentations", toc.n_presentations)
                                          : std::string{};
    const std::string container =
        to_mp4 ? fmt::format(", MP4, codecs {}", rfc6381)
               : (opts.crc.value_or(true) ? std::string{", raw with CRC"}
                                          : std::string{", raw without CRC"});
    status_println(status,
                   "encoded {} AC-4 frames -> {} ({} Hz, {}{}, {} kbps, dialnorm -{:g} dB{})",
                   frames->size(), out_path, config.sample_rate_hz, shape, presentations, bitrate,
                   dialnorm, container);
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
