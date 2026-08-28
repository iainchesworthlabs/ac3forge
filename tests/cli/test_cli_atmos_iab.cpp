#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"

// ac3cli's 'atmos-iab' command (roadmap IM1 phase 3 of 3 - see ROADMAP.md's "IAB (SMPTE ST 2098-2)
// reader" entry; apps/cli/commands/atmos.cpp's run_atmos_iab). Real, subprocess-level integration
// tests, the same shape test_cli_atmos_adm.cpp already uses for 'atmos-adm' and for the identical
// reason - main.cpp compiles everything into one binary with no library surface run_atmos_iab's
// own logic could be linked into this test binary and called directly.
//
// A separate file rather than folded into test_cli.cpp, gated the same two-part way
// test_cli_atmos_adm.cpp is (AC3FORGE_BUILD_ADM AND ac3cli actually built) - see
// tests/CMakeLists.txt's own comment.
//
// The byte-level IAB fixture below is a copy of tests/admbridge/test_iab_bridge.cpp's own flagship
// fixture (same Bed Center channel + hard-right-then-hard-left Object, same 300/800 Hz tones) -
// duplicated per this project's own established per-file test-helper convention (see that file's
// own comment) rather than shared: this file's own job is checking that the real ac3cli binary
// wires parse_iabitstream -> admbridge::build_iab -> AtmosEncoder together correctly end to end,
// not re-proving admbridge's own Table 19 mapping or coordinate conversion, which
// tests/admbridge/test_iab_bridge.cpp already does directly against the library API.

namespace fs = std::filesystem;

namespace {

fs::path scratch_dir() {
    auto dir = fs::path{AC3FORGE_TEST_SCRATCH_DIR} / "cli_iab";
    fs::create_directories(dir);
    return dir;
}

int run_cli(const std::string& args, const fs::path& log) {
    const std::string command =
        "\"" + std::string(AC3CLI_EXE) + "\" " + args + " > \"" + log.string() + "\" 2>&1";
#ifdef _WIN32
    const std::string wrapped = "\"" + command + "\"";
    return std::system(wrapped.c_str());
#else
    return std::system(command.c_str());
#endif
}

std::string read_log(const fs::path& log) {
    std::ifstream in{log, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

class BitWriter {
   public:
    void push_bits(std::uint64_t value, unsigned width) {
        for (unsigned i = 0; i < width; ++i) {
            push_bit(static_cast<unsigned>((value >> (width - 1 - i)) & 0x1u));
        }
    }

    void push_plex(std::uint64_t value, unsigned initial_width) {
        unsigned width = initial_width;
        while (true) {
            const std::uint64_t escape = (std::uint64_t{1} << width) - 1;
            if (value < escape) {
                push_bits(value, width);
                return;
            }
            push_bits(escape, width);
            width *= 2;
        }
    }

    void align_to_byte() {
        while (bit_count_ % 8 != 0) {
            push_bit(0);
        }
    }

    void push_raw_byte(unsigned char byte) {
        align_to_byte();
        bytes_.push_back(static_cast<std::byte>(byte));
        bit_count_ += 8;
    }

    [[nodiscard]] const std::vector<std::byte>& bytes() const { return bytes_; }

   private:
    void push_bit(unsigned bit) {
        if (bit_count_ % 8 == 0) {
            bytes_.push_back(std::byte{0});
        }
        if (bit) {
            bytes_.back() |= static_cast<std::byte>(1u << (7 - (bit_count_ % 8)));
        }
        ++bit_count_;
    }

    std::vector<std::byte> bytes_;
    std::size_t bit_count_ = 0;
};

void append(std::vector<std::byte>& out, const std::vector<std::byte>& more) {
    out.insert(out.end(), more.begin(), more.end());
}

std::vector<std::byte> wrap_element(std::uint32_t id, const std::vector<std::byte>& payload) {
    BitWriter bw;
    bw.push_plex(id, 8);
    bw.push_plex(payload.size(), 8);
    auto bytes = bw.bytes();
    append(bytes, payload);
    return bytes;
}

constexpr std::uint8_t kFrameRateCode = 0x8;  // 120 fps -> 2 pan sub blocks
constexpr std::uint32_t kSamplesPerIabFrame = 400;
constexpr unsigned kTotalIabFrames = 20;
constexpr std::uint16_t kPosMin = 0x8000;  // §5.4's own valid domain floor - see
constexpr std::uint16_t kPosMax = 0xFFFF;  // examples/encode_iab.cpp's identical comment

std::vector<std::byte> build_bed_payload() {
    BitWriter bw;
    bw.push_plex(1, 8);
    bw.push_bits(0, 1);
    bw.push_plex(1, 4);
    bw.push_plex(0x2, 4);  // Center
    bw.push_plex(1, 8);
    bw.push_bits(0, 2);
    bw.push_bits(0, 1);
    bw.push_bits(0x180, 10);
    bw.align_to_byte();
    bw.push_bits(0x01, 8);
    bw.push_plex(0, 8);
    return bw.bytes();
}

std::vector<std::byte> build_object_payload(std::uint16_t pos_x, std::uint16_t pos_y) {
    BitWriter bw;
    bw.push_plex(2, 8);
    bw.push_plex(2, 8);
    bw.push_bits(0, 1);
    bw.push_bits(0, 1);

    bw.push_bits(0, 2);
    bw.push_bits(0b001, 3);
    bw.push_bits(pos_x, 16);
    bw.push_bits(pos_y, 16);
    bw.push_bits(0, 16);
    bw.push_bits(0, 1);
    bw.push_bits(0, 1);
    bw.push_bits(0x1, 2);
    bw.push_bits(0, 4);
    bw.push_bits(0, 2);

    bw.push_bits(0, 1);  // sub block 1: carried forward

    bw.align_to_byte();
    bw.push_bits(0x01, 8);
    bw.push_plex(0, 8);
    return bw.bytes();
}

std::vector<std::byte> build_pcm_payload(std::uint32_t audio_data_id,
                                         const std::vector<std::int16_t>& samples) {
    BitWriter bw;
    bw.push_plex(audio_data_id, 8);
    for (const auto sample : samples) {
        bw.push_raw_byte(static_cast<unsigned char>(static_cast<std::uint16_t>(sample) & 0xFFu));
        bw.push_raw_byte(
            static_cast<unsigned char>((static_cast<std::uint16_t>(sample) >> 8) & 0xFFu));
    }
    return bw.bytes();
}

std::vector<std::byte> build_iaframe(unsigned index) {
    const bool right_half = index < kTotalIabFrames / 2;
    const std::uint16_t pos_x = right_half ? kPosMax : kPosMin;
    constexpr std::uint16_t kPosYMid = 0xC000;

    std::vector<std::int16_t> bed_samples(kSamplesPerIabFrame);
    std::vector<std::int16_t> object_samples(kSamplesPerIabFrame);
    for (std::uint32_t i = 0; i < kSamplesPerIabFrame; ++i) {
        const double t = static_cast<double>(index * kSamplesPerIabFrame + i) / 48000.0;
        bed_samples[i] =
            static_cast<std::int16_t>(0.3 * 32767.0 * std::sin(2.0 * std::numbers::pi * 300.0 * t));
        object_samples[i] =
            static_cast<std::int16_t>(0.3 * 32767.0 * std::sin(2.0 * std::numbers::pi * 800.0 * t));
    }

    BitWriter header;
    header.push_bits(1, 8);
    header.push_bits(0, 2);
    header.push_bits(0, 2);
    header.push_bits(kFrameRateCode, 4);
    header.push_plex(0, 8);
    header.align_to_byte();
    header.push_plex(4, 8);

    std::vector<std::byte> iaframe_payload = header.bytes();
    append(iaframe_payload, wrap_element(0x10, build_bed_payload()));
    append(iaframe_payload, wrap_element(0x40, build_object_payload(pos_x, kPosYMid)));
    append(iaframe_payload, wrap_element(0x400, build_pcm_payload(1, bed_samples)));
    append(iaframe_payload, wrap_element(0x400, build_pcm_payload(2, object_samples)));

    std::vector<std::byte> segment;
    segment.push_back(std::byte{0x01});
    for (int shift : {24, 16, 8, 0}) {
        segment.push_back(static_cast<std::byte>((0u >> shift) & 0xFFu));
    }
    segment.push_back(std::byte{0x02});
    const auto element = wrap_element(0x08, iaframe_payload);
    const auto frame_length = static_cast<std::uint32_t>(element.size());
    for (int shift : {24, 16, 8, 0}) {
        segment.push_back(static_cast<std::byte>((frame_length >> shift) & 0xFFu));
    }
    append(segment, element);
    return segment;
}

bool write_fixture(const fs::path& path) {
    std::vector<std::byte> file;
    for (unsigned i = 0; i < kTotalIabFrames; ++i) {
        append(file, build_iaframe(i));
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(file.data()),
              static_cast<std::streamsize>(file.size()));
    return static_cast<bool>(out);
}

double channel_energy(std::span<const float> samples) {
    double energy = 0.0;
    for (const auto v : samples) {
        const double sd = static_cast<double>(v);
        energy += sd * sd;
    }
    return energy;
}

}  // namespace

TEST_CASE("ac3cli atmos-iab parses, bridges and encodes a real IAB fixture end to end",
          "[cli][atmos-iab]") {
    const auto dir = scratch_dir();
    const auto fixture_path = dir / "atmos_iab_fixture.iab";
    REQUIRE(write_fixture(fixture_path));

    const auto out_path = dir / "atmos_iab_out.ec3";
    const auto log_path = dir / "atmos_iab.log";
    const auto rc = run_cli(
        "atmos-iab \"" + fixture_path.string() + "\" \"" + out_path.string() + "\" 448", log_path);
    INFO(read_log(log_path));
    CHECK(rc == 0);
    REQUIRE(fs::exists(out_path));
    CHECK(fs::file_size(out_path) > 0);

    // Decode what the CLI actually wrote - proves the real binary's argument parsing, the MXF/
    // elementary sniff, ac3::admbridge::build_iab call and per-frame AtmosEncoder loop are all
    // wired together correctly, not just that each piece works in isolation
    // (tests/admbridge/test_iab_bridge.cpp's own flagship test already covers that).
    std::ifstream stream_in{out_path, std::ios::binary};
    const std::vector<char> raw{std::istreambuf_iterator<char>{stream_in},
                                std::istreambuf_iterator<char>{}};
    std::vector<std::byte> stream_bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        stream_bytes[i] = static_cast<std::byte>(raw[i]);
    }

    const auto units = ac3::split_access_units(stream_bytes);
    REQUIRE(units.has_value());
    REQUIRE(units->size() >= 3);  // real content, more than one AC-3 frame

    ac3::Eac3Decoder decoder;
    constexpr int kCCh = 1;  // AC-3 3/2 coded order (Table 5.8): L, C, R, Ls, Rs.

    bool saw_center_energy = false;
    for (const auto& unit : *units) {
        const auto decoded = decoder.decode_access_unit(unit);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        if (channel_energy((*decoded)->channels[kCCh]) > 1.0) {
            saw_center_energy = true;
        }
    }
    // The bed (Center) is static throughout every frame.
    CHECK(saw_center_energy);
}

TEST_CASE("ac3cli atmos-iab reports a clear diagnosis for a file with no IAB essence",
          "[cli][atmos-iab]") {
    const auto dir = scratch_dir();
    const auto bad_path = dir / "atmos_iab_not_iab.iab";
    {
        std::ofstream out(bad_path, std::ios::binary);
        REQUIRE(out.is_open());
        out << "not an IAB file at all";
    }

    const auto out_path = dir / "atmos_iab_not_iab_out.ec3";
    const auto log_path = dir / "atmos_iab_not_iab.log";
    const auto rc =
        run_cli("atmos-iab \"" + bad_path.string() + "\" \"" + out_path.string() + "\"", log_path);
    CHECK(rc != 0);
    const auto log = read_log(log_path);
    // ac3iab::describe(IabError::...) - never a silent crash or an unlabeled non-zero exit.
    CHECK(log.find("error:") != std::string::npos);
    CHECK_FALSE(fs::exists(out_path));
}
