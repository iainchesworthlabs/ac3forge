#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "ac4/ac4.hpp"
#include "ac4/syntax.hpp"
#include "ac4dec/decoder.hpp"
#include "ac4enc/encoder.hpp"

// ac4::Encoder (src/ac4enc) over the configurations and input it takes, read
// back by the decoder (planning/ac4.md, the encoder's ladder, items 1 and 8).
//
// The first bytes choose the configuration - the channel layout, mono to
// 7.1, sample rate, bit rate, I-frame interval, dialnorm, codec mode, the A-CPL
// ones among them, the experimental tools, the size of the pieces the input
// arrives in, and phase E5's frame rate, rate mode, named I-frames, fragment
// start, loudness values, DRC modes and gains, downmix values and dialogue
// enhancement, from marked channels or a stem that is the input at half
// level, and phase E6's substreams and presentations - and the rest are the
// samples, as 32-bit floats, one channel after the
// other: silence, DC, full-scale square waves, clipping far past full scale,
// denormals and NaNs are all a few bytes away. What is held:
//
//   - a configuration create() takes encodes; one it refuses is refused as
//     kInvalidConfig, and input with a sample that is not finite as
//     kInvalidInput, and nothing else fails;
//   - every frame the encoder returns reads back through the decoder's syntax
//     layer to the end of every substream, and the decoder's trace of it is
//     the encoder's own, record for record;
//   - every frame decodes to PCM, and every sample of it is finite (every
//     frame is refused as kUnsupported where no presentation carries audio
//     the stream enables);
//   - a second encoder given the same configuration and input writes the same
//     bytes.
//
// A violation aborts, so libFuzzer keeps the input.
namespace {

[[noreturn]] void violated() {
    std::abort();
}

struct Take {
    std::span<const std::uint8_t> data;
    std::uint8_t byte() {
        if (data.empty()) {
            return 0;
        }
        const std::uint8_t b = data.front();
        data = data.subspan(1);
        return b;
    }
};

constexpr std::size_t kMaxSamples = 2048 * 3;  // per channel: three frames keep an execution short

struct Run {
    std::vector<std::vector<std::byte>> frames;
    std::vector<ac4::SyntaxRecord> trace;
    bool refused = false;
};

Run encode(const ac4::EncoderConfig& base, const std::vector<std::vector<float>>& input,
           const std::vector<std::vector<float>>* dialogue, std::size_t piece, bool expect_invalid_input) {
    Run run;
    ac4::EncoderConfig config = base;
    const auto sink = [&run](const ac4::SyntaxRecord& r) { run.trace.push_back(r); };
    config.trace = sink;
    auto encoder = ac4::Encoder::create(config);
    if (!encoder.has_value()) {
        if (encoder.error() != ac4::EncodeError::kInvalidConfig) {
            violated();
        }
        run.refused = true;
        return run;
    }
    const std::size_t total = input.front().size();
    for (std::size_t at = 0; at < total || total == 0; at += piece) {
        const std::size_t count = std::min(piece, total - at);
        std::vector<std::span<const float>> views;
        for (const std::vector<float>& channel : input) {
            views.emplace_back(std::span<const float>(channel).subspan(at, count));
        }
        std::vector<std::span<const float>> stem;
        if (dialogue != nullptr) {
            for (const std::vector<float>& channel : *dialogue) {
                stem.emplace_back(std::span<const float>(channel).subspan(at, count));
            }
        }
        auto frames = dialogue != nullptr ? encoder->encode(views, stem) : encoder->encode(views);
        if (!frames.has_value()) {
            if (frames.error() != ac4::EncodeError::kInvalidInput || !expect_invalid_input) {
                violated();
            }
            run.refused = true;
            return run;
        }
        for (ac4::EncodedFrame& frame : *frames) {
            run.frames.push_back(std::move(frame.raw_ac4_frame));
        }
        if (total == 0) {
            break;
        }
    }
    auto rest = encoder->flush();
    if (!rest.has_value()) {
        violated();
    }
    for (ac4::EncodedFrame& frame : *rest) {
        run.frames.push_back(std::move(frame.raw_ac4_frame));
    }
    return run;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    Take take{std::span<const std::uint8_t>(data, size)};
    ac4::EncoderConfig config;
    // The first byte's low bit chooses mono or stereo, as it always has; the
    // two bits over it can widen that to 5.0 or 5.1, or to 7.0 or 7.1 in the
    // 7.X layout the next two bits name (or in none, which is refused), the
    // bit over those asks for the experimental coding configurations, and the
    // one over that for the experimental A-CPL modes.
    constexpr std::array<ac4::AdditionalPair, 4> kPairs = {ac4::AdditionalPair::kNone, ac4::AdditionalPair::kBack,
                                                           ac4::AdditionalPair::kWide, ac4::AdditionalPair::kTopFront};
    const std::uint8_t layout = take.byte();
    const bool lfe = (layout & 1) != 0;
    switch ((layout >> 1) & 3) {
        case 1:
            config.channels = lfe ? 6 : 5;
            break;
        case 2:
            config.channels = lfe ? 8 : 7;
            config.experimental.seven_x = kPairs[static_cast<std::size_t>((layout >> 3) & 3)];
            break;
        default:
            config.channels = lfe ? 2 : 1;
            break;
    }
    config.experimental.coding_configs = (layout & 0x20) != 0;
    config.experimental.acpl = (layout & 0x40) != 0;
    // The sample rate byte's low bit; the two over it name the A-CPL mode.
    const std::uint8_t rate = take.byte();
    config.sample_rate_hz = (rate & 1) != 0 ? 44100 : 48000;
    // 4 to 1024 kbps, so the refusals below 8 are reached too.
    config.bitrate_kbps = 4 + static_cast<int>(take.byte()) * 4;
    // The interval takes the low five bits of its byte and dialnorm seven of
    // its; the bits over choose the codec mode, the rate's, either forced or an
    // A-CPL mode, and the experimental A-SPX tools.
    const std::uint8_t interval = take.byte();
    config.iframe_interval = 1 + (interval % 32);
    constexpr std::array<ac4::CodecMode, 3> kModes = {ac4::CodecMode::kAuto, ac4::CodecMode::kSimple,
                                                      ac4::CodecMode::kAspx};
    constexpr std::array<ac4::CodecMode, 4> kAcplModes = {ac4::CodecMode::kAspxAcpl1, ac4::CodecMode::kAspxAcpl2,
                                                          ac4::CodecMode::kAspxAcpl3, ac4::CodecMode::kAspxAcpl2};
    const auto mode = static_cast<std::size_t>((interval >> 5) & 3);
    config.codec_mode = mode < kModes.size() ? kModes[mode] : kAcplModes[static_cast<std::size_t>((rate >> 1) & 3)];
    const std::uint8_t dialnorm = take.byte();
    config.dialnorm_db = -static_cast<double>(dialnorm % 128) / 4.0;
    config.experimental.aspx_balance = (dialnorm & 0x80) != 0;
    config.experimental.aspx_interleave = (interval & 0x80) != 0;
    config.experimental.aspx_varvar = (dialnorm & 0x80) != 0 && (interval & 0x80) != 0;
    const std::size_t piece = 1 + static_cast<std::size_t>(take.byte()) * 37;
    // Phase E5. The timing byte: the frame rate in its low four bits (14 and
    // 15 reserved, and 44.1 kHz takes 13 alone, both refused), the rate mode
    // in the two over them, and named I-frames and a fragment start in the
    // top two.
    const std::uint8_t timing = take.byte();
    config.frame_rate_index = timing & 15;
    constexpr std::array<ac4::RateMode, 4> kRateModes = {ac4::RateMode::kConstant, ac4::RateMode::kAverage,
                                                         ac4::RateMode::kVariable, ac4::RateMode::kConstant};
    config.rate_mode = kRateModes[static_cast<std::size_t>((timing >> 4) & 3)];
    if ((timing & 0x40) != 0) {
        config.iframes = {3, 1};
    }
    if ((timing & 0x80) != 0) {
        config.fragment_starts = {1500};
    }
    // The metadata byte switches each element on; the values byte gives
    // their values. The downmix is refused below 5.X, and the Mid method but
    // for L and R.
    const std::uint8_t metadata = take.byte();
    const std::uint8_t values = take.byte();
    if ((metadata & 1) != 0) {
        ac4::FurtherLoudness loudness;
        loudness.practice =
            (values & 1) != 0 ? ac4::LoudnessPractice::kEbuR128 : ac4::LoudnessPractice::kNotIndicated;
        loudness.integrated_lkfs = -static_cast<double>(values % 64) / 2.0;
        loudness.loudness_range_lu = static_cast<double>(values % 32);
        loudness.max_true_peak_dbtp = -static_cast<double>(values % 16);
        loudness.max_momentary_lufs = -static_cast<double>(values % 48) / 2.0;
        loudness.max_short_term_lufs = -static_cast<double>(values % 40) / 2.0;
        config.loudness = loudness;
    }
    if ((metadata & 2) != 0) {
        constexpr std::array<ac4::DrcProfile, 6> kProfiles = {
            ac4::DrcProfile::kNone,         ac4::DrcProfile::kFilmStandard, ac4::DrcProfile::kFilmLight,
            ac4::DrcProfile::kMusicStandard, ac4::DrcProfile::kMusicLight,  ac4::DrcProfile::kSpeech};
        ac4::DrcConfig drc;
        drc.profile = kProfiles[values % kProfiles.size()];
        if ((metadata & 4) != 0) {
            // Table 161's modes, the third on a profile of its own and the
            // fourth repeating it, and with the next bit gains in each.
            for (int id = 0; id < 4; ++id) {
                ac4::DrcModeConfig drc_mode;
                drc_mode.id = id;
                if (id == 2) {
                    drc_mode.profile = kProfiles[static_cast<std::size_t>(values >> 3) % kProfiles.size()];
                }
                if (id == 3) {
                    drc_mode.repeat_of = 2;
                }
                if ((metadata & 8) != 0) {
                    drc_mode.gains_config = (values >> 6) & 3;
                }
                drc.modes.push_back(drc_mode);
            }
            config.experimental.drc_gains = (metadata & 8) != 0;
        }
        config.drc = drc;
    }
    if ((metadata & 16) != 0) {
        constexpr std::array<double, 7> kCentre = {3.0, 1.5, 0.0, -1.5, -3.0, -4.5, -6.0};
        constexpr std::array<double, 5> kSurround = {0.0, -1.5, -3.0, -4.5, -6.0};
        ac4::DownmixConfig downmix;
        downmix.loro_centre_db = kCentre[values % kCentre.size()];
        downmix.loro_surround_db = kSurround[values % kSurround.size()];
        if ((values & 0x20) != 0) {
            downmix.ltrt_centre_db = kCentre[static_cast<std::size_t>(values >> 2) % kCentre.size()];
            downmix.ltrt_surround_db = kSurround[static_cast<std::size_t>(values >> 2) % kSurround.size()];
        }
        if (config.channels == 6 || config.channels == 8) {
            downmix.lfe_db = 5.5 - static_cast<double>(values % 32);
        }
        downmix.preferred = static_cast<ac4::PreferredDownmix>(values % 4);
        downmix.loro_correction_db2 = (static_cast<double>(values % 31) - 15.0) / 2.0;
        config.downmix = downmix;
    }
    bool stem = false;
    if ((metadata & 32) != 0) {
        constexpr std::array<ac4::DialogueMethod, 4> kMethods = {
            ac4::DialogueMethod::kChannelIndependent, ac4::DialogueMethod::kMid,
            ac4::DialogueMethod::kCrossChannel, ac4::DialogueMethod::kChannelIndependent};
        ac4::DialogueConfig dialogue;
        dialogue.method = kMethods[static_cast<std::size_t>((metadata >> 6) & 3)];
        stem = dialogue.method == ac4::DialogueMethod::kCrossChannel || (values & 0x80) != 0;
        dialogue.source = stem ? ac4::DialogueSource::kStem : ac4::DialogueSource::kMarkedChannels;
        dialogue.left = config.channels > 1;
        dialogue.right = config.channels > 1;
        dialogue.centre = config.channels != 2 && dialogue.method != ac4::DialogueMethod::kMid;
        dialogue.max_gain_db = 3 * (1 + (values & 3));
        config.dialogue = dialogue;
    }

    // Phase E6. The shape byte's low three bits make the layout above the
    // first of several substreams and name the presentation of Part 2 Table
    // 53 that plays them together: 1 to 6 configurations 0 to 5 over a
    // second substream, and for 3 and 4 a third, the second in 2 and 5 a
    // dialogue enhancement substream carrying the first's hybrid waveform
    // (which the dialogue enhancement above must be on for); 7 the one
    // substream, with a configuration 6 presentation of EMDF payloads beside
    // it; 0 keeps the one substream. The bits over them put the other
    // substreams in stereo or, over that, in 3.0 (experimental); add a
    // presentation of each substream alone; give the substreams a language
    // and the first presentation a name; and put EMDF payloads in the first
    // presentation's emdf_info() and the first substream's metadata(). The
    // detail byte gives their values - the waveform's share, configuration
    // 5's classifiers, the groups' gains, the dialogue's and the associated
    // audio's mixing values and the payloads' fields - and the flags byte the
    // first presentation's md_compat and presentation_id, whether it is
    // pre-virtualized or enabled, and whether the decoder asks for the last.
    const std::uint8_t shape = take.byte();
    const std::uint8_t detail = take.byte();
    const std::uint8_t flags = take.byte();
    std::size_t presentation_count = 1;
    bool selectable = true;
    if (const int kind = shape & 7; kind != 0) {
        using Content = ac4::ContentClassifier;
        const bool named = (shape & 0x40) != 0;
        const int side = (shape & 0x10) != 0 ? 3 : ((shape & 8) != 0 ? 2 : 1);
        config.experimental.three_zero = side == 3;
        const auto payload = [detail, shape](int id) {
            ac4::EmdfPayload p;
            p.id = id;
            // A size past variable_bits(8)'s first 255 where both top bits are set.
            p.bytes.assign(static_cast<std::size_t>(detail % 5) + ((detail & 0xC0) == 0xC0 ? 300U : 0U), shape);
            if ((detail & 1) != 0) {
                p.sample_offset = detail;
            }
            if ((detail & 2) != 0) {
                p.duration = 1 + detail;
            }
            if ((detail & 4) != 0) {
                p.group_id = detail % 7;
            }
            if ((detail & 8) != 0) {
                p.codec_data = detail;
            }
            p.discard_unknown = (detail & 16) == 0;
            p.frame_aligned = (detail & 32) != 0;
            p.create_duplicate = (detail & 64) != 0;
            p.remove_duplicate = (shape & 1) != 0;
            p.priority = detail % 32;
            p.processing_allowed = detail % 4;
            return p;
        };
        const auto other = [named](Content content, int channels) {
            ac4::SubstreamConfig s;
            s.channels = channels;
            s.content = content;
            if (named) {
                s.language = "en";
            }
            return s;
        };
        std::vector<ac4::SubstreamConfig> subs(1);
        subs[0].channels = config.channels;
        subs[0].codec_mode = config.codec_mode;
        subs[0].content = Content::kCompleteMain;
        subs[0].dialogue = config.dialogue;
        ac4::SubstreamConfig enhancement;
        enhancement.enhances = 0;
        switch (kind) {
            case 1:
                subs[0].content = Content::kMusicAndEffects;
                subs.push_back(other(Content::kDialogue, side));
                break;
            case 2:
                subs.push_back(enhancement);
                break;
            case 3:
                subs.push_back(other(Content::kVisuallyImpaired, side));
                break;
            case 4:
                subs[0].content = Content::kMusicAndEffects;
                subs.push_back(other(Content::kDialogue, side));
                subs.push_back(other(Content::kCommentary, 1));
                break;
            case 5:
                subs.push_back(enhancement);
                subs.push_back(other(Content::kHearingImpaired, side));
                break;
            case 6:
                subs[0].content = (detail & 1) != 0 ? Content::kMusicAndEffects : Content::kCompleteMain;
                subs.push_back(other(static_cast<Content>((detail >> 1) & 7), side));
                break;
            default:
                break;
        }
        if ((kind == 2 || kind == 5) && subs[0].dialogue) {
            subs[0].dialogue->hybrid = true;
            subs[0].dialogue->waveform_share = static_cast<double>(detail & 15) / 15.0;
        }
        for (ac4::SubstreamConfig& s : subs) {
            if (s.content == Content::kDialogue) {
                ac4::DialogueMix mix;
                mix.max_gain_db = 3 * (1 + ((detail >> 4) & 3));
                if ((detail & 0x40) != 0 && s.channels <= 2) {
                    const double degrees = 1.5 * static_cast<double>(detail >> 1);
                    mix.pan_degrees.assign(static_cast<std::size_t>(s.channels), degrees);
                }
                s.dialogue_mix = mix;
            }
        }
        ac4::PresentationConfig all;
        if (kind != 7) {
            all.config = kind - 1;
        }
        for (std::size_t i = 0; i < subs.size(); ++i) {
            all.substreams.push_back(static_cast<int>(i));
        }
        if (named) {
            all.name.assign(static_cast<std::size_t>(1 + detail % 32), 'N');  // 32 bytes are refused
        }
        if ((detail & 0x80) != 0) {
            for (std::size_t i = 0; i < subs.size(); ++i) {
                const bool zero = i == 0 || subs[i].enhances.has_value();
                all.gains_db.push_back(zero ? 0.0 : -0.25 * static_cast<double>(detail & 15));
            }
        }
        if ((kind == 3 || kind == 4 || kind == 5) && (detail & 0x20) != 0) {
            ac4::AssociatedMix mix;
            mix.main_db = -0.3 * static_cast<double>(detail >> 4);
            if ((detail & 1) != 0) {
                mix.main_centre_db = -0.3 * static_cast<double>(detail & 15);
            }
            if ((detail & 2) != 0) {
                mix.main_front_db = -std::numeric_limits<double>::infinity();
            }
            if (subs.back().channels == 1) {
                mix.pan_degrees = 1.5 * static_cast<double>(detail & 0x7F);
            }
            all.associated = mix;
        }
        if ((shape & 0x80) != 0) {
            all.emdf.push_back(payload(detail % 64));  // 0 is refused, and from 31 the id escapes
            subs[0].emdf.push_back(payload(1 + detail % 40));
        }
        if ((flags & 8) != 0) {
            all.md_compat = flags & 7;  // 4 to 6 are refused, and one below the tracks' least
        }
        if ((flags & 16) != 0) {
            all.presentation_id = detail % 8;  // may be a later presentation's too, which is refused
        }
        all.pre_virtualized = (flags & 32) != 0;
        if ((flags & 64) != 0) {
            all.enabled = (detail & 1) == 0;
        }
        std::vector<ac4::PresentationConfig> presentations{all};
        if ((shape & 0x20) != 0) {
            for (std::size_t i = 0; i < subs.size(); ++i) {
                ac4::PresentationConfig one;
                one.substreams = {static_cast<int>(i)};
                presentations.push_back(one);
            }
        }
        if (kind == 7) {
            ac4::PresentationConfig emdf;
            emdf.config = 6;
            emdf.emdf.push_back(payload(1 + detail % 63));
            presentations.push_back(emdf);
        }
        selectable = std::ranges::any_of(presentations, [](const ac4::PresentationConfig& p) {
            return p.config != 6 && p.enabled.value_or(true);
        });
        presentation_count = presentations.size();
        config.substreams = std::move(subs);
        config.presentations = std::move(presentations);
    }
    // With several substreams, encode() takes each one's channels in turn, and
    // a dialogue enhancement substream none.
    int inputs = config.channels;
    if (!config.substreams.empty()) {
        inputs = 0;
        for (const ac4::SubstreamConfig& s : config.substreams) {
            inputs += s.enhances ? 0 : s.channels;
        }
    }

    const std::size_t floats = take.data.size() / sizeof(float);
    const std::size_t per_channel = std::min(floats / static_cast<std::size_t>(inputs), kMaxSamples);
    std::vector<std::vector<float>> input(static_cast<std::size_t>(inputs), std::vector<float>(per_channel));
    bool finite = true;
    for (std::size_t c = 0; c < input.size(); ++c) {
        for (std::size_t n = 0; n < per_channel; ++n) {
            float x = 0.0F;
            std::memcpy(&x, take.data.data() + (c * per_channel + n) * sizeof(float), sizeof(float));
            finite = finite && std::isfinite(x);
            input[c][n] = x;
        }
    }

    std::vector<std::vector<float>> dialogue;
    if (stem) {
        dialogue = input;
        for (std::vector<float>& channel : dialogue) {
            for (float& x : channel) {
                x *= 0.5F;
            }
        }
    }
    const std::vector<std::vector<float>>* stem_input = stem ? &dialogue : nullptr;
    const Run first = encode(config, input, stem_input, piece, !finite);
    if (first.refused) {
        return 0;
    }
    if (!finite) {
        violated();  // a sample that is not finite was taken
    }

    // Every frame reads to the end of every substream, with the encoder's trace.
    std::vector<ac4::SyntaxRecord> read;
    const auto sink = [&read](const ac4::SyntaxRecord& r) { read.push_back(r); };
    ac4::DecoderConfig decoder_config;
    decoder_config.syntax = sink;
    ac4::Decoder reader(decoder_config);
    // Every presentation the stream enables can be selected, whatever its
    // md_compat; the flags byte's top bit asks for the last.
    ac4::DecoderConfig decode_config;
    decode_config.level = 7;
    if ((flags & 0x80) != 0) {
        decode_config.presentation.index = presentation_count - 1;
    }
    ac4::Decoder decoder(decode_config);
    for (const std::vector<std::byte>& frame : first.frames) {
        const auto report = reader.parse(frame);
        if (!report.has_value()) {
            violated();
        }
        for (const ac4::SubstreamReport& substream : report->substreams) {
            if (substream.refused.has_value() || substream.bits_read != substream.size_bits) {
                violated();
            }
        }
        const auto decoded = decoder.decode(frame);
        if (!selectable) {
            if (decoded.has_value() || decoded.error() != ac4::DecodeError::kUnsupported) {
                violated();
            }
            continue;
        }
        if (!decoded.has_value() || !decoded->has_value()) {
            violated();
        }
        for (const std::vector<float>& channel : (*decoded)->channels) {
            for (const float x : channel) {
                if (!std::isfinite(x)) {
                    violated();
                }
            }
        }
    }
    if (read.size() != first.trace.size()) {
        violated();
    }
    for (std::size_t i = 0; i < read.size(); ++i) {
        if (read[i].substream != first.trace[i].substream || read[i].bit_offset != first.trace[i].bit_offset ||
            read[i].bits != first.trace[i].bits || read[i].value != first.trace[i].value) {
            violated();
        }
    }

    // The same input and configuration write the same bytes.
    const Run second = encode(config, input, stem_input, piece, false);
    if (second.frames != first.frames) {
        violated();
    }
    return 0;
}
