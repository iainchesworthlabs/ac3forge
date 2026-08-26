// Parses the same Immersive Audio Bitstream (SMPTE ST 2098-2:2022) content two ways: once as a
// bare elementary `.iab` file (ac3iab::parse_iabitstream, roadmap IM1 phase 1) and once wrapped in
// a synthetic MXF IAB Track File (ac3iab::parse_mxf_iab, roadmap IM1 phase 2), printing what each
// found to show the two agree - the point being that SMPTE ST 2067-201 clip-wraps the whole
// IABitstream as a single Generic Container KLV Value, so an MXF Track File's essence really is
// the identical byte sequence an elementary `.iab` file already has (see
// src/ac3iab/src/mxf_reader.cpp's own header comment for the full citation trail).
//
// ac3iab::ac3iab is codec-blind - this program does not either, it only proves both parsed graphs
// are navigable and agree. A real IAB Track File is a production Dolby Atmos cinema/IMF master
// this project has no license to embed, so - like examples/read_adm.cpp for its own container -
// this writes its own tiny-but-valid fixtures to temp files first.

#include <fmt/printf.h>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "ac3iab/ac3iab.hpp"
#include "ac3iab/mxf.hpp"

namespace {

std::string scratch_path(std::string_view name) {
    static const std::string run = std::to_string(
        (static_cast<std::uint64_t>(std::random_device{}()) << 32) ^
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    const std::string leaf = "ac3forge_" + run + "_" + std::string(name);
    return (std::filesystem::temp_directory_path() / leaf).string();
}

void put_u8(std::vector<std::byte>& out, std::uint8_t v) {
    out.push_back(static_cast<std::byte>(v));
}

void put_bytes(std::vector<std::byte>& out, const std::vector<std::byte>& more) {
    out.insert(out.end(), more.begin(), more.end());
}

// SMPTE ST 2098-2:2022 §7 Table 2 / §8: a two-frame elementary IABitstream - each frame a
// Preamble (empty) plus one IAFrame segment wrapping the smallest legal IaFrame (§10.2: Version 1,
// 48 kHz, 16-bit, FrameRate code 0x3 [48 fps], no Beds/Objects/essence). Two frames, not one, so
// this program's own output shows a real multi-frame count rather than the degenerate case of 1.
std::vector<std::byte> build_elementary_iabitstream(unsigned frame_count) {
    std::vector<std::byte> out;
    for (unsigned i = 0; i < frame_count; ++i) {
        put_u8(out, 0x01);  // PreambleTag
        put_u8(out, 0x00);  // PreambleLength BE32 = 0
        put_u8(out, 0x00);
        put_u8(out, 0x00);
        put_u8(out, 0x00);
        put_u8(out, 0x02);  // IAFrameTag
        put_u8(out, 0x00);  // IAFrameLength BE32 = 6 (ElementID+ElementSize+4-byte payload)
        put_u8(out, 0x00);
        put_u8(out, 0x00);
        put_u8(out, 0x06);
        put_u8(out, 0x08);  // ElementID Plex(8) = 0x08 (IA_FRAME, §10.1.1 Table 14)
        put_u8(out, 0x04);  // ElementSize Plex(8) = 4
        put_u8(out, 0x01);  // Version = 1
        put_u8(out, 0x03);  // SampleRate=0 (48 kHz), BitDepth=0 (16-bit), FrameRate=0x3 (48 fps)
        put_u8(out, 0x00);  // MaxRendered Plex(8) = 0
        put_u8(out, 0x00);  // SubElementCount Plex(8) = 0
    }
    return out;
}

bool write_file(const std::string& path, const std::vector<std::byte>& bytes) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

// SMPTE ST 336:2017 §5.3 short-form BER length (every value here is well under 128).
std::vector<std::byte> klv(const std::array<std::uint8_t, 16>& key,
                           const std::vector<std::byte>& value) {
    std::vector<std::byte> out;
    for (auto b : key) {
        put_u8(out, b);
    }
    put_u8(out, static_cast<std::uint8_t>(value.size()));
    put_bytes(out, value);
    return out;
}

// Wraps `iabitstream` as an IAB Track File: a Header Partition Pack (ST 377-1 Table 4/6, its
// Value left empty - this reader only checks Keys, never Partition Pack fields, so an empty Value
// is enough to prove that) followed by the one clip-wrapped Essence Element KLV (ST 2067-201 Table
// 4.2) whose Value is `iabitstream` itself, byte for byte - see mxf.hpp's own header comment for
// why that Value needs no reframing at all.
std::vector<std::byte> wrap_as_mxf(const std::vector<std::byte>& iabitstream) {
    constexpr std::array<std::uint8_t, 16> kHeaderPartitionKey = {
        0x06, 0x0E, 0x2B, 0x34, 0x02, 0x05, 0x01, 0x01,
        0x0D, 0x01, 0x02, 0x01, 0x01, 0x02, 0x04, 0x00};
    constexpr std::array<std::uint8_t, 16> kIabEssenceKey = {0x06, 0x0E, 0x2B, 0x34, 0x01, 0x02,
                                                             0x01, 0x01, 0x0D, 0x01, 0x03, 0x01,
                                                             0x16, 0xCC, 0x0D, 0x01};
    std::vector<std::byte> out;
    put_bytes(out, klv(kHeaderPartitionKey, {}));
    put_bytes(out, klv(kIabEssenceKey, iabitstream));
    return out;
}

}  // namespace

int main() {
    const auto iabitstream = build_elementary_iabitstream(2);

    const auto elementary_path = scratch_path("read_iab_elementary.iab");
    const auto mxf_path = scratch_path("read_iab_wrapped.mxf");
    if (!write_file(elementary_path, iabitstream) ||
        !write_file(mxf_path, wrap_as_mxf(iabitstream))) {
        fmt::printf("could not write fixture files\n");
        return 1;
    }

    const auto elementary = ac3iab::parse_iabitstream(elementary_path);
    const auto mxf = ac3iab::parse_mxf_iab(mxf_path);
    std::filesystem::remove(elementary_path);
    std::filesystem::remove(mxf_path);

    if (!elementary) {
        fmt::printf("parse_iabitstream failed: %.*s\n",
                    static_cast<int>(ac3iab::describe(elementary.error()).size()),
                    ac3iab::describe(elementary.error()).data());
        return 1;
    }
    if (!mxf) {
        fmt::printf("parse_mxf_iab failed: %.*s\n",
                    static_cast<int>(ac3iab::describe(mxf.error()).size()),
                    ac3iab::describe(mxf.error()).data());
        return 1;
    }

    fmt::printf("elementary .iab: %zu frame(s)\n", elementary->size());
    fmt::printf("MXF track file:  %zu frame(s)\n", mxf->size());
    if (elementary->size() != mxf->size()) {
        fmt::printf("frame counts disagree\n");
        return 1;
    }

    for (std::size_t i = 0; i < elementary->size(); ++i) {
        const auto& a = (*elementary)[i].frame;
        const auto& b = (*mxf)[i].frame;
        fmt::printf("  frame %zu: elementary %u Hz/%u-bit, MXF %u Hz/%u-bit\n", i, a.sample_rate,
                    a.bit_depth, b.sample_rate, b.bit_depth);
        if (a.sample_rate != b.sample_rate || a.bit_depth != b.bit_depth) {
            fmt::printf("frame %zu disagrees between the two containers\n", i);
            return 1;
        }
    }

    fmt::printf("both containers agree\n");
    return 0;
}
