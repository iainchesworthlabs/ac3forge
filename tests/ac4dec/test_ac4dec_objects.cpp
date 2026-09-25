// The decoder on the object audio streams of ac4dec_objects.hpp: A-JOC
// substreams over a var_channel_element() or a static 5.X downmix, and
// direct-coded dynamic objects, a bed and an intermediate spatial format, with
// their object audio metadata. Each stream reads with the writer's trace,
// record for record, every substream to its end.
//
// The streams under tests/golden/ac4dec/objects/ are the committed cases, byte
// for byte, and tests/golden/ac4dec/ holds tools/references/ac4_syntax.py's
// digests of them, which test_ac4dec_syntax.cpp holds the decoder to. With
// AC4DEC_WRITE_OBJECTS set to a directory, this writes the committed cases
// there instead of comparing them, to commit after a change to the builder.

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "ac4/ac4.hpp"
#include "ac4dec/decoder.hpp"
#include "ac4dec_objects.hpp"
#include "pcm/isf.hpp"
#include "tables/isf_tables.hpp"

namespace {

namespace fs = std::filesystem;
using ac4dec_test::BuiltObjectStream;
using ac4dec_test::ObjectCase;

constexpr int kFrames = 8;

// Every frame read, with the decoder's records the writer's - the same
// substreams, offsets, widths and values, in the same order - and every
// substream read to its end.
void parse_checked(const BuiltObjectStream& stream) {
    std::vector<ac4::SyntaxRecord> read;
    const auto keep = [&read](const ac4::SyntaxRecord& record) { read.push_back(record); };
    ac4::DecoderConfig config;
    config.syntax = keep;
    ac4::Decoder decoder(config);
    for (std::size_t f = 0; f < stream.frames.size(); ++f) {
        read.clear();
        const auto report = decoder.parse(stream.frames[f]);
        INFO("frame " << f);
        REQUIRE(report.has_value());
        for (const ac4::SubstreamReport& s : report->substreams) {
            INFO("substream " << s.index << ": " << s.refused_reason);
            CHECK_FALSE(s.refused.has_value());
            CHECK(s.bits_read == s.size_bits);
        }
        const std::vector<ac4::SyntaxRecord>& written = stream.traces[f];
        for (std::size_t i = 0; i < std::min(read.size(), written.size()); ++i) {
            const bool same = read[i].substream == written[i].substream &&
                              read[i].bit_offset == written[i].bit_offset &&
                              read[i].bits == written[i].bits && read[i].value == written[i].value;
            if (!same) {
                CAPTURE(i, written[i].name, read[i].name, written[i].substream, read[i].substream,
                        written[i].bit_offset, read[i].bit_offset, written[i].value, read[i].value);
                REQUIRE(same);
            }
        }
        REQUIRE(read.size() == written.size());
    }
}

// What decode() put out over a stream: each object's samples end to end, and
// the updates of its metadata in order.
struct Decoded {
    std::vector<std::vector<float>> samples;
    std::vector<std::vector<ac4::ObjectUpdate>> updates;
    std::vector<std::vector<std::size_t>> update_at;  // each update's sample in the whole output
    std::vector<ac4::DecodedObject> last;  // the last frame's objects, their samples dropped
};

Decoded decode_all(const BuiltObjectStream& stream, ac4::DecodingMode decoding,
                   double dialogue_db = 0.0) {
    ac4::DecoderConfig config;
    config.decoding = decoding;
    config.output.dialogue_enhancement_db = dialogue_db;
    ac4::Decoder decoder(config);
    Decoded out;
    for (std::size_t f = 0; f < stream.frames.size(); ++f) {
        const auto decoded = decoder.decode(stream.frames[f]);
        INFO("frame " << f << ": " << decoder.refusal_reason());
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        const ac4::DecodedFrame& frame = **decoded;
        if (out.samples.empty()) {
            out.samples.resize(frame.objects.size());
            out.updates.resize(frame.objects.size());
            out.update_at.resize(frame.objects.size());
        }
        REQUIRE(frame.objects.size() == out.samples.size());
        for (std::size_t o = 0; o < frame.objects.size(); ++o) {
            const ac4::DecodedObject& object = frame.objects[o];
            const std::size_t start = out.samples[o].size();
            out.samples[o].insert(out.samples[o].end(), object.samples.begin(),
                                  object.samples.end());
            out.updates[o].insert(out.updates[o].end(), object.updates.begin(),
                                  object.updates.end());
            for (const ac4::ObjectUpdate& update : object.updates) {
                out.update_at[o].push_back(start + update.sample);
            }
        }
        out.last = frame.objects;
        for (ac4::DecodedObject& object : out.last) {
            object.samples.clear();
        }
    }
    return out;
}

// The amplitude of `samples`' component at `hz`, through a Hann window.
double tone_amplitude(std::span<const float> samples, double hz) {
    const double w = 2.0 * std::numbers::pi * hz / 48000.0;
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
    return 2.0 * std::abs(sum) / weights;
}

// The samples after the first `skip` frames: the decoder's delay, and A-JOC's
// first ramp.
std::span<const float> steady(const std::vector<float>& samples, std::size_t skip = 4) {
    const std::size_t from = std::min(samples.size(), skip * 2048);
    return std::span<const float>(samples).subspan(from);
}

// Every object carries the tones the builder says at their amplitudes (within
// `tolerance_db`), and none of the other downmix or object tones; an object
// that takes a decorrelator's output carries its tones, at a level the check
// leaves alone.
void check_objects(const std::vector<ac4dec_test::ExpectedObject>& expected, const Decoded& decoded,
                   const std::vector<double>& all_tones, double tolerance_db) {
    REQUIRE(decoded.samples.size() == expected.size());
    for (std::size_t o = 0; o < expected.size(); ++o) {
        CAPTURE(o);
        const std::span<const float> s = steady(decoded.samples[o]);
        for (const auto& [hz, amplitude] : expected[o].tones) {
            CAPTURE(hz, amplitude);
            const double got = tone_amplitude(s, hz);
            if (expected[o].decorrelated) {
                CHECK(got > amplitude * 0.1);
            } else {
                CHECK(std::abs(20.0 * std::log10(got / amplitude)) < tolerance_db);
            }
        }
        for (const double hz : all_tones) {
            const bool own =
                std::ranges::any_of(expected[o].tones, [hz](const auto& t) { return t[0] == hz; });
            if (!own) {
                CAPTURE(hz);
                CHECK(tone_amplitude(s, hz) < 1e-4);
            }
        }
    }
}

std::vector<double> tones_of(const BuiltObjectStream& stream) {
    std::vector<double> tones;
    for (const auto& list : {stream.full, stream.core}) {
        for (const ac4dec_test::ExpectedObject& object : list) {
            for (const auto& tone : object.tones) {
                if (std::ranges::find(tones, tone[0]) == tones.end()) {
                    tones.push_back(tone[0]);
                }
            }
        }
    }
    return tones;
}

std::vector<std::byte> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    return bytes;
}

}  // namespace

TEST_CASE("object audio streams read as the encoder's writer wrote them", "[ac4dec][objects]") {
    for (const ObjectCase& c : ac4dec_test::committed_object_cases()) {
        CAPTURE(c.name);
        parse_checked(ac4dec_test::build_objects(c, kFrames));
    }
}

TEST_CASE("A-JOC downmixes of one to seven signals read in both codec modes", "[ac4dec][objects]") {
    // Every var_channel_element() shape: one signal, pairs, and an odd count
    // over each var_coding_config, SIMPLE and ASPX (companding_control() up
    // to five signals), with and without the LFE.
    for (int dmx = 1; dmx <= 7; ++dmx) {
        for (const bool aspx : {false, true}) {
            for (const int config : {0, 1}) {
                if (config == 1 && (dmx % 2 == 0 || dmx == 1)) {
                    continue;
                }
                ObjectCase c;
                c.name = "var";
                c.dmx = dmx;
                c.umx = dmx + 1;
                c.aspx = aspx;
                c.lfe = dmx % 2 == 0;
                c.var_coding_config = config;
                CAPTURE(dmx, aspx, config);
                parse_checked(ac4dec_test::build_objects(c, 5));
            }
        }
    }
}

TEST_CASE("the committed object streams are the builder's", "[ac4dec][objects]") {
    const fs::path committed = fs::path{AC4DEC_GOLDEN_DIR} / "objects";
    const char* write_to = std::getenv("AC4DEC_WRITE_OBJECTS");
    for (const ObjectCase& c : ac4dec_test::committed_object_cases()) {
        CAPTURE(c.name);
        const BuiltObjectStream stream =
            ac4dec_test::build_objects(c, ac4dec_test::kCommittedObjectFrames);
        const std::vector<std::byte> bytes = ac4dec_test::sync_framed(stream);
        if (write_to != nullptr) {
            fs::create_directories(write_to);
            std::ofstream out(fs::path{write_to} / (c.name + ".ac4"), std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
            REQUIRE(out.good());
            continue;
        }
        CHECK(read_file(committed / (c.name + ".ac4")) == bytes);
    }
}

TEST_CASE("A-JOC objects carry their dry coefficients' share of each downmix signal",
          "[ac4dec][objects]") {
    // Pseudocode 18 end to end: each object the sum of its coefficients times
    // the downmix's tones, in QinAJOC's order (Pseudocode 14a), with Table 28's
    // band of each tone; and in core decoding the downmix itself.
    for (const ObjectCase& c : ac4dec_test::committed_object_cases()) {
        if (c.kind != ObjectCase::Kind::kAjoc && c.kind != ObjectCase::Kind::kAjocStatic) {
            continue;
        }
        CAPTURE(c.name);
        const BuiltObjectStream stream = ac4dec_test::build_objects(c, 12);
        const std::vector<double> tones = tones_of(stream);
        check_objects(stream.full, decode_all(stream, ac4::DecodingMode::kFull), tones, 0.1);
        check_objects(stream.core, decode_all(stream, ac4::DecodingMode::kCore), tones, 0.1);
    }
}

TEST_CASE("direct-coded objects and a bed carry their own tones", "[ac4dec][objects]") {
    for (const ObjectCase& c : ac4dec_test::committed_object_cases()) {
        if (c.kind != ObjectCase::Kind::kDynamic && c.kind != ObjectCase::Kind::kBed) {
            continue;
        }
        CAPTURE(c.name);
        const BuiltObjectStream stream = ac4dec_test::build_objects(c, 12);
        const std::vector<double> tones = tones_of(stream);
        for (const ac4::DecodingMode mode : {ac4::DecodingMode::kFull, ac4::DecodingMode::kCore}) {
            check_objects(stream.full, decode_all(stream, mode), tones, 0.1);
        }
    }
}

TEST_CASE("A-JOC dialogue enhancement raises the dialogue object and its share of the core",
          "[ac4dec][objects]") {
    // Part 2 clause 5.8.2.3: in full decoding the dialogue object's dry and
    // wet coefficients times de_gain = 10^(min(G_DE, Gmax) / 20), Gmax (1 +
    // de_max_gain) x 3 dB, 9 dB for the builder's 2. Clause 5.8.2.4: in core
    // decoding each downmix signal plus (de_gain - 1) times its dialogue
    // downmix coefficient (Table 82) times the dialogue object as A-JOC's dry
    // matrix makes it from the downmix (Pseudocodes 23 and 24).
    ObjectCase c;
    c.name = "dialogue";
    c.kind = ObjectCase::Kind::kAjoc;
    c.dmx = 2;
    c.umx = 4;
    c.dialogue = true;
    const BuiltObjectStream stream = ac4dec_test::build_objects(c, 12);
    const std::vector<double> tones = tones_of(stream);
    REQUIRE(stream.core.size() == 2);
    const double amplitude = stream.core.front().tones.front()[1];
    for (const auto& [asked, applied] : {std::pair{6.0, 6.0}, std::pair{12.0, 9.0}}) {
        CAPTURE(asked);
        const double de_gain = std::pow(10.0, applied / 20.0);
        std::vector<ac4dec_test::ExpectedObject> full = stream.full;
        for (auto& tone : full.front().tones) {
            tone[1] *= de_gain;
        }
        check_objects(full, decode_all(stream, ac4::DecodingMode::kFull, asked), tones, 0.1);
        std::vector<ac4dec_test::ExpectedObject> core(stream.core.size());
        for (int ch = 0; ch < 2; ++ch) {
            for (int in = 0; in < 2; ++in) {
                const double a =
                    (ch == in ? 1.0 : 0.0) + (de_gain - 1.0) *
                                                 ac4dec_test::ajoc_dialogue_dmx_coefficient(c, ch) *
                                                 ac4dec_test::ajoc_dry_coefficient(c, 0, in);
                if (std::abs(a) > 1e-9) {
                    core[static_cast<std::size_t>(ch)].tones.push_back(
                        {ac4dec_test::ajoc_input_tone_hz(c, in), std::abs(a) * amplitude});
                }
            }
        }
        check_objects(core, decode_all(stream, ac4::DecodingMode::kCore, asked), tones, 0.1);
    }
}

TEST_CASE("a direct-coded dialogue substream's objects take dialogue enhancement up to its cap",
          "[ac4dec][objects]") {
    // Part 2 clause 5.8.2.5: de_gain = 10^(min(G_DE, Gmax) / 20) on every
    // object of a dialogue substream, Gmax 3 x (1 + dialog_max_gain) dB, 9 dB
    // for the builder's dialog_max_gain of 2.
    ObjectCase c;
    for (const ObjectCase& committed : ac4dec_test::committed_object_cases()) {
        if (committed.kind == ObjectCase::Kind::kDynamic) {
            c = committed;
        }
    }
    REQUIRE(c.kind == ObjectCase::Kind::kDynamic);
    c.dialogue = true;
    const BuiltObjectStream stream = ac4dec_test::build_objects(c, 12);
    const std::vector<double> tones = tones_of(stream);
    for (const auto& [asked, applied] :
         {std::pair{0.0, 0.0}, std::pair{6.0, 6.0}, std::pair{12.0, 9.0}}) {
        CAPTURE(asked);
        std::vector<ac4dec_test::ExpectedObject> expected = stream.full;
        for (ac4dec_test::ExpectedObject& object : expected) {
            for (auto& tone : object.tones) {
                tone[1] *= std::pow(10.0, applied / 20.0);
            }
        }
        check_objects(expected, decode_all(stream, ac4::DecodingMode::kFull, asked), tones, 0.1);
    }
}

TEST_CASE("object metadata takes effect at its update sample and moves object 0",
          "[ac4dec][objects]") {
    // Clause 5.9.2: block n of a frame's metadata takes effect sample_offset +
    // 32 x block_offset_factor into its codec frame, which comes out the
    // decoder's delay later, 1 313 samples at frame_rate_index 13; each block's
    // position is 6.3.9.8.4's, explicit or as a difference from the last.
    constexpr std::size_t kDelay = 1313;
    for (const ObjectCase& c : ac4dec_test::committed_object_cases()) {
        if (c.kind == ObjectCase::Kind::kBed || c.kind == ObjectCase::Kind::kIsf) {
            continue;
        }
        CAPTURE(c.name);
        const BuiltObjectStream stream = ac4dec_test::build_objects(c, 12);
        const Decoded decoded = decode_all(stream, ac4::DecodingMode::kFull);
        std::size_t moving = 0;
        int dynamic = -1;
        for (std::size_t o = 0; o < stream.full.size(); ++o) {
            const ac4dec_test::ExpectedObject& expected = stream.full[o];
            if (expected.positions.empty()) {
                continue;
            }
            ++dynamic;
            CAPTURE(o);
            // The builder's extras give every dynamic object but the first
            // extended precision in its I-frames' first block, +2 in X and -2
            // in Y (Tables 123 and 124), which the blocks after it reuse.
            const double ext_x = c.extras && dynamic != 0 ? 2.0 / 310.0 : 0.0;
            const double ext_y = c.extras && dynamic != 0 ? -2.0 / 310.0 : 0.0;
            const std::vector<ac4::ObjectUpdate>& updates = decoded.updates[o];
            // Every frame's blocks but the last frame's, which the delay holds
            // back past the stream's end.
            REQUIRE(updates.size() >=
                    (stream.frames.size() - 1) * static_cast<std::size_t>(c.blocks));
            for (std::size_t u = 0; u < updates.size(); ++u) {
                const std::size_t f = u / static_cast<std::size_t>(c.blocks);
                const std::size_t b = u % static_cast<std::size_t>(c.blocks);
                CAPTURE(u, f, b);
                const auto when = static_cast<std::size_t>(stream.update_samples[f][b]);
                CHECK(decoded.update_at[o][u] == f * 2048 + kDelay + when);
                if (b == 0) {
                    CHECK(updates[u].ramp_samples == 512);
                }
                if (b + 1 == static_cast<std::size_t>(c.blocks)) {
                    const std::array<int, 4>& pos = expected.positions[f];
                    const ac4::ObjectProperties& p = updates[u].properties;
                    CHECK(p.position[0] ==
                          Catch::Approx(std::clamp(pos[0] / 62.0 + ext_x, 0.0, 1.0)).margin(1e-12));
                    CHECK(p.position[1] ==
                          Catch::Approx(std::clamp(pos[1] / 62.0 + ext_y, 0.0, 1.0)).margin(1e-12));
                    CHECK(p.position[2] ==
                          Catch::Approx((pos[2] != 0 ? 1.0 : -1.0) * pos[3] / 15.0).margin(1e-12));
                }
            }
            moving += expected.positions.front() != expected.positions.back() ? 1U : 0U;
        }
        CHECK(moving >= 1);
    }
}

TEST_CASE("an intermediate spatial format renders to the output layout by Annex A.2.1",
          "[ac4dec][objects]") {
    // Clause 5.10.3.4: y = M x t, t the SR3.1.0.0 objects (M1 M2 M3 U1) in
    // order, over the two substreams that carry them, and M the attachment's
    // SR3100_to_<layout>, a row per object and a column per speaker in Table
    // A.27's order without the LFE. Each object's tone reaches each speaker at
    // its coefficient, and at +5 dB where the object's metadata sets that gain.
    using S = ac4::Speaker;
    using T = ac4::DownmixTarget;
    struct Target {
        T target = T::kAsCoded;
        std::size_t matrix = 0;  // the layout's index in kIsfMatrices
        std::vector<S> speakers;
    };
    const std::vector<Target> targets = {
        {T::kAsCoded,
         7,
         {S::kLeft, S::kRight, S::kCentre, S::kLeftSurround, S::kRightSurround, S::kLeftBack,
          S::kRightBack, S::kTopFrontLeft, S::kTopFrontRight, S::kTopBackLeft, S::kTopBackRight}},
        {T::k5X, 1, {S::kLeft, S::kRight, S::kCentre, S::kLeftSurround, S::kRightSurround}},
        {T::kStereo, 0, {S::kLeft, S::kRight}},
        {T::kMono, 0, {S::kCentre}},
        {T::k7X0,
         2,
         {S::kLeft, S::kRight, S::kCentre, S::kLeftSurround, S::kRightSurround, S::kLeftBack,
          S::kRightBack}},
        {T::k5X2,
         4,
         {S::kLeft, S::kRight, S::kCentre, S::kLeftSurround, S::kRightSurround, S::kTopSideLeft,
          S::kTopSideRight}},
        {T::k5X4,
         5,
         {S::kLeft, S::kRight, S::kCentre, S::kLeftSurround, S::kRightSurround, S::kTopFrontLeft,
          S::kTopFrontRight, S::kTopBackLeft, S::kTopBackRight}},
        {T::k7X2,
         6,
         {S::kLeft, S::kRight, S::kCentre, S::kLeftSurround, S::kRightSurround, S::kLeftBack,
          S::kRightBack, S::kTopSideLeft, S::kTopSideRight}},
    };
    ObjectCase isf;
    for (const ObjectCase& c : ac4dec_test::committed_object_cases()) {
        if (c.kind == ObjectCase::Kind::kIsf) {
            isf = c;
        }
    }
    REQUIRE(isf.kind == ObjectCase::Kind::kIsf);
    for (const bool gained : {false, true}) {
        ObjectCase c = isf;
        c.extras = gained;  // object_gain 15 - 10 = 5 dB for every object (Table 102)
        const BuiltObjectStream stream = ac4dec_test::build_objects(c, 12);
        REQUIRE(stream.full.size() == 4);
        const double gain = gained ? std::pow(10.0, 5.0 / 20.0) : 1.0;
        for (const Target& t : targets) {
            CAPTURE(gained, ac4::describe(t.target));
            ac4::DecoderConfig config;
            config.output.downmix = t.target;
            ac4::Decoder decoder(config);
            std::vector<std::vector<float>> channels(t.speakers.size());
            for (std::size_t f = 0; f < stream.frames.size(); ++f) {
                const auto decoded = decoder.decode(stream.frames[f]);
                INFO("frame " << f << ": " << decoder.refusal_reason());
                REQUIRE(decoded.has_value());
                REQUIRE(decoded->has_value());
                const ac4::DecodedFrame& frame = **decoded;
                CHECK(frame.objects.empty());
                REQUIRE(frame.speakers == t.speakers);
                REQUIRE(frame.channels.size() == channels.size());
                for (std::size_t s = 0; s < channels.size(); ++s) {
                    channels[s].insert(channels[s].end(), frame.channels[s].begin(),
                                       frame.channels[s].end());
                }
            }
            const std::span<const float> matrix = ac4::detail::tables::kIsfMatrices[0][t.matrix];
            const std::size_t columns = t.target == T::kMono ? 2 : t.speakers.size();
            REQUIRE(matrix.size() == 4 * columns);
            for (std::size_t s = 0; s < channels.size(); ++s) {
                for (std::size_t i = 0; i < 4; ++i) {
                    CAPTURE(s, i);
                    // The mono channel is the 2.X layout's L + R.
                    const double coefficient = t.target == T::kMono
                                                   ? static_cast<double>(matrix[i * 2]) +
                                                         static_cast<double>(matrix[i * 2 + 1])
                                                   : static_cast<double>(matrix[i * columns + s]);
                    const auto& [hz, amplitude] = stream.full[i].tones.front();
                    const double want = std::abs(coefficient) * amplitude * gain;
                    const double got = tone_amplitude(steady(channels[s]), hz);
                    CHECK(std::abs(got - want) < std::max(0.012 * want, 5e-4 * amplitude));
                }
            }
        }
    }
    // The generator's reading of the attachment, against its text:
    // SR3100_to_5's first and last rows, and SR15951_to_904's last value.
    const std::span<const float> to_5 = ac4::detail::tables::kIsfMatrices[0][1];
    CHECK(to_5[0] == 6.243139852e-01F);
    CHECK(to_5[3] == -2.890952832e-01F);
    CHECK(to_5[17] == 0.0F);
    CHECK(to_5[19] == 3.751586799e-01F);
    CHECK(ac4::detail::tables::kIsfMatrices[5][9].size() == 30 * 13);
}

TEST_CASE("an ISF object's gain ramps linearly from its update sample", "[ac4dec][objects]") {
    // Annex F.11: an update takes effect at its sample, reached over its
    // ramp_duration from the gain in force there; the ramp carries on into
    // the next frame.
    ac4::detail::IsfGain gain;
    std::vector<float> samples(64, 1.0F);
    ac4::ObjectUpdate update;
    update.sample = 8;
    update.ramp_samples = 80;
    update.properties.gain_db = -20.0;
    const std::array<ac4::ObjectUpdate, 1> updates = {update};
    gain.apply(samples, updates);
    CHECK(samples[7] == 1.0F);
    const double step = (0.1 - 1.0) / 80.0;
    CHECK(samples[8] == Catch::Approx(1.0 + step));
    CHECK(samples[63] == Catch::Approx(1.0 + 56.0 * step));
    std::vector<float> next(64, 1.0F);
    gain.apply(next, {});
    CHECK(next[23] == Catch::Approx(0.1));
    CHECK(next[63] == Catch::Approx(0.1));
    // An inactive object is silent from its update, a ramp of 0 at once.
    ac4::ObjectUpdate off;
    off.sample = 4;
    off.properties.active = false;
    const std::array<ac4::ObjectUpdate, 1> offs = {off};
    std::vector<float> last(16, 1.0F);
    gain.apply(last, offs);
    CHECK(last[3] == Catch::Approx(0.1));
    CHECK(last[4] == 0.0F);
}

TEST_CASE("Chromium's A-JOC stream decodes in full and core decoding", "[ac4dec][objects]") {
    // A local check: AC4DEC_AJOC_STREAM names Chromium's ac4-ajoc.ac4, which
    // is not committed. Every frame from the first I-frame decodes, with the
    // objects its table of contents lists - the upmix's in full decoding and
    // the downmix's in core decoding, the LFE first and each bed object at its
    // speaker - each a frame long and finite, with its metadata's updates
    // inside the frame and in order.
    const char* path = std::getenv("AC4DEC_AJOC_STREAM");
    if (path == nullptr) {
        SKIP("AC4DEC_AJOC_STREAM does not name Chromium's ac4-ajoc.ac4");
    }
    const std::vector<std::byte> bytes = read_file(fs::path{path});
    const ac4::ScanResult scan = ac4::scan(bytes);
    REQUIRE_FALSE(scan.frames.empty());
    for (const ac4::DecodingMode mode : {ac4::DecodingMode::kFull, ac4::DecodingMode::kCore}) {
        CAPTURE(ac4::describe(mode));
        const bool full = mode == ac4::DecodingMode::kFull;
        ac4::DecoderConfig config;
        config.decoding = mode;
        ac4::Decoder decoder(config);
        std::size_t decoded_frames = 0;
        for (std::size_t f = 0; f < scan.frames.size(); ++f) {
            CAPTURE(f);
            const auto raw = ac4::parse_raw_frame(scan.frames[f].raw_ac4_frame);
            REQUIRE(raw.has_value());
            const ac4::AjocSubstreamInfo* ajoc = nullptr;
            for (const ac4::SubstreamGroupInfo& group : raw->toc.substream_groups) {
                for (const ac4::GroupSubstream& sub : group.substreams) {
                    if (sub.kind == ac4::GroupSubstream::Kind::kAjoc && sub.ajoc) {
                        ajoc = &*sub.ajoc;
                    }
                }
            }
            REQUIRE(ajoc != nullptr);
            const auto decoded = decoder.decode(scan.frames[f].raw_ac4_frame);
            INFO(decoder.refusal_reason());
            REQUIRE(decoded.has_value());
            if (!decoded->has_value()) {
                CHECK(decoded_frames == 0);  // only while it waits for the first I-frame
                continue;
            }
            ++decoded_frames;
            const ac4::DecodedFrame& frame = **decoded;
            std::vector<ac4::ObjectEntry> listed;
            if (ajoc->b_lfe) {
                listed.push_back({.kind = ac4::ObjectKind::kBed,
                                  .lfe = true,
                                  .ajoc_coded = true,
                                  .speaker = 11});
            }
            if (!full && ajoc->b_static_dmx) {
                for (int s = 0; s < 5; ++s) {
                    listed.push_back({.kind = ac4::ObjectKind::kBed,
                                      .lfe = false,
                                      .ajoc_coded = true,
                                      .speaker = s});
                }
            } else {
                const std::vector<ac4::ObjectEntry>& assigned =
                    full ? ajoc->upmix_objects : ajoc->static_objects;
                listed.insert(listed.end(), assigned.begin(), assigned.end());
                const int signals =
                    full ? ajoc->n_fullband_upmix_signals : ajoc->n_fullband_dmx_signals;
                for (auto k = static_cast<int>(assigned.size()); k < signals; ++k) {
                    listed.push_back({.kind = ac4::ObjectKind::kDyn,
                                      .lfe = false,
                                      .ajoc_coded = true,
                                      .speaker = {}});
                }
            }
            REQUIRE(frame.objects.size() == listed.size());
            REQUIRE_FALSE(frame.objects.empty());
            const std::size_t length = frame.objects.front().samples.size();
            CHECK(length > 0);
            for (std::size_t o = 0; o < listed.size(); ++o) {
                CAPTURE(o);
                const ac4::DecodedObject& object = frame.objects[o];
                CHECK(object.kind == listed[o].kind);
                CHECK(object.lfe == listed[o].lfe);
                CHECK(object.speaker.has_value() == listed[o].speaker.has_value());
                CHECK(object.samples.size() == length);
                CHECK(
                    std::ranges::all_of(object.samples, [](float s) { return std::isfinite(s); }));
                for (std::size_t u = 0; u < object.updates.size(); ++u) {
                    CHECK(object.updates[u].sample < length);
                    if (u > 0) {
                        CHECK(object.updates[u].sample >= object.updates[u - 1].sample);
                    }
                }
            }
        }
        CHECK(decoded_frames + 4 >= scan.frames.size());
    }
}
