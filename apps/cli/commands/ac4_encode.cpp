#include <algorithm>
#include <array>
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

#include "../exit_codes.hpp"
#include "../support.hpp"
#include "iclforge/ac3/io/wav.hpp"
#include "iclforge/ac4/ac4.hpp"
#include "ac4_encode_core.hpp"
#include "iclforge/ac4enc/encoder.hpp"
#include "encode.hpp"

// ac4-encode: WAV to AC-4 through iclforge::ac4::Encoder (src/ac4enc), as a raw stream
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
// order `decode` writes them (apps/common/ac4_channels.hpp). The steps ac3gui's
// AC-4 encode shares, so that the two write the same bytes, are in
// apps/common/ac4_encode_core.hpp.

namespace ac3cli::commands {
namespace {

// The codec mode as Part 1 Table 95 names it, or Part 2 Table 73 the
// immersive element's.
[[nodiscard]] std::string_view mode_name(iclforge::ac4::CodecMode mode) {
    switch (mode) {
        case iclforge::ac4::CodecMode::kAspx:
            return "ASPX";
        case iclforge::ac4::CodecMode::kAspxAcpl1:
            return "ASPX_ACPL_1";
        case iclforge::ac4::CodecMode::kAspxAcpl2:
            return "ASPX_ACPL_2";
        case iclforge::ac4::CodecMode::kAspxAcpl3:
            return "ASPX_ACPL_3";
        case iclforge::ac4::CodecMode::kScpl:
            return "SCPL";
        case iclforge::ac4::CodecMode::kAspxScpl:
            return "ASPX_SCPL";
        case iclforge::ac4::CodecMode::kAspxAjcc:
            return "ASPX_AJCC";
        case iclforge::ac4::CodecMode::kAuto:
        case iclforge::ac4::CodecMode::kSimple:
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

[[nodiscard]] std::string_view rate_mode_name(iclforge::ac4::RateMode mode) {
    switch (mode) {
        case iclforge::ac4::RateMode::kAverage:
            return "average";
        case iclforge::ac4::RateMode::kVariable:
            return "variable";
        case iclforge::ac4::RateMode::kConstant:
            break;
    }
    return "constant";
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

// codec-mode='s values as iclforge::ac4::CodecMode, kAuto for "auto" and none.
[[nodiscard]] iclforge::ac4::CodecMode codec_mode_of(std::string_view name) {
    if (name == "simple") {
        return iclforge::ac4::CodecMode::kSimple;
    }
    if (name == "aspx") {
        return iclforge::ac4::CodecMode::kAspx;
    }
    if (name == "aspx-acpl-1") {
        return iclforge::ac4::CodecMode::kAspxAcpl1;
    }
    if (name == "aspx-acpl-2") {
        return iclforge::ac4::CodecMode::kAspxAcpl2;
    }
    if (name == "aspx-acpl-3") {
        return iclforge::ac4::CodecMode::kAspxAcpl3;
    }
    if (name == "scpl") {
        return iclforge::ac4::CodecMode::kScpl;
    }
    if (name == "aspx-scpl") {
        return iclforge::ac4::CodecMode::kAspxScpl;
    }
    if (name == "aspx-ajcc") {
        return iclforge::ac4::CodecMode::kAspxAjcc;
    }
    return iclforge::ac4::CodecMode::kAuto;
}

// One input of the stream: a substream's WAV file, its channels in the
// encoder's order, and its dialogue stem where it has one.
struct Input {
    iclforge::io::WavData wav;
    std::vector<iclforge::ac4::Speaker> speakers;
    std::vector<std::size_t> wav_index;  // the WAV file's channel of each encoder channel
    iclforge::io::WavData stem;
    bool has_stem = false;
};

// Reads substream `number`'s WAV file and its dialogue stem, and checks them
// against what the encoder takes; nothing, the message printed, where they are
// not.
[[nodiscard]] std::optional<Input> read_input(std::string_view path, std::size_t number,
                                              const Options::Ac4Encode::Dialogue& dialogue,
                                              iclforge::ac4::AdditionalPair pair, bool three_zero,
                                              bool back_pair) {
    auto wav = read_wav_arg(path);
    if (!wav.has_value()) {
        fmt::println(stderr, "error: {}: {}", path, iclforge::io::describe(wav.error()));
        return std::nullopt;
    }
    Input input;
    input.speakers =
        iclforge::apps::ac4_input_speakers(wav->channels.size(), pair, three_zero, back_pair);
    if (input.speakers.empty()) {
        fmt::println(stderr,
                     "error: {}: AC-4 encoding takes mono, stereo, 5.0, 5.1, 5.0.4 and 5.1.4, 7.0 "
                     "and 7.1 with experimental=7x-back, 7x-wide or 7x-top-front, 7.0.4 and 7.1.4 "
                     "with experimental=back-pair, and 3.0 with experimental=three-zero; "
                     "substream {} has {} channels",
                     path, number, wav->channels.size());
        return std::nullopt;
    }
    if (wav->sample_rate != 48000 && wav->sample_rate != 44100) {
        fmt::println(stderr, "error: {}: AC-4 encoding takes 48 or 44.1 kHz; the source is {} Hz",
                     path, wav->sample_rate);
        return std::nullopt;
    }
    input.wav_index = iclforge::apps::ac4_wav_index(input.speakers);
    // dialogue-stem=: the dialogue in the programme's channels, sample for
    // sample.
    if (!dialogue.stem.empty()) {
        auto stem = read_wav_arg(dialogue.stem);
        if (!stem.has_value()) {
            fmt::println(stderr, "error: {}: {}", dialogue.stem, iclforge::io::describe(stem.error()));
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
[[nodiscard]] std::optional<iclforge::ac4::DialogueConfig> dialogue_config(
    const Options::Ac4Encode::Dialogue& options, std::size_t channels) {
    const bool stem = !options.stem.empty();
    if (!options.channels && !stem) {
        return std::nullopt;
    }
    iclforge::ac4::DialogueConfig dialogue;
    dialogue.method = options.method;
    dialogue.source = stem ? iclforge::ac4::DialogueSource::kStem : iclforge::ac4::DialogueSource::kMarkedChannels;
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
    if (!meta.ac4_objects_path.empty()) {
        return run_ac4_encode_objects(in_path, out_path, bitrate, meta);
    }
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
    const bool to_mp4 = iclforge::apps::ac4_output_names_mp4(out_path);
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
    // the first that only iclforge::ac4::SubstreamConfig carries.
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

    iclforge::ac4::AdditionalPair pair = iclforge::ac4::AdditionalPair::kNone;
    if (meta.ac4_experimental_seven_x == "back") {
        pair = iclforge::ac4::AdditionalPair::kBack;
    } else if (meta.ac4_experimental_seven_x == "wide") {
        pair = iclforge::ac4::AdditionalPair::kWide;
    } else if (meta.ac4_experimental_seven_x == "top-front") {
        pair = iclforge::ac4::AdditionalPair::kTopFront;
    }
    // Every substream's input, a dialogue enhancement substream's none.
    std::vector<std::optional<Input>> inputs(count);
    for (std::size_t n = 0; n < count; ++n) {
        if (n == 0 || !substreams[n].path.empty()) {
            inputs[n] = read_input(n == 0 ? in_path : std::string_view{substreams[n].path}, n + 1,
                                   substreams[n].dialogue, pair, meta.ac4_experimental_three_zero,
                                   meta.ac4_experimental_back_pair);
            if (!inputs[n]) {
                return kExitInput;
            }
        }
    }
    const Input& main = *inputs.front();
    const std::vector<iclforge::ac4::Speaker>& speakers = main.speakers;
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
    const bool immersive = speakers.size() >= 9;
    const bool has_lfe = std::ranges::find(speakers, iclforge::ac4::Speaker::kLfe) != speakers.end();
    const bool downmix_named = opts.loro_centre_db || opts.loro_surround_db ||
                               opts.ltrt_centre_db || opts.ltrt_surround_db || opts.lfe_db ||
                               opts.preferred_downmix || opts.loro_correction_db ||
                               opts.ltrt_correction_db || opts.height_downmix || opts.height_db;
    // With several substreams the downmix goes to the presentations of 5.X
    // and 7.X, and the encoder says where there is none.
    if (count == 1 && downmix_named && !multichannel) {
        fmt::println(
            stderr,
            "error: the downmix options describe the stereo downmix of 5.0, 5.1, 7.0, 7.1 and the "
            "immersive layouts; the source is {}",
            iclforge::apps::ac4_layout_name(speakers.size(), pair));
        return kExitUsage;
    }
    if (opts.height_db && !opts.height_downmix) {
        fmt::println(stderr,
                     "error: height-gain= is the gain height-downmix= sends the top channels at; "
                     "give height-downmix=front, surround or front-and-surround");
        return kExitUsage;
    }
    if (count == 1 && opts.height_downmix && !immersive) {
        fmt::println(stderr,
                     "error: height-downmix= describes the top channels' downmix of 5.0.4, 5.1.4, "
                     "7.0.4 and 7.1.4; the source is {}",
                     iclforge::apps::ac4_layout_name(speakers.size(), pair));
        return kExitUsage;
    }
    if (count == 1 && opts.lfe_db && !has_lfe) {
        fmt::println(stderr, "error: lfemix= is the LFE's gain into the downmix, and {} has no LFE",
                     iclforge::apps::ac4_layout_name(speakers.size(), pair));
        return kExitUsage;
    }

    iclforge::ac4::EncoderConfig config;
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
    config.experimental.back_pair = meta.ac4_experimental_back_pair;
    config.experimental.ajcc = meta.ac4_experimental_ajcc;

    if (drc_named) {
        // Table 161's four modes on drc='s profile, and a mode named on a
        // profile of its own takes its curve, or repeats the first mode with
        // the same.
        iclforge::ac4::DrcConfig drc;
        drc.profile = opts.drc.value_or(drc.profile);
        for (int id = 0; id < 4; ++id) {
            iclforge::ac4::DrcModeConfig mode{.id = id, .gains_config = opts.drc_gains};
            const std::optional<iclforge::ac4::DrcProfile>& own =
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
        iclforge::ac4::DownmixConfig downmix;
        downmix.loro_centre_db = opts.loro_centre_db.value_or(downmix.loro_centre_db);
        downmix.loro_surround_db = opts.loro_surround_db.value_or(downmix.loro_surround_db);
        downmix.ltrt_centre_db = opts.ltrt_centre_db;
        downmix.ltrt_surround_db = opts.ltrt_surround_db;
        downmix.lfe_db = opts.lfe_db;
        downmix.preferred = opts.preferred_downmix.value_or(downmix.preferred);
        downmix.loro_correction_db2 = opts.loro_correction_db;
        downmix.ltrt_correction_db2 = opts.ltrt_correction_db;
        downmix.height = opts.height_downmix;
        downmix.height_db = opts.height_db.value_or(downmix.height_db);
        config.downmix = downmix;
    }
    if (!substream_form) {
        config.dialogue = dialogue_config(first.dialogue, speakers.size());
    } else {
        for (std::size_t n = 0; n < count; ++n) {
            const Options::Ac4Encode::Substream& s = substreams[n];
            iclforge::ac4::SubstreamConfig substream;
            substream.channels = inputs[n] ? static_cast<int>(inputs[n]->speakers.size()) : 0;
            substream.bitrate_kbps = s.bitrate_kbps;
            substream.codec_mode = codec_mode_of(n == 0 ? meta.ac4_codec_mode : s.codec_mode);
            substream.content = s.content;
            substream.language = s.language;
            if (inputs[n]) {
                substream.dialogue = dialogue_config(s.dialogue, inputs[n]->speakers.size());
            }
            if (s.max_dialogue_gain_db || !s.pan_degrees.empty()) {
                substream.dialogue_mix = iclforge::ac4::DialogueMix{.max_gain_db = s.max_dialogue_gain_db,
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
        iclforge::ac4::PresentationConfig presentation;
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
            presentation.associated = iclforge::ac4::AssociatedMix{.main_db = p.main_db,
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
    const auto refuse_config = [](const iclforge::ac4::EncoderConfig& refused) {
        fmt::println(stderr, "error: the encoder refuses {} (ac3cli help ac4-encode)",
                     iclforge::ac4::Encoder::refusal_reason(refused));
        return kExitUsage;
    };
    iclforge::ac4::EncoderConfig sized = config;
    if (opts.loudness) {
        iclforge::ac4::FurtherLoudness loudness;
        loudness.practice = *opts.loudness;
        loudness.integrated_lkfs = -23.0;
        loudness.loudness_range_lu = 0.0;
        loudness.max_true_peak_dbtp = 0.0;
        loudness.max_momentary_lufs = -23.0;
        loudness.max_short_term_lufs = -23.0;
        sized.loudness = loudness;
    }
    if (!iclforge::ac4::Encoder::create(sized).has_value()) {
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
        const auto measured = iclforge::apps::measure_ac4_programme(main_views, main.wav.sample_rate);
        if (!measured) {
            fmt::println(
                stderr,
                "error: no audio above the -70 LKFS absolute gate; pass dialnorm=<0..31.75> and "
                "leave loudness= out");
            return kExitRuntime;
        }
        if (measure_dialnorm) {
            dialnorm = iclforge::apps::ac4_dialnorm_for(measured->integrated);
        }
        status_println(status, "measured {:.2f} LKFS (BS.1770-4, gated) -> dialnorm -{:g} dB",
                       measured->integrated, dialnorm);
        if (opts.loudness) {
            config.loudness = iclforge::apps::ac4_further_loudness(*opts.loudness, *measured);
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
    const auto trace = [&trace_file, &trace_frame](const iclforge::ac4::SyntaxRecord& r) {
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

    auto encoder = iclforge::ac4::Encoder::create(config);
    if (!encoder.has_value()) {
        return refuse_config(config);
    }
    auto frames = stems ? encoder->encode(views, stem_views) : encoder->encode(views);
    if (!frames.has_value()) {
        fmt::println(stderr, "error: {}: {}", in_path, iclforge::ac4::describe(frames.error()));
        return kExitInput;
    }
    auto rest = encoder->flush();
    if (!rest.has_value()) {
        fmt::println(stderr, "error: {}", iclforge::ac4::describe(rest.error()));
        return kExitInput;
    }
    frames->insert(frames->end(), rest->begin(), rest->end());

    const iclforge::ac4::Toc& toc = encoder->toc();
    const auto packaged = iclforge::apps::package_ac4(*frames, toc, to_mp4, opts.crc.value_or(true));
    if (!packaged.has_value()) {
        fmt::println(stderr, "error: {}", packaged.error().message);
        return packaged.error().usage ? kExitUsage : kExitOutput;
    }
    const std::string& rfc6381 = packaged->rfc6381;
    if (!write_frames(out_path, packaged->chunks)) {
        return kExitOutput;
    }
    if (trace_file.is_open()) {
        trace_file.close();
        if (!trace_file) {
            fmt::println(stderr, "error: cannot write {}", meta.syntax_trace_path);
            return kExitOutput;
        }
    }
    const std::string shape =
        count > 1 ? fmt::format("{} substreams", count)
                  : std::string{iclforge::apps::ac4_layout_name(speakers.size(), pair)};
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
