#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <filesystem>
#include <fstream>

#include "ac3iab/ac3iab.hpp"
#include "ac3iab/mxf.hpp"

// The IAB reader against damaged input: every element this reader understands, built with
// randomly chosen but well-formed field values (a fixed seed, so a failure reproduces), parses
// whole and fails with kTruncated when its payload is cut short at any byte - with its
// ElementSize kept honest, so the cut lands inside the element's own fields rather than being
// caught by the element header. The random choices move the fields across byte boundaries, which
// is what reaches each field's own "ran out of bits" return. The segment framing gets the same
// treatment, and describe() names every error.
//
// Fixtures are written with this file's own MSB-first BitWriter, independently of
// src/ac3iab/src/bitreader.hpp, as test_ac3iab.cpp's are.

namespace {

class BitWriter {
public:
    void bits(std::uint64_t value, unsigned width) {
        for (unsigned i = 0; i < width; ++i) {
            bit(static_cast<unsigned>((value >> (width - 1 - i)) & 0x1U));
        }
    }

    // §5.2 Plex(n).
    void plex(std::uint64_t value, unsigned width) {
        while (width <= 32) {
            const std::uint64_t escape = (std::uint64_t{1} << width) - 1;
            if (value < escape) {
                bits(value, width);
                return;
            }
            bits(escape, width);
            width *= 2;
        }
    }

    void align() {
        while (count_ % 8 != 0) {
            bit(0);
        }
    }

    void append(const std::vector<std::byte>& more) {
        align();
        bytes_.insert(bytes_.end(), more.begin(), more.end());
        count_ += 8 * more.size();
    }

    [[nodiscard]] std::vector<std::byte> take() {
        align();
        return bytes_;
    }

private:
    void bit(unsigned b) {
        if (count_ % 8 == 0) {
            bytes_.push_back(std::byte{0});
        }
        if (b != 0) {
            bytes_.back() |= static_cast<std::byte>(1U << (7 - (count_ % 8)));
        }
        ++count_;
    }

    std::vector<std::byte> bytes_;
    std::size_t count_ = 0;
};

constexpr unsigned kFrameRate48Fps = 0x3;  // four pan sub blocks
constexpr unsigned kSubBlocks = 4;
constexpr std::uint32_t kBedDefinition = 0x10;
constexpr std::uint32_t kBedRemap = 0x20;
constexpr std::uint32_t kObjectDefinition = 0x40;
constexpr std::uint32_t kObjectZone19 = 0x80;
constexpr std::uint32_t kAudioDataDlc = 0x200;
constexpr std::uint32_t kAudioDataPcm = 0x400;
constexpr std::uint32_t kAuthoringToolInfo = 0x100;
constexpr std::uint32_t kUserData = 0x101;

std::vector<std::byte> element(std::uint32_t id, const std::vector<std::byte>& payload) {
    BitWriter w;
    w.plex(id, 8);
    w.plex(payload.size(), 8);
    w.append(payload);
    return w.take();
}

class Generator {
public:
    explicit Generator(std::uint32_t seed) : rng_(seed) {}

    unsigned pick(unsigned below) { return std::uniform_int_distribution<unsigned>(0, below - 1)(rng_); }
    bool coin() { return pick(2) == 1; }

    // A Plex(n) value, sometimes past the first escape.
    std::uint64_t plex_value(unsigned width) {
        const std::uint64_t first = (std::uint64_t{1} << width) - 1;
        return coin() ? pick(static_cast<unsigned>(first)) : first + pick(300);
    }

    void unity_gain(BitWriter& w) {
        const unsigned prefix = pick(4);
        w.bits(prefix, 2);
        if (prefix > 1) {
            w.bits(pick(1024), 10);
        }
    }
    void zone_gain(BitWriter& w) { unity_gain(w); }  // same shape
    void decor(BitWriter& w) {
        const unsigned prefix = pick(3);
        w.bits(prefix, 2);
        if (prefix > 1) {
            w.bits(pick(256), 8);
        }
    }
    void description(BitWriter& w) {
        const bool text = coin();
        w.bits((text ? 0x80U : 0U) | pick(0x40), 8);
        if (text) {
            for (unsigned n = pick(4); n > 0; --n) {
                w.bits('a' + pick(26), 8);
            }
            w.bits(0, 8);
        }
    }

    std::vector<std::byte> remap() {
        BitWriter w;
        w.plex(plex_value(8), 8);
        w.bits(pick(256), 8);
        const unsigned sources = 1 + pick(3);
        const unsigned destinations = 1 + pick(3);
        w.plex(sources, 4);
        w.plex(destinations, 4);
        for (unsigned sb = 0; sb < kSubBlocks; ++sb) {
            bool info = true;
            if (sb != 0) {
                info = coin();
                w.bits(info ? 1U : 0U, 1);
            }
            if (info) {
                for (unsigned d = 0; d < destinations; ++d) {
                    w.plex(pick(20), 4);
                    for (unsigned s = 0; s < sources; ++s) {
                        unity_gain(w);
                    }
                }
            }
        }
        w.align();
        w.plex(0, 8);
        return w.take();
    }

    std::vector<std::byte> bed(unsigned depth = 0) {
        BitWriter w;
        w.plex(plex_value(8), 8);
        const bool conditional = coin();
        w.bits(conditional ? 1U : 0U, 1);
        if (conditional) {
            w.bits(pick(256), 8);
        }
        const unsigned channels = 1 + pick(3);
        w.plex(channels, 4);
        for (unsigned c = 0; c < channels; ++c) {
            w.plex(plex_value(4), 4);
            w.plex(plex_value(8), 8);
            unity_gain(w);
            const bool decorrelated = coin();
            w.bits(decorrelated ? 1U : 0U, 1);
            if (decorrelated) {
                w.bits(0, 4);
                decor(w);
            }
        }
        w.bits(0x180, 10);
        w.align();
        description(w);
        const bool nested = depth == 0 && coin();
        w.plex(nested ? 2U : 1U, 8);
        w.append(element(kBedRemap, remap()));
        if (nested) {
            w.append(element(kBedDefinition, bed(depth + 1)));
        }
        return w.take();
    }

    std::vector<std::byte> zone19() {
        BitWriter w;
        for (unsigned sb = 0; sb < kSubBlocks; ++sb) {
            bool info = true;
            if (sb != 0) {
                info = coin();
                w.bits(info ? 1U : 0U, 1);
            }
            if (info) {
                for (int n = 0; n < 19; ++n) {
                    zone_gain(w);
                }
            }
        }
        return w.take();
    }

    std::vector<std::byte> object(unsigned depth = 0) {
        BitWriter w;
        w.plex(plex_value(8), 8);
        w.plex(plex_value(8), 8);
        const bool conditional = coin();
        w.bits(conditional ? 1U : 0U, 1);
        if (conditional) {
            w.bits(1, 1);
            w.bits(pick(256), 8);
        }
        w.bits(0, 1);
        for (unsigned sb = 0; sb < kSubBlocks; ++sb) {
            bool info = true;
            if (sb != 0) {
                info = coin();
                w.bits(info ? 1U : 0U, 1);
            }
            if (!info) {
                continue;
            }
            unity_gain(w);
            w.bits(0b001, 3);
            w.bits(pick(65536), 16);
            w.bits(pick(65536), 16);
            w.bits(pick(65536), 16);
            const bool snap = coin();
            w.bits(snap ? 1U : 0U, 1);
            if (snap) {
                const bool tolerance = coin();
                w.bits(tolerance ? 1U : 0U, 1);
                if (tolerance) {
                    w.bits(pick(4096), 12);
                }
                w.bits(0, 1);
            }
            const bool zones = coin();
            w.bits(zones ? 1U : 0U, 1);
            if (zones) {
                for (int n = 0; n < 9; ++n) {
                    zone_gain(w);
                }
            }
            const unsigned spread = pick(4);
            w.bits(spread, 2);
            if (spread == 0) {
                w.bits(pick(256), 8);
            } else if (spread == 2) {
                w.bits(pick(4096), 12);
            } else if (spread == 3) {
                w.bits(pick(4096), 12);
                w.bits(pick(4096), 12);
                w.bits(pick(4096), 12);
            }
            w.bits(0, 4);
            decor(w);
        }
        w.align();
        description(w);
        const bool nested = depth == 0 && coin();
        w.plex(nested ? 2U : 1U, 8);
        w.append(element(kObjectZone19, zone19()));
        if (nested) {
            w.append(element(kObjectDefinition, object(depth + 1)));
        }
        return w.take();
    }

private:
    std::mt19937 rng_;
};

// An IAFrame payload (§9.1 Table 5) at 48 kHz, 24-bit, 48 fps, around `children`.
std::vector<std::byte> iaframe(const std::vector<std::vector<std::byte>>& children) {
    BitWriter w;
    w.bits(1, 8);
    w.bits(0, 2);
    w.bits(1, 2);
    w.bits(kFrameRate48Fps, 4);
    w.plex(16, 8);
    w.align();
    w.plex(children.size(), 8);
    for (const auto& child : children) {
        w.append(child);
    }
    return w.take();
}

std::vector<std::byte> cut(const std::vector<std::byte>& bytes, std::size_t size) {
    return {bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(size)};
}

// Parses `payload` whole as element `id` inside a frame, then every strict prefix of it with an
// honest ElementSize, and counts the prefixes that were refused as truncated.
std::size_t refused_prefixes(std::uint32_t id, const std::vector<std::byte>& payload) {
    const auto whole = ac3iab::parse_iaframe(iaframe({element(id, payload)}));
    INFO("element " << id << ", " << payload.size() << " bytes");
    REQUIRE(whole.has_value());
    std::size_t refused = 0;
    for (std::size_t size = 0; size < payload.size(); ++size) {
        const auto parsed = ac3iab::parse_iaframe(iaframe({element(id, cut(payload, size))}));
        if (!parsed.has_value() && parsed.error() == ac3iab::IabError::kTruncated) {
            ++refused;
        }
    }
    return refused;
}

std::string text_of(const std::vector<std::byte>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::vector<std::byte> segment(const std::vector<std::byte>& frame_payload, const std::vector<std::byte>& preamble = {}) {
    BitWriter w;
    w.bits(0x01, 8);
    w.bits(preamble.size(), 32);
    w.append(preamble);
    const auto wrapped = element(0x08, frame_payload);
    w.bits(0x02, 8);
    w.bits(wrapped.size(), 32);
    w.append(wrapped);
    return w.take();
}

}  // namespace

TEST_CASE("randomly built BedDefinitions parse whole and are refused as truncated at every shorter size",
          "[ac3iab]") {
    Generator gen(0xB5D);
    for (int round = 0; round < 60; ++round) {
        const auto payload = gen.bed();
        CHECK(refused_prefixes(kBedDefinition, payload) == payload.size());
    }
}

TEST_CASE("randomly built ObjectDefinitions parse whole and are refused as truncated at every shorter size",
          "[ac3iab]") {
    Generator gen(0x0B7);
    for (int round = 0; round < 60; ++round) {
        const auto payload = gen.object();
        CHECK(refused_prefixes(kObjectDefinition, payload) == payload.size());
    }
}

TEST_CASE("a randomly built BedDefinition keeps what it was given", "[ac3iab]") {
    Generator gen(7);
    for (int round = 0; round < 20; ++round) {
        const auto frame = ac3iab::parse_iaframe(
            iaframe({element(kBedDefinition, gen.bed()), element(kObjectDefinition, gen.object())}));
        REQUIRE(frame.has_value());
        REQUIRE(frame->beds.size() == 1);
        REQUIRE(frame->objects.size() == 1);
        CHECK_FALSE(frame->beds[0].channels.empty());
        REQUIRE(frame->beds[0].remaps.size() == 1);
        CHECK(frame->beds[0].remaps[0].sub_blocks.size() == kSubBlocks);
        CHECK(frame->objects[0].sub_blocks.size() == kSubBlocks);
        CHECK(frame->objects[0].sub_blocks[0].has_pan_info);
        REQUIRE(frame->objects[0].zone19.has_value());
        for (const auto& block : frame->objects[0].sub_blocks) {
            CHECK(block.position.z >= 0.0);
            CHECK(block.position.z <= 1.0);
            CHECK(block.decorrelation >= 0.0);
            CHECK(block.decorrelation <= 1.0);
        }
    }
}

TEST_CASE("audio data, authoring and user data elements are refused as truncated when cut short",
          "[ac3iab]") {
    // PCM: an AudioDataID and one frame of 24-bit samples (48 fps at 48 kHz: 1000 samples).
    {
        BitWriter w;
        w.plex(300, 8);  // escalated AudioDataID
        for (int n = 0; n < 1000 * 3; ++n) {
            w.bits(static_cast<unsigned>(n) & 0xFFU, 8);
        }
        const auto pcm = w.take();
        for (const std::size_t size : {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{3}, pcm.size() - 1}) {
            const auto parsed = ac3iab::parse_iaframe(iaframe({element(kAudioDataPcm, cut(pcm, size))}));
            REQUIRE_FALSE(parsed.has_value());
            CHECK(parsed.error() == ac3iab::IabError::kTruncated);
        }
        const auto whole = ac3iab::parse_iaframe(iaframe({element(kAudioDataPcm, pcm)}));
        REQUIRE(whole.has_value());
        CHECK(whole->audio_pcm.at(0).audio_data_id == 300);
    }
    // DLC: an AudioDataID, a 16-bit DLCSize and that many coded bytes.
    {
        BitWriter w;
        w.plex(4, 8);
        w.bits(5, 16);
        for (int n = 0; n < 5; ++n) {
            w.bits(0xA0 + static_cast<unsigned>(n), 8);
        }
        const auto dlc = w.take();
        CHECK(refused_prefixes(kAudioDataDlc, dlc) == dlc.size());
    }
    // AuthoringToolInfo: a URI that loses its terminator.
    {
        const std::vector<std::byte> uri{std::byte{'x'}, std::byte{'y'}, std::byte{0}};
        CHECK(refused_prefixes(kAuthoringToolInfo, uri) == uri.size());
    }
    // UserData: the 16-byte id is required, the data after it is whatever remains.
    {
        std::vector<std::byte> user(16, std::byte{0x5A});
        user.push_back(std::byte{0x01});
        std::size_t refused = 0;
        for (std::size_t size = 0; size < 16; ++size) {
            const auto parsed = ac3iab::parse_iaframe(iaframe({element(kUserData, cut(user, size))}));
            refused += !parsed.has_value() && parsed.error() == ac3iab::IabError::kTruncated ? 1U : 0U;
        }
        CHECK(refused == 16);
        const auto short_data = ac3iab::parse_iaframe(iaframe({element(kUserData, cut(user, 16))}));
        REQUIRE(short_data.has_value());
        CHECK(short_data->user_data.at(0).data.empty());
    }
}

TEST_CASE("an IAFrame header and its element headers are refused as truncated when cut short", "[ac3iab]") {
    const auto payload = iaframe({element(kUserData, std::vector<std::byte>(16, std::byte{1}))});
    for (std::size_t size = 0; size < payload.size(); ++size) {
        const auto parsed = ac3iab::parse_iaframe(cut(payload, size));
        INFO("size " << size);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error() == ac3iab::IabError::kTruncated);
    }
}

TEST_CASE("an IA bitstream cut short anywhere is refused as truncated, and a wrong element is a bad frame tag",
          "[ac3iab]") {
    const std::vector<std::byte> preamble{std::byte{9}, std::byte{8}};
    const auto whole = segment(iaframe({}), preamble);
    {
        std::istringstream in(text_of(whole));
        const auto parsed = ac3iab::parse_iabitstream(in);
        REQUIRE(parsed.has_value());
        CHECK(parsed->at(0).preamble == preamble);
    }
    for (std::size_t size = 1; size < whole.size(); ++size) {
        std::istringstream in(text_of(cut(whole, size)));
        const auto parsed = ac3iab::parse_iabitstream(in);
        INFO("size " << size);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error() == ac3iab::IabError::kTruncated);
    }

    // The IAFrame segment wraps some element other than IA_FRAME.
    BitWriter w;
    w.bits(0x01, 8);
    w.bits(0, 32);
    const auto wrapped = element(kUserData, std::vector<std::byte>(16, std::byte{0}));
    w.bits(0x02, 8);
    w.bits(wrapped.size(), 32);
    w.append(wrapped);
    std::istringstream in(text_of(w.take()));
    const auto parsed = ac3iab::parse_iabitstream(in);
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error() == ac3iab::IabError::kBadFrameTag);

    // An IAFrame segment whose inner element declares more than the segment holds.
    BitWriter lying;
    lying.bits(0x01, 8);
    lying.bits(0, 32);
    lying.bits(0x02, 8);
    lying.bits(2, 32);
    lying.plex(0x08, 8);
    lying.plex(40, 8);
    std::istringstream lying_in(text_of(lying.take()));
    const auto lied = ac3iab::parse_iabitstream(lying_in);
    REQUIRE_FALSE(lied.has_value());
    CHECK(lied.error() == ac3iab::IabError::kTruncated);
}

TEST_CASE("an unreadable input is refused as cannot-open", "[ac3iab]") {
    const auto missing = ac3iab::parse_iabitstream(std::string(AC3FORGE_TEST_SCRATCH_DIR) + "/no-such-dir/none.iab");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error() == ac3iab::IabError::kCannotOpen);

    std::istringstream broken("");
    broken.setstate(std::ios::badbit);
    const auto bad = ac3iab::parse_iabitstream(broken);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error() == ac3iab::IabError::kCannotOpen);
}

TEST_CASE("describe names every IAB error", "[ac3iab]") {
    using ac3iab::IabError;
    for (const IabError error :
         {IabError::kCannotOpen, IabError::kTruncated, IabError::kBadEscape, IabError::kBadPreambleTag,
          IabError::kBadFrameTag, IabError::kReservedVersion, IabError::kReservedSampleRate,
          IabError::kReservedBitDepth, IabError::kReservedFrameRate, IabError::kUnterminatedString,
          IabError::kMxfBadKlv, IabError::kMxfNoIabEssence}) {
        const std::string_view text = ac3iab::describe(error);
        CHECK_FALSE(text.empty());
        CHECK(text != "unknown ac3iab error");
    }
    CHECK(ac3iab::describe(static_cast<IabError>(200)) == "unknown ac3iab error");
}

TEST_CASE("an IAFrame header at 96 kHz and 16 bits reads its rates, and its PCM at that rate",
          "[ac3iab]") {
    BitWriter w;
    w.bits(1, 8);
    w.bits(1, 2);  // 96 kHz
    w.bits(0, 2);  // 16-bit
    w.bits(kFrameRate48Fps, 4);
    w.plex(2, 8);
    w.align();
    w.plex(1, 8);
    BitWriter pcm;
    pcm.plex(1, 8);
    for (int n = 0; n < 2000; ++n) {  // 48 fps at 96 kHz: 2000 samples of 2 bytes
        pcm.bits(n == 0 ? 0x00U : 0xFFU, 8);
        pcm.bits(n == 0 ? 0x80U : 0x7FU, 8);
    }
    w.append(element(kAudioDataPcm, pcm.take()));
    const auto frame = ac3iab::parse_iaframe(w.take());
    REQUIRE(frame.has_value());
    CHECK(frame->sample_rate == 96000);
    CHECK(frame->bit_depth == 16);
    REQUIRE(frame->audio_pcm.size() == 1);
    REQUIRE(frame->audio_pcm[0].samples.size() == 2000);
    CHECK(frame->audio_pcm[0].samples[0] == -1.0F);  // 0x8000, little-endian
    CHECK(frame->audio_pcm[0].samples[1] > 0.99F);   // 0x7FFF
}

namespace {

std::vector<std::byte> mxf_key(bool iab) {
    std::vector<std::byte> key{std::byte{0x06}, std::byte{0x0E}, std::byte{0x2B}, std::byte{0x34},
                               std::byte{0x01}, std::byte{0x02}, std::byte{0x01}, std::byte{0x05},
                               std::byte{0x0D}, std::byte{0x01}, std::byte{0x03}, std::byte{0x01},
                               std::byte{0x16}, std::byte{0xCC}, std::byte{0x0D}, std::byte{0x01}};
    if (!iab) {
        key[12] = std::byte{0x15};
    }
    return key;
}

std::vector<std::byte> mxf_with(const std::vector<std::byte>& essence) {
    std::vector<std::byte> file = mxf_key(true);
    file.push_back(std::byte{0x83});  // long form, three length bytes
    file.push_back(std::byte{0});
    file.push_back(static_cast<std::byte>((essence.size() >> 8) & 0xFFU));
    file.push_back(static_cast<std::byte>(essence.size() & 0xFFU));
    file.insert(file.end(), essence.begin(), essence.end());
    return file;
}

}  // namespace

TEST_CASE("an MXF file cut inside a KLV Length is refused as truncated", "[ac3iab][mxf]") {
    const auto file = mxf_with(segment(iaframe({})));
    for (const std::size_t size : {std::size_t{16}, std::size_t{17}, std::size_t{19}}) {
        std::istringstream in(text_of(cut(file, size)));
        const auto parsed = ac3iab::parse_mxf_iab(in);
        INFO("size " << size);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error() == ac3iab::IabError::kTruncated);
    }
    // A long form with more than eight length bytes is not a KLV Length.
    auto too_long = mxf_key(false);
    too_long.push_back(std::byte{0x89});
    std::istringstream in(text_of(too_long));
    const auto parsed = ac3iab::parse_mxf_iab(in);
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error() == ac3iab::IabError::kMxfBadKlv);
}

TEST_CASE("an MXF file's essence that is not a valid IA bitstream is refused with the bitstream's own error",
          "[ac3iab][mxf]") {
    std::istringstream in(text_of(mxf_with({std::byte{0x07}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}})));
    const auto parsed = ac3iab::parse_mxf_iab(in);
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error() == ac3iab::IabError::kBadPreambleTag);
}

TEST_CASE("an MXF file is read from a path, and a missing path or a broken stream cannot be opened",
          "[ac3iab][mxf]") {
    const auto dir = std::filesystem::path{AC3FORGE_TEST_SCRATCH_DIR} / "ac3iab_truncation";
    std::filesystem::create_directories(dir);
    const auto path = dir / "clip.mxf";
    {
        const auto file = mxf_with(segment(iaframe({})));
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    }
    const auto read = ac3iab::parse_mxf_iab(path.string());
    REQUIRE(read.has_value());
    CHECK(read->size() == 1);
    std::filesystem::remove(path);

    const auto missing = ac3iab::parse_mxf_iab((dir / "none.mxf").string());
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error() == ac3iab::IabError::kCannotOpen);

    std::istringstream broken("");
    broken.setstate(std::ios::badbit);
    const auto bad = ac3iab::parse_mxf_iab(broken);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error() == ac3iab::IabError::kCannotOpen);
}
