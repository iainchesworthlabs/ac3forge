// Dialogue enhancement (src/ac4dec/src/pcm/de.hpp, ETSI TS 103 190-1 V1.4.1
// clause 5.7.8): Tables 209, 210 and 172 and the rendering vector; at 0 dB the
// tool leaves the matrices as bypassing it would; at its cap it applies the
// gains its parameters give to 0.01 dB, measured on known input, in each
// method; the matrices interpolate from frame to frame; and through
// ac4::Decoder, DEE's parameters leave the output alone at 0 dB and raise it
// at the cap.

#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac4/ac4.hpp"
#include "ac4dec/decoder.hpp"
#include "pcm/de.hpp"

namespace {

namespace detail = ac4::detail;
using QmfValue = std::complex<double>;

constexpr int kSlots = 32;
constexpr std::size_t kValues = kSlots * 64;
// Table 173's first subband of each band, and one past the last.
constexpr std::array<int, 9> kBandStart = {0, 1, 2, 4, 7, 11, 17, 27, 41};

int band_of(int subband) {
    for (int band = 0; band < 8; ++band) {
        if (subband < kBandStart[static_cast<std::size_t>(band + 1)]) {
            return band;
        }
    }
    return -1;
}

std::vector<QmfValue> random_matrix(unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> normal;
    std::vector<QmfValue> m(kValues);
    for (QmfValue& v : m) {
        v = {normal(rng), normal(rng)};
    }
    return m;
}

// 5.1's speakers, in the decoder's order.
constexpr std::array<ac4::Speaker, 6> kFiveOne = {
    ac4::Speaker::kLeft, ac4::Speaker::kRight,        ac4::Speaker::kCentre,
    ac4::Speaker::kLfe,  ac4::Speaker::kLeftSurround, ac4::Speaker::kRightSurround};

// A frame of channel-independent parameters: a value per channel and band.
detail::DeFrameValues channel_independent(std::array<bool, 3> processed, double max_gain) {
    detail::DeFrameValues values;
    values.active = true;
    values.method = 0;
    values.max_gain_db = max_gain;
    values.processed = processed;
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t band = 0; band < 8; ++band) {
            values.p[i][band] =
                0.25 + 0.1 * static_cast<double>(band) + 0.3 * static_cast<double>(i);
        }
    }
    return values;
}

struct Channels {
    std::vector<std::vector<QmfValue>> data;
    std::vector<std::vector<QmfValue>*> pointers;

    explicit Channels(std::size_t count, unsigned seed) : data(count) {
        for (std::size_t c = 0; c < count; ++c) {
            data[c] = random_matrix(seed + static_cast<unsigned>(c));
            pointers.push_back(&data[c]);
        }
    }
};

std::vector<std::byte> read_stream(const std::string& leg) {
    const std::filesystem::path path =
        std::filesystem::path{AC3FORGE_GOLDEN_EXTERNAL_BASELINE_DIR} / leg / "dee.ac4";
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    return bytes;
}

std::vector<std::vector<float>> decode_all(std::span<const std::byte> stream,
                                           double enhancement_db) {
    const ac4::ScanResult scan = ac4::scan(stream);
    REQUIRE_FALSE(scan.frames.empty());
    ac4::DecoderConfig config;
    config.output.dialogue_enhancement_db = enhancement_db;
    ac4::Decoder decoder(config);
    std::vector<std::vector<float>> out;
    for (const ac4::SyncFrame& frame : scan.frames) {
        const auto decoded = decoder.decode(frame.raw_ac4_frame);
        INFO(decoder.refusal_reason());
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        const ac4::DecodedFrame& pcm = **decoded;
        out.resize(pcm.channels.size());
        for (std::size_t c = 0; c < pcm.channels.size(); ++c) {
            out[c].insert(out[c].end(), pcm.channels[c].begin(), pcm.channels[c].end());
        }
    }
    return out;
}

}  // namespace

TEST_CASE("Tables 209, 210 and 172 dequantise dialogue enhancement's parameters", "[ac4dec][de]") {
    CHECK(detail::de_parameter(0, false) == 0.0);
    CHECK(std::abs(detail::de_parameter(10, false) - 1.0) < 1e-12);
    CHECK(std::abs(detail::de_parameter(15, false) - 1.5) < 1e-12);
    CHECK(detail::de_parameter(16, false) == 1.75);
    CHECK(detail::de_parameter(17, false) == 2.0);
    CHECK(detail::de_parameter(18, false) == 2.5);
    CHECK(detail::de_parameter(31, false) == 9.0);
    CHECK(std::abs(detail::de_parameter(-30, true) + 3.0) < 1e-12);
    CHECK(std::abs(detail::de_parameter(-1, true) + 0.1) < 1e-12);
    CHECK(std::abs(detail::de_parameter(30, true) - 3.0) < 1e-12);
    CHECK(detail::de_mix_coefficient(0) == 0.0);
    CHECK(detail::de_mix_coefficient(16) == 0.7071);
    CHECK(detail::de_mix_coefficient(31) == 1.0);
    // Clause 5.7.8.5: the last coefficient keeps the vector's energy.
    const auto two = detail::de_rendering(2, 0.6, 0.0);
    CHECK(std::abs(two[0] * two[0] + two[1] * two[1] - 1.0) < 1e-12);
    const auto three = detail::de_rendering(3, 0.6, 0.5);
    CHECK(std::abs(three[0] * three[0] + three[1] * three[1] + three[2] * three[2] - 1.0) < 1e-12);
    CHECK(detail::de_rendering(3, 0.9, 0.9)[2] == 0.0);
    CHECK(detail::de_rendering(1, 0.3, 0.3)[0] == 1.0);
}

TEST_CASE("dialogue enhancement at 0 dB leaves the matrices as the tool bypassed would",
          "[ac4dec][de]") {
    detail::DeStage stage;
    stage.configure(kSlots, kFiveOne);
    const detail::DeFrameValues values = channel_independent({true, true, true}, 9.0);
    for (unsigned frame = 0; frame < 4; ++frame) {
        Channels channels(kFiveOne.size(), 100 + 10 * frame);
        const auto before = channels.data;
        CHECK_FALSE(stage.active(0.0, values));
        stage.process(0.0, values, channels.pointers);
        CHECK(channels.data == before);
    }
}

TEST_CASE("at its cap, dialogue enhancement applies the gains its parameters give, to 0.01 dB",
          "[ac4dec][de]") {
    detail::DeStage stage;
    stage.configure(kSlots, kFiveOne);
    // L and C, capped at 9 dB and asked for 12: g = 10^(9/20) - 1.
    const detail::DeFrameValues values = channel_independent({true, false, true}, 9.0);
    const double g = std::pow(10.0, 9.0 / 20.0) - 1.0;
    // The first frame fades in from the identity; the second holds.
    Channels first(kFiveOne.size(), 7);
    stage.process(12.0, values, first.pointers);
    Channels channels(kFiveOne.size(), 11);
    const auto before = channels.data;
    stage.process(12.0, values, channels.pointers);
    for (std::size_t c = 0; c < kFiveOne.size(); ++c) {
        for (int slot = 0; slot < kSlots; ++slot) {
            for (int k = 0; k < 64; ++k) {
                const std::size_t at = static_cast<std::size_t>(slot * 64 + k);
                const int band = band_of(k);
                double expected = 1.0;
                if (band >= 0 && c == 0) {
                    expected = 1.0 + g * values.p[0][static_cast<std::size_t>(band)];
                } else if (band >= 0 && c == 2) {
                    expected = 1.0 + g * values.p[1][static_cast<std::size_t>(band)];
                }
                const double got = std::abs(channels.data[c][at]) / std::abs(before[c][at]);
                if (std::abs(20.0 * std::log10(got / expected)) >= 0.01) {
                    FAIL("channel " << c << " slot " << slot << " subband " << k << ": " << got
                                    << ", expected " << expected);
                }
            }
        }
    }
}

TEST_CASE("with de_ms_proc_flag, dialogue enhancement raises the Mid and leaves the Side",
          "[ac4dec][de]") {
    const std::array<ac4::Speaker, 2> stereo = {ac4::Speaker::kLeft, ac4::Speaker::kRight};
    detail::DeStage stage;
    stage.configure(kSlots, stereo);
    detail::DeFrameValues values = channel_independent({true, true, false}, 6.0);
    values.ms = true;
    const double g = std::pow(10.0, 6.0 / 20.0) - 1.0;
    for (int pass = 0; pass < 2; ++pass) {
        // Mid in the first half of the subbands, Side in the second.
        std::vector<QmfValue> left(kValues);
        std::vector<QmfValue> right(kValues);
        for (std::size_t i = 0; i < kValues; ++i) {
            const bool mid = i % 64 < 32;
            left[i] = {1.0, 0.5};
            right[i] = mid ? left[i] : -left[i];
        }
        std::array<std::vector<QmfValue>*, 2> matrices = {&left, &right};
        stage.process(6.0, values, matrices);
        if (pass == 0) {
            continue;  // the fade in
        }
        for (std::size_t i = 0; i < kValues; ++i) {
            const int k = static_cast<int>(i % 64);
            const int band = band_of(k);
            const double expected =
                (k < 32 && band >= 0) ? 1.0 + g * values.p[0][static_cast<std::size_t>(band)] : 1.0;
            CHECK(std::abs(std::abs(left[i]) / std::abs(QmfValue{1.0, 0.5}) - expected) < 1e-12);
            CHECK(std::abs(std::abs(right[i]) / std::abs(QmfValue{1.0, 0.5}) - expected) < 1e-12);
        }
    }
}

TEST_CASE("cross-channel dialogue enhancement adds g r p^T m to the processed channels",
          "[ac4dec][de]") {
    detail::DeStage stage;
    stage.configure(kSlots, kFiveOne);
    detail::DeFrameValues values;
    values.active = true;
    values.method = 1;
    values.max_gain_db = 12.0;
    values.processed = {true, true, true};
    values.r = detail::de_rendering(3, 0.6, 0.5);
    for (std::size_t band = 0; band < 8; ++band) {
        values.p[0][band] = 0.3;
        values.p[1][band] = -0.2;
        values.p[2][band] = 0.1 * static_cast<double>(band);
    }
    const double g = std::pow(10.0, 12.0 / 20.0) - 1.0;
    Channels first(kFiveOne.size(), 3);
    stage.process(12.0, values, first.pointers);
    Channels channels(kFiveOne.size(), 5);
    const auto m = channels.data;
    stage.process(12.0, values, channels.pointers);
    // L, R and C are the decoder's channels 0, 1 and 2.
    for (std::size_t at = 0; at < kValues; ++at) {
        const int band = band_of(static_cast<int>(at % 64));
        for (std::size_t i = 0; i < 3; ++i) {
            QmfValue expected = m[i][at];
            if (band >= 0) {
                QmfValue dialogue{};
                for (std::size_t j = 0; j < 3; ++j) {
                    dialogue += values.p[j][static_cast<std::size_t>(band)] * m[j][at];
                }
                expected += g * values.r[i] * dialogue;
            }
            CHECK(std::abs(channels.data[i][at] - expected) < 1e-12);
        }
        // The LFE and the surrounds take no part.
        CHECK(channels.data[3][at] == m[3][at]);
        CHECK(channels.data[5][at] == m[5][at]);
    }
}

TEST_CASE("dialogue enhancement moves from one frame's matrix to the next slot by slot",
          "[ac4dec][de]") {
    const std::array<ac4::Speaker, 1> mono = {ac4::Speaker::kCentre};
    detail::DeStage stage;
    stage.configure(kSlots, mono);
    const detail::DeFrameValues values = channel_independent({false, false, true}, 12.0);
    const double g = std::pow(10.0, 12.0 / 20.0) - 1.0;
    std::vector<QmfValue> centre(kValues, QmfValue{1.0, 0.0});
    std::array<std::vector<QmfValue>*, 1> matrices = {&centre};
    // From the identity: slot n takes (n + 1/2) / 32 of this frame's matrix.
    stage.process(12.0, values, matrices);
    for (int slot = 0; slot < kSlots; ++slot) {
        const double w = (slot + 0.5) / kSlots;
        const double expected = 1.0 + w * g * values.p[0][3];
        CHECK(std::abs(centre[static_cast<std::size_t>(slot * 64 + 5)].real() - expected) <
              1e-12);  // band 3
    }
}

TEST_CASE("DEE's dialogue enhancement leaves the output alone at 0 dB and raises it at its cap",
          "[ac4dec][de]") {
    // Speech: DEE sends channel-independent parameters for the pair it
    // detects dialogue in, capped at 9 dB.
    const std::vector<std::byte> stream = read_stream("ac4-20-speech-128");
    const auto plain = decode_all(stream, 0.0);
    ac4::Decoder bypassed;
    const auto enhanced = decode_all(stream, 9.0);
    REQUIRE(enhanced.size() == plain.size());
    // At 0 dB the output is the default decode's, sample for sample.
    const ac4::ScanResult scan = ac4::scan(stream);
    std::vector<float> reference;
    for (const ac4::SyncFrame& frame : scan.frames) {
        const auto decoded = bypassed.decode(frame.raw_ac4_frame);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        reference.insert(reference.end(), (**decoded).channels[0].begin(),
                         (**decoded).channels[0].end());
    }
    CHECK(plain[0] == reference);
    // At the cap the speech comes out louder.
    const auto energy = [](const std::vector<float>& x) {
        double sum = 0.0;
        for (const float v : x) {
            sum += static_cast<double>(v) * static_cast<double>(v);
        }
        return sum;
    };
    CHECK(10.0 * std::log10(energy(enhanced[0]) / energy(plain[0])) > 1.0);
}
