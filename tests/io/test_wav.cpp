#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "ac3/io/wav.hpp"

// ac3::io::read_wav/parse_wav (src/forge/src/io/wav.cpp) is the file every
// codec-path test in this suite leans on to get real audio in and decoded
// audio back out - but nothing exercises the parser itself: its RIFF/WAVE
// validation, its integer decode paths (every other test only round-trips
// float32, which write_wav_f32 always produces bit-exact), its
// WAVE_FORMAT_EXTENSIBLE handling, its RF64/BW64 ds64 sizes, or what happens
// when a data chunk's declared size overruns the bytes actually available.
// These tests build the raw RIFF bytes by hand so the parser's own logic -
// not another writer that happens to agree with it - is what is under test.
// That matters most for the depths this project never WRITES: 8/24/32-bit
// integer and 64-bit float have no in-repo writer to round-trip against, so
// every expected value below is worked out from the format definition.

using Catch::Approx;

namespace fs = std::filesystem;

namespace {

// See tests/cli/test_cli.cpp's own scratch_dir for the reasoning this copy
// shares; the leaf name below is this file's own.
fs::path scratch_dir() {
    auto dir = fs::path{AC3FORGE_TEST_SCRATCH_DIR} / "wav_reader";
    fs::create_directories(dir);
    return dir;
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

void put_le24(std::string& out, std::int32_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
}

// A canonical (non-extensible) fmt chunk + data chunk, with `declared_data_
// bytes` written into the data chunk's own size field - independent of how
// many bytes of `payload` are actually appended, so callers can build a file
// whose header lies about its own length.
std::string canonical_wav(std::uint16_t format_tag, std::uint16_t channels,
                          std::uint32_t sample_rate, std::uint16_t bits,
                          const std::string& payload, std::uint32_t declared_data_bytes) {
    const std::uint16_t block_align = static_cast<std::uint16_t>(channels * (bits / 8));
    const std::uint32_t byte_rate = sample_rate * block_align;

    std::string fmt;
    put_le16(fmt, format_tag);
    put_le16(fmt, channels);
    put_le32(fmt, sample_rate);
    put_le32(fmt, byte_rate);
    put_le16(fmt, block_align);
    put_le16(fmt, bits);

    std::string out;
    out += "RIFF";
    put_le32(out, static_cast<std::uint32_t>(4 + (8 + fmt.size()) + (8 + payload.size())));
    out += "WAVE";
    out += "fmt ";
    put_le32(out, static_cast<std::uint32_t>(fmt.size()));
    out += fmt;
    out += "data";
    put_le32(out, declared_data_bytes);
    out += payload;
    return out;
}

std::string canonical_wav(std::uint16_t format_tag, std::uint16_t channels,
                          std::uint32_t sample_rate, std::uint16_t bits,
                          const std::string& payload) {
    return canonical_wav(format_tag, channels, sample_rate, bits, payload,
                         static_cast<std::uint32_t>(payload.size()));
}

// WAVE_FORMAT_EXTENSIBLE (tag 0xFFFE): a 40-byte fmt chunk whose real format
// is the leading two bytes of the 16-byte SubFormat GUID, per parse_wav's own
// fmt_at + 32 read.
std::string extensible_wav(std::uint16_t real_format_tag, std::uint16_t channels,
                           std::uint32_t sample_rate, std::uint16_t bits,
                           const std::string& payload) {
    const std::uint16_t block_align = static_cast<std::uint16_t>(channels * (bits / 8));
    const std::uint32_t byte_rate = sample_rate * block_align;

    std::string fmt;
    put_le16(fmt, 0xFFFE);
    put_le16(fmt, channels);
    put_le32(fmt, sample_rate);
    put_le32(fmt, byte_rate);
    put_le16(fmt, block_align);
    put_le16(fmt, bits);
    put_le16(fmt, 22);    // cbSize: 22 bytes follow (valid bits + mask + GUID)
    put_le16(fmt, bits);  // wValidBitsPerSample
    put_le32(fmt, 0);     // dwChannelMask: not indicated
    put_le16(fmt, real_format_tag);
    // The rest of the 16-byte SubFormat GUID (KSDATAFORMAT_SUBTYPE_PCM/IEEE_
    // FLOAT's fixed tail) - parse_wav never reads past the leading 2 bytes,
    // so its exact value does not matter here.
    for (int i = 0; i < 14; ++i) {
        fmt.push_back(static_cast<char>(0));
    }
    REQUIRE(fmt.size() == 40);

    std::string out;
    out += "RIFF";
    put_le32(out, static_cast<std::uint32_t>(4 + (8 + fmt.size()) + (8 + payload.size())));
    out += "WAVE";
    out += "fmt ";
    put_le32(out, static_cast<std::uint32_t>(fmt.size()));
    out += fmt;
    out += "data";
    put_le32(out, static_cast<std::uint32_t>(payload.size()));
    out += payload;
    return out;
}

fs::path write_raw(const std::string& name, const std::string& bytes) {
    const auto path = scratch_dir() / name;
    std::ofstream out{path, std::ios::binary};
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return path;
}

}  // namespace

TEST_CASE("read_wav round-trips write_wav_f32 exactly, multi-channel", "[wav]") {
    const std::vector<std::vector<float>> channels = {
        {0.25f, -0.75f, 0.5f, -1.0f}, {0.1f, -0.2f, 0.3f, -0.4f}, {0.0f, 1.0f, -1.0f, 0.0f}};
    const auto path = scratch_dir() / "float_round_trip.wav";
    REQUIRE(ac3::io::write_wav_f32(path.string(), channels, 48000).has_value());

    const auto data = ac3::io::read_wav(path.string());
    REQUIRE(data.has_value());
    CHECK(data->sample_rate == 48000);
    REQUIRE(data->channels.size() == 3);
    REQUIRE(data->frame_count() == 4);
    for (std::size_t ch = 0; ch < 3; ++ch) {
        for (std::size_t n = 0; n < 4; ++n) {
            CHECK(data->channels[ch][n] == channels[ch][n]);
        }
    }
}

TEST_CASE("read_wav decodes PCM16 samples with the documented int16/32768 scaling", "[wav]") {
    // Interleaved stereo, hand-picked to cover both signed extremes and the
    // exact halves a wrong shift or a signedness slip would visibly miss.
    // std::int16_t's two's-complement range is asymmetric (-32768..32767),
    // so kMinInt16's exact float quotient is -1.0f while kMaxInt16's is not:
    // that is precisely why the two are checked against different tolerances
    // below, not a quirk of this test.
    std::string payload;
    const std::vector<std::int16_t> samples = {0, 0, 16384, -16384, 32767, -32768};
    for (const auto s : samples) {
        put_le16(payload, static_cast<std::uint16_t>(s));
    }
    const auto path = write_raw("pcm16.wav", canonical_wav(1, 2, 44100, 16, payload));

    const auto data = ac3::io::read_wav(path.string());
    REQUIRE(data.has_value());
    CHECK(data->sample_rate == 44100);
    REQUIRE(data->channels.size() == 2);
    REQUIRE(data->frame_count() == 3);
    // channel 0: frames 0,1,2 = samples[0], samples[2], samples[4]
    CHECK(data->channels[0][0] == 0.0f);
    CHECK(data->channels[0][1] == Approx(0.5).margin(1e-6));
    CHECK(data->channels[0][2] == Approx(32767.0 / 32768.0).margin(1e-6));
    // channel 1: frames 0,1,2 = samples[1], samples[3], samples[5]
    CHECK(data->channels[1][0] == 0.0f);
    CHECK(data->channels[1][1] == Approx(-0.5).margin(1e-6));
    CHECK(data->channels[1][2] == -1.0f);  // -32768 / 32768 is exact
}

TEST_CASE("read_wav via write_wav_pcm16_raw's own payload decodes back losslessly", "[wav]") {
    // write_wav_pcm16_raw is used for the IEC 61937 burst carrier, where the
    // payload must pass through untouched - this closes the loop by reading
    // it back through the same parser every other WAV path uses.
    std::string payload;
    for (const std::int16_t s : {std::int16_t{1000}, std::int16_t{-1000}, std::int16_t{4},
                                 std::int16_t{-4}}) {
        put_le16(payload, static_cast<std::uint16_t>(s));
    }
    const std::vector<std::byte> bytes(reinterpret_cast<const std::byte*>(payload.data()),
                                       reinterpret_cast<const std::byte*>(payload.data()) +
                                           payload.size());
    const auto path = scratch_dir() / "pcm16_raw.wav";
    REQUIRE(ac3::io::write_wav_pcm16_raw(path.string(), bytes, 48000, 2).has_value());

    const auto data = ac3::io::read_wav(path.string());
    REQUIRE(data.has_value());
    REQUIRE(data->channels.size() == 2);
    REQUIRE(data->frame_count() == 2);
    CHECK(data->channels[0][0] == Approx(1000.0 / 32768.0).margin(1e-9));
    CHECK(data->channels[1][0] == Approx(-1000.0 / 32768.0).margin(1e-9));
    CHECK(data->channels[0][1] == Approx(4.0 / 32768.0).margin(1e-9));
    CHECK(data->channels[1][1] == Approx(-4.0 / 32768.0).margin(1e-9));
}

TEST_CASE("read_wav resolves WAVE_FORMAT_EXTENSIBLE via the SubFormat GUID's leading tag",
         "[wav]") {
    SECTION("float32 subformat") {
        std::string payload;
        const float value = -0.5f;
        payload.append(reinterpret_cast<const char*>(&value), sizeof(value));
        const auto path =
            write_raw("extensible_float.wav", extensible_wav(3, 1, 48000, 32, payload));
        const auto data = ac3::io::read_wav(path.string());
        REQUIRE(data.has_value());
        REQUIRE(data->frame_count() == 1);
        CHECK(data->channels[0][0] == -0.5f);
    }
    SECTION("pcm16 subformat") {
        std::string payload;
        put_le16(payload, static_cast<std::uint16_t>(-16384));
        const auto path =
            write_raw("extensible_pcm16.wav", extensible_wav(1, 1, 48000, 16, payload));
        const auto data = ac3::io::read_wav(path.string());
        REQUIRE(data.has_value());
        REQUIRE(data->frame_count() == 1);
        CHECK(data->channels[0][0] == Approx(-0.5).margin(1e-6));
    }
}

TEST_CASE("read_wav clamps to the bytes actually present when the data chunk overstates its size",
         "[wav]") {
    // The header's data-chunk size field claims 1000 bytes; the file only
    // actually holds 8 (two stereo PCM16 frames). parse_wav is documented to
    // clamp `declared` to what is actually available rather than fail - this
    // pins that specific, easy-to-break-by-accident behaviour rather than
    // assuming it.
    std::string payload;
    for (const std::int16_t s : {std::int16_t{100}, std::int16_t{-100}, std::int16_t{200},
                                 std::int16_t{-200}}) {
        put_le16(payload, static_cast<std::uint16_t>(s));
    }
    const auto path =
        write_raw("overstated_size.wav", canonical_wav(1, 2, 48000, 16, payload, 1000));

    const auto data = ac3::io::read_wav(path.string());
    REQUIRE(data.has_value());
    REQUIRE(data->channels.size() == 2);
    CHECK(data->frame_count() == 2);  // 8 bytes / (2 ch * 2 bytes) = 2 frames, not the declared 1000
    CHECK(data->channels[0][0] == Approx(100.0 / 32768.0).margin(1e-9));
    CHECK(data->channels[1][1] == Approx(-200.0 / 32768.0).margin(1e-9));
}

TEST_CASE("read_wav rejects a path that cannot be opened", "[wav]") {
    const auto result = ac3::io::read_wav((scratch_dir() / "does_not_exist.wav").string());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac3::io::WavError::kCannotOpen);
}

TEST_CASE("read_wav rejects data that is not a RIFF/WAVE file", "[wav]") {
    SECTION("too short to even hold a header") {
        // "RIFF", four size bytes, then "WAV" - 11 bytes total, well under
        // the 44-byte minimum a real header needs.
        std::string bytes = "RIFF";
        bytes.resize(8, '\0');
        bytes += "WAV";
        const auto path = write_raw("too_short.wav", bytes);
        const auto result = ac3::io::read_wav(path.string());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ac3::io::WavError::kNotRiffWave);
    }
    SECTION("empty file") {
        std::istringstream in;
        const auto result = ac3::io::read_wav(in);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ac3::io::WavError::kNotRiffWave);
    }
    SECTION("wrong magic entirely") {
        std::string bytes(64, '\0');
        bytes.replace(0, 4, "JUNK");
        const auto path = write_raw("wrong_magic.wav", bytes);
        const auto result = ac3::io::read_wav(path.string());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ac3::io::WavError::kNotRiffWave);
    }
    SECTION("RIFF/WAVE magic present but neither fmt nor data chunk exists") {
        std::string bytes = "RIFF";
        put_le32(bytes, 40);
        bytes += "WAVE";
        bytes.append(40, '\0');  // padding, deliberately not "fmt " or "data"
        const auto path = write_raw("no_chunks.wav", bytes);
        const auto result = ac3::io::read_wav(path.string());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ac3::io::WavError::kNotRiffWave);
    }
}

TEST_CASE("read_wav decodes every integer PCM depth with the documented scaling", "[wav]") {
    // One full-scale-negative, one silent, one full-scale-positive sample per
    // depth, plus one arbitrary interior value - the scaling for each width is
    // stated in src/forge/src/io/wav_format.cpp's convert_sample and checked
    // here against values worked out by hand rather than against another
    // reader that might share the same mistake.
    SECTION("8-bit PCM is unsigned and biased by 128") {
        std::string payload;
        payload.push_back(static_cast<char>(0x00));  // -128 -> -1.0
        payload.push_back(static_cast<char>(0x80));  //    0 ->  0.0
        payload.push_back(static_cast<char>(0xFF));  // +127 -> +127/128
        payload.push_back(static_cast<char>(0xC0));  //  +64 -> +0.5
        const auto path = write_raw("depth_pcm8.wav", canonical_wav(1, 1, 48000, 8, payload));
        const auto result = ac3::io::read_wav(path.string());
        REQUIRE(result.has_value());
        REQUIRE(result->channels.size() == 1);
        REQUIRE(result->channels[0].size() == 4);
        CHECK(result->channels[0][0] == Approx(-1.0));
        CHECK(result->channels[0][1] == Approx(0.0));
        CHECK(result->channels[0][2] == Approx(127.0 / 128.0));
        CHECK(result->channels[0][3] == Approx(0.5));
    }
    SECTION("24-bit PCM is three little-endian bytes, signed, scaled by 2^23") {
        std::string payload;
        put_le24(payload, -8388608);  // -1.0
        put_le24(payload, 0);
        put_le24(payload, 8388607);
        put_le24(payload, 4194304);  // +0.5
        put_le24(payload, -1);       // the sign-extension case a naive shift gets wrong
        const auto path = write_raw("depth_pcm24.wav", canonical_wav(1, 1, 48000, 24, payload));
        const auto result = ac3::io::read_wav(path.string());
        REQUIRE(result.has_value());
        REQUIRE(result->channels[0].size() == 5);
        CHECK(result->channels[0][0] == Approx(-1.0));
        CHECK(result->channels[0][1] == Approx(0.0));
        CHECK(result->channels[0][2] == Approx(8388607.0 / 8388608.0));
        CHECK(result->channels[0][3] == Approx(0.5));
        CHECK(result->channels[0][4] == Approx(-1.0 / 8388608.0));
    }
    SECTION("32-bit PCM is signed, scaled by 2^31") {
        std::string payload;
        put_le32(payload, 0x80000000u);  // -2^31 -> -1.0
        put_le32(payload, 0u);
        put_le32(payload, 0x40000000u);  // +0.5
        const auto path = write_raw("depth_pcm32.wav", canonical_wav(1, 1, 48000, 32, payload));
        const auto result = ac3::io::read_wav(path.string());
        REQUIRE(result.has_value());
        REQUIRE(result->channels[0].size() == 3);
        CHECK(result->channels[0][0] == Approx(-1.0));
        CHECK(result->channels[0][1] == Approx(0.0));
        CHECK(result->channels[0][2] == Approx(0.5));
    }
    SECTION("64-bit IEEE float narrows to the same value") {
        std::string payload;
        for (const double v : {-1.0, 0.0, 0.25, 0.5}) {
            std::uint64_t bits = 0;
            std::memcpy(&bits, &v, sizeof(bits));
            put_le32(payload, static_cast<std::uint32_t>(bits & 0xFFFFFFFFu));
            put_le32(payload, static_cast<std::uint32_t>(bits >> 32));
        }
        const auto path = write_raw("depth_float64.wav", canonical_wav(3, 1, 48000, 64, payload));
        const auto result = ac3::io::read_wav(path.string());
        REQUIRE(result.has_value());
        REQUIRE(result->channels[0].size() == 4);
        CHECK(result->channels[0][0] == Approx(-1.0));
        CHECK(result->channels[0][1] == Approx(0.0));
        CHECK(result->channels[0][2] == Approx(0.25));
        CHECK(result->channels[0][3] == Approx(0.5));
    }
    SECTION("a 24-bit stereo file deinterleaves by the container width") {
        // The one shape a width-blind stride would silently scramble rather
        // than refuse: channel 1's samples read out of channel 0's bytes.
        std::string payload;
        put_le24(payload, 4194304);   // frame 0, L = +0.5
        put_le24(payload, -4194304);  // frame 0, R = -0.5
        put_le24(payload, 2097152);   // frame 1, L = +0.25
        put_le24(payload, -2097152);  // frame 1, R = -0.25
        const auto path = write_raw("pcm24_stereo.wav", canonical_wav(1, 2, 48000, 24, payload));
        const auto result = ac3::io::read_wav(path.string());
        REQUIRE(result.has_value());
        REQUIRE(result->channels.size() == 2);
        REQUIRE(result->channels[0].size() == 2);
        CHECK(result->channels[0][0] == Approx(0.5));
        CHECK(result->channels[1][0] == Approx(-0.5));
        CHECK(result->channels[0][1] == Approx(0.25));
        CHECK(result->channels[1][1] == Approx(-0.25));
    }
}

TEST_CASE("read_wav unwraps WAVE_FORMAT_EXTENSIBLE around a 24-bit payload", "[wav]") {
    // The professional-delivery shape: 24-bit inside EXTENSIBLE. The tag at
    // fmt+0 says 0xFFFE and says nothing about PCM; only the SubFormat GUID
    // does.
    std::string payload;
    put_le24(payload, 4194304);
    put_le24(payload, -8388608);
    const auto path = write_raw("ext_pcm24.wav", extensible_wav(1, 1, 48000, 24, payload));
    const auto result = ac3::io::read_wav(path.string());
    REQUIRE(result.has_value());
    REQUIRE(result->channels[0].size() == 2);
    CHECK(result->channels[0][0] == Approx(0.5));
    CHECK(result->channels[0][1] == Approx(-1.0));
}

TEST_CASE("read_wav reads an RF64/BW64 file's 64-bit ds64 sizes", "[wav]") {
    // The <data> chunk's own 32-bit size field is the 0xFFFFFFFF placeholder
    // EBU Tech 3306 3.1 mandates; the real length is in <ds64>. A reader that
    // trusted the 32-bit field would clamp to the file's real length and
    // still "work" here by accident, so this deliberately UNDERSTATES the
    // length in ds64 - only a reader that actually read ds64 stops early.
    std::string payload;
    for (int i = 0; i < 8; ++i) {
        put_le16(payload, static_cast<std::uint16_t>(static_cast<std::int16_t>(i * 1000)));
    }

    std::string fmt;
    put_le16(fmt, 1);
    put_le16(fmt, 1);
    put_le32(fmt, 48000);
    put_le32(fmt, 96000);
    put_le16(fmt, 2);
    put_le16(fmt, 16);

    std::string ds64;
    put_le32(ds64, 0);  // riffSize low/high - not consulted by the reader
    put_le32(ds64, 0);
    put_le32(ds64, 8);  // dataSize: four frames, not the eight actually present
    put_le32(ds64, 0);
    put_le32(ds64, 4);  // sampleCount
    put_le32(ds64, 0);
    put_le32(ds64, 0);  // tableLength

    const auto assemble = [&](const char* magic) {
        std::string out;
        out += magic;
        put_le32(out, 0xFFFFFFFFu);
        out += "WAVE";
        out += "ds64";
        put_le32(out, static_cast<std::uint32_t>(ds64.size()));
        out += ds64;
        out += "fmt ";
        put_le32(out, static_cast<std::uint32_t>(fmt.size()));
        out += fmt;
        out += "data";
        put_le32(out, 0xFFFFFFFFu);
        out += payload;
        return out;
    };

    for (const char* magic : {"RF64", "BW64"}) {
        const auto path = write_raw(std::string{magic} + "_ds64.wav", assemble(magic));
        const auto result = ac3::io::read_wav(path.string());
        REQUIRE(result.has_value());
        CHECK(result->sample_rate == 48000);
        REQUIRE(result->channels.size() == 1);
        CHECK(result->channels[0].size() == 4);
        CHECK(result->channels[0][1] == Approx(1000.0 / 32768.0));
    }
}

TEST_CASE("read_wav walks the chunk list rather than searching for a tag anywhere", "[wav]") {
    // A LIST chunk whose payload spells "data" - exactly what a naive
    // substring search finds first, and exactly what a real broadcast WAV's
    // bext/iXML metadata can contain by chance.
    std::string payload;
    put_le16(payload, static_cast<std::uint16_t>(static_cast<std::int16_t>(-16384)));
    put_le16(payload, 16384);

    std::string fmt;
    put_le16(fmt, 1);
    put_le16(fmt, 1);
    put_le32(fmt, 48000);
    put_le32(fmt, 96000);
    put_le16(fmt, 2);
    put_le16(fmt, 16);

    std::string decoy = "INFO";
    decoy += "data";
    put_le32(decoy, 4);
    decoy += "junk";

    std::string out;
    out += "RIFF";
    put_le32(out, 0);  // the reader never consults the RIFF size field
    out += "WAVE";
    out += "LIST";
    put_le32(out, static_cast<std::uint32_t>(decoy.size()));
    out += decoy;
    out += "fmt ";
    put_le32(out, static_cast<std::uint32_t>(fmt.size()));
    out += fmt;
    out += "data";
    put_le32(out, static_cast<std::uint32_t>(payload.size()));
    out += payload;

    const auto path = write_raw("decoy_data_chunk.wav", out);
    const auto result = ac3::io::read_wav(path.string());
    REQUIRE(result.has_value());
    REQUIRE(result->channels.size() == 1);
    REQUIRE(result->channels[0].size() == 2);
    CHECK(result->channels[0][0] == Approx(-0.5));
    CHECK(result->channels[0][1] == Approx(0.5));
}

TEST_CASE("read_wav refuses a chunk tag too close to the end to carry its own fields",
          "[wav]") {
    // parse_wav locates "fmt " and "data" by searching the whole buffer, then
    // reads fixed offsets past each. A file long enough to clear the 44-byte
    // minimum can still put either tag within a few bytes of the end, at which
    // point those reads index past the buffer - a heap over-read on an
    // attacker- or accident-supplied file. WavStreamReader::open already
    // guards the same field layout; these are the whole-file parser's half.
    SECTION("\"fmt \" in the last four bytes") {
        std::string bytes = "RIFF";
        put_le32(bytes, 48);
        bytes += "WAVE";
        bytes.append(36, 'x');
        bytes += "fmt ";  // offset 48; the format tag would be read at 56
        REQUIRE(bytes.size() == 52);
        const auto path = write_raw("fmt_at_end.wav", bytes);
        const auto result = ac3::io::read_wav(path.string());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ac3::io::WavError::kNotRiffWave);
    }
    SECTION("\"data\" without its own size field") {
        // A complete, valid fmt chunk, then a "data" tag with only three of
        // the four size bytes behind it.
        std::string bytes = "RIFF";
        put_le32(bytes, 40);
        bytes += "WAVE";
        bytes += "fmt ";
        put_le32(bytes, 16);
        put_le16(bytes, 1);      // PCM
        put_le16(bytes, 2);      // channels
        put_le32(bytes, 48000);  // sample rate
        put_le32(bytes, 192000);
        put_le16(bytes, 4);
        put_le16(bytes, 16);  // bits
        bytes += "data";
        bytes.append(3, char{0});  // one byte short of the declared-size field
        const auto path = write_raw("data_truncated_header.wav", bytes);
        const auto result = ac3::io::read_wav(path.string());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ac3::io::WavError::kNotRiffWave);
    }
    SECTION("WAVE_FORMAT_EXTENSIBLE tag without the extension it points into") {
        // fmt through `bits` is present (so the 24-byte guard passes) but the
        // chunk stops there, leaving no SubFormat GUID to resolve 0xFFFE
        // against. The tag stays 0xFFFE, which no supported-format branch
        // accepts, so this is a format refusal rather than an over-read.
        std::string bytes = "RIFF";
        put_le32(bytes, 40);
        bytes += "WAVE";
        bytes += "fmt ";
        put_le32(bytes, 16);
        put_le16(bytes, 0xFFFE);
        put_le16(bytes, 2);
        put_le32(bytes, 48000);
        put_le32(bytes, 192000);
        put_le16(bytes, 4);
        put_le16(bytes, 16);
        bytes += "data";
        put_le32(bytes, 0);
        const auto path = write_raw("extensible_no_extension.wav", bytes);
        const auto result = ac3::io::read_wav(path.string());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ac3::io::WavError::kUnsupportedFormat);
    }
}

TEST_CASE("read_wav rejects sample formats it does not support", "[wav]") {
    SECTION("an unpacked integer width that is not a whole number of bytes") {
        const auto path =
            write_raw("pcm20.wav", canonical_wav(1, 1, 48000, 20, std::string(8, '\0')));
        const auto result = ac3::io::read_wav(path.string());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ac3::io::WavError::kUnsupportedFormat);
    }
    SECTION("a non-PCM, non-float format tag (e.g. ADPCM)") {
        const auto path =
            write_raw("adpcm.wav", canonical_wav(2, 1, 48000, 4, std::string(4, '\0')));
        const auto result = ac3::io::read_wav(path.string());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ac3::io::WavError::kUnsupportedFormat);
    }
    SECTION("PCM at a float-only width") {
        const auto path =
            write_raw("pcm64.wav", canonical_wav(1, 1, 48000, 64, std::string(16, '\0')));
        const auto result = ac3::io::read_wav(path.string());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ac3::io::WavError::kUnsupportedFormat);
    }
    SECTION("IEEE float at an integer-only width") {
        const auto path =
            write_raw("float24.wav", canonical_wav(3, 1, 48000, 24, std::string(6, '\0')));
        const auto result = ac3::io::read_wav(path.string());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ac3::io::WavError::kUnsupportedFormat);
    }
    SECTION("zero channels") {
        const auto path =
            write_raw("zero_channels.wav", canonical_wav(1, 0, 48000, 16, std::string(4, '\0')));
        const auto result = ac3::io::read_wav(path.string());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ac3::io::WavError::kUnsupportedFormat);
    }
}

TEST_CASE("read_wav(std::istream&) parses the same bytes the path overload does", "[wav]") {
    const std::string bytes = canonical_wav(1, 1, 22050, 16, [] {
        std::string payload;
        put_le16(payload, static_cast<std::uint16_t>(1234));
        return payload;
    }());
    std::istringstream in{bytes, std::ios::binary};
    const auto data = ac3::io::read_wav(in);
    REQUIRE(data.has_value());
    CHECK(data->sample_rate == 22050);
    REQUIRE(data->frame_count() == 1);
    CHECK(data->channels[0][0] == Approx(1234.0 / 32768.0).margin(1e-9));
}

TEST_CASE("write_wav_f32 refuses empty channel data and never touches the filesystem", "[wav]") {
    const auto path = scratch_dir() / "must_not_exist.wav";
    fs::remove(path);
    const std::vector<std::vector<float>> empty;
    const auto result = ac3::io::write_wav_f32(path.string(), empty, 48000);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac3::io::WavError::kTruncated);
    CHECK_FALSE(fs::exists(path));
}

TEST_CASE("describe() gives every WavError a distinct, non-empty message", "[wav]") {
    const std::array errors = {ac3::io::WavError::kCannotOpen, ac3::io::WavError::kNotRiffWave,
                               ac3::io::WavError::kUnsupportedFormat,
                               ac3::io::WavError::kTruncated};
    for (const auto e : errors) {
        CAPTURE(static_cast<int>(e));
        CHECK_FALSE(ac3::io::describe(e).empty());
    }
    for (std::size_t i = 0; i < errors.size(); ++i) {
        for (std::size_t j = i + 1; j < errors.size(); ++j) {
            CHECK(ac3::io::describe(errors[i]) != ac3::io::describe(errors[j]));
        }
    }
}
