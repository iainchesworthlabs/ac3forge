// The decoder's presentations (planning/ac4.md, phase D7).
//
// Selection: a table of tables of contents built for it, of version 1 and
// version 0 presentations, each case a table of contents, a PresentationChoice
// and a level, and the presentation Part 2 clause 4.8.2 and the readings of
// src/ac4dec/ERRATA.md ("Selecting a presentation") select. The table is
// committed as tests/golden/ac4dec/presentations/presentation-selection.tsv, which
// tools/checks/test_ac4_presentation_selection.py holds the Python reference
// parser's selection to.
//
// Mixing: the streams under tests/golden/ac4dec/presentations/ are the test
// multiplexer's (ac4dec_mux.hpp) over DEE's tone legs and the encoder's
// sources beside them, byte for byte, and hold presentations of several
// substreams: music and effects with dialogue, main with associated audio,
// both, by content classifier, and main with a dialogue enhancement substream
// for the hybrid methods; and version 0 presentations, whose substreams carry
// their own dialnorms and mixing fields. Each substream carries tones of its
// own, so each mix is measured tone by tone against its formula (Part 1
// clauses 5.7.8.9 and 6.2.16, Part 2 clauses 4.8.3.17 to 4.8.5), to 0.01 dB,
// and as a waveform against the same formula applied to the substreams
// decoded alone.
// tools/references/ac4_syntax.py's digests of them are beside the others in
// tests/golden/ac4dec/. With AC4DEC_WRITE_PRESENTATIONS set to a directory,
// the streams and the selection table are written there instead of compared,
// to commit after a change to the multiplexer.

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac4/ac4.hpp"
#include "ac4/ac4_toc_writer.hpp"
#include "ac4dec/decoder.hpp"
#include "ac4dec_mux.hpp"
#include "pcm/mixer.hpp"

namespace {

namespace fs = std::filesystem;
using ac4::Speaker;
using ac4dec_test::MuxGroup;
using ac4dec_test::MuxLayout;
using ac4dec_test::MuxPresentation;
using ac4dec_test::MuxSource;

constexpr std::size_t kFrames = 24;
// The frames before the steady state: the decoder's delay, and the first
// frames' overlap.
constexpr std::size_t kSkippedSamples = 5 * 2048;
constexpr double kToleranceDb = 0.01;

std::vector<std::byte> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    return bytes;
}

void write_file(const fs::path& path, std::span<const std::byte> bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(out.good());
}

fs::path golden() {
    return fs::path{AC4DEC_GOLDEN_DIR};
}

MuxSource source(const fs::path& path) {
    const std::vector<std::byte> file = read_file(path);
    REQUIRE_FALSE(file.empty());
    return ac4dec_test::mux_source(file);
}

MuxSource dee(const char* leg) {
    return source(fs::path{AC3FORGE_GOLDEN_EXTERNAL_BASELINE_DIR} / leg / "dee.ac4");
}

MuxSource encoded(const char* name) {
    return source(golden() / "presentations" / "sources" / (std::string{name} + ".ac4"));
}

// --- The multiplexed streams ---------------------------------------------------

// Each substream's tones: DEE's 5.1 tones leg (L R C LFE Ls Rs), its 2.0 leg
// (L R), and the encoder's sources (tools/generators/gen_ac4_presentation_
// sources.py).
constexpr std::array<double, 6> kTones51 = {331.0, 457.0, 613.0, 47.0, 787.0, 953.0};
constexpr double kToneDialogueEn = 1117.0;
constexpr double kToneDialogueDe = 1373.0;
constexpr double kToneAd = 1531.0;
constexpr std::array<double, 2> kToneCommentary = {1699.0, 1847.0};
constexpr std::array<double, 2> kToneDialogueFr = {1931.0, 2083.0};
constexpr double kToneDe = 977.0;

// Part 1 clause 4.3.12.4.9: 1.5 degrees a step, clockwise.
constexpr int kPan330 = 220;
constexpr int kPan0 = 0;
constexpr int kPan30 = 20;

MuxGroup group(std::size_t from, std::optional<int> classifier, std::string language = {},
               std::optional<ac4::detail::DialogueMixCodes> dialogue = std::nullopt) {
    MuxGroup g;
    g.source = from;
    g.content_classifier = classifier;
    g.language = std::move(language);
    g.dialogue = dialogue;
    return g;
}

MuxPresentation presentation(std::optional<int> config, std::vector<int> groups, int id, int md_compat,
                             std::size_t from, ac4::detail::PresentationMixCodes mix = {}) {
    MuxPresentation p;
    p.presentation_config = config;
    p.groups = std::move(groups);
    p.presentation_id = id;
    p.md_compat = md_compat;
    p.source = from;
    p.mix = std::move(mix);
    return p;
}

ac4::detail::PresentationMixCodes gains(std::vector<int> sg) {
    ac4::detail::PresentationMixCodes mix;
    mix.sg_gain = std::move(sg);
    return mix;
}

ac4::detail::PresentationMixCodes associated(ac4::detail::AssociatedMixCodes codes, std::optional<std::vector<int>> sg = {}) {
    ac4::detail::PresentationMixCodes mix;
    mix.sg_gain = std::move(sg);
    mix.associated = codes;
    return mix;
}

// presentations-5_1: a 5.1 music and effects substream with dialogue in
// English and German, audio description, a stereo commentary and a stereo
// French dialogue; and a 2.0 main substream. Each presentation's id is in the
// comment; ids 20 and up are each group alone, which the mixes are measured
// against.
struct Built {
    std::vector<MuxSource> sources;
    MuxLayout layout;
};

Built stream_5_1() {
    Built b;
    b.sources = {dee("ac4-51-tones-384"),      dee("ac4-20-tones-192"),         encoded("dialogue-en-mono"),
                 encoded("dialogue-de-mono"),  encoded("ad-mono"),              encoded("commentary-stereo"),
                 encoded("dialogue-fr-stereo")};
    b.layout.groups = {
        group(0, 0b001),                                                                    // 0: music and effects
        group(2, 0b100, "en", ac4::detail::DialogueMixCodes{1, std::array{kPan330, 0}, 0}),  // 1: dialogue, 6 dB, 330
        group(3, 0b100, "de", ac4::detail::DialogueMixCodes{3, std::nullopt, 0}),            // 2: dialogue, 12 dB
        group(4, 0b010, "qad"),                                                             // 3: audio description
        group(5, 0b101, "en"),                                                              // 4: commentary, stereo
        group(6, 0b100, "fr", ac4::detail::DialogueMixCodes{0, std::array{kPan0, kPan30}, 0}),  // 5: dialogue, stereo
        group(1, 0b000),                                                                    // 6: 2.0 main
    };
    ac4::detail::AssociatedMixCodes scaled;
    scaled.scale_main = 20;         // -6 dB
    scaled.scale_main_centre = 10;  // -3 dB
    scaled.scale_main_front = 5;    // -1.5 dB
    scaled.pan_associated = kPan30;
    ac4::detail::AssociatedMixCodes at330;
    at330.pan_associated = kPan330;
    ac4::detail::AssociatedMixCodes at0;
    at0.pan_associated = kPan0;
    ac4::detail::AssociatedMixCodes at30;
    at30.pan_associated = kPan30;
    ac4::detail::AssociatedMixCodes front;
    front.scale_main_front = 10;  // -3 dB; a stereo associated substream, not panned
    b.layout.presentations = {
        presentation(0, {0, 1}, 1, 2, 0, gains({0, 8})),                     // M&E + English, -2 dB
        presentation(0, {0, 2}, 2, 2, 0),                                    // M&E + German
        presentation(3, {0, 1, 3}, 3, 2, 0, associated(scaled, std::vector{0, 0, 4})),  // + audio description
        presentation(2, {0, 3}, 4, 2, 0, associated(at330)),                 // main + AD at 330 degrees
        presentation(2, {0, 3}, 5, 2, 0, associated(at0)),                   // at 0 degrees
        presentation(2, {0, 3}, 6, 2, 0, associated(at30)),                  // at 30 degrees
        presentation(2, {0, 4}, 7, 2, 0, associated(front)),                 // main + stereo commentary
        presentation(0, {0, 5}, 8, 2, 0),                                    // M&E + stereo French
        presentation(5, {0, 1, 3}, 9, 2, 0, associated(at30, std::vector{0, 2, 6})),  // by classifier
        presentation(2, {6, 3}, 10, 1, 1, associated(at0)),                  // 2.0 main + AD at 0 degrees
        presentation(std::nullopt, {0}, 20, 1, 0),
        presentation(std::nullopt, {1}, 21, 0, 2),
        presentation(std::nullopt, {2}, 22, 0, 3),
        presentation(std::nullopt, {3}, 23, 0, 4),
        presentation(std::nullopt, {4}, 24, 0, 5),
        presentation(std::nullopt, {5}, 25, 0, 6),
        presentation(std::nullopt, {6}, 26, 0, 1),
    };
    return b;
}

// presentations-hybrid: main substreams whose dialogue enhancement is a
// hybrid method (Part 1 Table 170's 2 and 3), with the dialogue enhancement
// substream it takes: the channel independent method on a 5.1 main's C; the
// cross-channel method on a 2.0 main's L and R; the channel independent
// method on the Mid of L and R; and main, dialogue enhancement and associated
// audio together. Ids 10 and up are each group alone.
constexpr int kAlphaIndependent = 16;  // de_signal_contribution, alpha_c = x / 31
constexpr int kAlphaCross = 10;
constexpr int kAlphaMid = 8;
constexpr int kMixCoef1 = 12;  // Table 172: 0.448

ac4dec_test::MuxDe hybrid(int method, int channel_config, bool mid, std::array<int, 2> parameters, int alpha) {
    ac4dec_test::MuxDe de;
    de.config.method = method;
    de.config.max_gain = 3;  // 12 dB
    de.config.channel_config = channel_config;
    de.config.mid = mid;
    for (std::size_t band = 0; band < ac4::detail::kDeBands; ++band) {
        de.parameters.par[0][band] = parameters[0];
        de.parameters.par[1][band] = parameters[1];
    }
    de.parameters.mix = {kMixCoef1, 0};
    de.parameters.signal_contribution = alpha;
    return de;
}

Built stream_hybrid() {
    Built b;
    b.sources = {dee("ac4-51-tones-384"), dee("ac4-20-tones-192"), encoded("de-mono"), encoded("ad-mono")};
    MuxGroup independent = group(0, std::nullopt);
    independent.de = hybrid(2, 0b001, false, {5, 0}, kAlphaIndependent);  // C, p 0.5
    MuxGroup cross = group(1, std::nullopt);
    cross.de = hybrid(3, 0b110, false, {5, 3}, kAlphaCross);  // L and R, p 0.5 and 0.3
    MuxGroup mid = group(1, std::nullopt);
    mid.de = hybrid(2, 0b110, true, {5, 0}, kAlphaMid);  // the Mid of L and R, p 0.5
    ac4::detail::AssociatedMixCodes at30;
    at30.pan_associated = kPan30;
    b.layout.groups = {independent, group(2, std::nullopt), cross, mid, group(3, 0b010, "qad")};
    b.layout.presentations = {
        presentation(1, {0, 1}, 1, 2, 0),                                        // 5.1 main + DE
        presentation(4, {0, 1, 4}, 2, 2, 0, associated(at30, std::vector{0, 4})),  // + AD, -1 dB, at 30 degrees
        presentation(1, {2, 1}, 3, 1, 1),                                        // 2.0 main + DE, cross-channel
        presentation(1, {3, 1}, 4, 1, 1),                                        // 2.0 main + DE, Mid
        presentation(std::nullopt, {0}, 10, 1, 0),
        presentation(std::nullopt, {1}, 11, 0, 2),
        presentation(std::nullopt, {2}, 12, 0, 1),
        presentation(std::nullopt, {3}, 13, 0, 1),
        presentation(std::nullopt, {4}, 14, 0, 3),
    };
    return b;
}

// presentations-v0: version 0 presentations (bitstream_version 1), whose
// substreams carry their own dialnorm and mixing fields at sus_ver 0: 5.1
// music and effects at -31 dBFS, English dialogue at -25 panned to 330
// degrees, audio description at -27 with the main audio's scales and a pan to
// 30 degrees, a 2.0 main at -24, and stereo French dialogue at -31. Ids 10 and
// up are each substream alone.
constexpr int kDialnormMe = 124;  // -0.25 dB a step
constexpr int kDialnormEn = 100;
constexpr int kDialnormAd = 108;
constexpr int kDialnormStereoMain = 96;

struct BuiltV0 {
    std::vector<MuxSource> sources;
    ac4dec_test::MuxLayoutV0 layout;
};

ac4dec_test::MuxSubstreamV0 substream_v0(std::size_t from, int dialnorm_bits, std::optional<int> classifier = std::nullopt,
                                         std::string language = {}) {
    ac4dec_test::MuxSubstreamV0 s;
    s.source = from;
    s.dialnorm_bits = dialnorm_bits;
    s.content_classifier = classifier;
    s.language = std::move(language);
    return s;
}

ac4dec_test::MuxPresentationV0 presentation_v0(std::optional<int> config, std::vector<int> substreams, int id) {
    ac4dec_test::MuxPresentationV0 p;
    p.presentation_config = config;
    p.substreams = std::move(substreams);
    p.presentation_id = id;
    return p;
}

BuiltV0 stream_v0() {
    BuiltV0 b;
    b.sources = {dee("ac4-51-tones-384"), dee("ac4-20-tones-192"), encoded("dialogue-en-mono"), encoded("ad-mono"),
                 encoded("dialogue-fr-stereo")};
    ac4dec_test::MuxSubstreamV0 english = substream_v0(2, kDialnormEn, 0b100, "en");
    english.dialogue = ac4::detail::DialogueMixCodes{1, std::array{kPan330, 0}, 0};  // 6 dB, 330
    ac4dec_test::MuxSubstreamV0 ad = substream_v0(3, kDialnormAd, 0b010, "qad");
    ad.associated = ac4::detail::AssociatedMixCodes{.scale_main = 20,         // -6 dB
                                                    .scale_main_centre = 10,  // -3 dB
                                                    .scale_main_front = 5,    // -1.5 dB
                                                    .pan_associated = kPan30};
    ac4dec_test::MuxSubstreamV0 french = substream_v0(4, kDialnormMe, 0b100, "fr");
    french.dialogue = ac4::detail::DialogueMixCodes{0, std::array{kPan0, kPan30}, 0};  // 3 dB
    b.layout.substreams = {substream_v0(0, kDialnormMe), english, ad, substream_v0(1, kDialnormStereoMain), french};
    b.layout.presentations = {
        presentation_v0(0, {0, 1}, 1),     // M&E + English
        presentation_v0(2, {0, 2}, 2),     // main + audio description
        presentation_v0(3, {0, 1, 2}, 3),  // M&E + English + audio description
        presentation_v0(2, {3, 2}, 4),     // 2.0 main + audio description
        presentation_v0(0, {0, 4}, 5),     // M&E + stereo French
        presentation_v0(std::nullopt, {0}, 10),
        presentation_v0(std::nullopt, {1}, 11),
        presentation_v0(std::nullopt, {2}, 12),
        presentation_v0(std::nullopt, {3}, 13),
        presentation_v0(std::nullopt, {4}, 14),
    };
    return b;
}

std::vector<std::byte> build(const Built& b) {
    const std::vector<std::vector<std::byte>> frames = ac4dec_test::multiplex(b.sources, b.layout, kFrames);
    return ac4dec_test::mux_sync_framed(frames);
}

std::vector<std::byte> build(const BuiltV0& b) {
    const std::vector<std::vector<std::byte>> frames = ac4dec_test::multiplex_v0(b.sources, b.layout, kFrames);
    return ac4dec_test::mux_sync_framed(frames);
}

const std::vector<std::byte>& committed(std::string_view name) {
    static const std::vector<std::byte> five_one = read_file(golden() / "presentations" / "presentations-5_1.ac4");
    static const std::vector<std::byte> hybrid = read_file(golden() / "presentations" / "presentations-hybrid.ac4");
    static const std::vector<std::byte> v0 = read_file(golden() / "presentations" / "presentations-v0.ac4");
    if (name == "5_1") {
        return five_one;
    }
    return name == "hybrid" ? hybrid : v0;
}

// --- Decoding and measuring ------------------------------------------------------

struct Decoded {
    std::vector<Speaker> speakers;
    std::vector<std::vector<float>> channels;
    std::vector<std::optional<int>> presentation_ids;
};

Decoded decode(std::span<const std::byte> file, const ac4::DecoderConfig& config) {
    ac4::Decoder decoder(config);
    Decoded out;
    const ac4::ScanResult scan = ac4::scan(file);
    REQUIRE_FALSE(scan.stopped_at.has_value());
    for (const ac4::SyncFrame& frame : scan.frames) {
        const auto decoded = decoder.decode(frame.raw_ac4_frame);
        INFO(decoder.refusal_reason());
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        const ac4::DecodedFrame& pcm = **decoded;
        if (out.channels.empty()) {
            out.speakers = pcm.speakers;
            out.channels.resize(pcm.channels.size());
        }
        REQUIRE(pcm.speakers == out.speakers);
        for (std::size_t c = 0; c < pcm.channels.size(); ++c) {
            out.channels[c].insert(out.channels[c].end(), pcm.channels[c].begin(), pcm.channels[c].end());
        }
        out.presentation_ids.push_back(pcm.presentation_id);
    }
    return out;
}

// With `output_level_dbfs`, the output level gain alone, no compression.
Decoded decode_id(std::span<const std::byte> file, int id, double dialogue_gain_db = 0.0, double associated_gain_db = 0.0,
                  double dialogue_enhancement_db = 0.0, std::optional<double> output_level_dbfs = std::nullopt) {
    ac4::DecoderConfig config;
    config.presentation.presentation_id = id;
    config.output.dialogue_gain_db = dialogue_gain_db;
    config.output.associated_gain_db = associated_gain_db;
    config.output.dialogue_enhancement_db = dialogue_enhancement_db;
    config.output.output_level_dbfs = output_level_dbfs;
    config.output.drc = ac4::DrcMode::kOff;
    const Decoded decoded = decode(file, config);
    for (const std::optional<int>& got : decoded.presentation_ids) {
        REQUIRE(got == id);
    }
    return decoded;
}

// The amplitude of `samples`' component at `hz` through a Hann window, past
// the first frames.
double tone_amplitude(const Decoded& d, Speaker speaker, double hz) {
    const auto it = std::ranges::find(d.speakers, speaker);
    REQUIRE(it != d.speakers.end());
    const std::vector<float>& channel = d.channels[static_cast<std::size_t>(it - d.speakers.begin())];
    REQUIRE(channel.size() > kSkippedSamples + 4096);
    const std::span<const float> samples = std::span<const float>(channel).subspan(kSkippedSamples);
    const double w = 2.0 * std::numbers::pi * hz / 48000.0;
    const auto n = static_cast<double>(samples.size());
    std::complex<double> sum{};
    double weights = 0.0;
    for (std::size_t k = 0; k < samples.size(); ++k) {
        const double window = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(k) / (n - 1.0));
        sum += window * static_cast<double>(samples[k]) * std::polar(1.0, -w * static_cast<double>(k));
        weights += window;
    }
    return 2.0 * std::abs(sum) / weights;
}

double db(double x) {
    return 20.0 * std::log10(x);
}

// The tone is where the formula puts it at `expected_db` against the
// reference, and 60 dB under that in every other channel of the mix;
// `expected_db` unset puts it nowhere.
void check_tone(const Decoded& mix, const Decoded& reference, Speaker from, double hz,
                const std::vector<std::pair<Speaker, double>>& expected) {
    const double own = tone_amplitude(reference, from, hz);
    for (const Speaker speaker : mix.speakers) {
        CAPTURE(hz, ac4::describe(speaker));
        const auto it = std::ranges::find(expected, speaker, &std::pair<Speaker, double>::first);
        const double got = tone_amplitude(mix, speaker, hz);
        if (it != expected.end()) {
            CAPTURE(it->second, db(got / own));
            CHECK(std::abs(db(got / own) - it->second) < kToleranceDb);
        } else {
            CHECK(got < own * 1e-3);
        }
    }
}

// The whole output against Σ M x over the substreams decoded alone, M their
// matrices: what the mix leaves beside its formula, in dB under the output.
double residual_db(const Decoded& mix,
                   const std::vector<std::pair<const Decoded*, std::vector<std::vector<double>>>>& terms) {
    double error = 0.0;
    double energy = 0.0;
    for (std::size_t c = 0; c < mix.channels.size(); ++c) {
        for (std::size_t n = kSkippedSamples; n < mix.channels[c].size(); ++n) {
            double expected = 0.0;
            for (const auto& [decoded, matrix] : terms) {
                for (std::size_t j = 0; j < decoded->channels.size(); ++j) {
                    expected += matrix[c][j] * static_cast<double>(decoded->channels[j][n]);
                }
            }
            const double got = static_cast<double>(mix.channels[c][n]);
            error += (got - expected) * (got - expected);
            energy += expected * expected;
        }
    }
    return 10.0 * std::log10(std::max(error, 1e-300) / energy);
}

double from_db(double value) {
    return std::pow(10.0, value / 20.0);
}

// A matrix of `rows` x `columns` zeros.
std::vector<std::vector<double>> zeros(std::size_t rows, std::size_t columns) {
    return std::vector<std::vector<double>>(rows, std::vector<double>(columns, 0.0));
}

std::vector<std::vector<double>> diagonal(std::size_t n, double gain) {
    std::vector<std::vector<double>> m = zeros(n, n);
    for (std::size_t i = 0; i < n; ++i) {
        m[i][i] = gain;
    }
    return m;
}

// --- The selection table -----------------------------------------------------

struct SelectionCase {
    std::string name;
    std::vector<std::byte> frame;
    ac4::PresentationChoice choice{};
    int level = 3;
    std::optional<std::size_t> expected;
};

using ac4_toc_test::BitWriter;

// A frame of the table of contents `toc` over `substreams` one-byte substreams.
std::vector<std::byte> frame_of(BitWriter& toc, std::size_t substreams) {
    ac4_toc_test::index_table(toc, std::vector<std::size_t>(substreams, 1));
    toc.align();
    return ac4_toc_test::assemble(toc, std::vector<std::vector<std::byte>>(substreams, {std::byte{0}}));
}

// The version 1 broadcast: 5.1 music and effects (group 0), dialogue in
// English, German and British English (1, 2 and 5), audio description (3) and
// a stereo commentary (4). Substreams: 0 to 5 the presentation substreams, 6
// to 11 the groups'.
std::vector<std::byte> broadcast_v1() {
    BitWriter w;
    ac4_toc_test::toc_start(w, {.bitstream_version = 2, .sequence_counter = 1, .fs_index = 1,
                                .frame_rate_index = 13, .b_iframe_global = true, .n_presentations = 6});
    const auto p = [&w](std::optional<int> config, std::vector<int> groups, int substream, int md, int id,
                        bool virtualised = false) {
        ac4_toc_test::PresV1 pres;
        pres.presentation_config = config;
        pres.groups = std::move(groups);
        pres.presentation_substream = substream;
        pres.md_compat = md;
        pres.presentation_id = id;
        pres.b_pre_virtualized = virtualised;
        ac4_toc_test::presentation_v1(w, pres);
    };
    p(0, {0, 1}, 0, 2, 10);          // 0: M&E + English
    p(0, {0, 2}, 1, 2, 11);          // 1: M&E + German
    p(3, {0, 1, 3}, 2, 2, 12);       // 2: M&E + English + audio description
    p(2, {0, 4}, 3, 2, 13);          // 3: main + commentary
    p(0, {0, 5}, 4, 2, 14);          // 4: M&E + British English
    p(std::nullopt, {0}, 5, 1, 15, true);  // 5: pre-virtualized
    ac4_toc_test::chan_group(w, {{.ch_mode = 4, .substream_index = 6}}, 1, true, 0b001);
    ac4_toc_test::chan_group(w, {{.ch_mode = 0, .substream_index = 7}}, 1, true, 0b100, "en");
    ac4_toc_test::chan_group(w, {{.ch_mode = 0, .substream_index = 8}}, 1, true, 0b100, "de");
    ac4_toc_test::chan_group(w, {{.ch_mode = 0, .substream_index = 9}}, 1, true, 0b010, "qad");
    ac4_toc_test::chan_group(w, {{.ch_mode = 1, .substream_index = 10}}, 1, true, 0b101, "en");
    ac4_toc_test::chan_group(w, {{.ch_mode = 0, .substream_index = 11}}, 1, true, 0b100, "en-GB");
    return frame_of(w, 12);
}

// Presentations a decoder of level 3 may not select: disabled, a reserved
// md_compat, a presentation_version it does not decode, md_compat 7; then a
// stereo one it may.
std::vector<std::byte> filters_v1() {
    BitWriter w;
    ac4_toc_test::toc_start(w, {.bitstream_version = 2, .sequence_counter = 1, .fs_index = 1,
                                .frame_rate_index = 13, .b_iframe_global = true, .n_presentations = 5});
    const auto p = [&w](int group, int substream, int md, int version, std::optional<bool> enable) {
        ac4_toc_test::PresV1 pres;
        pres.groups = {group};
        pres.presentation_substream = substream;
        pres.md_compat = md;
        pres.presentation_version = version;
        pres.enable = enable;
        ac4_toc_test::presentation_v1(w, pres);
    };
    p(0, 0, 1, 1, false);        // 0: disabled
    p(0, 1, 4, 1, std::nullopt);  // 1: md_compat 4, reserved for version 1
    p(0, 2, 1, 3, std::nullopt);  // 2: presentation_version 3
    p(0, 3, 7, 1, std::nullopt);  // 3: unrestricted
    p(1, 4, 0, 1, true);          // 4: stereo, enabled
    ac4_toc_test::chan_group(w, {{.ch_mode = 4, .substream_index = 5}});
    ac4_toc_test::chan_group(w, {{.ch_mode = 1, .substream_index = 6}});
    return frame_of(w, 7);
}

// Presentations this decoder cannot decode: 22.2, and 5.1 at 96 kHz (its HSF
// extension); then a stereo one.
std::vector<std::byte> capability_v1() {
    BitWriter w;
    ac4_toc_test::toc_start(w, {.bitstream_version = 2, .sequence_counter = 1, .fs_index = 1,
                                .frame_rate_index = 13, .b_iframe_global = true, .n_presentations = 3});
    for (int i = 0; i < 3; ++i) {
        ac4_toc_test::PresV1 pres;
        pres.groups = {i};
        pres.presentation_substream = i;
        pres.md_compat = 7;
        ac4_toc_test::presentation_v1(w, pres);
    }
    ac4_toc_test::chan_group(w, {{.ch_mode = 15, .substream_index = 3}});
    ac4_toc_test::chan_group(w, {{.ch_mode = 4, .sf_multiplier = 0, .substream_index = 4}});
    ac4_toc_test::chan_group(w, {{.ch_mode = 1, .substream_index = 5}});
    return frame_of(w, 6);
}

// presentation_config 5: roles by content_classifier (Part 2 Table 54).
std::vector<std::byte> by_classifier_v1() {
    BitWriter w;
    ac4_toc_test::toc_start(w, {.bitstream_version = 2, .sequence_counter = 1, .fs_index = 1,
                                .frame_rate_index = 13, .b_iframe_global = true, .n_presentations = 2});
    ac4_toc_test::PresV1 with_ad;
    with_ad.presentation_config = 5;
    with_ad.groups = {0, 1, 2};
    with_ad.presentation_substream = 0;
    with_ad.md_compat = 2;
    ac4_toc_test::presentation_v1(w, with_ad);
    ac4_toc_test::PresV1 without;
    without.presentation_config = 5;
    without.groups = {0, 1};
    without.presentation_substream = 1;
    without.md_compat = 2;
    ac4_toc_test::presentation_v1(w, without);
    ac4_toc_test::chan_group(w, {{.ch_mode = 4, .substream_index = 2}}, 1, true, 0b000);
    ac4_toc_test::chan_group(w, {{.ch_mode = 0, .substream_index = 3}}, 1, true, 0b100, "es");
    ac4_toc_test::chan_group(w, {{.ch_mode = 0, .substream_index = 4}}, 1, true, 0b010);
    return frame_of(w, 5);
}

// Version 0 presentations (bitstream_version 1): M&E with English dialogue,
// main with audio description, a pre-virtualized French stereo one, a
// reserved md_compat, and a German stereo one of level 4.
std::vector<std::byte> broadcast_v0() {
    BitWriter w;
    ac4_toc_test::toc_start(w, {.bitstream_version = 1, .sequence_counter = 1, .fs_index = 1,
                                .frame_rate_index = 13, .b_iframe_global = true, .n_presentations = 5});
    using ac4_toc_test::SubInfoV0;
    const auto p = [&w](std::optional<int> config, int md, int id, std::vector<SubInfoV0> subs,
                        bool virtualised = false) {
        ac4_toc_test::PresV0 pres;
        pres.presentation_config = config;
        pres.md_compat = md;
        pres.presentation_id = id;
        pres.substreams = std::move(subs);
        pres.b_pre_virtualized = virtualised;
        ac4_toc_test::presentation_v0(w, pres);
    };
    p(0, 1, 1, {{.ch_mode = 4, .content_classifier = 0b001, .substream_index = 0},
                {.ch_mode = 0, .content_classifier = 0b100, .language = "en", .substream_index = 1}});
    p(2, 2, 2, {{.ch_mode = 4, .content_classifier = 0b000, .substream_index = 2},
                {.ch_mode = 0, .content_classifier = 0b010, .language = "qad", .substream_index = 3}});
    p(std::nullopt, 0, 3, {{.ch_mode = 1, .content_classifier = 0b000, .language = "fr", .substream_index = 4}}, true);
    p(std::nullopt, 5, 4, {{.ch_mode = 1, .substream_index = 5}});
    p(std::nullopt, 4, 5, {{.ch_mode = 1, .content_classifier = 0b000, .language = "de", .substream_index = 6}});
    return frame_of(w, 7);
}

std::vector<SelectionCase> selection_cases() {
    std::vector<SelectionCase> cases;
    const auto add = [&cases](std::string name, const std::vector<std::byte>& frame, ac4::PresentationChoice choice,
                              int level, std::optional<std::size_t> expected) {
        cases.push_back({std::move(name), frame, std::move(choice), level, expected});
    };
    const std::vector<std::byte> v1 = broadcast_v1();
    const auto language = [](const char* tag) {
        ac4::PresentationChoice c;
        c.language = tag;
        return c;
    };
    const auto wants = [](int classifier, ac4::AssociatedType type = ac4::AssociatedType::kAny) {
        ac4::PresentationChoice c;
        c.associated = classifier;
        c.associated_type = type;
        return c;
    };
    // No preference: the first that carries no associated audio and is not
    // pre-virtualized.
    add("v1 no preference takes the first plain presentation", v1, {}, 3, 0);
    add("v1 a language takes the presentation of its dialogue", v1, language("de"), 3, 1);
    add("v1 a whole tag before its primary subtag", v1, language("en-GB"), 3, 4);
    add("v1 a primary subtag matches a longer tag", v1, language("de-AT"), 3, 1);
    add("v1 a language no presentation has leaves the rest to decide", v1, language("fr"), 3, 0);
    add("v1 audio description", v1, wants(0b010), 3, 2);
    add("v1 audio description refined by Table 92", v1, wants(0b010, ac4::AssociatedType::kAudioDescription), 3, 2);
    add("v1 a refinement no presentation carries", v1, wants(0b010, ac4::AssociatedType::kSpokenSubtitles), 3, 0);
    add("v1 commentary", v1, wants(0b101), 3, 3);
    {
        ac4::PresentationChoice c = language("de");
        c.associated = 0b010;
        add("v1 language before associated audio", v1, c, 3, 1);
    }
    {
        ac4::PresentationChoice c;
        c.headphones = true;
        add("v1 headphones take the pre-virtualized presentation", v1, c, 3, 5);
    }
    {
        ac4::PresentationChoice c;
        c.presentation_id = 13;
        add("v1 a presentation_id", v1, c, 3, 3);
        c.presentation_id = 99;
        add("v1 an absent presentation_id leaves the rest to decide", v1, c, 3, 0);
    }
    {
        ac4::PresentationChoice c;
        c.index = 2;
        add("v1 a position", v1, c, 3, 2);
    }
    add("v1 level 1 leaves out md_compat 2", v1, wants(0b010), 1, 5);
    add("v1 level 0 selects none", v1, {}, 0, std::nullopt);
    const std::vector<std::byte> filters = filters_v1();
    add("v1 disabled reserved and unknown versions are not selected", filters, {}, 3, 4);
    add("v1 level 7 takes md_compat 7", filters, {}, 7, 3);
    {
        ac4::PresentationChoice c;
        c.index = 0;
        add("v1 a disabled presentation asked for by position", filters, c, 3, 4);
    }
    add("v1 presentations this decoder cannot decode", capability_v1(), {}, 7, 2);
    const std::vector<std::byte> classified = by_classifier_v1();
    add("v1 configuration 5 without associated audio by default", classified, language("es"), 3, 1);
    add("v1 no preference passes over a first presentation with associated audio", classified, {}, 3, 1);
    add("v1 configuration 5 audio description by classifier", classified, wants(0b010), 3, 0);
    const std::vector<std::byte> v0 = broadcast_v0();
    add("v0 no preference", v0, {}, 3, 0);
    add("v0 a language", v0, language("fr"), 3, 2);
    add("v0 audio description", v0, wants(0b010), 3, 1);
    {
        ac4::PresentationChoice c;
        c.headphones = true;
        add("v0 headphones", v0, c, 3, 2);
    }
    add("v0 level 3 leaves out md_compat 4", v0, language("de"), 3, 0);
    add("v0 level 4 takes md_compat 4", v0, language("de"), 4, 4);
    {
        ac4::PresentationChoice c;
        c.presentation_id = 2;
        add("v0 a presentation_id", v0, c, 3, 1);
    }
    return cases;
}

std::string hex(std::span<const std::byte> bytes) {
    std::string out;
    for (const std::byte b : bytes) {
        const auto v = std::to_integer<unsigned>(b);
        out.push_back("0123456789abcdef"[v >> 4U]);
        out.push_back("0123456789abcdef"[v & 0xFU]);
    }
    return out;
}

std::string_view type_name(ac4::AssociatedType type) {
    switch (type) {
        case ac4::AssociatedType::kAny:
            return "any";
        case ac4::AssociatedType::kAudioDescription:
            return "audio-description";
        case ac4::AssociatedType::kAudioDescriptionSubtitles:
            return "audio-description-subtitles";
        case ac4::AssociatedType::kSpokenSubtitles:
            return "spoken-subtitles";
        case ac4::AssociatedType::kEmergencyInformation:
            return "emergency-information";
    }
    return "?";
}

// One line per case: name, frame, presentation_id, index, language,
// associated, associated type, headphones, level, expected ('-' for none).
std::string selection_table() {
    std::ostringstream out;
    out << "# ac4-presentation-selection/1: name\tframe\tpresentation_id\tindex\tlanguage\tassociated\t"
           "associated_type\theadphones\tlevel\texpected\n";
    const auto optional = [](const auto& value) {
        return value ? std::to_string(*value) : std::string{"-"};
    };
    for (const SelectionCase& c : selection_cases()) {
        out << c.name << '\t' << hex(c.frame) << '\t' << optional(c.choice.presentation_id) << '\t'
            << optional(c.choice.index) << '\t' << (c.choice.language.empty() ? "-" : c.choice.language) << '\t'
            << optional(c.choice.associated) << '\t' << type_name(c.choice.associated_type) << '\t'
            << (c.choice.headphones ? 1 : 0) << '\t' << c.level << '\t' << optional(c.expected) << '\n';
    }
    return out.str();
}

}  // namespace

TEST_CASE("a table of tables of contents selects as Part 2 clause 4.8.2 requires", "[ac4dec][presentations]") {
    for (const SelectionCase& c : selection_cases()) {
        CAPTURE(c.name);
        const auto parsed = ac4::parse_raw_frame(c.frame);
        REQUIRE(parsed.has_value());
        CHECK(ac4::select_presentation(parsed->toc, c.choice, c.level) == c.expected);
    }
}

TEST_CASE("the committed presentation streams and selection table are the builders'", "[ac4dec][presentations]") {
    const char* write_to = std::getenv("AC4DEC_WRITE_PRESENTATIONS");
    const std::string table = selection_table();
    const std::vector<std::byte> five_one = build(stream_5_1());
    const std::vector<std::byte> hybrid = build(stream_hybrid());
    const std::vector<std::byte> v0 = build(stream_v0());
    if (write_to != nullptr) {
        const fs::path out{write_to};
        write_file(out / "presentations" / "presentations-5_1.ac4", five_one);
        write_file(out / "presentations" / "presentations-hybrid.ac4", hybrid);
        write_file(out / "presentations" / "presentations-v0.ac4", v0);
        std::ofstream(out / "presentations" / "presentation-selection.tsv", std::ios::binary) << table;
        return;
    }
    CHECK(committed("5_1") == five_one);
    CHECK(committed("hybrid") == hybrid);
    CHECK(committed("v0") == v0);
    std::ifstream in(golden() / "presentations" / "presentation-selection.tsv", std::ios::binary);
    std::string on_disk((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // A checkout may give the text file CRLF line ends.
    std::erase(on_disk, '\r');
    CHECK(on_disk == table);
}

TEST_CASE("every presentation of the multiplexed streams reads to its end", "[ac4dec][presentations]") {
    for (const char* name : {"5_1", "hybrid", "v0"}) {
        CAPTURE(name);
        ac4::Decoder decoder;
        const ac4::ScanResult scan = ac4::scan(committed(name));
        REQUIRE(scan.frames.size() == kFrames);
        for (const ac4::SyncFrame& frame : scan.frames) {
            REQUIRE(frame.crc_ok == true);
            const auto report = decoder.parse(frame.raw_ac4_frame);
            REQUIRE(report.has_value());
            for (const ac4::SubstreamReport& s : report->substreams) {
                CAPTURE(s.index, s.refused_reason);
                CHECK_FALSE(s.refused.has_value());
                CHECK(s.bits_read == s.size_bits);
            }
        }
    }
}

TEST_CASE("the pan law meets Table 216 at its three angles", "[ac4dec][presentations]") {
    const std::array five_one = {Speaker::kLeft, Speaker::kRight, Speaker::kCentre,
                                 Speaker::kLfe,  Speaker::kLeftSurround, Speaker::kRightSurround};
    const std::array stereo = {Speaker::kLeft, Speaker::kRight};
    std::array<double, 6> g{};
    ac4::detail::pan_gains(330.0, five_one, g);
    CHECK(g == std::array{1.0, 0.0, 0.0, 0.0, 0.0, 0.0});
    ac4::detail::pan_gains(0.0, five_one, g);
    CHECK(g == std::array{0.0, 0.0, 1.0, 0.0, 0.0, 0.0});
    ac4::detail::pan_gains(30.0, five_one, g);
    CHECK(g == std::array{0.0, 1.0, 0.0, 0.0, 0.0, 0.0});
    std::array<double, 2> s{};
    ac4::detail::pan_gains(0.0, stereo, s);
    CHECK(s == std::array{0.5, 0.5});
    ac4::detail::pan_gains(330.0, stereo, s);
    CHECK(s == std::array{1.0, 0.0});
    // Between the angles, the two channels either side share the signal
    // linearly; round the back of a 5.1 layout, the surrounds at 110 and 250.
    ac4::detail::pan_gains(15.0, five_one, g);
    CHECK(std::abs(g[2] - 0.5) < 1e-12);
    CHECK(std::abs(g[1] - 0.5) < 1e-12);
    ac4::detail::pan_gains(180.0, five_one, g);
    CHECK(std::abs(g[4] - 0.5) < 1e-12);
    CHECK(std::abs(g[5] - 0.5) < 1e-12);
    ac4::detail::pan_gains(70.0, five_one, g);
    CHECK(std::abs(g[1] - 0.5) < 1e-12);
    CHECK(std::abs(g[5] - 0.5) < 1e-12);
    ac4::detail::pan_gains(-30.0, five_one, g);
    CHECK(g[0] == 1.0);
}

TEST_CASE("music and effects with dialogue mixes as Part 1 clause 6.2.16.1 gives", "[ac4dec][presentations]") {
    const std::vector<std::byte>& file = committed("5_1");
    const Decoded me = decode_id(file, 20);
    const Decoded english = decode_id(file, 21);
    const Decoded german = decode_id(file, 22);
    const Decoded french = decode_id(file, 25);
    REQUIRE(english.speakers == std::vector{Speaker::kCentre});
    // Group gains 0 and -2 dB; English at 330 degrees, into L.
    const Decoded mix = decode_id(file, 1);
    REQUIRE(mix.speakers == me.speakers);
    for (std::size_t c = 0; c < me.speakers.size(); ++c) {
        check_tone(mix, me, me.speakers[c], kTones51[c], {{me.speakers[c], 0.0}});
    }
    check_tone(mix, english, Speaker::kCentre, kToneDialogueEn, {{Speaker::kLeft, -2.0}});
    std::vector<std::vector<double>> into = zeros(6, 1);
    into[0][0] = from_db(-2.0);
    CHECK(residual_db(mix, {{&me, diagonal(6, 1.0)}, {&english, into}}) < -100.0);
    // g_dialog: -6 dB; +9 dB is capped at the stream's 6 dB; below -120 dB,
    // silence.
    check_tone(decode_id(file, 1, -6.0), english, Speaker::kCentre, kToneDialogueEn, {{Speaker::kLeft, -8.0}});
    check_tone(decode_id(file, 1, 9.0), english, Speaker::kCentre, kToneDialogueEn, {{Speaker::kLeft, 4.0}});
    check_tone(decode_id(file, 1, -130.0), english, Speaker::kCentre, kToneDialogueEn, {});
    // German: no pan, so 0 degrees, into C; its g_dialog_max is 12 dB.
    check_tone(decode_id(file, 2), german, Speaker::kCentre, kToneDialogueDe, {{Speaker::kCentre, 0.0}});
    check_tone(decode_id(file, 2, 12.0), german, Speaker::kCentre, kToneDialogueDe, {{Speaker::kCentre, 12.0}});
    // French, stereo: its L at 0 degrees, its R at 30; g_dialog_max 3 dB.
    const Decoded fr = decode_id(file, 8, 6.0);
    check_tone(fr, french, Speaker::kLeft, kToneDialogueFr[0], {{Speaker::kCentre, 3.0}});
    check_tone(fr, french, Speaker::kRight, kToneDialogueFr[1], {{Speaker::kRight, 3.0}});
}

TEST_CASE("main with associated audio mixes as Part 1 clause 6.2.16.2 gives", "[ac4dec][presentations]") {
    const std::vector<std::byte>& file = committed("5_1");
    const Decoded main = decode_id(file, 20);
    const Decoded ad = decode_id(file, 23);
    const Decoded commentary = decode_id(file, 24);
    const Decoded stereo_main = decode_id(file, 26);
    // Mono audio description panned to Table 216's three angles, and at 0
    // degrees into a stereo main, 0.5 in each.
    check_tone(decode_id(file, 4), ad, Speaker::kCentre, kToneAd, {{Speaker::kLeft, 0.0}});
    check_tone(decode_id(file, 5), ad, Speaker::kCentre, kToneAd, {{Speaker::kCentre, 0.0}});
    check_tone(decode_id(file, 6), ad, Speaker::kCentre, kToneAd, {{Speaker::kRight, 0.0}});
    const Decoded into_stereo = decode_id(file, 10);
    check_tone(into_stereo, ad, Speaker::kCentre, kToneAd,
               {{Speaker::kLeft, db(0.5)}, {Speaker::kRight, db(0.5)}});
    std::vector<std::vector<double>> halves = zeros(2, 1);
    halves[0][0] = 0.5;
    halves[1][0] = 0.5;
    CHECK(residual_db(into_stereo, {{&stereo_main, diagonal(2, 1.0)}, {&ad, halves}}) < -100.0);
    // g_assoc.
    check_tone(decode_id(file, 4, 0.0, -10.0), ad, Speaker::kCentre, kToneAd, {{Speaker::kLeft, -10.0}});
    // A stereo commentary channel to channel, the main's front channels by
    // scale_main_front, -3 dB.
    const Decoded with_commentary = decode_id(file, 7);
    for (std::size_t c = 0; c < main.speakers.size(); ++c) {
        const bool front = main.speakers[c] == Speaker::kLeft || main.speakers[c] == Speaker::kRight;
        check_tone(with_commentary, main, main.speakers[c], kTones51[c], {{main.speakers[c], front ? -3.0 : 0.0}});
    }
    check_tone(with_commentary, commentary, Speaker::kLeft, kToneCommentary[0], {{Speaker::kLeft, 0.0}});
    check_tone(with_commentary, commentary, Speaker::kRight, kToneCommentary[1], {{Speaker::kRight, 0.0}});
}

TEST_CASE("music and effects, dialogue and associated audio mix as Part 1 clause 6.2.16.3 gives",
          "[ac4dec][presentations]") {
    const std::vector<std::byte>& file = committed("5_1");
    const Decoded me = decode_id(file, 20);
    const Decoded english = decode_id(file, 21);
    const Decoded ad = decode_id(file, 23);
    // scale_main -6 dB on every channel, scale_main_front -1.5 on L and R,
    // scale_main_centre -3 on C; the dialogue scaled as the main audio is, its
    // own channel C, then panned to L; the audio description's group -1 dB,
    // at 30 degrees.
    const Decoded mix = decode_id(file, 3);
    std::vector<double> main_db(6, -6.0);
    main_db[0] = main_db[1] = -7.5;
    main_db[2] = -9.0;
    for (std::size_t c = 0; c < me.speakers.size(); ++c) {
        check_tone(mix, me, me.speakers[c], kTones51[c], {{me.speakers[c], main_db[c]}});
    }
    check_tone(mix, english, Speaker::kCentre, kToneDialogueEn, {{Speaker::kLeft, -9.0}});
    check_tone(mix, ad, Speaker::kCentre, kToneAd, {{Speaker::kRight, -1.0}});
    std::vector<std::vector<double>> main_matrix = zeros(6, 6);
    for (std::size_t c = 0; c < 6; ++c) {
        main_matrix[c][c] = from_db(main_db[c]);
    }
    std::vector<std::vector<double>> dialogue = zeros(6, 1);
    dialogue[0][0] = from_db(-9.0);
    std::vector<std::vector<double>> described = zeros(6, 1);
    described[1][0] = from_db(-1.0);
    CHECK(residual_db(mix, {{&me, main_matrix}, {&english, dialogue}, {&ad, described}}) < -100.0);
    // presentation_config 5 by content classifier: dialogue -0.5 dB into L,
    // audio description -1.5 dB into R, no scaling where none is sent.
    const Decoded classified = decode_id(file, 9);
    check_tone(classified, english, Speaker::kCentre, kToneDialogueEn, {{Speaker::kLeft, -0.5}});
    check_tone(classified, ad, Speaker::kCentre, kToneAd, {{Speaker::kRight, -1.5}});
    check_tone(classified, me, Speaker::kCentre, kTones51[2], {{Speaker::kCentre, 0.0}});
}

TEST_CASE("a hybrid dialogue enhancement method takes its waveform from the dialogue enhancement substream",
          "[ac4dec][presentations]") {
    const std::vector<std::byte>& file = committed("hybrid");
    const Decoded main51 = decode_id(file, 10);
    const Decoded waveform = decode_id(file, 11);
    const Decoded main20 = decode_id(file, 12);
    const Decoded main20_mid = decode_id(file, 13);
    // Part 1 clause 5.7.8.9: g = 10^(G/20) - 1 split by alpha_c.
    constexpr double kGain = 6.0;
    const double g = std::pow(10.0, kGain / 20.0) - 1.0;
    const auto split = [g](int alpha) {
        const double a = static_cast<double>(alpha) / 31.0;
        return std::pair{(1.0 - a) * g, a * g};
    };
    // At 0 dB the substream adds nothing.
    const Decoded off = decode_id(file, 1);
    for (std::size_t c = 0; c < main51.speakers.size(); ++c) {
        check_tone(off, main51, main51.speakers[c], kTones51[c], {{main51.speakers[c], 0.0}});
    }
    check_tone(off, waveform, Speaker::kCentre, kToneDe, {});
    // The channel independent method on C: (1 + g_p p) m + g_s d.
    {
        const auto [gp, gs] = split(kAlphaIndependent);
        const Decoded on = decode_id(file, 1, 0.0, 0.0, kGain);
        check_tone(on, main51, Speaker::kCentre, kTones51[2], {{Speaker::kCentre, db(1.0 + gp * 0.5)}});
        check_tone(on, main51, Speaker::kLeft, kTones51[0], {{Speaker::kLeft, 0.0}});
        check_tone(on, waveform, Speaker::kCentre, kToneDe, {{Speaker::kCentre, db(gs)}});
        std::vector<std::vector<double>> m = diagonal(6, 1.0);
        m[2][2] = 1.0 + gp * 0.5;
        std::vector<std::vector<double>> d = zeros(6, 1);
        d[2][0] = gs;
        CHECK(residual_db(on, {{&main51, m}, {&waveform, d}}) < -100.0);
        // Without the substream, the parameters alone at the whole gain.
        check_tone(decode_id(file, 10, 0.0, 0.0, kGain), main51, Speaker::kCentre, kTones51[2],
                   {{Speaker::kCentre, db(1.0 + g * 0.5)}});
    }
    // With associated audio as well (presentation_config 4): the audio
    // description's group -1 dB at 30 degrees, the waveform as before.
    {
        const auto [gp, gs] = split(kAlphaIndependent);
        const Decoded ad = decode_id(file, 14);
        const Decoded on = decode_id(file, 2, 0.0, 0.0, kGain);
        check_tone(on, main51, Speaker::kCentre, kTones51[2], {{Speaker::kCentre, db(1.0 + gp * 0.5)}});
        check_tone(on, waveform, Speaker::kCentre, kToneDe, {{Speaker::kCentre, db(gs)}});
        check_tone(on, ad, Speaker::kCentre, kToneAd, {{Speaker::kRight, -1.0}});
        std::vector<std::vector<double>> m = diagonal(6, 1.0);
        m[2][2] = 1.0 + gp * 0.5;
        std::vector<std::vector<double>> d = zeros(6, 1);
        d[2][0] = gs;
        std::vector<std::vector<double>> described = zeros(6, 1);
        described[1][0] = from_db(-1.0);
        CHECK(residual_db(on, {{&main51, m}, {&waveform, d}, {&ad, described}}) < -100.0);
    }
    // The cross-channel method on L and R: (I + g_p r p^T) m + r g_s d, r
    // from de_mix_coef1_idx (Table 172's 0.448 and the rest of a unit
    // vector), p 0.5 and 0.3.
    {
        const auto [gp, gs] = split(kAlphaCross);
        const double r0 = 0.448;
        const double r1 = std::sqrt(1.0 - r0 * r0);
        const Decoded on = decode_id(file, 3, 0.0, 0.0, kGain);
        check_tone(on, main20, Speaker::kLeft, kTones51[0],
                   {{Speaker::kLeft, db(1.0 + gp * r0 * 0.5)}, {Speaker::kRight, db(gp * r1 * 0.5)}});
        check_tone(on, main20, Speaker::kRight, kTones51[1],
                   {{Speaker::kLeft, db(gp * r0 * 0.3)}, {Speaker::kRight, db(1.0 + gp * r1 * 0.3)}});
        check_tone(on, waveform, Speaker::kCentre, kToneDe, {{Speaker::kLeft, db(r0 * gs)}, {Speaker::kRight, db(r1 * gs)}});
    }
    // The channel independent method on the Mid of L and R:
    // 1/2 [1 1; 1 -1] diag(1 + g_p p, 1) [1 1; 1 -1] m + 1/2 g_s (1, 1) d.
    {
        const auto [gp, gs] = split(kAlphaMid);
        const double x = gp * 0.5;
        const Decoded on = decode_id(file, 4, 0.0, 0.0, kGain);
        check_tone(on, main20_mid, Speaker::kLeft, kTones51[0],
                   {{Speaker::kLeft, db(1.0 + x / 2.0)}, {Speaker::kRight, db(x / 2.0)}});
        check_tone(on, waveform, Speaker::kCentre, kToneDe,
                   {{Speaker::kLeft, db(gs / 2.0)}, {Speaker::kRight, db(gs / 2.0)}});
    }
}

TEST_CASE("version 0 presentations mix by their substreams' own metadata and dialnorms", "[ac4dec][presentations]") {
    const std::vector<std::byte>& file = committed("v0");
    const Decoded me = decode_id(file, 10);
    const Decoded english = decode_id(file, 11);
    const Decoded ad = decode_id(file, 12);
    const Decoded stereo_main = decode_id(file, 13);
    const Decoded french = decode_id(file, 14);
    // The gain 2^((a - b) / 6) from dialnorm b to dialnorm a, in dB.
    const auto dialnorm_gain_db = [](int a_bits, int b_bits) {
        return db(std::pow(2.0, (-0.25 * static_cast<double>(a_bits) + 0.25 * static_cast<double>(b_bits)) / 6.0));
    };
    // Music and effects with English: the dialogue's fields from its own
    // extended_metadata(), g_dialog_max 6 dB and a pan to 330 degrees.
    const Decoded mix = decode_id(file, 1);
    for (std::size_t c = 0; c < me.speakers.size(); ++c) {
        check_tone(mix, me, me.speakers[c], kTones51[c], {{me.speakers[c], 0.0}});
    }
    check_tone(mix, english, Speaker::kCentre, kToneDialogueEn, {{Speaker::kLeft, 0.0}});
    check_tone(decode_id(file, 1, 9.0), english, Speaker::kCentre, kToneDialogueEn, {{Speaker::kLeft, 6.0}});
    std::vector<std::vector<double>> into_left = zeros(6, 1);
    into_left[0][0] = 1.0;
    CHECK(residual_db(mix, {{&me, diagonal(6, 1.0)}, {&english, into_left}}) < -100.0);
    // Main with audio description: the main audio's scales and the pan from
    // the associated substream's extended_metadata(), and the audio
    // description levelled from its own dialnorm to the main one's (Part 1
    // clause 6.2.16.0; Part 2 clause 4.8.5.2 and Table 16).
    std::vector<double> main_db(6, -6.0);
    main_db[0] = main_db[1] = -7.5;
    main_db[2] = -9.0;
    const Decoded described = decode_id(file, 2);
    for (std::size_t c = 0; c < me.speakers.size(); ++c) {
        check_tone(described, me, me.speakers[c], kTones51[c], {{me.speakers[c], main_db[c]}});
    }
    const double ad_to_main = dialnorm_gain_db(kDialnormMe, kDialnormAd);
    check_tone(described, ad, Speaker::kCentre, kToneAd, {{Speaker::kRight, ad_to_main}});
    check_tone(decode_id(file, 2, 0.0, -10.0), ad, Speaker::kCentre, kToneAd, {{Speaker::kRight, ad_to_main - 10.0}});
    std::vector<std::vector<double>> main_matrix = zeros(6, 6);
    for (std::size_t c = 0; c < 6; ++c) {
        main_matrix[c][c] = from_db(main_db[c]);
    }
    std::vector<std::vector<double>> into_right = zeros(6, 1);
    into_right[1][0] = from_db(ad_to_main);
    CHECK(residual_db(described, {{&me, main_matrix}, {&ad, into_right}}) < -100.0);
    // All three: Table 16 takes the dialogue's dialnorm, so the audio
    // description comes to -25 dBFS; the dialogue is scaled as the main audio
    // is, its own channel C.
    const Decoded all = decode_id(file, 3);
    for (std::size_t c = 0; c < me.speakers.size(); ++c) {
        check_tone(all, me, me.speakers[c], kTones51[c], {{me.speakers[c], main_db[c]}});
    }
    check_tone(all, english, Speaker::kCentre, kToneDialogueEn, {{Speaker::kLeft, -9.0}});
    check_tone(all, ad, Speaker::kCentre, kToneAd, {{Speaker::kRight, dialnorm_gain_db(kDialnormEn, kDialnormAd)}});
    // A 2.0 main at -24 dBFS: the audio description at 30 degrees, into R.
    const Decoded into_stereo = decode_id(file, 4);
    check_tone(into_stereo, stereo_main, Speaker::kLeft, kTones51[0], {{Speaker::kLeft, -7.5}});
    check_tone(into_stereo, ad, Speaker::kCentre, kToneAd,
               {{Speaker::kRight, dialnorm_gain_db(kDialnormStereoMain, kDialnormAd)}});
    // Stereo French: its L at 0 degrees, its R at 30.
    const Decoded fr = decode_id(file, 5);
    check_tone(fr, french, Speaker::kLeft, kToneDialogueFr[0], {{Speaker::kCentre, 0.0}});
    check_tone(fr, french, Speaker::kRight, kToneDialogueFr[1], {{Speaker::kRight, 0.0}});
    // At an output level, each substream alone goes to it from its own
    // dialnorm and a presentation from Table 16's: the music and effects of
    // presentation 1 follow the English dialogue's -25 dBFS, 6 dB under their
    // own level, and the audio description, levelled, plays as it does alone.
    constexpr double kLevel = -31.0;
    const Decoded me_at = decode_id(file, 10, 0.0, 0.0, 0.0, kLevel);
    const Decoded english_at = decode_id(file, 11, 0.0, 0.0, 0.0, kLevel);
    const Decoded ad_at = decode_id(file, 12, 0.0, 0.0, 0.0, kLevel);
    const Decoded mix_at = decode_id(file, 1, 0.0, 0.0, 0.0, kLevel);
    check_tone(mix_at, me_at, Speaker::kCentre, kTones51[2],
               {{Speaker::kCentre, dialnorm_gain_db(kDialnormMe, kDialnormEn)}});
    check_tone(mix_at, english_at, Speaker::kCentre, kToneDialogueEn, {{Speaker::kLeft, 0.0}});
    check_tone(decode_id(file, 2, 0.0, 0.0, 0.0, kLevel), ad_at, Speaker::kCentre, kToneAd, {{Speaker::kRight, 0.0}});
}

TEST_CASE("the decoder selects by its configuration and reports what it decoded", "[ac4dec][presentations]") {
    const std::vector<std::byte>& file = committed("5_1");
    ac4::DecoderConfig config;
    config.presentation.language = "de";
    const Decoded german = decode(file, config);
    CHECK(german.presentation_ids.front() == 2);
    config.presentation = {};
    config.presentation.associated = 0b101;
    CHECK(decode(file, config).presentation_ids.front() == 7);
    // No preference: the first presentation, music and effects with English.
    CHECK(decode(file, ac4::DecoderConfig{}).presentation_ids.front() == 1);
}

TEST_CASE("a mixed presentation conceals a lost frame with all its substreams", "[ac4dec][presentations]") {
    const std::vector<std::byte>& file = committed("5_1");
    ac4::DecoderConfig config;
    config.presentation.presentation_id = 1;
    config.concealment = ac4::ConcealmentPolicy::kRepeatFade;
    ac4::Decoder decoder(config);
    const ac4::ScanResult scan = ac4::scan(file);
    std::size_t concealed = 0;
    // bitstream_version 3 and more: a table of contents that does not read.
    const std::vector<std::byte> unreadable{std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}};
    for (std::size_t f = 0; f < scan.frames.size(); ++f) {
        const auto decoded = decoder.decode(f == 12 ? std::span<const std::byte>(unreadable) : scan.frames[f].raw_ac4_frame);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        CHECK((**decoded).speakers.size() == 6);
        if ((**decoded).concealed) {
            ++concealed;
            CHECK((**decoded).presentation_id == 1);
        }
    }
    CHECK(concealed == 1);
}
