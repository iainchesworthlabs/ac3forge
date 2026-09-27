// ac4::Encoder's object audio end to end (planning/ac4.md, phase E9): A-JOC
// substreams of ETSI TS 103 190-2 V1.3.1 clause 5.7, over a computed downmix
// in a var_channel_element() and over a static 5.1 bed, with bed objects, the
// LFE and decorrelation, and direct-coded object substreams, each with its
// object audio metadata. Each stream reads back with the trace the encoder
// recorded; decoded in full, each object's reconstruction is scored against
// the object the encoder was given, by correlation and SNR, against floors at
// the first measurement less a margin; core decoding gives the downmix and its
// metadata; and metadata updates come out at the sample their input sample
// does. Every object carries a tone of its own at the middle of a QMF subband,
// each in a parameter band of its own.
//
// With AC4ENC_WRITE_OBJECTS set to a directory, the committed object streams
// (tests/golden/ac4dec/objects/encoder-*.ac4) are written there; with
// AC4ENC_WRITE_LISTENING, ten-second streams for listening.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac4/ac4.hpp"
#include "ac4/syntax.hpp"
#include "ac4dec/decoder.hpp"
#include "ac4enc/encoder.hpp"
#include "sanitized.hpp"

namespace {

namespace fs = std::filesystem;
using ac3::test::kSanitized;

constexpr double kRate = 48000.0;
constexpr int kFrame = 2048;
constexpr double kAmplitude = 0.1;  // -20 dBFS
constexpr std::size_t kSamples = kSanitized ? 24576 : 48000;
constexpr double kLfeHz = 47.0;
// QMF subbands 375 Hz wide at 48 kHz, each object's tone at the middle of
// one, in a parameter band of its own at 15 and 23 bands (Table 28).
constexpr std::array<int, 8> kSubbands = {1, 3, 5, 7, 9, 12, 16, 22};

[[nodiscard]] double tone_hz(std::size_t k) {
    return (kSubbands[k % kSubbands.size()] + 0.5) * kRate / 128.0;
}

std::vector<float> tone(double hz, std::size_t count, double amplitude = kAmplitude) {
    std::vector<float> x(count);
    for (std::size_t n = 0; n < count; ++n) {
        x[n] = static_cast<float>(amplitude *
                                  std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) / kRate));
    }
    return x;
}

// An object's configuration: dynamic at `x` across the front, or a bed, or
// the LFE.
ac4::ObjectConfig dynamic_at(double x, double y = 0.0) {
    ac4::ObjectConfig o;
    o.properties.position = {x, y, 0.0};
    return o;
}

ac4::ObjectConfig bed(ac4::BedChannel channel) {
    ac4::ObjectConfig o;
    o.bed = channel;
    return o;
}

ac4::ObjectConfig lfe() {
    ac4::ObjectConfig o;
    o.lfe = true;
    return o;
}

// A stream of `objects`, each its tone (the LFE 47 Hz), at `kbps`.
struct Case {
    std::string name;
    ac4::ObjectsConfig objects;
    int kbps = 256;
};

ac4::EncoderConfig config_of(const Case& c) {
    ac4::EncoderConfig config;
    config.bitrate_kbps = c.kbps;
    config.experimental.objects = true;
    ac4::SubstreamConfig s;
    s.objects = c.objects;
    config.substreams = {s};
    return config;
}

std::vector<std::vector<float>> input_of(const Case& c, std::size_t count) {
    std::vector<std::vector<float>> input;
    std::size_t tones = 0;
    for (const ac4::ObjectConfig& o : c.objects.objects) {
        input.push_back(o.lfe ? tone(kLfeHz, count) : tone(tone_hz(tones++), count));
    }
    return input;
}

struct Encoded {
    std::vector<ac4::EncodedFrame> frames;
    std::vector<ac4::SyntaxRecord> trace;
    int delay = 0;
    int decoder_delay = 0;
};

Encoded encode(const ac4::EncoderConfig& base, const std::vector<std::vector<float>>& input,
               std::span<const ac4::ObjectMetadataUpdate> updates = {}) {
    Encoded out;
    ac4::EncoderConfig config = base;
    config.trace = [&out](const ac4::SyntaxRecord& r) { out.trace.push_back(r); };
    INFO(ac4::Encoder::refusal_reason(base));
    auto encoder = ac4::Encoder::create(config);
    REQUIRE(encoder.has_value());
    out.delay = encoder->delay_samples();
    out.decoder_delay = encoder->decoder_delay_samples();
    std::vector<std::span<const float>> views;
    for (const auto& channel : input) {
        views.emplace_back(channel);
    }
    auto frames = encoder->encode(views, updates);
    REQUIRE(frames.has_value());
    out.frames = *frames;
    auto rest = encoder->flush();
    REQUIRE(rest.has_value());
    out.frames.insert(out.frames.end(), rest->begin(), rest->end());
    return out;
}

// Every substream of every frame reads to its end, and the decoder's trace is
// the encoder's, record for record.
void check_frames_read_back(const Encoded& encoded) {
    std::vector<ac4::SyntaxRecord> read;
    ac4::DecoderConfig config;
    config.syntax = [&read](const ac4::SyntaxRecord& r) { read.push_back(r); };
    ac4::Decoder decoder(config);
    for (const ac4::EncodedFrame& frame : encoded.frames) {
        const auto report = decoder.parse(frame.raw_ac4_frame);
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

// What decode() put out: each object's samples end to end, its kind, and each
// update at its sample in the output.
struct DecodedObjects {
    std::vector<std::vector<float>> samples;
    std::vector<ac4::DecodedObject> last;
    struct Update {
        std::size_t object = 0;
        std::int64_t sample = 0;
        ac4::ObjectProperties properties;
    };
    std::vector<Update> updates;
};

DecodedObjects decode(const Encoded& encoded, ac4::DecodingMode mode) {
    ac4::DecoderConfig config;
    config.decoding = mode;
    ac4::Decoder decoder(config);
    DecodedObjects out;
    std::int64_t start = 0;
    for (const ac4::EncodedFrame& frame : encoded.frames) {
        const auto decoded = decoder.decode(frame.raw_ac4_frame);
        INFO(decoder.refusal_reason());
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        const ac4::DecodedFrame& pcm = **decoded;
        if (out.samples.empty()) {
            out.samples.resize(pcm.objects.size());
        }
        REQUIRE(pcm.objects.size() == out.samples.size());
        for (std::size_t o = 0; o < pcm.objects.size(); ++o) {
            const ac4::DecodedObject& object = pcm.objects[o];
            out.samples[o].insert(out.samples[o].end(), object.samples.begin(), object.samples.end());
            for (const ac4::ObjectUpdate& u : object.updates) {
                out.updates.push_back({o, start + static_cast<std::int64_t>(u.sample), u.properties});
            }
        }
        out.last = pcm.objects;
        start += static_cast<std::int64_t>(pcm.samples);
    }
    return out;
}

struct Score {
    double correlation = 0.0;
    double snr_db = 0.0;
};

// `decoded` against `reference` `lag` samples later, over the reference from
// two frames in to a frame before its end.
Score score(std::span<const float> reference, std::span<const float> decoded, std::size_t lag) {
    double xx = 0.0;
    double yy = 0.0;
    double xy = 0.0;
    double ee = 0.0;
    for (std::size_t n = 2 * kFrame; n + kFrame < reference.size() && n + lag < decoded.size(); ++n) {
        const auto x = static_cast<double>(reference[n]);
        const auto y = static_cast<double>(decoded[n + lag]);
        xx += x * x;
        yy += y * y;
        xy += x * y;
        ee += (x - y) * (x - y);
    }
    return {xy / std::sqrt(std::max(xx * yy, 1e-300)), 10.0 * std::log10(xx / std::max(ee, 1e-300))};
}

// The decoded objects of full decoding in the encoder's order: the LFE first,
// then the bed objects and the dynamic objects in their order (A-JOC), or each
// substream's LFE and objects (direct-coded, whose LFE rides the first).
std::vector<std::size_t> decoded_order(const ac4::ObjectsConfig& objects) {
    std::vector<std::size_t> out;
    for (std::size_t o = 0; o < objects.objects.size(); ++o) {
        if (objects.objects[o].lfe) {
            out.push_back(o);
        }
    }
    for (const bool beds : {true, false}) {
        for (std::size_t o = 0; o < objects.objects.size(); ++o) {
            const ac4::ObjectConfig& c = objects.objects[o];
            if (!c.lfe && c.bed.has_value() == beds) {
                out.push_back(o);
            }
        }
    }
    return out;
}

// Each object scored against what the encoder was given; each floor's
// correlation and SNR, by object in the encoder's order.
void check_scores(const Case& c, const Encoded& encoded, std::span<const Score> floors) {
    const std::vector<std::vector<float>> input = input_of(c, kSamples);
    const DecodedObjects full = decode(encoded, ac4::DecodingMode::kFull);
    const std::vector<std::size_t> order = decoded_order(c.objects);
    REQUIRE(full.samples.size() == order.size());
    const auto lag = static_cast<std::size_t>(encoded.delay + encoded.decoder_delay);
    for (std::size_t d = 0; d < order.size(); ++d) {
        const std::size_t o = order[d];
        const Score s = score(input[o], full.samples[d], lag);
        CAPTURE(c.name, o, s.correlation, s.snr_db);
        CHECK(s.correlation >= floors[o].correlation);
        CHECK(s.snr_db >= floors[o].snr_db);
    }
}

// Eight dynamic objects across the front in the order of their azimuth, four
// downmix signals: each pair of neighbours summed into one.
Case computed() {
    Case c{.name = "encoder-ajoc-computed", .objects = {}, .kbps = 256};
    for (int k = 0; k < 8; ++k) {
        c.objects.objects.push_back(dynamic_at(k / 7.0));
    }
    c.objects.downmix_signals = 4;
    return c;
}

// Six dynamic objects around the room and the LFE over a static 5.1 bed.
Case static_bed() {
    Case c{.name = "encoder-ajoc-static-5_1", .objects = {}, .kbps = 320};
    for (const auto& [x, y] : std::array<std::array<double, 2>, 6>{
             {{0.0, 0.0}, {1.0, 0.0}, {0.5, 0.0}, {0.0, 1.0}, {1.0, 1.0}, {0.25, 0.0}}}) {
        c.objects.objects.push_back(dynamic_at(x, y));
    }
    c.objects.objects.push_back(lfe());
    c.objects.downmix = ac4::AjocDownmix::kStatic51;
    return c;
}

// Two bed objects, three dynamic objects and the LFE, three downmix signals,
// with decorrelation.
Case beds_and_decorrelation() {
    Case c{.name = "encoder-ajoc-beds-decorr", .objects = {}, .kbps = 256};
    c.objects.objects = {lfe(),
                         bed(ac4::BedChannel::kLeft),
                         bed(ac4::BedChannel::kRight),
                         dynamic_at(0.25, 0.5),
                         dynamic_at(0.5, 1.0),
                         dynamic_at(0.75, 0.5)};
    c.objects.downmix_signals = 3;
    c.objects.decorrelation = true;
    c.objects.screen_size_ratio_code = 20;
    return c;
}

// Four dynamic objects and the LFE, direct-coded in a 3.0 substream with the
// LFE and a mono one.
Case direct() {
    Case c{.name = "encoder-direct", .objects = {}, .kbps = 256};
    c.objects.objects = {dynamic_at(0.0), dynamic_at(0.33), lfe(), dynamic_at(0.67), dynamic_at(1.0)};
    c.objects.coding = ac4::ObjectCoding::kDirect;
    return c;
}

std::vector<std::byte> sync_framed(const Encoded& encoded) {
    std::vector<std::byte> out;
    for (const ac4::EncodedFrame& frame : encoded.frames) {
        const std::vector<std::byte> framed = ac4::sync_frame(frame.raw_ac4_frame, true);
        out.insert(out.end(), framed.begin(), framed.end());
    }
    return out;
}

void write_file(const fs::path& path, std::span<const std::byte> bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(out.good());
}

}  // namespace

TEST_CASE("an A-JOC substream's objects decode as the encoder was given them",
          "[ac4enc][objects]") {
    // Floors at the first measurement (2026-09-27) less 0.02 in correlation
    // and 1 dB in SNR, by object in the encoder's order, the LFE's among
    // them. Every measured correlation was above 0.99999.
    SECTION("a computed downmix of four signals for eight objects") {
        const Case c = computed();
        const Encoded encoded = encode(config_of(c), input_of(c, kSamples));
        check_frames_read_back(encoded);
        const std::array<Score, 8> floors = {{{0.98, 61.0},
                                              {0.98, 63.2},
                                              {0.98, 61.9},
                                              {0.98, 61.3},
                                              {0.98, 59.7},
                                              {0.98, 60.2},
                                              {0.98, 60.0},
                                              {0.98, 60.9}}};
        check_scores(c, encoded, floors);
    }
    SECTION("a static 5.1 bed for six objects and the LFE") {
        const Case c = static_bed();
        const Encoded encoded = encode(config_of(c), input_of(c, kSamples));
        check_frames_read_back(encoded);
        // Object 5 sits between L and C, whose pans share it.
        const std::array<Score, 7> floors = {{{0.98, 61.1},
                                              {0.98, 60.8},
                                              {0.98, 60.7},
                                              {0.98, 60.6},
                                              {0.98, 60.3},
                                              {0.98, 39.6},
                                              {0.98, 68.1}}};
        check_scores(c, encoded, floors);
    }
    SECTION("bed objects and the LFE, with decorrelation") {
        const Case c = beds_and_decorrelation();
        const Encoded encoded = encode(config_of(c), input_of(c, kSamples));
        check_frames_read_back(encoded);
        // The tones rebuild whole, so the decorrelators are enabled and
        // carry no energy.
        CHECK(std::ranges::count_if(encoded.trace, [](const ac4::SyntaxRecord& r) {
                  return r.name == "ajoc_num_decorr" && r.value == 3;
              }) == std::ssize(encoded.frames));
        const std::array<Score, 6> floors = {{{0.98, 68.1},
                                              {0.98, 60.7},
                                              {0.98, 60.1},
                                              {0.98, 59.9},
                                              {0.98, 60.2},
                                              {0.98, 60.4}}};
        check_scores(c, encoded, floors);
    }
}

TEST_CASE("direct-coded objects decode as the encoder was given them", "[ac4enc][objects]") {
    const Case c = direct();
    const Encoded encoded = encode(config_of(c), input_of(c, kSamples));
    check_frames_read_back(encoded);
    const std::array<Score, 5> floors = {{{0.98, 71.6},
                                          {0.98, 72.9},
                                          {0.98, 68.1},
                                          {0.98, 71.7},
                                          {0.98, 73.0}}};
    check_scores(c, encoded, floors);
}

TEST_CASE("core decoding of an A-JOC substream gives its downmix with its metadata",
          "[ac4enc][objects]") {
    const Case c = computed();
    const std::vector<std::vector<float>> input = input_of(c, kSamples);
    const Encoded encoded = encode(config_of(c), input);
    const DecodedObjects core = decode(encoded, ac4::DecodingMode::kCore);
    // The four downmix signals, dynamic objects: each the sum of a pair of
    // neighbours at the pair's centre.
    REQUIRE(core.samples.size() == 4);
    const auto lag = static_cast<std::size_t>(encoded.delay + encoded.decoder_delay);
    // The first measurement (2026-09-27) less 1 dB.
    const std::array<double, 4> floors = {74.4, 72.9, 71.9, 71.8};
    for (std::size_t k = 0; k < 4; ++k) {
        std::vector<float> sum(kSamples, 0.0F);
        for (const std::size_t o : {2 * k, 2 * k + 1}) {
            for (std::size_t n = 0; n < kSamples; ++n) {
                sum[n] += input[o][n];
            }
        }
        const Score s = score(sum, core.samples[k], lag);
        CAPTURE(k, s.correlation, s.snr_db);
        CHECK(s.correlation >= 0.98);
        CHECK(s.snr_db >= floors[k]);
        const ac4::DecodedObject& object = core.last[k];
        CHECK(object.kind == ac4::ObjectKind::kDyn);
        const double x = (2.0 * static_cast<double>(k) + 0.5) / 7.0;
        CAPTURE(object.properties.position[0], x);
        CHECK(std::abs(object.properties.position[0] - x) <= 1.0 / 62.0);
        CHECK(std::abs(object.properties.position[1]) <= 1.0 / 62.0);
    }
}

TEST_CASE("an object's metadata updates come out where their input samples do",
          "[ac4enc][objects]") {
    for (const ac4::ObjectCoding coding : {ac4::ObjectCoding::kAjoc, ac4::ObjectCoding::kDirect}) {
        CAPTURE(coding == ac4::ObjectCoding::kAjoc);
        Case c{.name = "moving", .objects = {}, .kbps = 128};
        c.objects.objects = {dynamic_at(0.0)};
        c.objects.coding = coding;
        std::vector<ac4::ObjectMetadataUpdate> updates;
        for (int k = 1; k <= 3; ++k) {
            ac4::ObjectMetadataUpdate u;
            u.object = 0;
            u.sample = 11111 * k;
            u.properties.position = {k / 4.0, k / 8.0, 0.0};
            updates.push_back(u);
        }
        const Encoded encoded = encode(config_of(c), input_of(c, kSamples), updates);
        check_frames_read_back(encoded);
        const DecodedObjects full = decode(encoded, ac4::DecodingMode::kFull);
        const std::int64_t lag = encoded.delay + encoded.decoder_delay;
        for (const ac4::ObjectMetadataUpdate& u : updates) {
            CAPTURE(u.sample);
            const auto found = std::ranges::find_if(full.updates, [&](const DecodedObjects::Update& d) {
                return std::abs(d.properties.position[0] - u.properties.position[0]) <= 1.0 / 124.0 &&
                       std::abs(d.properties.position[1] - u.properties.position[1]) <= 1.0 / 124.0;
            });
            REQUIRE(found != full.updates.end());
            CHECK(found->sample <= u.sample + lag);
            CHECK(found->sample > u.sample + lag - 32);
        }
    }
}

TEST_CASE("the object encoder's streams are the same from the same input", "[ac4enc][objects]") {
    for (const Case& c : {computed(), static_bed(), beds_and_decorrelation(), direct()}) {
        CAPTURE(c.name);
        const std::vector<std::vector<float>> input = input_of(c, 4 * kFrame);
        const Encoded a = encode(config_of(c), input);
        const Encoded b = encode(config_of(c), input);
        REQUIRE(a.frames.size() == b.frames.size());
        for (std::size_t f = 0; f < a.frames.size(); ++f) {
            CHECK(a.frames[f].raw_ac4_frame == b.frames[f].raw_ac4_frame);
        }
    }
}

TEST_CASE("the committed encoder object streams are the configurations'", "[ac4enc][objects]") {
    const char* write_to = std::getenv("AC4ENC_WRITE_OBJECTS");
    for (const Case& c : {computed(), static_bed(), beds_and_decorrelation(), direct()}) {
        CAPTURE(c.name);
        const Encoded encoded = encode(config_of(c), input_of(c, 6 * kFrame));
        if (write_to != nullptr) {
            write_file(fs::path{write_to} / "objects" / (c.name + ".ac4"), sync_framed(encoded));
            continue;
        }
        // The bytes are not promised across toolchains: the committed stream
        // is held to the configuration's table of contents, and its digests
        // to both readers (tests/golden/ac4dec/objects-encoder-*.tsv).
        const fs::path path = fs::path{AC4DEC_GOLDEN_DIR} / "objects" / (c.name + ".ac4");
        std::ifstream in(path, std::ios::binary);
        REQUIRE(in.good());
        const std::vector<char> chars((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::vector<std::byte> file(chars.size());
        std::ranges::transform(chars, file.begin(), [](char ch) { return static_cast<std::byte>(ch); });
        const ac4::ScanResult scan = ac4::scan(file);
        REQUIRE_FALSE(scan.frames.empty());
        const auto on_disk = ac4::parse_raw_frame(scan.frames.front().raw_ac4_frame);
        const auto made = ac4::parse_raw_frame(encoded.frames.front().raw_ac4_frame);
        REQUIRE(on_disk.has_value());
        REQUIRE(made.has_value());
        REQUIRE(on_disk->toc.substream_groups.size() == made->toc.substream_groups.size());
        REQUIRE(on_disk->toc.presentations_v1.size() == made->toc.presentations_v1.size());
        CHECK(on_disk->toc.presentations_v1.front().md_compat == made->toc.presentations_v1.front().md_compat);
        CHECK(on_disk->toc.substream_groups.front().substreams.size() ==
              made->toc.substream_groups.front().substreams.size());
        CHECK(on_disk->toc.substream_groups.front().b_channel_coded ==
              made->toc.substream_groups.front().b_channel_coded);
    }
}

TEST_CASE("the encoder refuses the object configurations it does not write", "[ac4enc][objects]") {
    const auto reason = [](const std::function<void(ac4::EncoderConfig&)>& change) {
        ac4::EncoderConfig config = config_of(computed());
        change(config);
        return std::string{ac4::Encoder::refusal_reason(config)};
    };
    CHECK(reason([](ac4::EncoderConfig&) {}).empty());
    CHECK(reason([](ac4::EncoderConfig& c) { c.experimental.objects = false; }) ==
          "objects without experimental.objects");
    CHECK(reason([](ac4::EncoderConfig& c) {
              c.substreams.push_back(ac4::SubstreamConfig{});
              c.presentations = {ac4::PresentationConfig{.substreams = {0}},
                                 ac4::PresentationConfig{.substreams = {1}}};
          }) == "an object substream beside other substreams: it is the stream's one");
    CHECK(reason([](ac4::EncoderConfig& c) { c.frame_rate_index = 2; }) ==
          "objects at a frame_rate_index other than 13");
    CHECK(reason([](ac4::EncoderConfig& c) { c.substreams[0].objects->objects.clear(); }) ==
          "an object substream without objects");
    CHECK(reason([](ac4::EncoderConfig& c) {
              c.substreams[0].objects->objects[0].properties.position[0] = 1.5;
          }) == "an object's properties off the ranges ObjectProperties gives them");
    CHECK(reason([](ac4::EncoderConfig& c) {
              c.substreams[0].objects->objects[0].lfe = true;
              c.substreams[0].objects->objects[1].lfe = true;
          }) == "more than one LFE object");
    CHECK(reason([](ac4::EncoderConfig& c) { c.substreams[0].objects->downmix_signals = 9; }) ==
          "a computed downmix of no signal, of more than 11 or of more than its full-band objects");
    CHECK(reason([](ac4::EncoderConfig& c) { c.substreams[0].objects->downmix = ac4::AjocDownmix::kStatic51; }) ==
          "a static 5.1 downmix without an LFE object");
    CHECK(reason([](ac4::EncoderConfig& c) { c.substreams[0].objects->parameter_bands = 10; }) ==
          "A-JOC parameter bands other than Table 78's 23, 15, 12, 9, 7, 5, 3 or 1");
    CHECK(reason([](ac4::EncoderConfig& c) {
              c.substreams[0].objects->coding = ac4::ObjectCoding::kDirect;
              c.substreams[0].objects->objects[0].bed = ac4::BedChannel::kLeft;
          }) == "bed objects in direct-coded object substreams");
    CHECK(reason([](ac4::EncoderConfig& c) { c.substreams[0].codec_mode = ac4::CodecMode::kAspxAcpl2; }) ==
          "an object substream's codec mode other than kAuto, kSimple or kAspx");
    CHECK(reason([](ac4::EncoderConfig& c) { c.substreams[0].dialogue = ac4::DialogueConfig{}; }) ==
          "dialogue enhancement, dialogue mixing values or a dialogue enhancement waveform in an "
          "object substream");
    CHECK(reason([](ac4::EncoderConfig& c) {
              c.presentations = {ac4::PresentationConfig{.config = 1, .substreams = {0, 0}}};
          }) != "");
    CHECK(reason([](ac4::EncoderConfig& c) {
              c.presentations = {ac4::PresentationConfig{.substreams = {0}, .md_compat = 2}};
          }) == "an md_compat below the least its tracks need, or in 4 to 6 (Part 2 Table 55)");

    // An update for an object the substream lacks, or off its ranges.
    const Case c = computed();
    auto encoder = ac4::Encoder::create(config_of(c));
    REQUIRE(encoder.has_value());
    const std::vector<std::vector<float>> input = input_of(c, 256);
    std::vector<std::span<const float>> views(input.begin(), input.end());
    ac4::ObjectMetadataUpdate bad;
    bad.object = 8;
    CHECK_FALSE(encoder->encode(views, std::span(&bad, 1)).has_value());
    bad.object = 0;
    bad.properties.priority = 2.0;
    CHECK_FALSE(encoder->encode(views, std::span(&bad, 1)).has_value());
}

TEST_CASE("the object streams for listening are written where AC4ENC_WRITE_LISTENING says",
          "[ac4enc][objects]") {
    // Ten seconds each: a tone crossing the front from the left wall to the
    // right one over eight seconds, a quiet static bed of five tones on L, R,
    // C, Ls and Rs, and the LFE, in A-JOC over a computed downmix and over a
    // static 5.1 bed, and direct-coded with the bed as dynamic objects at the
    // loudspeakers.
    const char* dir = std::getenv("AC4ENC_WRITE_LISTENING");
    if (dir == nullptr) {
        SKIP("AC4ENC_WRITE_LISTENING does not name a directory");
    }
    constexpr std::size_t kLength = 480000;
    const std::array<std::array<double, 2>, 5> speakers = {
        {{0.0, 0.0}, {1.0, 0.0}, {0.5, 0.0}, {0.0, 1.0}, {1.0, 1.0}}};
    const std::array<ac4::BedChannel, 5> channels = {
        ac4::BedChannel::kLeft, ac4::BedChannel::kRight, ac4::BedChannel::kCentre,
        ac4::BedChannel::kLeftSurround, ac4::BedChannel::kRightSurround};
    for (const std::string_view name : {"listen-e9-ajoc-computed", "listen-e9-ajoc-static-5_1", "listen-e9-direct"}) {
        const bool is_direct = name == "listen-e9-direct";
        Case c{.name = std::string{name}, .objects = {}, .kbps = 384};
        std::vector<std::vector<float>> input;
        c.objects.objects.push_back(dynamic_at(0.0));
        input.push_back(tone(1000.0, kLength, 0.2));
        for (std::size_t k = 0; k < speakers.size(); ++k) {
            c.objects.objects.push_back(is_direct || name == "listen-e9-ajoc-static-5_1"
                                            ? dynamic_at(speakers[k][0], speakers[k][1])
                                            : bed(channels[k]));
            input.push_back(tone(220.0 * static_cast<double>(k + 2), kLength, 0.03));
        }
        c.objects.objects.push_back(lfe());
        input.push_back(tone(kLfeHz, kLength, 0.1));
        if (is_direct) {
            c.objects.coding = ac4::ObjectCoding::kDirect;
        } else if (name == "listen-e9-ajoc-static-5_1") {
            c.objects.downmix = ac4::AjocDownmix::kStatic51;
        } else {
            c.objects.downmix_signals = 4;
        }
        // The crossing: from X 0 to 1 in steps a tenth of a second apart,
        // each ramped over its step, from the first second to the ninth.
        std::vector<ac4::ObjectMetadataUpdate> updates;
        for (int step = 0; step <= 80; ++step) {
            ac4::ObjectMetadataUpdate u;
            u.object = 0;
            u.sample = 48000 + step * 4800;
            u.ramp_samples = 2048;
            u.properties.position = {step / 80.0, 0.0, 0.0};
            updates.push_back(u);
        }
        const Encoded encoded = encode(config_of(c), input, updates);
        write_file(fs::path{dir} / (c.name + ".ac4"), sync_framed(encoded));
    }
}
