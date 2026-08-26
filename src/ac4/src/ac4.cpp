#include "ac4/ac4.hpp"

#include <array>
#include <unordered_map>

namespace ac4 {

std::string_view describe(Error error) {
    switch (error) {
        case Error::kTruncated:
            return "truncated: a declared length or size runs past the end of the data";
        case Error::kLostSync:
            return "lost sync: sync_word was neither 0xAC40 nor 0xAC41";
        case Error::kUnsupportedBitstreamVersion:
            return "bitstream_version > 2 is not decodable per TS 103 190-2 §6.3.2.1.1";
        case Error::kObjectCodedGroup:
            return "substream group is object/A-JOC/OAMD-coded (b_channel_coded=0); "
                   "TS 103 190-2 clause 6.3.2.8-6.3.2.12 not implemented";
    }
    return "unknown ac4::Error";
}

namespace {

// MSB-first bit reader with a sticky failure state - the same shape
// ac3::core::BitReader uses for overflow, extended here to also carry the
// two explicit refusal conditions (kUnsupportedBitstreamVersion,
// kObjectCodedGroup) so every parse_* helper below can bail out with a
// plain early return instead of threading std::expected through the whole
// call tree. Only parse_raw_frame(), at the boundary, converts the final
// state to std::expected.
class Reader {
   public:
    explicit Reader(std::span<const std::byte> data) : data_(data) {}

    [[nodiscard]] std::uint32_t bits(int n) {
        std::uint32_t value = 0;
        for (int i = 0; i < n; ++i) {
            value = (value << 1) | read_bit();
        }
        return value;
    }

    void byte_align() { position_ = (position_ + 7) & ~std::size_t{7}; }

    // Reads and discards n bits - every call site below that consumes a
    // reserved/unused field rather than a value it goes on to use.
    void skip(int n) { (void)bits(n); }

    void fail(Error error) {
        if (!error_) {
            error_ = error;
        }
    }

    [[nodiscard]] std::optional<Error> error() const {
        // Truncation wins even over an explicit fail() call made afterwards:
        // once real data has run out, every subsequent read returns a
        // phantom 0, so any "logical" refusal a parse_* helper derives from
        // one of those phantom bits (e.g. a b_channel_coded that reads as 0
        // only because it ran off the end) is itself meaningless and would
        // misreport the actual cause as something more specific than it is.
        if (overflowed_) {
            return Error::kTruncated;
        }
        if (error_) {
            return error_;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::size_t bit_position() const { return position_; }

   private:
    [[nodiscard]] std::uint32_t read_bit() {
        const std::size_t byte_index = position_ >> 3;
        if (byte_index >= data_.size()) {
            overflowed_ = true;
            ++position_;
            return 0;
        }
        const auto bit =
            (std::to_integer<std::uint32_t>(data_[byte_index]) >> (7 - (position_ & 7))) & 1u;
        ++position_;
        return bit;
    }

    std::span<const std::byte> data_;
    std::size_t position_ = 0;
    bool overflowed_ = false;
    std::optional<Error> error_;
};

// Table 3 (§4.2.2): a value sent as groups of n_bits, MSB group first, each
// followed by a continuation bit.
std::uint32_t variable_bits(Reader& r, int n_bits) {
    std::uint32_t value = 0;
    while (true) {
        value += r.bits(n_bits);
        if (!r.bits(1)) {
            return value;
        }
        value <<= n_bits;
        value += (1u << n_bits);
    }
}

// The `substream_index; ...2; if (==3) += variable_bits(2)` shape repeated
// by every *_substream_info element (§4.3.3.7.9 and its Part 2
// counterparts) to name a row of substream_index_table().
int parse_substream_index_ref(Reader& r) {
    std::uint32_t idx = r.bits(2);
    if (idx == 3) {
        idx += variable_bits(r, 2);
    }
    return static_cast<int>(idx);
}

// --- Annex G: AC-4 sync frame -----------------------------------------------

std::uint16_t crc16(std::span<const std::byte> data) {
    // Annex G.4.2: generator polynomial x^16+x^15+x^2+1, initial state
    // 0x0000, no reflection, no final XOR.
    std::uint16_t crc = 0x0000;
    constexpr std::uint16_t kPoly = 0x8005;
    for (const std::byte b : data) {
        crc ^= static_cast<std::uint16_t>(std::to_integer<unsigned>(b) << 8);
        for (int i = 0; i < 8; ++i) {
            crc = (crc & 0x8000) ? static_cast<std::uint16_t>((crc << 1) ^ kPoly)
                                 : static_cast<std::uint16_t>(crc << 1);
        }
    }
    return crc;
}

}  // namespace

ScanResult scan(std::span<const std::byte> data) {
    ScanResult result;
    std::size_t pos = 0;
    while (pos + 4 <= data.size()) {
        const auto sync = static_cast<std::uint16_t>((std::to_integer<unsigned>(data[pos]) << 8) |
                                                     std::to_integer<unsigned>(data[pos + 1]));
        if (sync != 0xAC40 && sync != 0xAC41) {
            result.stopped_at = Error::kLostSync;
            result.stopped_at_offset = pos;
            return result;
        }
        std::uint32_t frame_size = (std::to_integer<unsigned>(data[pos + 2]) << 8) |
                                   std::to_integer<unsigned>(data[pos + 3]);
        std::size_t header = 4;
        if (frame_size == 0xFFFF) {
            if (pos + 7 > data.size()) {
                result.stopped_at = Error::kTruncated;
                result.stopped_at_offset = pos;
                return result;
            }
            frame_size = (std::to_integer<unsigned>(data[pos + 4]) << 16) |
                         (std::to_integer<unsigned>(data[pos + 5]) << 8) |
                         std::to_integer<unsigned>(data[pos + 6]);
            header = 7;
        }
        const std::size_t frame_start = pos + header;
        const std::size_t frame_end = frame_start + frame_size;
        const bool has_crc = sync == 0xAC41;
        const std::size_t total = frame_end + (has_crc ? 2 : 0);
        if (frame_end > data.size() || total > data.size()) {
            result.stopped_at = Error::kTruncated;
            result.stopped_at_offset = pos;
            return result;
        }
        std::optional<bool> crc_ok;
        if (has_crc) {
            const auto want =
                static_cast<std::uint16_t>((std::to_integer<unsigned>(data[frame_end]) << 8) |
                                           std::to_integer<unsigned>(data[frame_end + 1]));
            crc_ok = crc16(data.subspan(pos + 2, frame_end - (pos + 2))) == want;
        }
        result.frames.push_back(SyncFrame{
            .offset = pos,
            .sync_word = sync,
            .raw_ac4_frame = data.subspan(frame_start, frame_size),
            .crc_ok = crc_ok,
        });
        pos = total;
    }
    return result;
}

namespace {

// --- §4.2.14.15 emdf_reserved / §4.2.3.5 emdf_info --------------------------

// Table 80. Despite the clause title, the syntax table itself is headed
// emdf_protection() - the same element, called as emdf_reserved() from
// emdf_info(). Two independent 2-bit length codes (0/1/4/16 bytes each,
// added together) bound a trailing reserved run; unlike the classic
// Annex H EMDF container's own prim/sec protection fields (0/8/32/128
// BITS each - see the eac3_parse.py reference), this one counts BYTES
// and uses a different power-of-four table.
void parse_emdf_reserved(Reader& r) {
    int n_skip_bytes = 0;
    const std::uint32_t primary = r.bits(2);
    const std::uint32_t secondary = r.bits(2);
    if (primary > 0) {
        n_skip_bytes += 1 << (2 * (primary - 1));
    }
    if (secondary > 0) {
        n_skip_bytes += 1 << (2 * (secondary - 1));
    }
    r.skip(8 * n_skip_bytes);
}

struct EmdfInfo {
    int emdf_version = 0;
    int key_id = 0;
    std::optional<int> payloads_substream_index;
};

EmdfInfo parse_emdf_info(Reader& r) {
    EmdfInfo info;
    info.emdf_version = static_cast<int>(r.bits(2));
    if (info.emdf_version == 3) {
        info.emdf_version += static_cast<int>(variable_bits(r, 2));
    }
    info.key_id = static_cast<int>(r.bits(3));
    if (info.key_id == 7) {
        info.key_id += static_cast<int>(variable_bits(r, 3));
    }
    if (r.bits(1)) {  // b_emdf_payloads_substream_info
        info.payloads_substream_index = parse_substream_index_ref(r);
    }
    parse_emdf_reserved(r);
    return info;
}

// --- §4.2.3.7 content_type --------------------------------------------------

ContentType parse_content_type(Reader& r) {
    ContentType ct;
    ct.content_classifier = static_cast<int>(r.bits(3));
    if (r.bits(1)) {      // b_language_indicator
        if (r.bits(1)) {  // b_serialized_language_tag
            r.skip(1);    // b_start_tag
            r.skip(16);   // language_tag_chunk
        } else {
            const int n = static_cast<int>(r.bits(6));
            std::vector<std::byte> tag(static_cast<std::size_t>(n));
            for (auto& b : tag) {
                b = static_cast<std::byte>(r.bits(8));
            }
            ct.language_tag = std::move(tag);
        }
    }
    return ct;
}

// --- §4.2.3.4 frame_rate_multiply_info / §6.2.1.4 frame_rate_fractions_info -

// Table 87 (§4.3.3.5.3): resolves frame_rate_factor (1, 2 or 4).
int parse_frame_rate_multiply_info(Reader& r, int frame_rate_index) {
    switch (frame_rate_index) {
        case 2:
        case 3:
        case 4:
            if (r.bits(1)) {  // b_multiplier
                return r.bits(1) ? 4 : 2;
            }
            return 1;
        case 0:
        case 1:
        case 7:
        case 8:
        case 9:
            return r.bits(1) ? 2 : 1;  // b_multiplier
        default:
            return 1;
    }
}

void parse_frame_rate_fractions_info(Reader& r, int frame_rate_index, int frame_rate_factor) {
    switch (frame_rate_index) {
        case 5:
        case 6:
        case 7:
        case 8:
        case 9:
            if (frame_rate_factor == 1 && r.bits(1)) {
                // frame_rate_fraction = 2; unused downstream in this scope.
            }
            break;
        case 10:
        case 11:
        case 12:
            if (r.bits(1)) {  // b_frame_rate_fraction
                r.skip(1);    // b_frame_rate_fraction_is_4
            }
            break;
        default:
            break;
    }
}

// --- §4.2.3.9 ac4_hsf_ext_substream_info ------------------------------------
// Part 1 has no parameter; Part 2 gates it on b_substreams_present
// (§6.2.1.14). Both shapes just name a substream_index_table() row, which
// this parser does not need (the HSF extension substream's own bytes are
// still accounted for via substream_index_table()'s sizes; it is simply
// reported as "other", the same as any non-channel-audio substream).
void parse_hsf_ext_substream_info(Reader& r, bool b_substreams_present) {
    if (b_substreams_present) {
        parse_substream_index_ref(r);
    }
}

// --- §4.2.3.8 / §6.2.1.5 presentation_config_ext_info -----------------------

void parse_presentation_config_ext_info(Reader& r) {
    std::uint32_t n_skip_bytes = r.bits(5);
    if (r.bits(1)) {  // b_more_skip_bytes
        n_skip_bytes += variable_bits(r, 2) << 5;
    }
    r.skip(8 * static_cast<int>(n_skip_bytes));
}

// --- Table 90 (§4.3.3.7.5): bitrate_indicator -------------------------------

int read_bitrate_indicator(Reader& r) {
    // A 3-bit code whose LSB is always 0; an odd 3-bit prefix extends to 5
    // bits (the same variable-width shape channel_mode uses, with its own
    // prefix set).
    std::uint32_t v = r.bits(3);
    if (v & 1) {
        v = (v << 2) | r.bits(2);
    }
    return static_cast<int>(v);
}

std::optional<int> bitrate_kbps(int indicator) {
    static const std::unordered_map<int, int> kTable = {
        {0b000, 16},   {0b010, 20},   {0b100, 24},   {0b110, 28},   {0b00100, 32}, {0b00101, 40},
        {0b00110, 48}, {0b00111, 56}, {0b01100, 64}, {0b01101, 80}, {0b01110, 96}, {0b01111, 112},
    };
    const auto it = kTable.find(indicator);
    return it == kTable.end() ? std::nullopt : std::optional<int>(it->second);
}

// --- Table 88 (§4.3.3.7.1) / Table 56 (§6.3.2.7.2): channel_mode -----------

struct ChannelModeEntry {
    int code;
    std::string_view name;
    int ch_mode;
};

constexpr std::array<ChannelModeEntry, 11> kChannelModeV0 = {{
    {0b0, "Mono", 0},
    {0b10, "Stereo", 1},
    {0b1100, "3.0", 2},
    {0b1101, "5.0", 3},
    {0b1110, "5.1", 4},
    {0b1111000, "7.0: 3/4/0", 5},
    {0b1111001, "7.1: 3/4/0.1", 6},
    {0b1111010, "7.0: 5/2/0", 7},
    {0b1111011, "7.1: 5/2/0.1", 8},
    {0b1111100, "7.0: 3/2/2", 9},
    {0b1111101, "7.1: 3/2/2.1", 10},
}};

constexpr std::array<ChannelModeEntry, 16> kChannelModeV1 = {{
    {0b0, "Mono", 0},
    {0b10, "Stereo", 1},
    {0b1100, "3.0", 2},
    {0b1101, "5.0", 3},
    {0b1110, "5.1", 4},
    {0b1111000, "7.0: 3/4/0", 5},
    {0b1111001, "7.1: 3/4/0.1", 6},
    {0b1111010, "7.0: 5/2/0", 7},
    {0b1111011, "7.1: 5/2/0.1", 8},
    {0b1111100, "7.0: 3/2/2", 9},
    {0b1111101, "7.1: 3/2/2.1", 10},
    {0b11111100, "7.0.4", 11},
    {0b11111101, "7.1.4", 12},
    {0b111111100, "9.0.4", 13},
    {0b111111101, "9.1.4", 14},
    {0b111111110, "22.2", 15},
}};

template <std::size_t N>
std::pair<std::string, std::optional<int>> lookup_channel_mode(
    const std::array<ChannelModeEntry, N>& table, int code) {
    for (const auto& e : table) {
        if (e.code == code) {
            return {std::string(e.name), e.ch_mode};
        }
    }
    return {"reserved", std::nullopt};
}

// §4.2.3.6 ac4_substream_info (presentation_version 0 channel_mode, Table 88).
ChannelSubstreamInfo parse_substream_info_v0(Reader& r, int fs_index, int frame_rate_factor) {
    ChannelSubstreamInfo info;
    std::uint32_t cm = r.bits(1);
    if (cm != 0) {
        cm = (cm << 1) | r.bits(1);
        if (cm != 0b10) {
            cm = (cm << 2) | r.bits(2);
            if (cm != 0b1100 && cm != 0b1101 && cm != 0b1110) {
                cm = (cm << 3) | r.bits(3);
                if (cm == 0b1111111) {
                    cm += variable_bits(r, 2);
                }
            }
        }
    }
    info.channel_mode = static_cast<int>(cm);
    std::tie(info.channel_mode_name, info.ch_mode) =
        lookup_channel_mode(kChannelModeV0, info.channel_mode);
    if (fs_index == 1 && r.bits(1)) {  // b_sf_multiplier
        info.sf_multiplier = static_cast<int>(r.bits(1));
    }
    if (r.bits(1)) {  // b_bitrate_info
        info.bitrate_kbps = bitrate_kbps(read_bitrate_indicator(r));
    }
    if (cm == 0b1111010 || cm == 0b1111011 || cm == 0b1111100 || cm == 0b1111101) {
        r.skip(1);  // add_ch_base
    }
    if (r.bits(1)) {  // b_content_type
        info.content_type = parse_content_type(r);
    }
    for (int i = 0; i < frame_rate_factor; ++i) {
        r.skip(1);  // b_iframe
    }
    info.substream_index = parse_substream_index_ref(r);
    return info;
}

// §6.3.2.7 ac4_substream_info_chan (presentation_version 1 channel_mode,
// Table 56).
ChannelSubstreamInfo parse_substream_info_chan(Reader& r, int fs_index, int frame_rate_factor,
                                               bool b_substreams_present) {
    ChannelSubstreamInfo info;
    std::uint32_t cm = r.bits(1);
    if (cm != 0) {
        cm = (cm << 1) | r.bits(1);
        if (cm != 0b10) {
            cm = (cm << 2) | r.bits(2);
            if (cm != 0b1100 && cm != 0b1101 && cm != 0b1110) {
                cm = (cm << 3) | r.bits(3);
                // Table 56's 7-bit codes stop at 0b1111101 (7.1: 3/2/2.1);
                // the two remaining 7-bit values are BOTH incomplete
                // prefixes of DIFFERENT length - 0b1111110 needs one more
                // bit (11111100/11111101, both terminal), while 0b1111111
                // needs two more (11111110|0/1 and 11111111|0/1, the
                // latter - 0b111111111 - triggering the variable_bits()
                // extension). Reading a fixed-width chunk here regardless
                // of which 7-bit prefix was seen misreads every
                // 9.x/22.2 channel_mode and desyncs the frame.
                if (cm != 0b1111000 && cm != 0b1111001 && cm != 0b1111010 && cm != 0b1111011 &&
                    cm != 0b1111100 && cm != 0b1111101) {
                    if (cm == 0b1111110) {
                        cm = (cm << 1) | r.bits(1);
                    } else {  // cm == 0b1111111
                        cm = (cm << 1) | r.bits(1);
                        cm = (cm << 1) | r.bits(1);
                        if (cm == 0b111111111) {
                            cm += variable_bits(r, 2);
                        }
                    }
                }
            }
        }
    }
    info.channel_mode = static_cast<int>(cm);
    std::tie(info.channel_mode_name, info.ch_mode) =
        lookup_channel_mode(kChannelModeV1, info.channel_mode);
    if (cm == 0b11111100 || cm == 0b11111101 || cm == 0b111111100 || cm == 0b111111101) {
        OriginalContent oc;
        oc.b_4_back_channels_present = r.bits(1) != 0;
        oc.b_centre_present = r.bits(1) != 0;
        oc.top_channels_present = static_cast<int>(r.bits(2));
        info.original_content = oc;
    }
    if (fs_index == 1 && r.bits(1)) {  // b_sf_multiplier
        info.sf_multiplier = static_cast<int>(r.bits(1));
    }
    if (r.bits(1)) {  // b_bitrate_info
        info.bitrate_kbps = bitrate_kbps(read_bitrate_indicator(r));
    }
    if (cm == 0b1111010 || cm == 0b1111011 || cm == 0b1111100 || cm == 0b1111101) {
        r.skip(1);  // add_ch_base
    }
    for (int i = 0; i < frame_rate_factor; ++i) {
        r.skip(1);  // b_audio_ndot
    }
    if (b_substreams_present) {
        info.substream_index = parse_substream_index_ref(r);
    }
    return info;
}

// --- §4.2.3.3 presentation_version ------------------------------------------

int parse_presentation_version(Reader& r) {
    int version = 0;
    while (r.bits(1)) {
        ++version;
    }
    return version;
}

// --- §4.2.3.2 ac4_presentation_info (presentation_version 0 path) ----------

constexpr std::array<std::array<std::string_view, 3>, 6> kPresentationConfigRoles = {{
    {"M+E", "Dialog", ""},
    {"Main", "DE", ""},
    {"Main", "Associate", ""},
    {"M+E", "Dialog", "Associate"},
    {"Main", "DE", "Associate"},
    {"Main", "", ""},
}};
constexpr std::array<int, 6> kPresentationConfigRoleCounts = {2, 2, 2, 3, 3, 1};

PresentationInfoV0 parse_presentation_info_v0(Reader& r, int fs_index, int frame_rate_index) {
    PresentationInfoV0 pres;
    const bool b_single_substream = r.bits(1) != 0;
    std::optional<int> presentation_config;
    if (!b_single_substream) {
        std::uint32_t pc = r.bits(3);
        if (pc == 7) {
            pc += variable_bits(r, 2);
        }
        presentation_config = static_cast<int>(pc);
    }
    pres.presentation_config = presentation_config;
    pres.presentation_version = parse_presentation_version(r);
    if (!b_single_substream && presentation_config == 6) {
        return pres;  // b_add_emdf_substreams = 1; the additional-EMDF loop
                      // below runs on the caller's own n bits, same as any
                      // other exit from this function - see parse_toc().
    }
    pres.md_compat = static_cast<int>(r.bits(3));
    if (r.bits(1)) {  // b_belongs_to_presentation_id
        pres.presentation_id = static_cast<int>(variable_bits(r, 2));
    }
    const int frame_rate_factor = parse_frame_rate_multiply_info(r, frame_rate_index);
    parse_emdf_info(r);
    if (b_single_substream) {
        pres.substreams.emplace_back("main",
                                     parse_substream_info_v0(r, fs_index, frame_rate_factor));
    } else {
        const bool b_hsf_ext = r.bits(1) != 0;
        if (*presentation_config >= 0 && *presentation_config <= 5) {
            const auto& roles =
                kPresentationConfigRoles[static_cast<std::size_t>(*presentation_config)];
            const int n_roles =
                kPresentationConfigRoleCounts[static_cast<std::size_t>(*presentation_config)];
            for (int i = 0; i < n_roles; ++i) {
                pres.substreams.emplace_back(
                    std::string(roles[static_cast<std::size_t>(i)]),
                    parse_substream_info_v0(r, fs_index, frame_rate_factor));
                if (i == 0 && b_hsf_ext) {
                    parse_hsf_ext_substream_info(r, true);
                }
            }
        } else {
            parse_presentation_config_ext_info(r);
        }
    }
    r.skip(1);        // b_pre_virtualized
    if (r.bits(1)) {  // b_add_emdf_substreams
        std::uint32_t n = r.bits(2);
        if (n == 0) {
            n = variable_bits(r, 2) + 4;
        }
        for (std::uint32_t i = 0; i < n; ++i) {
            parse_emdf_info(r);
        }
    }
    return pres;
}

// --- §6.2.1.6 ac4_substream_group_info / §6.2.1.8 ac4_substream_info_chan --

// frame_rate_factor is a frame-global quantity in the spec's own telling
// (§6.3.2.1.3's b_iframe_global talks about "a series of 2 or 4
// substreams" at the whole-FRAME level, not per presentation), even though
// the only element that transmits it, frame_rate_multiply_info(), is
// called once per presentation inside ac4_presentation_v1_info() - ahead
// of, and structurally separate from, this function's own call site in
// ac4_toc()'s substream-group loop. ac4_substream_info_chan()'s
// b_audio_ndot loop (§6.2.1.8) bounds itself on a bare `frame_rate_factor`
// with no parameter, i.e. ambient state rather than a per-group value, so
// the caller (parse_toc()) resolves it once, from the first presentation,
// and threads it through explicitly instead of re-deriving it per group.
SubstreamGroupInfo parse_substream_group_info(Reader& r, int fs_index, int frame_rate_factor) {
    SubstreamGroupInfo group;
    group.b_substreams_present = r.bits(1) != 0;
    const bool b_hsf_ext = r.bits(1) != 0;
    const std::uint32_t b_single_substream = r.bits(1);
    std::uint32_t n_lf_substreams;
    if (b_single_substream) {
        n_lf_substreams = 1;
    } else {
        n_lf_substreams = r.bits(2) + 2;
        if (n_lf_substreams == 5) {
            n_lf_substreams += variable_bits(r, 2);
        }
    }
    if (!r.bits(1)) {  // b_channel_coded
        r.fail(Error::kObjectCodedGroup);
        return group;
    }
    for (std::uint32_t i = 0; i < n_lf_substreams; ++i) {
        // sus_ver only exists for bitstream_version == 1; the caller only
        // reaches this function for bitstream_version >= 2 (see parse_toc()'s
        // dispatch), where it is implicitly 1 (extended ac4_substream()
        // syntax) per §6.2.1.6.
        auto chan =
            parse_substream_info_chan(r, fs_index, frame_rate_factor, group.b_substreams_present);
        if (b_hsf_ext) {
            parse_hsf_ext_substream_info(r, group.b_substreams_present);
        }
        group.substreams.push_back(std::move(chan));
    }
    if (r.bits(1)) {  // b_content_type
        group.content_type = parse_content_type(r);
    }
    return group;
}

// --- §6.2.1.3 ac4_presentation_v1_info / §6.2.1.7 ac4_sgi_specifier --------

constexpr std::array<int, 5> kV1ConfigGroupCounts = {2, 1, 2, 3, 2};  // presentation_config 0-4

// §6.2.1.7. `ac4_sgi_specifier()`'s own bitstream_version == 1 branch
// (inlining a whole ac4_substream_group_info() rather than a group_index
// reference) is unreachable here: parse_toc() only calls
// parse_presentation_v1_info() - and so this - for bitstream_version >= 2,
// per §6.2.1.1's own `if (bitstream_version <= 1) {legacy} else {v1}`
// dispatch. Every group is referenced by index, resolved later against
// Toc::substream_groups.
int parse_sgi_specifier(Reader& r) {
    std::uint32_t group_index = r.bits(3);
    if (group_index == 7) {
        group_index += variable_bits(r, 2);
    }
    return static_cast<int>(group_index);
}

// fs_index is not read here: parse_sgi_specifier()'s own bitstream_version
// == 1 branch (the only one that would have needed it, to resolve
// ac4_substream_info_chan()'s b_sf_multiplier) is unreachable from this
// call graph - see parse_sgi_specifier()'s own comment.
PresentationInfoV1 parse_presentation_v1_info(Reader& r, int bitstream_version,
                                              int frame_rate_index) {
    PresentationInfoV1 pres;
    const bool b_single_substream_group = r.bits(1) != 0;
    std::optional<int> presentation_config;
    if (!b_single_substream_group) {
        std::uint32_t pc = r.bits(3);
        if (pc == 7) {
            pc += variable_bits(r, 2);
        }
        presentation_config = static_cast<int>(pc);
    }
    pres.presentation_config = presentation_config;
    if (bitstream_version != 1) {
        pres.presentation_version = parse_presentation_version(r);
    }
    if (!b_single_substream_group && presentation_config == 6) {
        return pres;  // b_add_emdf_substreams = 1; nothing further this parser tracks.
    }
    if (bitstream_version != 1) {
        pres.md_compat = static_cast<int>(r.bits(3));
    }
    if (r.bits(1)) {          // b_presentation_id
        variable_bits(r, 2);  // presentation_id, unused downstream
    }
    pres.frame_rate_factor = parse_frame_rate_multiply_info(r, frame_rate_index);
    parse_frame_rate_fractions_info(r, frame_rate_index, pres.frame_rate_factor);
    parse_emdf_info(r);
    if (r.bits(1)) {  // b_presentation_filter
        pres.enable_presentation = r.bits(1) != 0;
    }
    if (b_single_substream_group) {
        pres.group_refs.push_back(parse_sgi_specifier(r));
    } else {
        r.skip(1);  // b_multi_pid
        if (presentation_config && *presentation_config >= 0 && *presentation_config <= 4) {
            const int n = kV1ConfigGroupCounts[static_cast<std::size_t>(*presentation_config)];
            for (int i = 0; i < n; ++i) {
                pres.group_refs.push_back(parse_sgi_specifier(r));
            }
        } else if (presentation_config == 5) {
            std::uint32_t n = r.bits(2) + 2;
            if (n == 5) {
                n += variable_bits(r, 2);
            }
            for (std::uint32_t i = 0; i < n; ++i) {
                pres.group_refs.push_back(parse_sgi_specifier(r));
            }
        } else {
            parse_presentation_config_ext_info(r);
        }
    }
    r.skip(1);  // b_pre_virtualized
    const bool b_add_emdf_substreams = r.bits(1) != 0;
    // ac4_presentation_substream_info() (§6.2.1.12)
    r.skip(1);  // b_alternative
    r.skip(1);  // b_pres_ndot
    parse_substream_index_ref(r);
    if (b_add_emdf_substreams) {
        std::uint32_t n = r.bits(2);
        if (n == 0) {
            n = variable_bits(r, 2) + 4;
        }
        for (std::uint32_t i = 0; i < n; ++i) {
            parse_emdf_info(r);
        }
    }
    return pres;
}

// --- §4.2.3.11 substream_index_table ----------------------------------------

void parse_substream_index_table(Reader& r, Toc& toc) {
    std::uint32_t n_substreams = r.bits(2);
    if (n_substreams == 0) {
        n_substreams = variable_bits(r, 2) + 4;
    }
    bool b_size_present = true;
    if (n_substreams == 1) {
        b_size_present = r.bits(1) != 0;
    }
    toc.n_substreams = static_cast<int>(n_substreams);
    if (b_size_present) {
        for (std::uint32_t s = 0; s < n_substreams; ++s) {
            // Table 14: b_more_bits precedes substream_size[s], not the
            // other way around.
            const bool b_more_bits = r.bits(1) != 0;
            std::uint32_t size = r.bits(10);
            if (b_more_bits) {
                size += variable_bits(r, 2) << 10;
            }
            toc.substream_sizes.push_back(static_cast<int>(size));
        }
    }
}

}  // namespace

namespace {

// §6.3.2.1.8: total_n_substream_groups is derived, not transmitted - 1 +
// the highest group_index any ac4_sgi_specifier() referenced.
int total_substream_groups(const std::vector<PresentationInfoV1>& presentations) {
    int max_group_index = -1;
    for (const auto& p : presentations) {
        for (const int ref : p.group_refs) {
            max_group_index = std::max(max_group_index, ref);
        }
    }
    return max_group_index + 1;
}

std::expected<Toc, Error> parse_toc(Reader& r) {
    Toc toc;
    std::uint32_t bitstream_version = r.bits(2);
    if (bitstream_version == 3) {
        bitstream_version += variable_bits(r, 2);
    }
    if (bitstream_version > 2) {
        return std::unexpected(Error::kUnsupportedBitstreamVersion);
    }
    toc.bitstream_version = static_cast<int>(bitstream_version);
    toc.sequence_counter = static_cast<int>(r.bits(10));
    if (r.bits(1)) {  // b_wait_frames
        toc.wait_frames = static_cast<int>(r.bits(3));
        if (*toc.wait_frames > 0) {
            r.skip(2);  // br_code (Part 2) / reserved (Part 1) - both 2 bits
        }
    }
    const int fs_index = static_cast<int>(r.bits(1));
    toc.sample_rate_hz = fs_index == 1 ? 48000 : 44100;  // Table 82
    toc.frame_rate_index = static_cast<int>(r.bits(4));
    toc.b_iframe_global = r.bits(1) != 0;
    const std::uint32_t b_single_presentation = r.bits(1);
    if (b_single_presentation) {
        toc.n_presentations = 1;
    } else if (r.bits(1)) {  // b_more_presentations
        toc.n_presentations = static_cast<int>(variable_bits(r, 2) + 2);
    } else {
        toc.n_presentations = 0;
    }
    // §4.3.3.2.10/.11 (Part 1) / §6.2.1.1 (Part 2, identical shape): where
    // substream 0's payload starts, relative to the end of the byte-aligned
    // ac4_toc(), in bytes. Defaults to 0 when b_payload_base is unset.
    if (r.bits(1)) {  // b_payload_base
        toc.payload_base = static_cast<int>(r.bits(5)) + 1;
        if (toc.payload_base == 0x20) {
            toc.payload_base += static_cast<int>(variable_bits(r, 3));
        }
    }
    if (toc.bitstream_version <= 1) {
        toc.presentations_v0.reserve(static_cast<std::size_t>(toc.n_presentations));
        for (int i = 0; i < toc.n_presentations; ++i) {
            toc.presentations_v0.push_back(
                parse_presentation_info_v0(r, fs_index, toc.frame_rate_index));
        }
    } else {
        if (r.bits(1)) {      // b_program_id
            r.skip(16);       // short_program_id
            if (r.bits(1)) {  // b_program_uuid_present
                r.skip(32);

                r.skip(32);

                r.skip(32);

                r.skip(32);  // program_uuid, 16 bytes - split to stay within bits()'s 32-bit width
            }
        }
        toc.presentations_v1.reserve(static_cast<std::size_t>(toc.n_presentations));
        for (int i = 0; i < toc.n_presentations; ++i) {
            toc.presentations_v1.push_back(
                parse_presentation_v1_info(r, toc.bitstream_version, toc.frame_rate_index));
        }
        const int total_groups = total_substream_groups(toc.presentations_v1);
        // frame_rate_factor is frame-global in practice - see
        // parse_substream_group_info()'s own comment - so the first
        // presentation's resolved value is what every group uses.
        const int group_frame_rate_factor =
            toc.presentations_v1.empty() ? 1 : toc.presentations_v1.front().frame_rate_factor;
        toc.substream_groups.reserve(static_cast<std::size_t>(total_groups));
        for (int i = 0; i < total_groups; ++i) {
            toc.substream_groups.push_back(
                parse_substream_group_info(r, fs_index, group_frame_rate_factor));
            if (const auto err = r.error()) {
                return std::unexpected(*err);
            }
        }
    }
    parse_substream_index_table(r, toc);
    r.byte_align();
    if (const auto err = r.error()) {
        return std::unexpected(*err);
    }
    return toc;
}

// Table 15 (Part 1) / Table 50 (Part 2): substream_index_table() is one flat
// array, but each entry's *type* - and so which ac4_substream_data element
// actually sits there - is decided by which kind of *_info element
// referenced it. ac4_substream_info()/ac4_substream_info_chan() map to
// ac4_substream() (the audio_size-prefixed shape parse_substream_header()
// reads); ac4_presentation_substream_info() and emdf_info()'s payloads
// reference map to ac4_presentation_substream() and
// emdf_payloads_substream() instead, neither of which this parser
// transcribes.
std::vector<bool> channel_substream_indices(const Toc& toc) {
    std::vector<bool> is_chan(static_cast<std::size_t>(toc.n_substreams), false);
    auto mark = [&](std::optional<int> idx) {
        if (idx && *idx >= 0 && static_cast<std::size_t>(*idx) < is_chan.size()) {
            is_chan[static_cast<std::size_t>(*idx)] = true;
        }
    };
    if (!toc.substream_groups.empty()) {
        for (const auto& group : toc.substream_groups) {
            for (const auto& sub : group.substreams) {
                mark(sub.substream_index);
            }
        }
    } else {
        for (const auto& pres : toc.presentations_v0) {
            for (const auto& [role, sub] : pres.substreams) {
                mark(sub.substream_index);
            }
        }
    }
    return is_chan;
}

// §4.2.4.2 / §6.2.2.2 ac4_substream(): outer envelope only (audio_size).
int parse_substream_header(Reader& r) {
    std::uint32_t audio_size = r.bits(15);
    if (r.bits(1)) {  // b_more_bits
        audio_size += variable_bits(r, 7) << 15;
    }
    return static_cast<int>(audio_size);
}

}  // namespace

std::expected<RawFrame, Error> parse_raw_frame(std::span<const std::byte> raw_ac4_frame) {
    Reader r(raw_ac4_frame);
    auto toc_result = parse_toc(r);
    if (!toc_result) {
        return std::unexpected(toc_result.error());
    }
    RawFrame result;
    result.toc = std::move(*toc_result);
    const std::size_t toc_bytes = (r.bit_position() + 7) / 8;
    const auto chan_indices = channel_substream_indices(result.toc);
    std::size_t offset = toc_bytes + static_cast<std::size_t>(result.toc.payload_base);
    for (int index = 0; index < result.toc.n_substreams; ++index) {
        const auto size =
            static_cast<std::size_t>(result.toc.substream_sizes[static_cast<std::size_t>(index)]);
        // substream_index_table()'s own sizes are trusted, self-declared
        // lengths (§4.3.3.12.4) - nothing earlier in parse_toc() cross-checks
        // them against how much data `raw_ac4_frame` actually holds, since
        // the TOC itself can be, and normally is, far smaller than the
        // frame. A caller-supplied span that ends before the last declared
        // substream is exactly the truncated-file case this parser exists
        // to report cleanly rather than emit a Substream with a byte range
        // that reaches past the data it was handed.
        if (offset + size > raw_ac4_frame.size()) {
            return std::unexpected(Error::kTruncated);
        }
        Substream sub;
        sub.offset = offset;
        sub.size = size;
        sub.is_channel_audio = static_cast<std::size_t>(index) < chan_indices.size() &&
                               chan_indices[static_cast<std::size_t>(index)];
        if (sub.is_channel_audio && size >= 3) {
            Reader sub_r(raw_ac4_frame.subspan(offset, size));
            sub.audio_size = parse_substream_header(sub_r);
        }
        result.substreams.push_back(sub);
        offset += size;
    }
    return result;
}

}  // namespace ac4
