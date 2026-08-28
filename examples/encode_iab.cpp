// A real Immersive Audio Bitstream, all the way to a Dolby Atmos E-AC-3 (DD+ JOC) elementary
// stream.
//
// Roadmap item IM1 phase 3 of 3 (the last piece - phase 1 is ac3iab::ac3iab, src/ac3iab; phase 2
// is ac3iab::parse_mxf_iab, src/ac3iab/src/mxf_reader.cpp). This is a minimal, standalone
// illustration of the same pipeline ac3cli's 'atmos-iab' command drives for real:
// ac3iab::parse_iabitstream() reads the frame sequence, ac3::admbridge::build_iab() maps it onto
// ac3::oba::AtmosEncoder's flat object-list input shape (one bed channel pinned in place, one
// dynamic object panned by its own authored motion), and a plain per-frame loop calls
// ac3::oba::evaluate_placements() plus AtmosEncoder::encode_frame() the same way every other Atmos
// example in this directory does. The CLI command and this example deliberately share nothing but
// that library API - see docs/library/adm-bridge.md's own note on why no separate "driving loop"
// abstraction exists (the same reasoning applies here).
//
// Like examples/read_iab.cpp, this writes its own tiny-but-valid elementary IABitstream fixture to
// a temp file first, rather than shipping a real Dolby Atmos cinema master this project has no
// license to embed: one Bed channel (Center) and one Object that holds hard right for the first
// half of the clip and then jumps hard left for the second half - the same "visibly tracks the
// authored automation" shape encode_adm.cpp's own fixture uses, adapted to what IAB's per-frame
// (not whole-file audioBlockFormat) automation model can actually express - see
// ac3/admbridge/iab_bridge.hpp's own top comment on why a Bed/Object's position is one value per
// IAFrame here, not a file-length keyframe sequence.
//
// Run with `--write-fixture <path>` to just write that same fixture to a real file and exit,
// skipping the parse/bridge/encode demo below - see main()'s own comment on why
// tools/ci/run_codec_matrix.sh uses exactly this to drive a real `ac3cli atmos-iab` invocation
// (the same convention encode_adm.cpp's own --write-fixture already established).

#include <fmt/printf.h>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/admbridge/iab_bridge.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/oba/atmos.hpp"
#include "ac3/oba/motion.hpp"
#include "ac3iab/ac3iab.hpp"

namespace {

std::string scratch_path(std::string_view name) {
    static const std::string run = std::to_string(
        (static_cast<std::uint64_t>(std::random_device{}()) << 32) ^
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    const std::string leaf = "ac3forge_" + run + "_" + std::string(name);
    return (std::filesystem::temp_directory_path() / leaf).string();
}

// A from-scratch MSB-first bit writer (SMPTE ST 2098-2:2022 §5.1), used only to build this
// fixture - independent of src/ac3iab's own reader, the same "independent fixture" convention
// tests/ac3iab/test_ac3iab.cpp and examples/read_iab.cpp already establish (a third copy is within
// this project's own established limit - see encode_adm.cpp's identical note for its ADM fixture).
class BitWriter {
   public:
    void push_bits(std::uint64_t value, unsigned width) {
        for (unsigned i = 0; i < width; ++i) {
            push_bit(static_cast<unsigned>((value >> (width - 1 - i)) & 0x1u));
        }
    }

    // §5.2 Plex(n) encode.
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

// §9 Table 3 / §10.1: wraps a payload with its ElementID/ElementSize header.
std::vector<std::byte> wrap_element(std::uint32_t id, const std::vector<std::byte>& payload) {
    BitWriter bw;
    bw.push_plex(id, 8);
    bw.push_plex(payload.size(), 8);
    auto bytes = bw.bytes();
    append(bytes, payload);
    return bytes;
}

constexpr std::uint8_t kFrameRateCode = 0x8;        // §10.2.4 Table 17: 120 fps -> 2 pan sub blocks
                                                    // (Table 23), the number build_object_payload()
                                                    // writes below
constexpr std::uint32_t kSamplesPerIabFrame = 400;  // Table 18: 120 fps @ 48 kHz
constexpr std::uint32_t kSampleRate = 48000;
constexpr unsigned kTotalIabFrames = 20;  // 8000 samples total - >= 3 full AC-3 frames (1536 each)

// §5.4: valid ObjectPosX/Y domain is the UPPER half of the 16-bit range (2^15 <= Dn <= 2^16 - 1),
// mapping to [0, 1] - the formula's own stated domain, confirmed directly against the published
// clause text rather than assumed from field width alone. 0x8000 (2^15) -> ~0.0; 0xFFFF -> 1.0.
constexpr std::uint16_t kPosMin = 0x8000;  // ~left wall / front wall
constexpr std::uint16_t kPosMax = 0xFFFF;  // right wall / back wall

// §9.2 Table 6: one Bed channel (Center), unity gain, no decorrelation.
std::vector<std::byte> build_bed_payload() {
    BitWriter bw;
    bw.push_plex(1, 8);       // MetaID
    bw.push_bits(0, 1);       // ConditionalBed = 0
    bw.push_plex(1, 4);       // ChannelCount = 1
    bw.push_plex(0x2, 4);     // ChannelID = Center (Table 19)
    bw.push_plex(1, 8);       // AudioDataID
    bw.push_bits(0, 2);       // ChannelGainPrefix = unity
    bw.push_bits(0, 1);       // ChannelDecorInfoExists = 0
    bw.push_bits(0x180, 10);  // Reserved, set to 0x180
    bw.align_to_byte();
    bw.push_bits(0x01, 8);  // AudioDescription = not indicated
    bw.push_plex(0, 8);     // SubElementCount = 0
    return bw.bytes();
}

// §9.4 Table 8: one Object, held at (x, y) for this whole IAFrame (both sub blocks - the second
// carries PanInfoExists = 0, "no new panning information", per §10.5.4's own carry-forward rule).
std::vector<std::byte> build_object_payload(std::uint16_t pos_x, std::uint16_t pos_y) {
    BitWriter bw;
    bw.push_plex(2, 8);  // MetaID
    bw.push_plex(2, 8);  // AudioDataID
    bw.push_bits(0, 1);  // ConditionalObject = 0
    bw.push_bits(0, 1);  // Reserved, set to 0

    // Sub block 0 - PanInfoExists is implicit (always present, no bit).
    bw.push_bits(0, 2);       // ObjectGainPrefix = unity
    bw.push_bits(0b001, 3);   // Reserved
    bw.push_bits(pos_x, 16);  // ObjectPosX
    bw.push_bits(pos_y, 16);  // ObjectPosY
    bw.push_bits(0, 16);      // ObjectPosZ = 0.0 (screen height - DistanceZ's full range is valid)
    bw.push_bits(0, 1);       // ObjectSnap = 0 (no tolerance field follows)
    bw.push_bits(0, 1);       // ObjectZoneControl = 0
    bw.push_bits(0x1, 2);     // ObjectSpreadMode = None
    bw.push_bits(0, 4);       // Reserved
    bw.push_bits(0, 2);       // ObjectDecorCoefPrefix = none

    // Sub block 1 - carried forward from sub block 0 (same position for this whole IAFrame; this
    // demo varies position frame to frame, not sub-block to sub-block).
    bw.push_bits(0, 1);  // PanInfoExists = 0

    bw.align_to_byte();
    bw.push_bits(0x01, 8);  // AudioDescription = not indicated
    bw.push_plex(0, 8);     // SubElementCount = 0
    return bw.bytes();
}

// §9.7/§10.8.1: little-endian 16-bit PCM.
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

// One §9.1 Table 5 IaFrame, wrapped as one §7 Table 2 Preamble+IAFrame segment pair.
std::vector<std::byte> build_iaframe(unsigned index) {
    const bool right_half = index < kTotalIabFrames / 2;
    const std::uint16_t pos_x = right_half ? kPosMax : kPosMin;  // hard right, then hard left
    constexpr std::uint16_t kPosYMid = 0xC000;                   // ~0.5 (mid unit-cube domain)

    const double amplitude = 0.3;
    std::vector<std::int16_t> bed_samples(kSamplesPerIabFrame);
    std::vector<std::int16_t> object_samples(kSamplesPerIabFrame);
    for (std::uint32_t i = 0; i < kSamplesPerIabFrame; ++i) {
        const double t =
            static_cast<double>(index * kSamplesPerIabFrame + i) / static_cast<double>(kSampleRate);
        bed_samples[i] = static_cast<std::int16_t>(amplitude * 32767.0 *
                                                   std::sin(2.0 * std::numbers::pi * 300.0 * t));
        object_samples[i] = static_cast<std::int16_t>(amplitude * 32767.0 *
                                                      std::sin(2.0 * std::numbers::pi * 800.0 * t));
    }

    // §9.1 Table 5: IaFrame's own fixed fields, then 4 children (BedDefinition, ObjectDefinition,
    // and one AudioDataPCM each for the bed and the object).
    BitWriter header;
    header.push_bits(1, 8);               // Version
    header.push_bits(0, 2);               // SampleRate = 48 kHz
    header.push_bits(0, 2);               // BitDepth = 16-bit
    header.push_bits(kFrameRateCode, 4);  // FrameRate
    header.push_plex(0, 8);               // MaxRendered
    header.align_to_byte();
    header.push_plex(4, 8);  // SubElementCount

    std::vector<std::byte> iaframe_payload = header.bytes();
    append(iaframe_payload, wrap_element(0x10, build_bed_payload()));  // BedDefinition
    append(iaframe_payload,
           wrap_element(0x40, build_object_payload(pos_x, kPosYMid)));  // ObjectDefinition
    append(iaframe_payload,
           wrap_element(0x400, build_pcm_payload(1, bed_samples)));  // AudioDataPCM (bed)
    append(iaframe_payload,
           wrap_element(0x400, build_pcm_payload(2, object_samples)));  // AudioDataPCM (object)

    std::vector<std::byte> segment;
    segment.push_back(std::byte{0x01});  // PreambleTag
    for (int shift : {24, 16, 8, 0}) {
        segment.push_back(static_cast<std::byte>((0u >> shift) & 0xFFu));  // PreambleLength = 0
    }
    segment.push_back(std::byte{0x02});                        // IAFrameTag
    const auto element = wrap_element(0x08, iaframe_payload);  // IAFrame's own ElementID (Table 14)
    const auto frame_length = static_cast<std::uint32_t>(element.size());
    for (int shift : {24, 16, 8, 0}) {
        segment.push_back(static_cast<std::byte>((frame_length >> shift) & 0xFFu));
    }
    append(segment, element);
    return segment;
}

bool write_fixture(const std::string& path) {
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

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::string_view{argv[1]} == "--write-fixture") {
        if (!write_fixture(argv[2])) {
            fmt::printf("could not write fixture file\n");
            return 1;
        }
        return 0;
    }

    const auto fixture_path = scratch_path("encode_iab_fixture.iab");
    if (!write_fixture(fixture_path)) {
        fmt::printf("could not write fixture file\n");
        return 1;
    }

    // Step 1: ac3iab::ac3iab (phases 1-2) - frame framing and element graph.
    const auto frames = ac3iab::parse_iabitstream(fixture_path);
    std::filesystem::remove(fixture_path);
    if (!frames) {
        fmt::printf("parse_iabitstream failed: %.*s\n",
                    static_cast<int>(ac3iab::describe(frames.error()).size()),
                    ac3iab::describe(frames.error()).data());
        return 1;
    }

    // Step 2: ac3::admbridge (phase 3) - Bed/Object identity, coordinate conversion, and the
    // per-IAFrame position/gain timeline, mapped onto AtmosEncoder's flat object-list input shape.
    const auto bridged = ac3::admbridge::build_iab(*frames);
    if (!bridged) {
        fmt::printf("build_iab failed: %.*s\n",
                    static_cast<int>(ac3::admbridge::describe(bridged.error()).size()),
                    ac3::admbridge::describe(bridged.error()).data());
        return 1;
    }

    fmt::printf("bridged %zu channel(s) from %zu IAB frame(s)\n", bridged->channel_count(),
                frames->size());
    for (std::size_t i = 0; i < bridged->channel_count(); ++i) {
        fmt::printf("  %s: %s\n", bridged->channel_ids[i].c_str(),
                    bridged->is_bed[i] ? "bed channel" : "dynamic object");
    }

    // Step 3: drive AtmosEncoder::encode_frame() in a loop - ac3::oba::evaluate_placements() reads
    // each channel's ac3::oba::ObjectPath at the frame's own end time, the same pattern every other
    // Atmos example in this directory uses.
    const auto objects = static_cast<int>(bridged->channel_count());
    ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = 448}, objects};

    const auto total_samples = bridged->pcm.empty() ? std::size_t{0} : bridged->pcm.front().size();
    const auto total_frames = total_samples / static_cast<std::size_t>(ac3::kSamplesPerFrame);
    std::vector<std::span<const float>> views(bridged->channel_count());
    std::vector<std::byte> stream;

    for (std::size_t f = 0; f < total_frames; ++f) {
        const auto start = f * static_cast<std::size_t>(ac3::kSamplesPerFrame);
        for (std::size_t ch = 0; ch < bridged->channel_count(); ++ch) {
            views[ch] = std::span<const float>(bridged->pcm[ch])
                            .subspan(start, static_cast<std::size_t>(ac3::kSamplesPerFrame));
        }
        const double t =
            static_cast<double>(start + static_cast<std::size_t>(ac3::kSamplesPerFrame)) /
            static_cast<double>(kSampleRate);
        const auto placement = ac3::oba::evaluate_placements(bridged->paths, t);

        const auto unit = encoder.encode_frame(views, placement);
        if (!unit) {
            fmt::printf("encode_frame failed: %d\n", static_cast<int>(unit.error()));
            return 1;
        }
        stream.insert(stream.end(), unit->bytes.begin(), unit->bytes.end());
    }

    const auto out_path = scratch_path("encode_iab_out.ec3");
    std::ofstream out(out_path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(stream.data()),
              static_cast<std::streamsize>(stream.size()));
    const bool wrote = static_cast<bool>(out);
    out.close();
    std::filesystem::remove(out_path);
    if (!wrote) {
        fmt::printf("could not write output stream\n");
        return 1;
    }

    std::size_t bed_count = 0;
    for (const bool is_bed : bridged->is_bed) {
        bed_count += is_bed ? 1 : 0;
    }
    fmt::printf(
        "%zu bytes of DD+ JOC E-AC-3 from %zu IAB-authored frame(s): %zu bed channel(s) + "
        "%zu dynamic object(s)\n",
        stream.size(), total_frames, bed_count, bridged->channel_count() - bed_count);
    return 0;
}
