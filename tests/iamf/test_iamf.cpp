#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "iamf/iamf.hpp"

// These tests read iamf::mux()'s output back with an independent OBU/ISOBMFF walker rather than
// comparing against bytes this same code produced - the same reasoning as test_mp4.cpp's own
// header comment: a muxer checked only against itself proves nothing about whether a real IAMF
// parser can open the file. The walker below re-derives field offsets straight from the IAMF
// v1.1.0 specification, independently of src/iamf/src/obu_detail.hpp and isobmff_detail.hpp.

namespace {

using Bytes = std::vector<std::byte>;

[[nodiscard]] std::uint8_t byte_at(std::span<const std::byte> data, std::size_t offset) {
    return std::to_integer<std::uint8_t>(data[offset]);
}

[[nodiscard]] std::uint16_t u16_at(std::span<const std::byte> data, std::size_t offset) {
    return static_cast<std::uint16_t>((static_cast<std::uint32_t>(byte_at(data, offset)) << 8) |
                                      byte_at(data, offset + 1));
}

[[nodiscard]] std::uint32_t u32_at(std::span<const std::byte> data, std::size_t offset) {
    return (static_cast<std::uint32_t>(byte_at(data, offset)) << 24) |
           (static_cast<std::uint32_t>(byte_at(data, offset + 1)) << 16) |
           (static_cast<std::uint32_t>(byte_at(data, offset + 2)) << 8) |
           static_cast<std::uint32_t>(byte_at(data, offset + 3));
}

[[nodiscard]] std::int16_t s16_at(std::span<const std::byte> data, std::size_t offset) {
    return static_cast<std::int16_t>(u16_at(data, offset));
}

// PCM samples (IAMF §3.11.4) are little-endian (sample_format_flags = 0x01), unlike every
// ISOBMFF box field above - a distinct convention this needs its own reader for.
[[nodiscard]] std::int16_t s16_le_at(std::span<const std::byte> data, std::size_t offset) {
    const auto value = static_cast<std::uint16_t>(
        static_cast<std::uint32_t>(byte_at(data, offset)) |
        (static_cast<std::uint32_t>(byte_at(data, offset + 1)) << 8));
    return static_cast<std::int16_t>(value);
}

[[nodiscard]] std::string fourcc_at(std::span<const std::byte> data, std::size_t offset) {
    std::string s(4, '\0');
    for (std::size_t i = 0; i < 4; ++i) {
        s[i] = static_cast<char>(byte_at(data, offset + i));
    }
    return s;
}

// AV1 Bitstream & Decoding Process Specification §5.3.3 leb128() - see obu_detail.hpp's own
// citation. `consumed` is set to the number of bytes the value occupied. Not [[nodiscard]] -
// several call sites below only need `consumed`, to skip a field this writer's own value for is
// not being asserted at that point.
std::uint64_t leb128_at(std::span<const std::byte> data, std::size_t offset,
                        std::size_t& consumed) {
    std::uint64_t value = 0;
    std::size_t i = 0;
    for (; i < 8; ++i) {
        const auto b = byte_at(data, offset + i);
        value |= static_cast<std::uint64_t>(b & 0x7F) << (7 * i);
        if ((b & 0x80) == 0) {
            ++i;
            break;
        }
    }
    consumed = i;
    return value;
}

// --- ISOBMFF box walker (ISO/IEC 14496-12 §4.2) -----------------------------------------------

struct Element {
    std::string type;
    std::size_t payload = 0;
    std::uint64_t length = 0;
};

bool is_container(const std::string& type) {
    return type == "moov" || type == "trak" || type == "mdia" || type == "minf" ||
           type == "stbl" || type == "dinf";
}

void walk(std::span<const std::byte> file, std::size_t pos, std::size_t end,
         std::vector<Element>& out) {
    while (pos < end) {
        REQUIRE(pos + 8 <= end);
        const auto size = u32_at(file, pos);
        const auto type = fourcc_at(file, pos + 4);
        REQUIRE(size >= 8);
        REQUIRE(pos + size <= end);
        out.push_back({type, pos + 8, size - 8});
        if (is_container(type)) {
            walk(file, pos + 8, pos + size, out);
        }
        pos += size;
    }
    REQUIRE(pos == end);
}

std::vector<Element> parse(std::span<const std::byte> file) {
    std::vector<Element> out;
    walk(file, 0, file.size(), out);
    return out;
}

const Element* find(const std::vector<Element>& elements, const std::string& type) {
    for (const auto& e : elements) {
        if (e.type == type) {
            return &e;
        }
    }
    return nullptr;
}

// --- OBU walker (IAMF §3.2) --------------------------------------------------------------------

struct Obu {
    std::uint8_t obu_type = 0;  // top 5 bits of the header byte
    std::size_t payload = 0;
    std::uint64_t length = 0;
};

// Walks a flat run of OBUs (no trimming/extension - this writer never sets either flag, so every
// header is exactly 1 byte plus a leb128 size).
std::vector<Obu> parse_obus(std::span<const std::byte> data) {
    std::vector<Obu> out;
    std::size_t pos = 0;
    while (pos < data.size()) {
        const auto header = byte_at(data, pos);
        REQUIRE((header & 0x07) == 0);  // redundant/trim/ext all 0
        std::size_t consumed = 0;
        const auto size = leb128_at(data, pos + 1, consumed);
        REQUIRE(consumed > 0);
        const std::size_t payload_offset = pos + 1 + consumed;
        REQUIRE(payload_offset + size <= data.size());
        out.push_back({static_cast<std::uint8_t>(header >> 3), payload_offset, size});
        pos = payload_offset + size;
    }
    REQUIRE(pos == data.size());
    return out;
}

// IASampleEntry ('iamf', extends AudioSampleEntry) -> its one child, IAConfigurationBox
// ('iacb'). stsd body: version+flags(4), entry_count(4), then the sample entry.
struct IaSampleEntryInfo {
    std::uint16_t channelcount = 0;
    std::uint32_t samplerate_fixed = 0;
    Bytes config_obus;
};

IaSampleEntryInfo read_sample_entry(std::span<const std::byte> file, const Element& stsd) {
    const std::size_t entry_offset = stsd.payload + 8;
    const auto entry_size = u32_at(file, entry_offset);
    REQUIRE(fourcc_at(file, entry_offset + 4) == "iamf");
    const std::size_t body = entry_offset + 8;
    IaSampleEntryInfo info;
    info.channelcount = u16_at(file, body + 16);
    info.samplerate_fixed = u32_at(file, body + 24);
    const std::size_t iacb_offset = body + 28;
    const auto iacb_size = u32_at(file, iacb_offset);
    REQUIRE(fourcc_at(file, iacb_offset + 4) == "iacb");
    REQUIRE(iacb_offset + iacb_size == entry_offset + entry_size);
    const std::size_t iacb_body = iacb_offset + 8;
    REQUIRE(byte_at(file, iacb_body) == 1);  // configurationVersion
    std::size_t consumed = 0;
    const auto config_size = leb128_at(file, iacb_body + 1, consumed);
    const std::size_t config_offset = iacb_body + 1 + consumed;
    REQUIRE(config_offset + config_size == iacb_offset + iacb_size);
    info.config_obus.assign(file.begin() + static_cast<std::ptrdiff_t>(config_offset),
                            file.begin() + static_cast<std::ptrdiff_t>(config_offset + config_size));
    return info;
}

std::vector<std::uint32_t> read_stco(std::span<const std::byte> file, const Element& stco) {
    const auto entry_count = u32_at(file, stco.payload + 4);
    std::vector<std::uint32_t> offsets(entry_count);
    for (std::uint32_t i = 0; i < entry_count; ++i) {
        offsets[i] = u32_at(file, stco.payload + 8 + static_cast<std::size_t>(i) * 4);
    }
    return offsets;
}

std::vector<std::uint32_t> read_stsz(std::span<const std::byte> file, const Element& stsz) {
    const auto sample_count = u32_at(file, stsz.payload + 8);
    std::vector<std::uint32_t> sizes(sample_count);
    for (std::uint32_t i = 0; i < sample_count; ++i) {
        sizes[i] = u32_at(file, stsz.payload + 12 + static_cast<std::size_t>(i) * 4);
    }
    return sizes;
}

[[nodiscard]] std::int16_t to_q7_8(float value) {
    const float scaled = std::round(value * 256.0F);
    return static_cast<std::int16_t>(std::clamp(scaled, -32768.0F, 32767.0F));
}

[[nodiscard]] std::int32_t quantize_pcm(float sample, int bit_depth) {
    const auto full_scale = static_cast<double>((std::uint32_t{1} << (bit_depth - 1)) - 1);
    const double scaled = std::round(static_cast<double>(sample) * full_scale);
    return static_cast<std::int32_t>(std::clamp(scaled, -full_scale - 1.0, full_scale));
}

iamf::Frame make_frame(std::uint32_t samples_per_frame, float value_per_sample) {
    iamf::Frame frame;
    for (auto& channel : frame.channels) {
        channel.assign(samples_per_frame, value_per_sample);
    }
    return frame;
}

}  // namespace

TEST_CASE("IAMF file parses as well-formed ISOBMFF boxes carrying the iamf brand", "[iamf]") {
    const iamf::AudioTrack track{.samples_per_frame = 960};
    const std::vector<iamf::Frame> frames{make_frame(960, 0.1F), make_frame(960, -0.2F),
                                          make_frame(960, 0.3F)};
    const auto file = iamf::mux(track, frames);
    REQUIRE(file.has_value());

    const auto elements = parse(*file);
    REQUIRE(find(elements, "ftyp") != nullptr);
    REQUIRE(find(elements, "moov") != nullptr);
    REQUIRE(find(elements, "mdat") != nullptr);
    REQUIRE(find(elements, "trak") != nullptr);
    REQUIRE(find(elements, "mdia") != nullptr);
    REQUIRE(find(elements, "minf") != nullptr);
    REQUIRE(find(elements, "stbl") != nullptr);
    REQUIRE(find(elements, "dinf") != nullptr);

    const auto* ftyp = find(elements, "ftyp");
    CHECK(fourcc_at(*file, ftyp->payload) == "iamf");  // major_brand, IAMF §6.1
    CHECK((fourcc_at(*file, ftyp->payload + 8) == "iamf" ||
          fourcc_at(*file, ftyp->payload + 12) == "iamf"));  // 'iamf' in compatible_brands
}

TEST_CASE("IAMF sample entry carries the four Descriptor OBUs in the required order", "[iamf]") {
    const iamf::AudioTrack track{.samples_per_frame = 512};
    const std::vector<iamf::Frame> frames{make_frame(512, 0.0F)};
    const auto file = iamf::mux(track, frames);
    REQUIRE(file.has_value());
    const auto elements = parse(*file);

    const auto* stsd = find(elements, "stsd");
    REQUIRE(stsd != nullptr);
    const auto entry = read_sample_entry(*file, *stsd);
    CHECK(entry.channelcount == 0);      // IAMF §6.2.3: SHALL be 0
    CHECK(entry.samplerate_fixed == 0);  // IAMF §6.2.3: SHALL be 0

    const auto obus = parse_obus(entry.config_obus);
    REQUIRE(obus.size() == 4);
    CHECK(obus[0].obu_type == 31);  // IA Sequence Header (IAMF §3.2/§6.2.4)
    CHECK(obus[1].obu_type == 0);   // Codec Config
    CHECK(obus[2].obu_type == 1);   // Audio Element
    CHECK(obus[3].obu_type == 2);   // Mix Presentation
}

TEST_CASE("IA Sequence Header OBU declares the iamf code and Simple Profile", "[iamf]") {
    const iamf::AudioTrack track{};
    const std::vector<iamf::Frame> frames{make_frame(track.samples_per_frame, 0.0F)};
    const auto file = iamf::mux(track, frames);
    REQUIRE(file.has_value());
    const auto entry = read_sample_entry(*file, *find(parse(*file), "stsd"));
    const auto obus = parse_obus(entry.config_obus);

    const auto& header = obus[0];
    CHECK(fourcc_at(entry.config_obus, header.payload) == "iamf");  // ia_code, IAMF §3.4
    CHECK(byte_at(entry.config_obus, header.payload + 4) == 0);     // primary_profile: Simple
    CHECK(byte_at(entry.config_obus, header.payload + 5) == 0);     // additional_profile: Simple
}

TEST_CASE("Codec Config OBU declares ipcm at the track's own sample rate and bit depth", "[iamf]") {
    const iamf::AudioTrack track{.sample_rate = 96000, .bit_depth = 32, .samples_per_frame = 480};
    const std::vector<iamf::Frame> frames{make_frame(480, 0.0F)};
    const auto file = iamf::mux(track, frames);
    REQUIRE(file.has_value());
    const auto entry = read_sample_entry(*file, *find(parse(*file), "stsd"));
    const auto obus = parse_obus(entry.config_obus);

    const auto& cfg = obus[1];
    std::size_t pos = cfg.payload;
    std::size_t consumed = 0;
    leb128_at(entry.config_obus, pos, consumed);  // codec_config_id, value not asserted
    pos += consumed;
    CHECK(fourcc_at(entry.config_obus, pos) == "ipcm");  // codec_id, IAMF §3.5
    pos += 4;
    const auto num_samples_per_frame = leb128_at(entry.config_obus, pos, consumed);
    CHECK(num_samples_per_frame == 480);
    pos += consumed;
    CHECK(s16_at(entry.config_obus, pos) == 0);  // audio_roll_distance: 0 for ipcm, IAMF §3.5
    pos += 2;
    CHECK(byte_at(entry.config_obus, pos) == 0x01);  // sample_format_flags: little-endian
    pos += 1;
    CHECK(byte_at(entry.config_obus, pos) == 32);  // sample_size
    pos += 1;
    CHECK(u32_at(entry.config_obus, pos) == 96000);  // sample_rate
}

TEST_CASE("Audio Element OBU declares a channel-based 7.1.4 layer with 5 coupled substreams",
         "[iamf]") {
    const iamf::AudioTrack track{};
    const std::vector<iamf::Frame> frames{make_frame(track.samples_per_frame, 0.0F)};
    const auto file = iamf::mux(track, frames);
    REQUIRE(file.has_value());
    const auto entry = read_sample_entry(*file, *find(parse(*file), "stsd"));
    const auto obus = parse_obus(entry.config_obus);

    const auto& ae = obus[2];
    std::size_t pos = ae.payload;
    std::size_t consumed = 0;
    leb128_at(entry.config_obus, pos, consumed);  // audio_element_id
    pos += consumed;
    CHECK((byte_at(entry.config_obus, pos) >> 5) == 0);  // audio_element_type: CHANNEL_BASED
    pos += 1;
    leb128_at(entry.config_obus, pos, consumed);  // codec_config_id
    pos += consumed;

    const auto num_substreams = leb128_at(entry.config_obus, pos, consumed);
    REQUIRE(num_substreams == 7);
    pos += consumed;
    for (std::uint64_t id = 0; id < num_substreams; ++id) {
        CHECK(leb128_at(entry.config_obus, pos, consumed) == id);  // §3.6.3.3 ordering
        pos += consumed;
    }
    const auto num_parameters = leb128_at(entry.config_obus, pos, consumed);
    CHECK(num_parameters == 0);
    pos += consumed;

    CHECK((byte_at(entry.config_obus, pos) >> 5) == 1);  // ScalableChannelLayoutConfig num_layers
    pos += 1;
    const auto layer_byte = byte_at(entry.config_obus, pos);
    CHECK((layer_byte >> 4) == 7);          // loudspeaker_layout: 7.1.4ch
    CHECK(((layer_byte >> 3) & 1) == 0);    // output_gain_is_present_flag
    CHECK(((layer_byte >> 2) & 1) == 0);    // recon_gain_is_present_flag
    pos += 1;
    CHECK(byte_at(entry.config_obus, pos) == 7);  // substream_count
    pos += 1;
    CHECK(byte_at(entry.config_obus, pos) == 5);  // coupled_substream_count
}

TEST_CASE("Mix Presentation OBU carries the mandatory Stereo layout plus the 7.1.4 layout",
         "[iamf]") {
    iamf::AudioTrack track{};
    track.stereo_loudness = {.integrated_loudness_lkfs = -23.0F, .digital_peak_dbfs = -1.5F};
    track.layout_714_loudness = {.integrated_loudness_lkfs = -18.25F, .digital_peak_dbfs = -0.3F};
    const std::vector<iamf::Frame> frames{make_frame(track.samples_per_frame, 0.0F)};
    const auto file = iamf::mux(track, frames);
    REQUIRE(file.has_value());
    const auto entry = read_sample_entry(*file, *find(parse(*file), "stsd"));
    const auto obus = parse_obus(entry.config_obus);

    const auto& mp = obus[3];
    std::size_t pos = mp.payload;
    std::size_t consumed = 0;
    CHECK(leb128_at(entry.config_obus, pos, consumed) == 0);  // mix_presentation_id
    pos += consumed;
    CHECK(leb128_at(entry.config_obus, pos, consumed) == 0);  // count_label
    pos += consumed;
    CHECK(leb128_at(entry.config_obus, pos, consumed) == 1);  // num_sub_mixes
    pos += consumed;
    CHECK(leb128_at(entry.config_obus, pos, consumed) == 1);  // num_audio_elements
    pos += consumed;
    CHECK(leb128_at(entry.config_obus, pos, consumed) == 0);  // audio_element_id
    pos += consumed;

    CHECK(byte_at(entry.config_obus, pos) == 0);  // RenderingConfig: headphones_rendering_mode
    pos += 1;
    CHECK(leb128_at(entry.config_obus, pos, consumed) == 0);  // rendering_config_extension_size
    pos += consumed;

    // element_mix_gain: MixGainParamDefinition (§3.7.2 extends ParamDefinition, §3.6.1)
    CHECK(leb128_at(entry.config_obus, pos, consumed) == 0);  // parameter_id
    pos += consumed;
    CHECK(leb128_at(entry.config_obus, pos, consumed) == track.sample_rate);  // parameter_rate
    pos += consumed;
    CHECK(byte_at(entry.config_obus, pos) == 0);  // param_definition_mode
    pos += 1;
    CHECK(leb128_at(entry.config_obus, pos, consumed) == track.samples_per_frame);  // duration
    pos += consumed;
    CHECK(leb128_at(entry.config_obus, pos, consumed) ==
         track.samples_per_frame);  // constant_subblock_duration
    pos += consumed;
    CHECK(s16_at(entry.config_obus, pos) == 0);  // default_mix_gain: 0 dB
    pos += 2;

    // output_mix_gain: same shape, parameter_id 1
    CHECK(leb128_at(entry.config_obus, pos, consumed) == 1);
    pos += consumed;
    leb128_at(entry.config_obus, pos, consumed);  // parameter_rate
    pos += consumed;
    pos += 1;  // param_definition_mode
    leb128_at(entry.config_obus, pos, consumed);  // duration
    pos += consumed;
    leb128_at(entry.config_obus, pos, consumed);  // constant_subblock_duration
    pos += consumed;
    pos += 2;  // default_mix_gain

    CHECK(leb128_at(entry.config_obus, pos, consumed) == 2);  // num_layouts
    pos += consumed;

    // Layout 1: Stereo, Sound System A (§3.7.3) - layout_type(2)=2 | sound_system(4)=0 | reserved(2).
    CHECK(byte_at(entry.config_obus, pos) == 0x80);
    pos += 1;
    CHECK(byte_at(entry.config_obus, pos) == 0);  // LoudnessInfo::info_type
    pos += 1;
    CHECK(s16_at(entry.config_obus, pos) == to_q7_8(track.stereo_loudness.integrated_loudness_lkfs));
    pos += 2;
    CHECK(s16_at(entry.config_obus, pos) == to_q7_8(track.stereo_loudness.digital_peak_dbfs));
    pos += 2;

    // Layout 2: 7.1.4ch, Sound System J (§3.7.3) - sound_system=9.
    CHECK(byte_at(entry.config_obus, pos) == 0xA4);
    pos += 1;
    CHECK(byte_at(entry.config_obus, pos) == 0);
    pos += 1;
    CHECK(s16_at(entry.config_obus, pos) ==
         to_q7_8(track.layout_714_loudness.integrated_loudness_lkfs));
    pos += 2;
    CHECK(s16_at(entry.config_obus, pos) == to_q7_8(track.layout_714_loudness.digital_peak_dbfs));
    pos += 2;

    CHECK(pos == mp.payload + mp.length);  // no MixPresentationTags: nothing follows num_layouts
}

TEST_CASE("Audio Frame OBUs carry byte-exact PCM in the spec's substream order", "[iamf]") {
    // A distinct value per channel (§3.6.2's L,C,R,Lss,Rss,Lrs,Rrs,Ltf,Rtf,Ltb,Rtb,LFE order,
    // matching iamf::Frame::channels) - if the substream/channel-pairing table in obu_detail.hpp
    // ever swapped two entries (e.g. Lss/Rss with Lrs/Rrs), the quantized bytes checked below
    // would land in the wrong substream and this test would fail.
    constexpr int kBitDepth = 16;
    constexpr std::uint32_t kSamplesPerFrame = 4;
    const iamf::AudioTrack track{.bit_depth = kBitDepth, .samples_per_frame = kSamplesPerFrame};

    iamf::Frame frame;
    for (std::size_t ch = 0; ch < frame.channels.size(); ++ch) {
        const float value = -0.9F + static_cast<float>(ch) * 0.15F;  // 12 distinct, non-colliding
        frame.channels[ch].assign(kSamplesPerFrame, value);
    }
    const std::vector<iamf::Frame> frames{frame};
    const auto file = iamf::mux(track, frames);
    REQUIRE(file.has_value());

    const auto elements = parse(*file);
    const auto* mdat = find(elements, "mdat");
    REQUIRE(mdat != nullptr);
    const std::span<const std::byte> sample{file->data() + static_cast<std::ptrdiff_t>(mdat->payload),
                                            mdat->length};
    const auto obus = parse_obus(sample);
    REQUIRE(obus.size() == 7);

    // channel_a/channel_b mirror obu_detail.hpp's own kSubstreamLayout table (§3.6.3.3's
    // ordering), reconstructed independently here rather than included from production code.
    struct Pair {
        int a;
        int b;  // -1 if mono
    };
    constexpr std::array<Pair, 7> kExpected{{
        {0, 2}, {3, 4}, {5, 6}, {7, 8}, {9, 10}, {1, -1}, {11, -1},
    }};

    for (int id = 0; id < 7; ++id) {
        CHECK(obus[static_cast<std::size_t>(id)].obu_type == 6 + id);  // Audio_Frame_ID<id>

        const auto& pair = kExpected[static_cast<std::size_t>(id)];
        const auto value_a = frame.channels[static_cast<std::size_t>(pair.a)].front();
        const auto expected_a = quantize_pcm(value_a, kBitDepth);
        std::size_t pos = obus[static_cast<std::size_t>(id)].payload;
        const auto actual_a = s16_le_at(sample, pos);
        CHECK(actual_a == static_cast<std::int16_t>(expected_a));

        if (pair.b >= 0) {
            const auto value_b = frame.channels[static_cast<std::size_t>(pair.b)].front();
            const auto expected_b = quantize_pcm(value_b, kBitDepth);
            const auto actual_b = s16_le_at(sample, pos + 2);
            CHECK(actual_b == static_cast<std::int16_t>(expected_b));
            CHECK(obus[static_cast<std::size_t>(id)].length == kSamplesPerFrame * 2 * 2);
        } else {
            CHECK(obus[static_cast<std::size_t>(id)].length == kSamplesPerFrame * 2);
        }
    }
}

TEST_CASE("IAMF chunk offsets and stsz sizes index mdat exactly", "[iamf]") {
    const iamf::AudioTrack track{.samples_per_frame = 32};
    const std::vector<iamf::Frame> frames{make_frame(32, 0.1F), make_frame(32, -0.4F),
                                          make_frame(32, 0.7F)};
    const auto file = iamf::mux(track, frames);
    REQUIRE(file.has_value());
    const auto elements = parse(*file);

    const auto* stsz = find(elements, "stsz");
    const auto* stco = find(elements, "stco");
    const auto* mdat = find(elements, "mdat");
    REQUIRE(stsz != nullptr);
    REQUIRE(stco != nullptr);
    REQUIRE(mdat != nullptr);
    const auto sizes = read_stsz(*file, *stsz);
    const auto offsets = read_stco(*file, *stco);
    REQUIRE(sizes.size() == frames.size());
    REQUIRE(offsets.size() == frames.size());

    std::uint32_t cursor = static_cast<std::uint32_t>(mdat->payload);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        CHECK(offsets[i] == cursor);
        // Every IA Sample this writer produces is the same size (12 fixed-width channels, one
        // bit depth) - so this also confirms stsz did not just repeat one frame's size.
        CHECK(sizes[i] == sizes[0]);
        // Each offset lands on a real OBU header whose type is the first substream's
        // (Audio_Frame_ID0 = 6), proving the cumulative-offset arithmetic, not just the box
        // fields' internal consistency.
        CHECK((byte_at(*file, offsets[i]) >> 3) == 6);
        cursor += sizes[i];
    }
}

TEST_CASE("iamf::mux validates the track and every frame", "[iamf]") {
    const iamf::AudioTrack track{};

    SECTION("no frames") {
        const auto result = iamf::mux(track, {});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == iamf::MuxError::kNoFrames);
    }

    SECTION("unsupported sample rate") {
        iamf::AudioTrack bad = track;
        bad.sample_rate = 44099;  // not in IAMF §3.11.4's {44100,16000,32000,48000,96000}
        const std::vector<iamf::Frame> frames{make_frame(bad.samples_per_frame, 0.0F)};
        const auto result = iamf::mux(bad, frames);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == iamf::MuxError::kInvalidTrack);
    }

    SECTION("unsupported bit depth") {
        iamf::AudioTrack bad = track;
        bad.bit_depth = 20;  // not in IAMF §3.11.4's {16,24,32}
        const std::vector<iamf::Frame> frames{make_frame(bad.samples_per_frame, 0.0F)};
        const auto result = iamf::mux(bad, frames);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == iamf::MuxError::kInvalidTrack);
    }

    SECTION("zero samples_per_frame") {
        iamf::AudioTrack bad = track;
        bad.samples_per_frame = 0;
        const std::vector<iamf::Frame> frames{iamf::Frame{}};
        const auto result = iamf::mux(bad, frames);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == iamf::MuxError::kInvalidTrack);
    }

    SECTION("a channel does not carry samples_per_frame samples") {
        auto frame = make_frame(track.samples_per_frame, 0.0F);
        frame.channels[3].pop_back();  // one sample short
        const std::vector<iamf::Frame> frames{frame};
        const auto result = iamf::mux(track, frames);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == iamf::MuxError::kFrameSizeMismatch);
    }
}
