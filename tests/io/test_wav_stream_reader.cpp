#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include "ac3/io/wav.hpp"

// WavStreamReader exists so an encode of a feature-length input holds one
// block of samples resident instead of the whole file twice over (raw bytes
// plus the planar float copy - see read_wav's own doc comment). Its contract
// is sample-for-sample equality with read_wav on the same file: these tests
// pin that equality on every supported format, plus the block-boundary
// arithmetic a whole-file reader never has to get right.

namespace {

struct TempWav {
    std::string path;
    explicit TempWav(const char* name) {
        path = (std::filesystem::temp_directory_path() / name).string();
    }
    ~TempWav() { std::remove(path.c_str()); }
};

std::vector<std::vector<float>> tone_channels(std::size_t channels, std::size_t frames) {
    std::vector<std::vector<float>> out(channels, std::vector<float>(frames));
    for (std::size_t ch = 0; ch < channels; ++ch) {
        for (std::size_t i = 0; i < frames; ++i) {
            const double f = 220.0 * static_cast<double>(ch + 1);
            out[ch][i] = static_cast<float>(
                0.4 * std::sin(2.0 * std::numbers::pi * f * static_cast<double>(i) / 48000.0));
        }
    }
    return out;
}

void put_le16(std::string& out, std::uint16_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
}

void put_le32(std::string& out, std::uint32_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
}

}  // namespace

TEST_CASE("WavStreamReader matches read_wav block by block on a float32 file", "[wav]") {
    // 2500 frames: not a multiple of any of the block sizes below, so the
    // final short read is exercised too.
    const auto channels = tone_channels(3, 2500);
    TempWav wav{"ac3_wsr_f32.wav"};
    REQUIRE(ac3::io::write_wav_f32(wav.path, channels, 48000).has_value());

    const auto whole = ac3::io::read_wav(wav.path);
    REQUIRE(whole.has_value());

    ac3::io::WavStreamReader reader;
    REQUIRE(reader.open(wav.path).has_value());
    REQUIRE(reader.is_open());
    CHECK(reader.sample_rate() == 48000);
    CHECK(reader.channels() == 3);
    CHECK(reader.frame_count() == whole->frame_count());

    // Deliberately awkward block sizes, including one larger than the file.
    for (const std::size_t block : {7uz, 512uz, 1536uz, 4096uz}) {
        REQUIRE(reader.open(wav.path).has_value());  // re-open rewinds
        std::vector<std::vector<float>> planar(3, std::vector<float>(block));
        std::vector<std::span<float>> views(planar.begin(), planar.end());
        std::size_t at = 0;
        while (true) {
            const auto got = reader.read_planar(views, block);
            REQUIRE(got.has_value());
            if (*got == 0) {
                break;
            }
            for (std::size_t ch = 0; ch < 3; ++ch) {
                for (std::size_t i = 0; i < *got; ++i) {
                    REQUIRE(planar[ch][i] == whole->channels[ch][at + i]);
                }
            }
            at += *got;
        }
        REQUIRE(at == whole->frame_count());
    }
}

TEST_CASE("WavStreamReader converts PCM16 exactly as read_wav does", "[wav]") {
    // A PCM16 file via the raw-payload writer: every 16-bit value from a
    // deterministic walk, two channels interleaved.
    constexpr std::size_t kFrames = 999;
    std::vector<std::byte> payload(kFrames * 2 * 2);
    for (std::size_t i = 0; i < payload.size(); i += 2) {
        const auto value = static_cast<std::uint16_t>((i * 2654435761u) >> 8);
        payload[i] = static_cast<std::byte>(value & 0xFF);
        payload[i + 1] = static_cast<std::byte>(value >> 8);
    }
    TempWav wav{"ac3_wsr_pcm16.wav"};
    REQUIRE(ac3::io::write_wav_pcm16_raw(wav.path, payload, 48000, 2).has_value());

    const auto whole = ac3::io::read_wav(wav.path);
    REQUIRE(whole.has_value());
    REQUIRE(whole->frame_count() == kFrames);

    ac3::io::WavStreamReader reader;
    REQUIRE(reader.open(wav.path).has_value());
    CHECK(reader.channels() == 2);
    CHECK(reader.frame_count() == kFrames);

    std::vector<std::vector<float>> planar(2, std::vector<float>(256));
    std::vector<std::span<float>> views(planar.begin(), planar.end());
    std::size_t at = 0;
    while (true) {
        const auto got = reader.read_planar(views, 256);
        REQUIRE(got.has_value());
        if (*got == 0) {
            break;
        }
        for (std::size_t ch = 0; ch < 2; ++ch) {
            for (std::size_t i = 0; i < *got; ++i) {
                REQUIRE(planar[ch][i] == whole->channels[ch][at + i]);
            }
        }
        at += *got;
    }
    REQUIRE(at == kFrames);
}

TEST_CASE("WavStreamReader matches read_wav on every widened sample format", "[wav]") {
    // The depths this project never writes: no in-repo writer to round-trip
    // against, so the file is assembled by hand and the two readers are
    // checked against each other sample for sample. That is the contract
    // that matters here - a caller must be able to swap one for the other -
    // and test_wav.cpp separately pins each depth's absolute scaling.
    constexpr std::size_t kFrames = 777;
    constexpr std::uint16_t kChannels = 2;

    struct Case {
        const char* name;
        std::uint16_t format_tag;
        std::uint16_t bits;
    };
    const auto cases = std::array<Case, 4>{{{"ac3_wsr_pcm8.wav", 1, 8},
                                            {"ac3_wsr_pcm24.wav", 1, 24},
                                            {"ac3_wsr_pcm32.wav", 1, 32},
                                            {"ac3_wsr_f64.wav", 3, 64}}};

    for (const auto& one : cases) {
        INFO(one.name);
        const std::size_t width = one.bits / 8u;
        std::string payload;
        for (std::size_t i = 0; i < kFrames * kChannels; ++i) {
            // A deterministic walk over the whole container range, so the
            // sign bit and the low bytes are both exercised at every width.
            const auto word = static_cast<std::uint64_t>(i * 2654435761u) ^ (i << 21);
            for (std::size_t b = 0; b < width; ++b) {
                payload.push_back(static_cast<char>((word >> (8 * b)) & 0xFF));
            }
        }

        std::string fmt;
        put_le16(fmt, one.format_tag);
        put_le16(fmt, kChannels);
        put_le32(fmt, 48000);
        put_le32(fmt, static_cast<std::uint32_t>(48000 * kChannels * width));
        put_le16(fmt, static_cast<std::uint16_t>(kChannels * width));
        put_le16(fmt, one.bits);

        std::string file;
        file += "RIFF";
        put_le32(file, static_cast<std::uint32_t>(4 + 8 + fmt.size() + 8 + payload.size()));
        file += "WAVE";
        file += "fmt ";
        put_le32(file, static_cast<std::uint32_t>(fmt.size()));
        file += fmt;
        file += "data";
        put_le32(file, static_cast<std::uint32_t>(payload.size()));
        file += payload;

        TempWav wav{one.name};
        {
            std::ofstream out{wav.path, std::ios::binary};
            out.write(file.data(), static_cast<std::streamsize>(file.size()));
        }

        const auto whole = ac3::io::read_wav(wav.path);
        REQUIRE(whole.has_value());
        REQUIRE(whole->frame_count() == kFrames);

        ac3::io::WavStreamReader reader;
        REQUIRE(reader.open(wav.path).has_value());
        CHECK(reader.channels() == kChannels);
        CHECK(reader.frame_count() == kFrames);

        std::vector<std::vector<float>> planar(kChannels, std::vector<float>(256));
        std::vector<std::span<float>> views(planar.begin(), planar.end());
        std::size_t at = 0;
        while (true) {
            const auto got = reader.read_planar(views, 256);
            REQUIRE(got.has_value());
            if (*got == 0) {
                break;
            }
            for (std::size_t ch = 0; ch < kChannels; ++ch) {
                for (std::size_t i = 0; i < *got; ++i) {
                    REQUIRE(planar[ch][i] == whole->channels[ch][at + i]);
                }
            }
            at += *got;
        }
        REQUIRE(at == kFrames);
    }
}

TEST_CASE("WavStreamReader refuses what read_wav refuses", "[wav]") {
    ac3::io::WavStreamReader reader;

    SECTION("missing file") {
        const auto result = reader.open("ac3_wsr_does_not_exist.wav");
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ac3::io::WavError::kCannotOpen);
        CHECK_FALSE(reader.is_open());
    }

    SECTION("not a WAV") {
        TempWav bogus{"ac3_wsr_bogus.wav"};
        {
            std::vector<std::vector<float>> one(1, std::vector<float>(4, 0.0f));
            REQUIRE(ac3::io::write_wav_f32(bogus.path, one, 48000).has_value());
        }
        // Overwrite the RIFF tag.
        {
            std::fstream f{bogus.path, std::ios::binary | std::ios::in | std::ios::out};
            f.write("JUNK", 4);
        }
        const auto result = reader.open(bogus.path);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ac3::io::WavError::kNotRiffWave);
    }

    SECTION("reading while closed") {
        std::vector<float> buffer(16, 0.0f);
        const std::array<std::span<float>, 1> views{std::span{buffer}};
        const auto result = reader.read_planar(views, 16);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ac3::io::WavError::kCannotOpen);
    }
}

TEST_CASE("WavStreamReader clamps a data chunk that claims more than the file holds", "[wav]") {
    const auto channels = tone_channels(2, 300);
    TempWav wav{"ac3_wsr_clamped.wav"};
    REQUIRE(ac3::io::write_wav_f32(wav.path, channels, 48000).has_value());
    // Chop the last 100 frames' bytes off the file; the header still claims
    // 300 frames. read_wav yields the 200 real ones - so must this.
    std::filesystem::resize_file(wav.path,
                                 std::filesystem::file_size(wav.path) - 100 * 2 * 4);

    const auto whole = ac3::io::read_wav(wav.path);
    REQUIRE(whole.has_value());
    REQUIRE(whole->frame_count() == 200);

    ac3::io::WavStreamReader reader;
    REQUIRE(reader.open(wav.path).has_value());
    CHECK(reader.frame_count() == 200);

    std::vector<std::vector<float>> planar(2, std::vector<float>(1536));
    std::vector<std::span<float>> views(planar.begin(), planar.end());
    const auto got = reader.read_planar(views, 1536);
    REQUIRE(got.has_value());
    CHECK(*got == 200);
    const auto done = reader.read_planar(views, 1536);
    REQUIRE(done.has_value());
    CHECK(*done == 0);
}
