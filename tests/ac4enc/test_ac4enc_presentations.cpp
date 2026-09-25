// The AC-4 encoder's presentations and substreams (planning/ac4.md, phase E6):
// streams of several substreams, each in a substream group of its own, and
// presentations of every presentation_config of Part 2 Table 53 made of them,
// with alternative presentations' names, languages, content classifiers, group
// gains, the associated audio's and the dialogue's mixing values, md_compat,
// EMDF payloads, and the hybrid dialogue enhancement methods' waveform in a
// substream of its own.
//
// Each substream carries tones of its own, so that the decoder's selection and
// mixing (phase D7) on the encoder's stream can be measured tone by tone
// against the formulas of Part 1 clauses 5.7.8.9 and 6.2.16 and Part 2 clauses
// 4.8.3.17 to 4.8.4, to 0.01 dB, each against the substream decoded alone
// through a presentation of its own. The rules a presentation keeps are held
// by refusals. With AC4ENC_WRITE_PRESENTATIONS set to a directory, the
// broadcast and hybrid streams are written there, each with the configuration
// it was made from as JSON, to commit under tests/golden/ac4dec/presentations/,
// where tools/checks/mix_ac4_decode.py holds their mixes to the formulas and
// tools/checks/check_ac4_encode_readers.py holds MediaInfo's reading of them to
// the configuration.

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
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac4/ac4.hpp"
#include "ac4/syntax.hpp"
#include "ac4dec/decoder.hpp"
#include "ac4enc/encoder.hpp"

namespace {

namespace fs = std::filesystem;
using ac4::ContentClassifier;
using ac4::Speaker;

constexpr int kRate = 48000;
constexpr std::size_t kFrameLength = 2048;
constexpr std::size_t kInputFrames = 20;
// The frames before the steady state: the encoder's and the decoder's delays
// and the first frames' overlap.
constexpr std::size_t kSkippedSamples = 5 * kFrameLength;
constexpr double kToleranceDb = 0.01;
constexpr double kAmplitude = 0.1;

// --- Signals, encoding and decoding ------------------------------------------------

// A tone that fades out over its last kFade samples, so that the stream's end
// sends nothing above the tone's band: dialogue enhancement's bands stop at 15
// kHz, and a formula that raises every band alike holds only where nothing is
// above them.
constexpr std::size_t kFade = 1024;

std::vector<float> tone(double hz, double amplitude = kAmplitude) {
    std::vector<float> out(kInputFrames * kFrameLength);
    for (std::size_t n = 0; n < out.size(); ++n) {
        const std::size_t left = out.size() - n;
        const double fade =
            left >= kFade
                ? 1.0
                : 0.5 - 0.5 * std::cos(std::numbers::pi * static_cast<double>(left) / kFade);
        out[n] = static_cast<float>(
            fade * amplitude *
            std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) / kRate));
    }
    return out;
}

struct Encoded {
    std::vector<std::vector<std::byte>> frames;
    std::vector<ac4::SyntaxRecord> trace;
};

// Encodes planar input, with the dialogue in it where a substream takes a
// stem, in pieces of 3000 samples, then flushes.
Encoded encode(const ac4::EncoderConfig& base, const std::vector<std::vector<float>>& input,
               const std::vector<std::vector<float>>* dialogue = nullptr) {
    Encoded out;
    ac4::EncoderConfig config = base;
    const auto sink = [&out](const ac4::SyntaxRecord& r) { out.trace.push_back(r); };
    config.trace = sink;
    auto encoder = ac4::Encoder::create(config);
    REQUIRE(encoder.has_value());
    const std::size_t total = input.front().size();
    constexpr std::size_t kPiece = 3000;
    for (std::size_t at = 0; at < total; at += kPiece) {
        const std::size_t count = std::min(kPiece, total - at);
        std::vector<std::span<const float>> views;
        std::vector<std::span<const float>> stem;
        for (const auto& channel : input) {
            views.emplace_back(std::span<const float>(channel).subspan(at, count));
        }
        if (dialogue != nullptr) {
            for (const auto& channel : *dialogue) {
                stem.emplace_back(std::span<const float>(channel).subspan(at, count));
            }
        }
        auto frames = dialogue != nullptr ? encoder->encode(views, stem) : encoder->encode(views);
        REQUIRE(frames.has_value());
        for (ac4::EncodedFrame& frame : *frames) {
            out.frames.push_back(std::move(frame.raw_ac4_frame));
        }
    }
    auto rest = encoder->flush();
    REQUIRE(rest.has_value());
    for (ac4::EncodedFrame& frame : *rest) {
        out.frames.push_back(std::move(frame.raw_ac4_frame));
    }
    return out;
}

// Every substream of every frame reads to its end, and the decoder's trace is
// the encoder's, record for record.
void read_back(const Encoded& encoded) {
    std::vector<ac4::SyntaxRecord> read;
    const auto sink = [&read](const ac4::SyntaxRecord& r) { read.push_back(r); };
    ac4::DecoderConfig config;
    config.syntax = sink;
    ac4::Decoder decoder(config);
    for (const std::vector<std::byte>& frame : encoded.frames) {
        const auto report = decoder.parse(frame);
        REQUIRE(report.has_value());
        for (const ac4::SubstreamReport& substream : report->substreams) {
            CAPTURE(substream.index, substream.refused_reason);
            REQUIRE_FALSE(substream.refused.has_value());
            CHECK(substream.bits_read == substream.size_bits);
        }
    }
    REQUIRE(read.size() == encoded.trace.size());
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < read.size(); ++i) {
        const bool same = read[i].substream == encoded.trace[i].substream &&
                          read[i].bit_offset == encoded.trace[i].bit_offset &&
                          read[i].bits == encoded.trace[i].bits &&
                          read[i].value == encoded.trace[i].value;
        if (!same && mismatches++ < 5) {
            CAPTURE(i, encoded.trace[i].name, read[i].name, encoded.trace[i].value, read[i].value);
            CHECK(same);
        }
    }
    CHECK(mismatches == 0);
}

struct Decoded {
    std::vector<Speaker> speakers;
    std::vector<std::vector<float>> channels;
};

// Presentation `id` decoded, with the listener's gains, and at an output
// level where one is given (the output level gain alone, no compression).
Decoded decode_id(const Encoded& encoded, int id, double dialogue_gain_db = 0.0,
                  double associated_gain_db = 0.0, double dialogue_enhancement_db = 0.0) {
    ac4::DecoderConfig config;
    config.presentation.presentation_id = id;
    config.output.dialogue_gain_db = dialogue_gain_db;
    config.output.associated_gain_db = associated_gain_db;
    config.output.dialogue_enhancement_db = dialogue_enhancement_db;
    config.output.drc = ac4::DrcMode::kOff;
    ac4::Decoder decoder(config);
    Decoded out;
    for (const std::vector<std::byte>& frame : encoded.frames) {
        const auto decoded = decoder.decode(frame);
        INFO(decoder.refusal_reason());
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        const ac4::DecodedFrame& pcm = **decoded;
        REQUIRE(pcm.presentation_id == id);
        if (out.channels.empty()) {
            out.speakers = pcm.speakers;
            out.channels.resize(pcm.channels.size());
        }
        REQUIRE(pcm.speakers == out.speakers);
        for (std::size_t c = 0; c < pcm.channels.size(); ++c) {
            out.channels[c].insert(out.channels[c].end(), pcm.channels[c].begin(),
                                   pcm.channels[c].end());
        }
    }
    return out;
}

// The complex amplitude of the component at `hz` of `speaker`'s channel,
// through a Hann window, past the first frames.
std::complex<double> component(const Decoded& d, Speaker speaker, double hz) {
    const auto it = std::ranges::find(d.speakers, speaker);
    REQUIRE(it != d.speakers.end());
    const std::vector<float>& channel =
        d.channels[static_cast<std::size_t>(it - d.speakers.begin())];
    REQUIRE(channel.size() > kSkippedSamples + 4096);
    const std::span<const float> samples = std::span<const float>(channel).subspan(kSkippedSamples);
    const double w = 2.0 * std::numbers::pi * hz / kRate;
    const auto n = static_cast<double>(samples.size());
    std::complex<double> sum{};
    double weights = 0.0;
    for (std::size_t k = 0; k < samples.size(); ++k) {
        const double window =
            0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(k) / (n - 1.0));
        sum +=
            window * static_cast<double>(samples[k]) * std::polar(1.0, -w * static_cast<double>(k));
        weights += window;
    }
    return 2.0 * sum / weights;
}

double amplitude(const Decoded& d, Speaker speaker, double hz) {
    return std::abs(component(d, speaker, hz));
}

double db(double x) {
    return 20.0 * std::log10(x);
}

// The tone at `hz` of `reference`'s `from` channel is where the formula puts
// it in `mix`, at each `expected` dB against its level in the reference, and
// 60 dB under that in every other channel.
void check_tone(const Decoded& mix, const Decoded& reference, Speaker from, double hz,
                const std::vector<std::pair<Speaker, double>>& expected) {
    const double own = amplitude(reference, from, hz);
    REQUIRE(own > 1e-3);
    for (const Speaker speaker : mix.speakers) {
        CAPTURE(hz, ac4::describe(speaker));
        const auto it = std::ranges::find(expected, speaker, &std::pair<Speaker, double>::first);
        const double got = amplitude(mix, speaker, hz);
        if (it != expected.end()) {
            CAPTURE(it->second, db(got / own));
            CHECK(std::abs(db(got / own) - it->second) < kToleranceDb);
        } else {
            CHECK(got < own * 1e-3);
        }
    }
}

// --- The broadcast stream ------------------------------------------------------------

// Each substream's tones: a 5.1 music and effects substream (L R C LFE Ls Rs),
// mono dialogue in English and German, mono audio description, a stereo
// commentary and stereo French dialogue.
constexpr std::array<double, 6> kTones51 = {331.0, 457.0, 613.0, 47.0, 787.0, 953.0};
constexpr double kToneEnglish = 1117.0;
constexpr double kToneGerman = 1373.0;
constexpr double kToneDescription = 1531.0;
constexpr std::array<double, 2> kToneCommentary = {1699.0, 1847.0};
constexpr std::array<double, 2> kToneFrench = {1931.0, 2083.0};

struct Stream {
    ac4::EncoderConfig config;
    std::vector<std::vector<float>> input;
    std::vector<std::vector<float>> dialogue;  // empty without a stem
};

ac4::SubstreamConfig substream(int channels, int kbps, std::optional<ContentClassifier> content,
                               std::string language = {}) {
    ac4::SubstreamConfig s;
    s.channels = channels;
    s.bitrate_kbps = kbps;
    s.content = content;
    s.language = std::move(language);
    return s;
}

ac4::PresentationConfig presentation(std::optional<int> config, std::vector<int> substreams,
                                     int id) {
    ac4::PresentationConfig p;
    p.config = config;
    p.substreams = std::move(substreams);
    p.presentation_id = id;
    return p;
}

ac4::EmdfPayload payload(int id, std::vector<std::uint8_t> bytes) {
    ac4::EmdfPayload p;
    p.id = id;
    p.bytes = std::move(bytes);
    return p;
}

// Presentation ids: 1 to 7 the mixes, 8 an alternative one, 20 to 25 each
// substream alone, 30 one disabled and pre-virtualized. EMDF payloads in an
// EMDF payloads substream of presentation 1's and in the English dialogue's
// metadata().
Stream broadcast() {
    Stream s;
    ac4::EncoderConfig& c = s.config;
    c.bitrate_kbps = 640;
    c.dialnorm_db = -27.0;
    ac4::SubstreamConfig me = substream(6, 256, ContentClassifier::kMusicAndEffects);
    ac4::SubstreamConfig english = substream(1, 64, ContentClassifier::kDialogue, "en");
    english.dialogue_mix = ac4::DialogueMix{.max_gain_db = 6, .pan_degrees = {330.0}};
    english.emdf = {payload(3, {9, 8, 7, 6})};
    english.emdf.front().discard_unknown = false;
    english.emdf.front().frame_aligned = true;
    english.emdf.front().priority = 5;
    ac4::SubstreamConfig german = substream(1, 64, ContentClassifier::kDialogue, "de");
    german.dialogue_mix = ac4::DialogueMix{.max_gain_db = 12, .pan_degrees = {}};
    ac4::SubstreamConfig described = substream(1, 48, ContentClassifier::kVisuallyImpaired, "qad");
    ac4::SubstreamConfig commentary = substream(2, 80, ContentClassifier::kCommentary, "en");
    ac4::SubstreamConfig french = substream(2, 80, ContentClassifier::kDialogue, "fr");
    french.dialogue_mix = ac4::DialogueMix{.max_gain_db = 3, .pan_degrees = {0.0, 30.0}};
    c.substreams = {me, english, german, described, commentary, french};

    ac4::PresentationConfig p1 = presentation(0, {0, 1}, 1);
    p1.gains_db = {0.0, -2.0};
    p1.emdf = {payload(2, {1, 2, 3})};
    p1.loudness = ac4::FurtherLoudness{};
    p1.loudness->practice = ac4::LoudnessPractice::kEbuR128;
    p1.loudness->integrated_lkfs = -23.0;
    ac4::PresentationConfig p2 = presentation(0, {0, 2}, 2);
    p2.dialnorm_db = -24.0;
    ac4::PresentationConfig p3 = presentation(3, {0, 1, 3}, 3);
    p3.gains_db = {0.0, 0.0, -1.0};
    p3.associated = ac4::AssociatedMix{
        .main_db = -6.0, .main_centre_db = -3.0, .main_front_db = -1.5, .pan_degrees = 30.0};
    ac4::PresentationConfig p4 = presentation(2, {0, 3}, 4);
    p4.associated = ac4::AssociatedMix{.main_db = std::nullopt,
                                       .main_centre_db = std::nullopt,
                                       .main_front_db = std::nullopt,
                                       .pan_degrees = 330.0};
    ac4::PresentationConfig p5 = presentation(2, {0, 4}, 5);
    p5.associated = ac4::AssociatedMix{.main_db = std::nullopt,
                                       .main_centre_db = std::nullopt,
                                       .main_front_db = -3.0,
                                       .pan_degrees = std::nullopt};
    ac4::PresentationConfig p6 = presentation(0, {0, 5}, 6);
    ac4::PresentationConfig p7 = presentation(5, {0, 1, 3}, 7);
    p7.gains_db = {0.0, -0.5, -1.5};
    p7.associated = ac4::AssociatedMix{.main_db = std::nullopt,
                                       .main_centre_db = std::nullopt,
                                       .main_front_db = std::nullopt,
                                       .pan_degrees = 30.0};
    ac4::PresentationConfig p8 = presentation(0, {0, 2}, 8);
    p8.name = "Deutsch";
    ac4::PresentationConfig hidden = presentation(std::nullopt, {0}, 30);
    hidden.enabled = false;
    hidden.pre_virtualized = true;
    c.presentations = {p1, p2, p3, p4, p5, p6, p7, p8};
    for (int i = 0; i < 6; ++i) {
        c.presentations.push_back(presentation(std::nullopt, {i}, 20 + i));
    }
    c.presentations.push_back(hidden);

    for (const double hz : kTones51) {
        s.input.push_back(tone(hz));
    }
    s.input.push_back(tone(kToneEnglish));
    s.input.push_back(tone(kToneGerman));
    s.input.push_back(tone(kToneDescription));
    s.input.push_back(tone(kToneCommentary[0]));
    s.input.push_back(tone(kToneCommentary[1]));
    s.input.push_back(tone(kToneFrench[0]));
    s.input.push_back(tone(kToneFrench[1]));
    return s;
}

// --- The hybrid stream ---------------------------------------------------------------

// Main substreams whose dialogue enhancement is a hybrid method, and the
// dialogue enhancement substreams their waveforms go in: a 5.1 main whose C
// carries dialogue alone, by the channel-independent method; a stereo main
// whose L and R carry the same dialogue, by the Mid; and with audio
// description, main with dialogue enhancement and associated audio. Ids 10
// and up are each substream alone.
constexpr double kToneDialogue = 1201.0;
constexpr int kShareIndependent = 16;  // de_signal_contribution, the waveform's share x 31
constexpr int kShareMid = 8;

Stream hybrid() {
    Stream s;
    ac4::EncoderConfig& c = s.config;
    c.bitrate_kbps = 512;
    ac4::SubstreamConfig main51 = substream(6, 256, ContentClassifier::kCompleteMain);
    main51.dialogue = ac4::DialogueConfig{};
    main51.dialogue->method = ac4::DialogueMethod::kChannelIndependent;
    main51.dialogue->centre = true;
    main51.dialogue->max_gain_db = 12;
    main51.dialogue->hybrid = true;
    main51.dialogue->waveform_share = kShareIndependent / 31.0;
    ac4::SubstreamConfig waveform51;
    waveform51.enhances = 0;
    waveform51.bitrate_kbps = 64;
    ac4::SubstreamConfig main20 = substream(2, 96, ContentClassifier::kCompleteMain);
    main20.dialogue = ac4::DialogueConfig{};
    main20.dialogue->method = ac4::DialogueMethod::kMid;
    main20.dialogue->left = true;
    main20.dialogue->right = true;
    main20.dialogue->centre = false;
    main20.dialogue->max_gain_db = 12;
    main20.dialogue->hybrid = true;
    main20.dialogue->waveform_share = kShareMid / 31.0;
    ac4::SubstreamConfig waveform20;
    waveform20.enhances = 2;
    waveform20.bitrate_kbps = 48;
    ac4::SubstreamConfig described = substream(1, 48, ContentClassifier::kVisuallyImpaired, "qad");
    c.substreams = {main51, waveform51, main20, waveform20, described};
    ac4::PresentationConfig p1 = presentation(1, {0, 1}, 1);
    ac4::PresentationConfig p2 = presentation(4, {0, 1, 4}, 2);
    p2.gains_db = {0.0, 0.0, -1.0};
    p2.associated = ac4::AssociatedMix{.main_db = std::nullopt,
                                       .main_centre_db = std::nullopt,
                                       .main_front_db = std::nullopt,
                                       .pan_degrees = 30.0};
    ac4::PresentationConfig p3 = presentation(1, {2, 3}, 3);
    c.presentations = {p1, p2, p3};
    for (int i = 0; i < 5; ++i) {
        c.presentations.push_back(presentation(std::nullopt, {i}, 10 + i));
    }
    // The 5.1 main: tones in L R LFE Ls Rs, the dialogue alone in C. The
    // stereo main: the dialogue in L and R alike.
    for (std::size_t ch = 0; ch < 6; ++ch) {
        s.input.push_back(ch == 2 ? tone(kToneDialogue) : tone(kTones51[ch]));
    }
    s.input.push_back(tone(kToneDialogue, kAmplitude / 2.0));
    s.input.push_back(tone(kToneDialogue, kAmplitude / 2.0));
    s.input.push_back(tone(kToneDescription));
    return s;
}

// Part 1 clause 5.7.8.9: g = 10^(G/20) - 1, split by the waveform's share.
std::pair<double, double> split_gain(double gain_db, int share) {
    const double g = std::pow(10.0, gain_db / 20.0) - 1.0;
    const double a = static_cast<double>(share) / 31.0;
    return {(1.0 - a) * g, a * g};
}

// A stereo main, and an EMDF-only presentation (presentation_config 6), in
// a stream of its own: MediaInfo reads no audio substream of a stream with
// one.
Stream emdf() {
    Stream s;
    s.config.bitrate_kbps = 128;
    s.config.substreams = {substream(2, 96, ContentClassifier::kCompleteMain)};
    ac4::PresentationConfig only;
    only.config = 6;
    only.emdf = {payload(1, {0xE6, 0x06}), payload(40, {})};
    s.config.presentations = {presentation(std::nullopt, {0}, 1), only};
    s.input = {tone(kTones51[0]), tone(kTones51[1])};
    return s;
}

const Encoded& encoded_emdf() {
    static const Encoded encoded = [] {
        const Stream s = emdf();
        return encode(s.config, s.input);
    }();
    return encoded;
}

const Encoded& encoded_broadcast() {
    static const Encoded encoded = [] {
        const Stream s = broadcast();
        return encode(s.config, s.input);
    }();
    return encoded;
}

const Encoded& encoded_hybrid() {
    static const Encoded encoded = [] {
        const Stream s = hybrid();
        return encode(s.config, s.input);
    }();
    return encoded;
}

// --- The configurations as JSON, for the readers outside the project ----------------

std::string json_string(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out + "\"";
}

// What tools/checks/check_ac4_encode_readers.py holds MediaInfo's reading to:
// each presentation's configuration, id, level, name and dialnorm, and each
// substream's channels, classifier and language.
std::string configuration_json(const ac4::EncoderConfig& config, const ac4::Toc& toc) {
    std::ostringstream out;
    out << "{\n  \"substreams\": [\n";
    for (std::size_t i = 0; i < toc.substream_groups.size(); ++i) {
        const ac4::SubstreamGroupInfo& g = toc.substream_groups[i];
        const ac4::SubstreamConfig& s = config.substreams[i];
        out << "    {\"ch_mode\": " << g.substreams.front().chan->ch_mode.value_or(-1)
            << ", \"content_classifier\": "
            << (s.content ? std::to_string(static_cast<int>(*s.content)) : std::string{"null"})
            << ", \"language\": " << json_string(s.language) << ", \"enhances\": "
            << (s.enhances ? std::to_string(*s.enhances) : std::string{"null"}) << "}"
            << (i + 1 < toc.substream_groups.size() ? ",\n" : "\n");
    }
    out << "  ],\n  \"presentations\": [\n";
    for (std::size_t i = 0; i < config.presentations.size(); ++i) {
        const ac4::PresentationConfig& p = config.presentations[i];
        const ac4::PresentationInfoV1& info = toc.presentations_v1[i];
        out << "    {\"presentation_config\": "
            << (p.config ? std::to_string(*p.config) : std::string{"null"})
            << ", \"substreams\": [";
        for (std::size_t k = 0; k < p.substreams.size(); ++k) {
            out << (k > 0 ? ", " : "") << p.substreams[k];
        }
        out << "], \"presentation_id\": "
            << (info.presentation_id ? std::to_string(*info.presentation_id) : std::string{"null"})
            << ", \"md_compat\": "
            << (info.md_compat ? std::to_string(*info.md_compat) : std::string{"null"})
            << ", \"name\": " << json_string(p.name)
            << ", \"dialnorm_db\": " << p.dialnorm_db.value_or(config.dialnorm_db)
            << ", \"enabled\": " << (p.enabled.value_or(true) ? "true" : "false") << "}"
            << (i + 1 < config.presentations.size() ? ",\n" : "\n");
    }
    out << "  ]\n}\n";
    return out.str();
}

std::vector<std::byte> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    std::ranges::transform(raw, bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    return bytes;
}

void write_file(const fs::path& path, std::span<const std::byte> bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    REQUIRE(out.good());
}

std::vector<std::byte> sync_framed(const Encoded& encoded) {
    std::vector<std::byte> out;
    for (const std::vector<std::byte>& frame : encoded.frames) {
        const std::vector<std::byte> framed = ac4::sync_frame(frame, true);
        out.insert(out.end(), framed.begin(), framed.end());
    }
    return out;
}

fs::path committed(std::string_view name) {
    return fs::path{AC4DEC_GOLDEN_DIR} / "presentations" / (std::string{name} + ".ac4");
}

ac4::Toc toc_of(std::span<const std::byte> frame) {
    const auto parsed = ac4::parse_raw_frame(frame);
    REQUIRE(parsed.has_value());
    return parsed->toc;
}

// The refusal of a configuration that breaks a rule, where the same
// configuration keeping it encodes: create() alone decides both.
bool accepted(const ac4::EncoderConfig& config) {
    return ac4::Encoder::create(config).has_value();
}

}  // namespace

TEST_CASE("the default stream has a presentation_id and the level its layout needs",
          "[ac4enc][presentations]") {
    for (const auto& [channels, level] :
         {std::pair{1, 0}, std::pair{2, 0}, std::pair{5, 1}, std::pair{6, 1}}) {
        CAPTURE(channels);
        ac4::EncoderConfig config;
        config.channels = channels;
        config.bitrate_kbps = 64 * channels;
        auto encoder = ac4::Encoder::create(config);
        REQUIRE(encoder.has_value());
        const ac4::Toc& toc = encoder->toc();
        REQUIRE(toc.presentations_v1.size() == 1);
        // Part 2 Table 55: 2 tracks at md_compat 0, 6 at 1, the LFE not
        // counted; and a presentation_id, which CMAF asks every presentation
        // to carry (Annex H.1.2.1).
        CHECK(toc.presentations_v1[0].md_compat == level);
        CHECK(toc.presentations_v1[0].presentation_id == 0);
    }
    ac4::EncoderConfig seven;
    seven.channels = 8;
    seven.bitrate_kbps = 512;
    seven.experimental.seven_x = ac4::AdditionalPair::kBack;
    auto encoder = ac4::Encoder::create(seven);
    REQUIRE(encoder.has_value());
    CHECK(encoder->toc().presentations_v1[0].md_compat == 2);
}

TEST_CASE("the table of contents holds the presentations and substreams as configured",
          "[ac4enc][presentations]") {
    const Stream s = broadcast();
    const Encoded& encoded = encoded_broadcast();
    REQUIRE_FALSE(encoded.frames.empty());
    const ac4::Toc toc = toc_of(encoded.frames.front());
    CHECK(toc.bitstream_version == 2);
    REQUIRE(toc.presentations_v1.size() == s.config.presentations.size());
    REQUIRE(toc.substream_groups.size() == s.config.substreams.size());
    for (std::size_t i = 0; i < s.config.presentations.size(); ++i) {
        CAPTURE(i);
        const ac4::PresentationConfig& p = s.config.presentations[i];
        const ac4::PresentationInfoV1& got = toc.presentations_v1[i];
        CHECK(got.presentation_version == 1);
        CHECK(got.presentation_config == p.config);
        if (p.config == 6) {
            // EMDF payloads alone, in a substream of their own.
            CHECK(got.group_refs.empty());
            CHECK_FALSE(got.presentation_id.has_value());
            CHECK_FALSE(got.presentation_substream_index.has_value());
            CHECK(got.emdf_payloads_substream_indices.size() == 1);
            continue;
        }
        CHECK(got.presentation_id == p.presentation_id);
        CHECK(got.group_refs == p.substreams);
        CHECK(got.b_alternative == !p.name.empty());
        CHECK(got.b_pre_virtualized == p.pre_virtualized);
        CHECK(got.enable_presentation == p.enabled);
        CHECK(got.emdf_payloads_substream_indices.size() == (p.emdf.empty() ? 0U : 1U));
    }
    // Part 2 Table 55: music and effects in 5.1 with mono dialogue is six
    // tracks, level 1, and with audio description seven, level 2; each
    // substream alone its own level.
    CHECK(toc.presentations_v1[0].md_compat == 1);
    CHECK(toc.presentations_v1[2].md_compat == 2);
    CHECK(toc.presentations_v1[5].md_compat == 2);  // with stereo French: seven
    CHECK(toc.presentations_v1[8].md_compat == 1);  // the 5.1 alone
    CHECK(toc.presentations_v1[9].md_compat == 0);
    CHECK(toc.presentations_v1[12].md_compat == 0);  // the stereo commentary alone
    for (std::size_t i = 0; i < s.config.substreams.size(); ++i) {
        CAPTURE(i);
        const ac4::SubstreamConfig& sub = s.config.substreams[i];
        const ac4::SubstreamGroupInfo& g = toc.substream_groups[i];
        REQUIRE(g.substreams.size() == 1);
        REQUIRE(g.content_type.has_value());
        CHECK(g.content_type->content_classifier == static_cast<int>(*sub.content));
        std::string language;
        for (const std::byte b : g.content_type->language_tag.value_or(std::vector<std::byte>{})) {
            language.push_back(static_cast<char>(b));
        }
        CHECK(language == sub.language);
    }
    // The same configuration in every frame (Part 2 Annex H.1.2.4).
    for (const std::vector<std::byte>& frame : encoded.frames) {
        const ac4::Toc other = toc_of(frame);
        CHECK(other.presentations_v1.size() == toc.presentations_v1.size());
        CHECK(other.substream_groups.size() == toc.substream_groups.size());
        for (std::size_t i = 0; i < other.presentations_v1.size(); ++i) {
            CHECK(other.presentations_v1[i].presentation_id ==
                  toc.presentations_v1[i].presentation_id);
            CHECK(other.presentations_v1[i].presentation_config ==
                  toc.presentations_v1[i].presentation_config);
        }
    }
}

TEST_CASE("the encoder's presentations read back with its own trace", "[ac4enc][presentations]") {
    read_back(encoded_broadcast());
    read_back(encoded_hybrid());
    read_back(encoded_emdf());
}

TEST_CASE("names and EMDF payloads read back as configured", "[ac4enc][presentations]") {
    const Encoded& encoded = encoded_broadcast();
    // The records of the first frame, an I-frame, by substream.
    std::vector<ac4::SyntaxRecord> read;
    const auto sink = [&read](const ac4::SyntaxRecord& r) { read.push_back(r); };
    ac4::DecoderConfig config;
    config.syntax = sink;
    ac4::Decoder decoder(config);
    REQUIRE(decoder.parse(encoded.frames.front()).has_value());
    const ac4::Toc toc = toc_of(encoded.frames.front());
    const auto values = [&read](int substream, std::string_view name) {
        std::vector<std::uint64_t> out;
        for (const ac4::SyntaxRecord& r : read) {
            if (r.substream == substream && r.name == name) {
                out.push_back(r.value);
            }
        }
        return out;
    };
    // Presentation 8's name, whole, with the 0 that says so (Part 2 clause
    // 6.3.3.1.4).
    const int named = *toc.presentations_v1[7].presentation_substream_index;
    const std::vector<std::uint64_t> name = values(named, "presentation_name");
    const std::string_view deutsch = "Deutsch";
    REQUIRE(name.size() == deutsch.size() + 1);
    for (std::size_t i = 0; i < deutsch.size(); ++i) {
        CHECK(name[i] == static_cast<std::uint64_t>(deutsch[i]));
    }
    CHECK(name.back() == 0);
    // The payloads: presentation 1's in the EMDF payloads substream its
    // emdf_info() names, and the English dialogue's in its metadata(), each
    // list closed by an id of 0.
    const int first = toc.presentations_v1[0].emdf_payloads_substream_indices.at(0);
    CHECK(values(first, "emdf_payload_id") == std::vector<std::uint64_t>{2, 0});
    CHECK(values(first, "emdf_payload_byte") == std::vector<std::uint64_t>{1, 2, 3});
    const int english = *toc.substream_groups[1].substreams[0].chan->substream_index;
    CHECK(values(english, "emdf_payload_id") == std::vector<std::uint64_t>{3, 0});
    CHECK(values(english, "emdf_payload_byte") == std::vector<std::uint64_t>{9, 8, 7, 6});
    CHECK(values(english, "priority") == std::vector<std::uint64_t>{5});
    // The EMDF-only presentation: no substream groups, presentation_id or
    // presentation substream, and its payloads in a substream of their own.
    read.clear();
    ac4::Decoder emdf_reader(config);
    REQUIRE(emdf_reader.parse(encoded_emdf().frames.front()).has_value());
    const ac4::Toc emdf_toc = toc_of(encoded_emdf().frames.front());
    REQUIRE(emdf_toc.presentations_v1.size() == 2);
    const ac4::PresentationInfoV1& only = emdf_toc.presentations_v1[1];
    CHECK(only.presentation_config == 6);
    CHECK(only.group_refs.empty());
    CHECK_FALSE(only.presentation_id.has_value());
    CHECK_FALSE(only.presentation_substream_index.has_value());
    REQUIRE(only.emdf_payloads_substream_indices.size() == 1);
    const int payloads = only.emdf_payloads_substream_indices.front();
    // Id 40 escapes: 31, then variable_bits(5) of 9.
    CHECK(values(payloads, "emdf_payload_id") == std::vector<std::uint64_t>{1, 31, 9, 0});
    CHECK(values(payloads, "emdf_payload_byte") == std::vector<std::uint64_t>{0xE6, 0x06});
    // A decoder plays the stream's one presentation of audio.
    CHECK(ac4::select_presentation(emdf_toc, {}, 3) == 0);
}

TEST_CASE("the decoder selects the encoder's presentations as configured",
          "[ac4enc][presentations]") {
    const ac4::Toc toc = toc_of(encoded_broadcast().frames.front());
    const auto selected_id = [&toc](const ac4::PresentationChoice& choice,
                                    int level = 3) -> std::optional<int> {
        const std::optional<std::size_t> index = ac4::select_presentation(toc, choice, level);
        if (!index) {
            return std::nullopt;
        }
        return toc.presentations_v1[*index].presentation_id;
    };
    CHECK(selected_id({}) == 1);  // the first, with no associated audio
    ac4::PresentationChoice german;
    german.language = "de";
    CHECK(selected_id(german) == 2);
    ac4::PresentationChoice french;
    french.language = "fr";
    CHECK(selected_id(french) == 6);
    ac4::PresentationChoice description;
    description.associated = static_cast<int>(ContentClassifier::kVisuallyImpaired);
    CHECK(selected_id(description) == 3);
    ac4::PresentationChoice commentary;
    commentary.associated = static_cast<int>(ContentClassifier::kCommentary);
    CHECK(selected_id(commentary) == 5);
    ac4::PresentationChoice by_id;
    by_id.presentation_id = 8;
    CHECK(selected_id(by_id) == 8);
    // Disabled: never selected, not even by its id.
    by_id.presentation_id = 30;
    CHECK(selected_id(by_id) == 1);
    // Level 1 leaves out the mixes of seven tracks, and level 0 every one
    // with 5.1 in it: the English dialogue alone is the first left.
    CHECK(selected_id(description, 1) == 4);
    CHECK(selected_id({}, 0) == 21);
}

TEST_CASE("music and effects with dialogue mix as configured", "[ac4enc][presentations]") {
    const Encoded& e = encoded_broadcast();
    const Decoded me = decode_id(e, 20);
    const Decoded english = decode_id(e, 21);
    const Decoded german = decode_id(e, 22);
    const Decoded french = decode_id(e, 25);
    // Group gains 0 and -2 dB; English at 330 degrees, into L.
    const Decoded mix = decode_id(e, 1);
    REQUIRE(mix.speakers == me.speakers);
    for (std::size_t c = 0; c < me.speakers.size(); ++c) {
        check_tone(mix, me, me.speakers[c], kTones51[c], {{me.speakers[c], 0.0}});
    }
    check_tone(mix, english, Speaker::kCentre, kToneEnglish, {{Speaker::kLeft, -2.0}});
    // g_dialog: -6 dB, and +9 dB capped at the configured 6.
    check_tone(decode_id(e, 1, -6.0), english, Speaker::kCentre, kToneEnglish,
               {{Speaker::kLeft, -8.0}});
    check_tone(decode_id(e, 1, 9.0), english, Speaker::kCentre, kToneEnglish,
               {{Speaker::kLeft, 4.0}});
    // German: no pan, so C; its cap 12 dB. The alternative presentation 8 is
    // the same mix.
    check_tone(decode_id(e, 2, 12.0), german, Speaker::kCentre, kToneGerman,
               {{Speaker::kCentre, 12.0}});
    check_tone(decode_id(e, 8), german, Speaker::kCentre, kToneGerman, {{Speaker::kCentre, 0.0}});
    // French, stereo: its L at 0 degrees, its R at 30; capped at 3 dB.
    const Decoded fr = decode_id(e, 6, 6.0);
    check_tone(fr, french, Speaker::kLeft, kToneFrench[0], {{Speaker::kCentre, 3.0}});
    check_tone(fr, french, Speaker::kRight, kToneFrench[1], {{Speaker::kRight, 3.0}});
}

TEST_CASE("associated audio mixes as configured", "[ac4enc][presentations]") {
    const Encoded& e = encoded_broadcast();
    const Decoded main = decode_id(e, 20);
    const Decoded english = decode_id(e, 21);
    const Decoded described = decode_id(e, 23);
    const Decoded commentary = decode_id(e, 24);
    // Configuration 3: the main audio's scales, the dialogue scaled as the
    // main audio is, the audio description's group at -1 dB and at 30
    // degrees.
    const Decoded all = decode_id(e, 3);
    std::array<double, 6> main_db{-7.5, -7.5, -9.0, -6.0, -6.0, -6.0};
    for (std::size_t c = 0; c < main.speakers.size(); ++c) {
        check_tone(all, main, main.speakers[c], kTones51[c], {{main.speakers[c], main_db[c]}});
    }
    check_tone(all, english, Speaker::kCentre, kToneEnglish, {{Speaker::kLeft, -9.0}});
    check_tone(all, described, Speaker::kCentre, kToneDescription, {{Speaker::kRight, -1.0}});
    // Configuration 2: at 330 degrees, g_assoc -10 dB.
    check_tone(decode_id(e, 4), described, Speaker::kCentre, kToneDescription,
               {{Speaker::kLeft, 0.0}});
    check_tone(decode_id(e, 4, 0.0, -10.0), described, Speaker::kCentre, kToneDescription,
               {{Speaker::kLeft, -10.0}});
    // A stereo commentary channel to channel, the main's L and R at -3 dB.
    const Decoded with_commentary = decode_id(e, 5);
    for (std::size_t c = 0; c < main.speakers.size(); ++c) {
        const bool front =
            main.speakers[c] == Speaker::kLeft || main.speakers[c] == Speaker::kRight;
        check_tone(with_commentary, main, main.speakers[c], kTones51[c],
                   {{main.speakers[c], front ? -3.0 : 0.0}});
    }
    check_tone(with_commentary, commentary, Speaker::kLeft, kToneCommentary[0],
               {{Speaker::kLeft, 0.0}});
    check_tone(with_commentary, commentary, Speaker::kRight, kToneCommentary[1],
               {{Speaker::kRight, 0.0}});
    // Configuration 5, by classifier: the dialogue at -0.5 dB into L, the
    // audio description at -1.5 dB into R, the main audio unscaled.
    const Decoded classified = decode_id(e, 7);
    check_tone(classified, english, Speaker::kCentre, kToneEnglish, {{Speaker::kLeft, -0.5}});
    check_tone(classified, described, Speaker::kCentre, kToneDescription,
               {{Speaker::kRight, -1.5}});
    check_tone(classified, main, Speaker::kCentre, kTones51[2], {{Speaker::kCentre, 0.0}});
}

TEST_CASE("the hybrid methods add the dialogue enhancement substream's waveform",
          "[ac4enc][presentations]") {
    const Encoded& e = encoded_hybrid();
    const Decoded main51 = decode_id(e, 10);
    const Decoded waveform51 = decode_id(e, 11);
    const Decoded main20 = decode_id(e, 12);
    const Decoded waveform20 = decode_id(e, 13);
    const Decoded described = decode_id(e, 14);
    constexpr double kGain = 6.0;
    const double g = std::pow(10.0, kGain / 20.0) - 1.0;
    // At 0 dB nothing is added.
    const Decoded off = decode_id(e, 1);
    for (std::size_t c = 0; c < main51.speakers.size(); ++c) {
        const double hz = c == 2 ? kToneDialogue : kTones51[c];
        check_tone(off, main51, main51.speakers[c], hz, {{main51.speakers[c], 0.0}});
    }
    // The channel-independent method on C, which carries the dialogue alone,
    // so its parameter is 1: (1 + g_p) m + g_s d, m and d the same dialogue
    // in the main substream and in the waveform. The other channels as they
    // are.
    const auto raised = [](const Decoded& mix, const Decoded& main, const Decoded& waveform,
                           Speaker at, Speaker waveform_at, double main_gain,
                           double waveform_gain) {
        const std::complex<double> expected =
            main_gain * component(main, at, kToneDialogue) +
            waveform_gain * component(waveform, waveform_at, kToneDialogue);
        const double got = amplitude(mix, at, kToneDialogue);
        CAPTURE(ac4::describe(at), db(got), db(std::abs(expected)));
        CHECK(std::abs(db(got) - db(std::abs(expected))) < kToleranceDb);
    };
    {
        const auto [gp, gs] = split_gain(kGain, kShareIndependent);
        const Decoded on = decode_id(e, 1, 0.0, 0.0, kGain);
        raised(on, main51, waveform51, Speaker::kCentre, Speaker::kCentre, 1.0 + gp, gs);
        for (const std::size_t c : {0U, 1U, 4U, 5U}) {
            check_tone(on, main51, main51.speakers[c], kTones51[c], {{main51.speakers[c], 0.0}});
        }
        // In all the dialogue rises by G, near enough for the two codings.
        CHECK(std::abs(db(amplitude(on, Speaker::kCentre, kToneDialogue) /
                          amplitude(main51, Speaker::kCentre, kToneDialogue)) -
                       kGain) < 0.1);
        // Without the substream, the parameters alone at the whole gain.
        const Decoded alone = decode_id(e, 10, 0.0, 0.0, kGain);
        check_tone(alone, main51, Speaker::kCentre, kToneDialogue,
                   {{Speaker::kCentre, db(1.0 + g)}});
        // With audio description as well (configuration 4): its group -1 dB
        // at 30 degrees, the waveform as before.
        const Decoded with_described = decode_id(e, 2, 0.0, 0.0, kGain);
        raised(with_described, main51, waveform51, Speaker::kCentre, Speaker::kCentre, 1.0 + gp,
               gs);
        check_tone(with_described, described, Speaker::kCentre, kToneDescription,
                   {{Speaker::kRight, -1.0}});
    }
    // The Mid of L and R, which carry the same dialogue: its parameter 1, the
    // waveform L's and R's dialogue summed, half of it into each.
    {
        const auto [gp, gs] = split_gain(kGain, kShareMid);
        const Decoded on = decode_id(e, 3, 0.0, 0.0, kGain);
        raised(on, main20, waveform20, Speaker::kLeft, Speaker::kCentre, 1.0 + gp, gs / 2.0);
        raised(on, main20, waveform20, Speaker::kRight, Speaker::kCentre, 1.0 + gp, gs / 2.0);
        CHECK(std::abs(amplitude(waveform20, Speaker::kCentre, kToneDialogue) -
                       2.0 * amplitude(main20, Speaker::kLeft, kToneDialogue)) <
              0.01 * amplitude(waveform20, Speaker::kCentre, kToneDialogue));
    }
}

TEST_CASE("the cross-channel hybrid method renders its waveform by the dialogue's panning",
          "[ac4enc][presentations]") {
    // A stereo main whose dialogue, a stem apart from the programme, is panned
    // (0.448, 0.894), which Table 172 carries exactly: the waveform is its
    // projection on that panning, and the decoder renders it back by it.
    constexpr double kLeft = 0.448;
    const double right = std::sqrt(1.0 - kLeft * kLeft);
    constexpr int kShare = 31;
    ac4::EncoderConfig config;
    config.bitrate_kbps = 256;
    ac4::SubstreamConfig main = substream(2, 160, ContentClassifier::kCompleteMain);
    main.dialogue = ac4::DialogueConfig{};
    main.dialogue->method = ac4::DialogueMethod::kCrossChannel;
    main.dialogue->source = ac4::DialogueSource::kStem;
    main.dialogue->left = true;
    main.dialogue->right = true;
    main.dialogue->centre = false;
    main.dialogue->max_gain_db = 12;
    main.dialogue->hybrid = true;
    main.dialogue->waveform_share = kShare / 31.0;
    ac4::SubstreamConfig waveform;
    waveform.enhances = 0;
    waveform.bitrate_kbps = 64;
    config.substreams = {main, waveform};
    config.presentations = {presentation(1, {0, 1}, 1), presentation(std::nullopt, {0}, 2),
                            presentation(std::nullopt, {1}, 3)};
    const std::vector<std::vector<float>> input = {tone(kTones51[0]), tone(kTones51[1])};
    const std::vector<std::vector<float>> stem = {tone(kToneDialogue, kAmplitude * kLeft),
                                                  tone(kToneDialogue, kAmplitude * right)};
    const Encoded e = encode(config, input, &stem);
    read_back(e);
    const Decoded alone = decode_id(e, 2);
    const Decoded wave = decode_id(e, 3);
    // The waveform is the dialogue itself, at its whole level.
    CHECK(std::abs(db(amplitude(wave, Speaker::kCentre, kToneDialogue) / kAmplitude)) < 0.1);
    const auto [gp, gs] = split_gain(6.0, kShare);
    const Decoded on = decode_id(e, 1, 0.0, 0.0, 6.0);
    check_tone(on, wave, Speaker::kCentre, kToneDialogue,
               {{Speaker::kLeft, db(kLeft * gs)}, {Speaker::kRight, db(right * gs)}});
    check_tone(on, alone, Speaker::kLeft, kTones51[0], {{Speaker::kLeft, 0.0}});
    check_tone(on, alone, Speaker::kRight, kTones51[1], {{Speaker::kRight, 0.0}});
    CHECK(gp == 0.0);
}

TEST_CASE("3.0 carries the dialogue of a music and effects presentation and a waveform",
          "[ac4enc][presentations]") {
    ac4::EncoderConfig config;
    config.bitrate_kbps = 512;
    config.experimental.three_zero = true;
    ac4::SubstreamConfig me = substream(6, 256, ContentClassifier::kMusicAndEffects);
    ac4::SubstreamConfig dialogue = substream(3, 128, ContentClassifier::kDialogue, "en");
    ac4::SubstreamConfig main = substream(6, 256, ContentClassifier::kCompleteMain);
    main.dialogue = ac4::DialogueConfig{};
    main.dialogue->left = true;
    main.dialogue->right = true;
    main.dialogue->centre = true;
    main.dialogue->hybrid = true;
    ac4::SubstreamConfig waveform;
    waveform.enhances = 2;
    waveform.bitrate_kbps = 128;
    config.bitrate_kbps = 800;
    config.substreams = {me, dialogue, main, waveform};
    config.presentations = {
        presentation(0, {0, 1}, 1),          presentation(1, {2, 3}, 2),
        presentation(std::nullopt, {0}, 10), presentation(std::nullopt, {1}, 11),
        presentation(std::nullopt, {2}, 12), presentation(std::nullopt, {3}, 13)};
    std::vector<std::vector<float>> input;
    for (const double hz : kTones51) {
        input.push_back(tone(hz));
    }
    constexpr std::array<double, 3> kDialogue30 = {1117.0, 1373.0, 1531.0};
    for (const double hz : kDialogue30) {
        input.push_back(tone(hz));
    }
    for (std::size_t c = 0; c < 6; ++c) {
        input.push_back(tone(c < 3 ? kDialogue30[c] : kTones51[c]));
    }
    const Encoded e = encode(config, input);
    read_back(e);
    const ac4::Toc toc = toc_of(e.frames.front());
    CHECK(toc.substream_groups[1].substreams[0].chan->ch_mode == 2);
    CHECK(toc.substream_groups[3].substreams[0].chan->ch_mode == 2);
    // The dialogue's three channels go channel to channel into the music and
    // effects' L, R and C.
    const Decoded me_alone = decode_id(e, 10);
    const Decoded dialogue_alone = decode_id(e, 11);
    REQUIRE(dialogue_alone.speakers ==
            std::vector{Speaker::kLeft, Speaker::kRight, Speaker::kCentre});
    const Decoded mix = decode_id(e, 1);
    for (std::size_t c = 0; c < 3; ++c) {
        check_tone(mix, dialogue_alone, dialogue_alone.speakers[c], kDialogue30[c],
                   {{dialogue_alone.speakers[c], 0.0}});
    }
    check_tone(mix, me_alone, Speaker::kLfe, kTones51[3], {{Speaker::kLfe, 0.0}});
    // The waveform's three channels raise L, R and C, one each.
    const Decoded main_alone = decode_id(e, 12);
    const Decoded wave = decode_id(e, 13);
    const Decoded on = decode_id(e, 2, 0.0, 0.0, 6.0);
    for (std::size_t c = 0; c < 3; ++c) {
        const Speaker at = main_alone.speakers[c];
        CHECK(std::abs(db(amplitude(on, at, kDialogue30[c]) /
                          amplitude(main_alone, at, kDialogue30[c])) -
                       6.0) < 0.1);
        CHECK(amplitude(wave, wave.speakers[c], kDialogue30[c]) > 0.5 * kAmplitude);
    }
    // Without the experimental option, 3.0 is refused.
    config.experimental.three_zero = false;
    CHECK_FALSE(accepted(config));
}

TEST_CASE("presentations that break the rules are refused", "[ac4enc][presentations]") {
    const auto base = [] {
        ac4::EncoderConfig c;
        c.bitrate_kbps = 256;
        c.substreams = {substream(2, 128, ContentClassifier::kMusicAndEffects),
                        substream(1, 64, ContentClassifier::kDialogue, "en")};
        c.presentations = {presentation(0, {0, 1}, 1), presentation(std::nullopt, {0}, 2)};
        return c;
    };
    REQUIRE(accepted(base()));
    // Part 1 clause 6.2.16.0: dialogue adds no channel the music and effects
    // lack, but for a mono one.
    {
        ac4::EncoderConfig c = base();
        c.substreams[0].channels = 1;
        c.substreams[1].channels = 2;
        CHECK_FALSE(accepted(c));
        c.substreams[1].channels = 1;
        CHECK(accepted(c));
    }
    // Associated audio likewise: 5.1 against a stereo main.
    {
        ac4::EncoderConfig c = base();
        c.substreams[1] = substream(6, 96, ContentClassifier::kVisuallyImpaired);
        c.presentations = {presentation(2, {0, 1}, 1)};
        CHECK_FALSE(accepted(c));
    }
    // Part 1 clause 4.3.3.7.1: 3.0 codes a dialogue enhancement signal, or
    // the dialogue of a music and effects presentation, alone.
    {
        ac4::EncoderConfig c = base();
        c.experimental.three_zero = true;
        c.substreams[0].channels = 6;
        c.substreams[1].channels = 3;
        CHECK(accepted(c));
        c.presentations = {presentation(std::nullopt, {0}, 1), presentation(std::nullopt, {1}, 3)};
        CHECK_FALSE(accepted(c));  // 3.0 alone, and the dialogue of no presentation
        c.substreams[0].content = ContentClassifier::kCompleteMain;
        c.presentations = {presentation(5, {0, 1}, 1)};  // with a complete main
        CHECK_FALSE(accepted(c));
        c.substreams[1].content = ContentClassifier::kVisuallyImpaired;
        c.presentations = {presentation(2, {0, 1}, 1)};  // as associated audio
        CHECK_FALSE(accepted(c));
    }
    // CMAF (Part 2 Annex H.1.2.1): 64 presentations at most, each id its own.
    {
        ac4::EncoderConfig c = base();
        c.presentations.clear();
        for (int i = 0; i < 64; ++i) {
            c.presentations.push_back(presentation(0, {0, 1}, i));
        }
        CHECK(accepted(c));
        c.presentations.push_back(presentation(0, {0, 1}, 64));
        CHECK_FALSE(accepted(c));
        c = base();
        c.presentations[1].presentation_id = 1;
        CHECK_FALSE(accepted(c));
    }
    // Table 53: the configuration's count of substreams, a dialogue
    // enhancement substream where it asks for one, and every substream played.
    {
        ac4::EncoderConfig c = base();
        c.presentations = {presentation(3, {0, 1}, 1)};
        CHECK_FALSE(accepted(c));
        c.presentations = {presentation(1, {0, 1}, 1)};
        CHECK_FALSE(accepted(c));
        c.presentations = {presentation(std::nullopt, {0}, 1)};
        CHECK_FALSE(accepted(c));  // the dialogue is played by none
        c.presentations = {presentation(0, {0, 0}, 1)};
        CHECK_FALSE(accepted(c));
    }
    // Levels, names and gains.
    {
        ac4::EncoderConfig c = base();
        c.presentations[0].md_compat = 0;  // three tracks
        CHECK_FALSE(accepted(c));
        c.presentations[0].md_compat = 3;
        CHECK(accepted(c));
        c.presentations[0].md_compat = 5;  // reserved
        CHECK_FALSE(accepted(c));
        c = base();
        c.presentations[0].name = std::string(31, 'x');
        CHECK(accepted(c));
        c.presentations[0].name = std::string(32, 'x');
        CHECK_FALSE(accepted(c));
        c = base();
        c.presentations[0].gains_db = {0.0, -0.3};  // not on Table 70's scale
        CHECK_FALSE(accepted(c));
        c.presentations[0].gains_db = {0.0, -0.25};
        CHECK(accepted(c));
        c.presentations[1].gains_db = {-1.0};  // a single group: none sent
        CHECK_FALSE(accepted(c));
    }
    // The associated audio's values need associated audio, and a pan a mono
    // substream.
    {
        ac4::EncoderConfig c = base();
        c.presentations[0].associated = ac4::AssociatedMix{.main_db = -6.0,
                                                           .main_centre_db = std::nullopt,
                                                           .main_front_db = std::nullopt,
                                                           .pan_degrees = std::nullopt};
        CHECK_FALSE(accepted(c));
        c = base();
        c.substreams[1] = substream(2, 64, ContentClassifier::kCommentary);
        c.presentations = {presentation(2, {0, 1}, 1)};
        c.presentations[0].associated = ac4::AssociatedMix{.main_db = std::nullopt,
                                                           .main_centre_db = std::nullopt,
                                                           .main_front_db = std::nullopt,
                                                           .pan_degrees = 30.0};
        CHECK_FALSE(accepted(c));
    }
    // A language needs a content_type() to go in; the EMDF-only
    // configuration payloads and nothing else; the substreams' rates within
    // the stream's.
    {
        ac4::EncoderConfig c = base();
        c.substreams[0].content.reset();
        c.substreams[0].language = "en";
        CHECK_FALSE(accepted(c));
        c = base();
        ac4::PresentationConfig emdf;
        emdf.config = 6;
        c.presentations.push_back(emdf);
        CHECK_FALSE(accepted(c));
        c.presentations.back().emdf = {payload(1, {1})};
        CHECK(accepted(c));
        c.presentations.back().substreams = {0};
        CHECK_FALSE(accepted(c));
        c = base();
        c.substreams[0].bitrate_kbps = 250;
        CHECK_FALSE(accepted(c));
    }
    // A hybrid method's waveform: for a hybrid main, one at most.
    {
        ac4::EncoderConfig c = base();
        ac4::SubstreamConfig waveform;
        waveform.enhances = 0;
        c.substreams.push_back(waveform);
        c.presentations.push_back(presentation(1, {0, 2}, 3));
        CHECK_FALSE(accepted(c));  // substream 0 has no hybrid method
        c.substreams[0].dialogue = ac4::DialogueConfig{};
        c.substreams[0].dialogue->left = true;
        c.substreams[0].dialogue->right = true;
        c.substreams[0].dialogue->centre = false;
        c.substreams[0].dialogue->hybrid = true;
        CHECK(accepted(c));
        c.substreams.push_back(waveform);
        CHECK_FALSE(accepted(c));
    }
}

TEST_CASE("the committed encoder presentation streams are the configurations'",
          "[ac4enc][presentations]") {
    const char* write_to = std::getenv("AC4ENC_WRITE_PRESENTATIONS");
    const std::array<std::pair<std::string_view, Stream>, 3> streams = {
        std::pair{"encoder-broadcast", broadcast()}, std::pair{"encoder-hybrid", hybrid()},
        std::pair{"encoder-emdf", emdf()}};
    for (const auto& [name, stream] : streams) {
        CAPTURE(name);
        const Encoded& encoded = name == "encoder-broadcast" ? encoded_broadcast()
                                 : name == "encoder-hybrid"  ? encoded_hybrid()
                                                             : encoded_emdf();
        const ac4::Toc toc = toc_of(encoded.frames.front());
        if (write_to != nullptr) {
            const fs::path out = fs::path{write_to} / "presentations";
            write_file(out / (std::string{name} + ".ac4"), sync_framed(encoded));
            std::ofstream(out / (std::string{name} + ".json"), std::ios::binary)
                << configuration_json(stream.config, toc);
            continue;
        }
        // The encoder's bytes are not promised across toolchains, so the
        // committed stream is held to the configuration's table of contents,
        // and to what its readers read in it.
        const std::vector<std::byte> file = read_file(committed(name));
        const ac4::ScanResult scan = ac4::scan(file);
        REQUIRE_FALSE(scan.frames.empty());
        const ac4::Toc on_disk = toc_of(scan.frames.front().raw_ac4_frame);
        REQUIRE(on_disk.presentations_v1.size() == toc.presentations_v1.size());
        REQUIRE(on_disk.substream_groups.size() == toc.substream_groups.size());
        for (std::size_t i = 0; i < toc.presentations_v1.size(); ++i) {
            CHECK(on_disk.presentations_v1[i].presentation_id ==
                  toc.presentations_v1[i].presentation_id);
            CHECK(on_disk.presentations_v1[i].presentation_config ==
                  toc.presentations_v1[i].presentation_config);
            CHECK(on_disk.presentations_v1[i].group_refs == toc.presentations_v1[i].group_refs);
            CHECK(on_disk.presentations_v1[i].md_compat == toc.presentations_v1[i].md_compat);
            CHECK(on_disk.presentations_v1[i].b_alternative ==
                  toc.presentations_v1[i].b_alternative);
        }
        for (std::size_t g = 0; g < toc.substream_groups.size(); ++g) {
            CHECK(on_disk.substream_groups[g].substreams.front().chan->ch_mode ==
                  toc.substream_groups[g].substreams.front().chan->ch_mode);
        }
        std::ifstream in(
            fs::path{AC4DEC_GOLDEN_DIR} / "presentations" / (std::string{name} + ".json"),
            std::ios::binary);
        std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::erase(json, '\r');
        CHECK(json == configuration_json(stream.config, toc));
    }
}
