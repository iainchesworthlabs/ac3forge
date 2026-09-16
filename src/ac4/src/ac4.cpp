#include "ac4/ac4.hpp"

#include <array>
#include <bit>
#include <limits>
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
    }
    return "unknown ac4::Error";
}

namespace {

// MSB-first bit reader with a sticky failure state - the same shape
// ac3::core::BitReader uses for overflow, extended here to also carry the
// explicit refusal condition (kUnsupportedBitstreamVersion) so every parse_*
// helper below can bail out with a plain early return instead of threading
// std::expected through the whole call tree. Only parse_raw_frame(), at the
// boundary, converts the final state to std::expected.
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

    // Discards n bytes by moving the read position, for a byte count the
    // stream chose: presentation_config_ext_info's n_skip_bytes, which
    // variable_bits() lets reach 2^32. skip(8 * n) overflowed int on such a
    // count, and would then have walked the phantom bits past the end of the
    // data one at a time. A count past the end marks the reader overflowed,
    // as reading those bits would have.
    void skip_bytes(std::uint32_t n) {
        const std::uint64_t end = std::uint64_t{data_.size()} * 8;
        const std::uint64_t target = std::uint64_t{position_} + std::uint64_t{n} * 8;
        if (target <= end) {
            position_ = static_cast<std::size_t>(target);
            return;
        }
        overflowed_ = true;
        if (position_ < end) {
            position_ = static_cast<std::size_t>(end);
        }
    }

    // Bit-granular twin of skip_bytes(), for a bit count the stream chose
    // (oamd_common_data()'s add_data, after trim()/bed_render_info()/
    // headphone() spend some of add_data_bytes*8) rather than a whole byte
    // count - same 64-bit-safe arithmetic, same reasoning.
    void skip_bits(std::uint64_t n) {
        const std::uint64_t end = std::uint64_t{data_.size()} * 8;
        const std::uint64_t target = std::uint64_t{position_} + n;
        if (target <= end) {
            position_ = static_cast<std::size_t>(target);
            return;
        }
        overflowed_ = true;
        if (position_ < end) {
            position_ = static_cast<std::size_t>(end);
        }
    }

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
    // Summed unsigned and converted once, as parse_substream_index_ref() does:
    // variable_bits() reaches 2^32, and adding it to an int can overflow.
    std::uint32_t emdf_version = r.bits(2);
    if (emdf_version == 3) {
        emdf_version += variable_bits(r, 2);
    }
    info.emdf_version = static_cast<int>(emdf_version);
    std::uint32_t key_id = r.bits(3);
    if (key_id == 7) {
        key_id += variable_bits(r, 3);
    }
    info.key_id = static_cast<int>(key_id);
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

// Returns frame_rate_fraction: 1, or the 2 or 4 transmission frames one coded
// frame is spread over in the efficient high frame rate mode (Part 2 clause
// 5.1.3, Table 18). A reader that takes each transmission frame for a whole
// one misreads a stream in that mode, so the value is reported rather than
// dropped.
int parse_frame_rate_fractions_info(Reader& r, int frame_rate_index, int frame_rate_factor) {
    switch (frame_rate_index) {
        case 5:
        case 6:
        case 7:
        case 8:
        case 9:
            if (frame_rate_factor == 1 && r.bits(1)) {  // b_frame_rate_fraction
                return 2;
            }
            break;
        case 10:
        case 11:
        case 12:
            if (r.bits(1)) {  // b_frame_rate_fraction
                return r.bits(1) ? 4 : 2;  // b_frame_rate_fraction_is_4
            }
            break;
        default:
            break;
    }
    return 1;
}

// --- §4.2.3.9 ac4_hsf_ext_substream_info ------------------------------------
// Part 1 has no parameter; Part 2 gates it on b_substreams_present
// (§6.2.1.14). Both shapes just name a substream_index_table() row: the
// index of the ac4_substream() that holds this element's owner's
// ac4_hsf_ext_substream() content.
std::optional<int> parse_hsf_ext_substream_info(Reader& r, bool b_substreams_present) {
    if (b_substreams_present) {
        return parse_substream_index_ref(r);
    }
    return std::nullopt;
}

// --- §4.2.3.8 / §6.2.1.5 presentation_config_ext_info -----------------------

// Skipped as n_skip_bytes whole bytes. For bitstream_version 1 with
// presentation_config 7, §6.2.1.5 puts a nested ac4_presentation_v1_info() at
// the start of those bytes and counts it inside n_skip_bytes, so skipping
// keeps the TOC in step; such a presentation reports presentation_config 7
// and no substreams. Table 4 of Part 2 allows that shape, and it is the only
// route to ac4_sgi_specifier()'s inline ac4_substream_group_info() and its
// sus_ver bit - neither parsed here. No stream observed writes it.
void parse_presentation_config_ext_info(Reader& r) {
    std::uint32_t n_skip_bytes = r.bits(5);
    if (r.bits(1)) {  // b_more_skip_bytes
        n_skip_bytes += variable_bits(r, 2) << 5;
    }
    // Not skip(8 * n): see Reader::skip_bytes(). Found by fuzz_ac4_parse once
    // ac4_objects was built with UndefinedBehaviorSanitizer.
    r.skip_bytes(n_skip_bytes);
}

// --- Table 90 (§4.3.3.7.5): bitrate_indicator -------------------------------

// Returns Table 90's own brate_ind column (0..19) - NOT the raw "Value of
// bitrate_indicator" bit pattern the table also lists, which is ambiguous
// as a plain integer: the 3-bit terminal codes 0b100/0b110 (4/6) equal the
// 5-bit extended codes 0b00100/0b00110 (also 4/6) once read into an int,
// since leading zeros don't change a binary literal's value. A 3-bit code
// with LSB 0 is terminal (brate_ind = v/2, 0-3); LSB 1 extends to 5 bits,
// continuing the same sequence: prefix 001 -> 4-7, 011 -> 8-11, 101 ->
// 12-15, 111 -> 16-19 (the last two prefixes are Table 90's own combined
// "0b1X1XX (8 further values), Unlimited" row - deliberately absent from
// bitrate_kbps() below, which reports them as unmapped).
int read_bitrate_indicator(Reader& r) {
    const std::uint32_t v = r.bits(3);
    if (!(v & 1)) {
        return static_cast<int>(v / 2);
    }
    const std::uint32_t extra = r.bits(2);
    return static_cast<int>(4 + (v / 2) * 4 + extra);
}

std::optional<int> bitrate_kbps(int indicator) {
    static const std::unordered_map<int, int> kTable = {
        {0, 16}, {1, 20}, {2, 24}, {3, 28}, {4, 32},  {5, 40},
        {6, 48}, {7, 56}, {8, 64}, {9, 80}, {10, 96}, {11, 112},
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
        info.add_ch_base = r.bits(1) != 0;
    }
    if (r.bits(1)) {  // b_content_type
        info.content_type = parse_content_type(r);
    }
    for (int i = 0; i < frame_rate_factor; ++i) {
        info.b_iframe.push_back(r.bits(1) != 0);
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
        info.add_ch_base = r.bits(1) != 0;
    }
    for (int i = 0; i < frame_rate_factor; ++i) {
        info.b_iframe.push_back(r.bits(1) != 0);  // b_audio_ndot
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

// --- §4.2.3.2 / §6.2.1.2 / §6.2.1.3 n_add_emdf_substreams -------------------

// The loop that ends both presentation info elements. It follows their
// `presentation_config == 6` if/else, so both branches reach it: an EMDF-only
// presentation sets b_add_emdf_substreams without transmitting it, then
// transmits the count and every emdf_info() the same as any other.
void parse_add_emdf_substreams(Reader& r, std::vector<int>& payloads_substream_indices) {
    std::uint32_t n = r.bits(2);  // n_add_emdf_substreams
    if (n == 0) {
        n = variable_bits(r, 2) + 4;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        if (const EmdfInfo emdf = parse_emdf_info(r); emdf.payloads_substream_index) {
            payloads_substream_indices.push_back(*emdf.payloads_substream_index);
        }
        // n reaches here through variable_bits() and so runs to 2^32.
        // parse_emdf_info() does real work per iteration, so without
        // this a 200-byte frame spends six seconds walking a count no
        // data backs - the reader is the only thing that ends it.
        // parse_toc() checks r.error() after every presentation it parses.
        if (r.error()) {
            break;
        }
    }
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
    bool b_add_emdf_substreams = false;
    if (!b_single_substream && presentation_config == 6) {
        // An EMDF-only presentation: nothing but the loop below.
        b_add_emdf_substreams = true;
    } else {
        pres.md_compat = static_cast<int>(r.bits(3));
        if (r.bits(1)) {  // b_belongs_to_presentation_id
            pres.presentation_id = static_cast<int>(variable_bits(r, 2));
        }
        const int frame_rate_factor = parse_frame_rate_multiply_info(r, frame_rate_index);
        if (const EmdfInfo emdf = parse_emdf_info(r); emdf.payloads_substream_index) {
            pres.emdf_payloads_substream_indices.push_back(*emdf.payloads_substream_index);
        }
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
                    auto& sub = pres.substreams.emplace_back(
                        std::string(roles[static_cast<std::size_t>(i)]),
                        parse_substream_info_v0(r, fs_index, frame_rate_factor));
                    if (i == 0 && b_hsf_ext) {
                        sub.second.hsf_ext_substream_index = parse_hsf_ext_substream_info(r, true);
                    }
                }
            } else {
                parse_presentation_config_ext_info(r);
            }
        }
        pres.b_pre_virtualized = r.bits(1) != 0;
        b_add_emdf_substreams = r.bits(1) != 0;
    }
    if (b_add_emdf_substreams) {
        parse_add_emdf_substreams(r, pres.emdf_payloads_substream_indices);
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
// --- §6.2.1.13 oamd_substream_info ------------------------------------------

OamdSubstreamInfo parse_oamd_substream_info(Reader& r, bool b_substreams_present) {
    OamdSubstreamInfo info;
    info.b_oamd_ndot = r.bits(1) != 0;
    if (b_substreams_present) {
        info.substream_index = parse_substream_index_ref(r);
    }
    return info;
}

// --- §6.2.1.10 bed_dyn_obj_assignment ---------------------------------------

// Table 62 (§6.3.2.10.5, direct-coded) and Table 63 (A-JOC-coded) both
// index bed_chan_assign_code the same way: how many BED objects the code
// expands to. The two tables differ (direct-coded's counts run one higher
// per entry, room for its own extra LFE slot at index 3) so each caller
// passes its own.
constexpr std::array<int, 8> kBedChanAssignCountAjoc = {2, 3, 5, 7, 9, 7, 9, 11};
constexpr std::array<int, 8> kBedChanAssignCountDirect = {2, 3, 6, 8, 10, 8, 10, 12};
constexpr std::array<int, 10> kStdBedGroupSize = {2, 1, 1, 2, 2, 2, 2, 2, 2, 1};
// isf_config's object count, read by both bed_dyn_obj_assignment() and
// ac4_substream_info_obj().
constexpr std::array<int, 6> kIsfCounts = {4, 8, 10, 14, 15, 30};

// The object count a 3-bit code names in a table shorter than eight entries
// (kIsfCounts here, kNumObjects in parse_substream_info_obj()). Codes past the
// end of the table are reserved and name no count, so they expand to no
// objects rather than reading past the array; parsing continues, as it does
// for a reserved channel_mode or bitrate code. No caller reads bits per
// object, so the count cannot desync the frame. Found by fuzz_ac4_parse once
// ac4_objects was built with AddressSanitizer: the committed
// ac4-substream-size-not-transmitted regression input reads kNumObjects[6].
template <std::size_t N>
int count_for_code(const std::array<int, N>& table, std::uint32_t code) {
    return code < N ? table[code] : 0;
}

// §6.2.1.10 / §6.3.2.10.8. Always ajoc_coded=true - this element only
// appears inside ac4_substream_info_ajoc().
std::vector<ObjectEntry> parse_bed_dyn_obj_assignment(Reader& r, int n_signals) {
    std::vector<ObjectEntry> objects;
    auto add = [&](ObjectKind kind, bool lfe) { objects.push_back({kind, lfe, true}); };

    if (r.bits(1)) {  // b_dyn_objects_only
        return objects;  // every object in this substream is dynamic and unlisted here
    }
    if (r.bits(1)) {  // b_isf
        const int n_isf = count_for_code(kIsfCounts, r.bits(3));
        for (int i = 0; i < n_isf; ++i) {
            add(ObjectKind::kIsf, false);
        }
        return objects;
    }
    if (r.bits(1)) {  // b_ch_assign_code
        const int count = kBedChanAssignCountAjoc[r.bits(3)];
        for (int i = 0; i < count; ++i) {
            add(ObjectKind::kBed, false);
        }
        return objects;
    }
    if (!r.bits(1)) {  // b_channel_assignment_flags_present
        // Neither an assignment code nor explicit flags: one nonstd_bed_
        // channel_assignment code (§6.3.2.10.8) per bed signal, n_bed_signals
        // of them (1, unless n_signals > 1 lets more than one be named).
        // Unsigned: bed_ch_bits reaches 31, and 2^31 - 1 plus one overflows
        // an int.
        std::uint32_t n_bed_signals = 1;
        if (n_signals > 1) {
            const int bed_ch_bits = std::bit_width(static_cast<unsigned>(n_signals - 1));
            n_bed_signals = r.bits(bed_ch_bits) + 1;
        }
        for (std::uint32_t b = 0; b < n_bed_signals; ++b) {
            if (r.bits(4) != 3) {  // nonstd_bed_channel_assignment
                add(ObjectKind::kBed, false);
            }
            // n_bed_signals is sized from n_signals, which the caller lets
            // reach 2^32 through variable_bits() (n_fullband_upmix_signals
            // == 16 opens that escape). Once the data is gone r.bits(4)
            // returns a phantom 0 - never the 3 that would skip the append -
            // so without this the loop keeps growing `objects` for as long
            // as the count says: 1.8 GB and six seconds, on a 200-byte
            // frame, before this check existed.
            if (r.error()) {
                break;
            }
        }
        return objects;
    }
    if (r.bits(1)) {  // b_nonstd_bed_channel_assignment_flags_present
        // Table 64: array position (16 - channel_order); array position 0
        // is the FIRST bit transmitted, so it becomes the MSB (bit 16) of a
        // monolithic MSB-first r.bits(17) - position 16 is the LAST bit
        // transmitted, the LSB (bit 0). flag[j] therefore sits at bit
        // (16-j), i.e. flag[16-i] sits at bit i. Cross-checked against
        // §6.3.2.10.8 EXAMPLE 2's worked value in tests/ac4/test_ac4.cpp.
        const std::uint32_t flags = r.bits(17);
        for (int i = 0; i < 17; ++i) {
            if ((flags >> i) & 1) {  // flag[16-i]
                if (i != 3 && i != 16) {
                    add(ObjectKind::kBed, false);
                }
            }
        }
    } else {
        const std::uint32_t flags = r.bits(10);
        for (int i = 0; i < 10; ++i) {
            if ((flags >> i) & 1) {  // flag[9-i], same reasoning as above
                if (i != 2 && i != 9) {
                    for (int j = 0; j < kStdBedGroupSize[static_cast<std::size_t>(i)]; ++j) {
                        add(ObjectKind::kBed, false);
                    }
                }
            }
        }
    }
    return objects;
}

// --- §6.2.8.13-16 tool_tb_to_f_s[_b] / tool_tf_to_f_s[_b], §6.2.9.9-10 -----
// tool_t2_to_f_s[_b]: eight tables, three call shapes total, differing only
// in field names - one shared reader (see GainTool in ac4.hpp).

GainTool parse_gain_tool(Reader& r, bool has_side_branch) {
    GainTool tool;
    if (r.bits(1)) {  // b_..._to_front
        tool.code_a = static_cast<int>(r.bits(3));
        tool.code_b = 7;
        return tool;
    }
    if (!has_side_branch) {
        tool.code_b = static_cast<int>(r.bits(3));
        return tool;
    }
    if (r.bits(1)) {  // b_..._to_side
        tool.code_b = static_cast<int>(r.bits(3));
        return tool;
    }
    tool.code_b = 7;
    tool.code_c = static_cast<int>(r.bits(3));
    return tool;
}

// --- §6.2.8.8a stereo_dmx_coeff ----------------------------------------------

StereoDmxCoeff parse_stereo_dmx_coeff(Reader& r) {
    StereoDmxCoeff c;
    c.loro_centre_mixgain = static_cast<int>(r.bits(3));
    c.loro_surround_mixgain = static_cast<int>(r.bits(3));
    if (r.bits(1)) {  // b_ltrt_mixinfo
        c.ltrt_centre_mixgain = static_cast<int>(r.bits(3));
        c.ltrt_surround_mixgain = static_cast<int>(r.bits(3));
    }
    if (r.bits(1)) {  // b_lfe_mixinfo
        c.lfe_mixgain = static_cast<int>(r.bits(5));
    }
    c.preferred_dmx_method = static_cast<int>(r.bits(2));
    return c;
}

// --- §6.2.8.8 bed_render_info ------------------------------------------------

std::optional<BedRenderInfo> parse_bed_render_info(Reader& r) {
    if (!r.bits(1)) {  // b_bed_render_info
        return std::nullopt;
    }
    BedRenderInfo info;
    if (r.bits(1)) {  // b_stereo_dmx_coeff
        info.stereo_dmx_coeff = parse_stereo_dmx_coeff(r);
    }
    if (!r.bits(1)) {  // b_cdmx_data_present
        return info;
    }
    if (r.bits(1)) {  // b_cdmx_w_to_f
        info.gain_w_to_f_code = static_cast<int>(r.bits(3));
    }
    if (r.bits(1)) {  // b_cdmx_b4_to_b2
        info.gain_b4_to_b2_code = static_cast<int>(r.bits(3));
    }
    if (r.bits(1)) {  // b_tm_ch_present
        if (r.bits(1)) {  // b_cdmx_t2_to_f_s_b
            info.t2_to_f_s_b = parse_gain_tool(r, true);
        }
        if (r.bits(1)) {  // b_cdmx_t2_to_f_s
            info.t2_to_f_s = parse_gain_tool(r, false);
        }
    }
    const bool b_tb_ch_present = r.bits(1) != 0;
    if (b_tb_ch_present) {
        if (r.bits(1)) {  // b_cdmx_tb_to_f_s_b
            info.tb_to_f_s_b = parse_gain_tool(r, true);
        }
        if (r.bits(1)) {  // b_cdmx_tb_to_f_s
            info.tb_to_f_s = parse_gain_tool(r, false);
        }
    }
    const bool b_tf_ch_present = r.bits(1) != 0;
    if (b_tf_ch_present) {
        if (r.bits(1)) {  // b_cdmx_tf_to_f_s_b
            info.tf_to_f_s_b = parse_gain_tool(r, true);
        }
        if (r.bits(1)) {  // b_cdmx_tf_to_f_s
            info.tf_to_f_s = parse_gain_tool(r, false);
        }
    }
    if ((b_tb_ch_present || b_tf_ch_present) && r.bits(1)) {  // b_cdmx_tfb_to_tm
        info.gain_tfb_to_tm_code = static_cast<int>(r.bits(3));
    }
    return info;
}

// --- §6.2.8.9 trim / §6.2.8.9a headphone -------------------------------------

// §6.3.9.10.4: "the number of trim configurations is nine".
constexpr int kNumTrimConfigs = 9;

std::optional<Trim> parse_trim(Reader& r) {
    if (!r.bits(1)) {  // b_trim_present
        return std::nullopt;
    }
    Trim trim;
    trim.warp_mode = static_cast<int>(r.bits(2));
    r.skip(2);  // reserved
    trim.global_trim_mode = static_cast<int>(r.bits(2));
    if (trim.global_trim_mode == 0b10) {
        trim.configs.reserve(kNumTrimConfigs);
        for (int i = 0; i < kNumTrimConfigs; ++i) {
            if (r.bits(1)) {  // b_default_trim
                trim.configs.push_back(std::nullopt);
                continue;
            }
            TrimConfig cfg;
            cfg.disabled = r.bits(1) != 0;  // b_disable_trim
            if (!cfg.disabled) {
                cfg.presence = static_cast<int>(r.bits(5));  // trim_balance_presence[]
                if (cfg.presence & 0b10000) {                // [4]
                    cfg.trim_centre = static_cast<int>(r.bits(4));
                }
                if (cfg.presence & 0b01000) {  // [3]
                    cfg.trim_surround = static_cast<int>(r.bits(4));
                }
                if (cfg.presence & 0b00100) {  // [2]
                    cfg.trim_height = static_cast<int>(r.bits(4));
                }
                if (cfg.presence & 0b00010) {  // [1]: sign, amount
                    const int sign = static_cast<int>(r.bits(1));
                    cfg.bal3d_y_tb = {sign, static_cast<int>(r.bits(4))};
                }
                if (cfg.presence & 0b00001) {  // [0]: sign, amount
                    const int sign = static_cast<int>(r.bits(1));
                    cfg.bal3d_y_lis = {sign, static_cast<int>(r.bits(4))};
                }
            }
            trim.configs.push_back(cfg);
        }
    }
    return trim;
}

std::optional<Headphone> parse_headphone(Reader& r) {
    if (!r.bits(1)) {  // b_headphone
        return std::nullopt;
    }
    Headphone hp;
    hp.hp_operation_mode = static_cast<int>(r.bits(3));
    if (hp.hp_operation_mode == 0b001 || hp.hp_operation_mode == 0b010) {
        hp.b_head_track_disable_all = r.bits(1) != 0;
    }
    return hp;
}

// --- §6.2.8.1 oamd_common_data ------------------------------------------------

// Embedded, at the TOC level, in ac4_substream_info_ajoc() when it sets
// b_oamd_common_data_present - see ac4.hpp's module docs for where its
// second call site (oamd_substream(), never walked here) sits.
OamdCommonData parse_oamd_common_data(Reader& r) {
    OamdCommonData data;
    data.b_default_screen_size_ratio = r.bits(1) != 0;
    if (!data.b_default_screen_size_ratio) {
        data.master_screen_size_ratio_code = static_cast<int>(r.bits(5));
    }
    data.b_bed_object_chan_distribute = r.bits(1) != 0;
    if (!r.bits(1)) {  // b_additional_data
        return data;
    }
    std::uint64_t add_data_bytes = r.bits(1) + 1;  // add_data_bytes_minus1
    if (add_data_bytes == 2) {
        add_data_bytes += variable_bits(r, 2);
    }
    std::uint64_t add_data_bits = add_data_bytes * 8;

    // bits_used = X(); add_data_bits -= bits_used, tracked by reader
    // position rather than each parser returning its own bit count. A
    // nested element reading past its remaining budget - only possible on a
    // malformed stream, since a real encoder sizes add_data_bytes to fit
    // exactly what it wrote - fails the substream the same way running past
    // the actual end of the data would, rather than let the elements after
    // it be read from the wrong position.
    const auto spend = [&](auto&& parse) {
        const std::size_t start = r.bit_position();
        auto value = parse(r);
        const std::size_t consumed = r.bit_position() - start;
        if (consumed > add_data_bits) {
            r.fail(Error::kTruncated);
            add_data_bits = 0;
        } else {
            add_data_bits -= consumed;
        }
        return value;
    };

    data.trim = spend(parse_trim);
    if (add_data_bits && !r.error()) {
        data.bed_render_info = spend(parse_bed_render_info);
    }
    if (add_data_bits && !r.error()) {
        data.headphone = spend(parse_headphone);
    }
    if (add_data_bits && !r.error()) {
        r.skip_bits(add_data_bits);  // add_data: raw bits this parser does not interpret
    }
    return data;
}

// --- §6.2.1.9 ac4_substream_info_ajoc ---------------------------------------

AjocSubstreamInfo parse_substream_info_ajoc(Reader& r, int fs_index, int frame_rate_factor,
                                             bool b_substreams_present) {
    AjocSubstreamInfo info;
    info.b_lfe = r.bits(1) != 0;
    info.b_static_dmx = r.bits(1) != 0;
    if (info.b_static_dmx) {
        info.n_fullband_dmx_signals = 5;
    } else {
        info.n_fullband_dmx_signals = static_cast<int>(r.bits(4)) + 1;
        info.static_objects = parse_bed_dyn_obj_assignment(r, info.n_fullband_dmx_signals);
    }
    if (r.bits(1)) {  // b_oamd_common_data_present
        info.oamd_common_data = parse_oamd_common_data(r);
        if (r.error()) {
            return info;
        }
    }
    // Summed unsigned for the same reason as parse_emdf_info()'s escapes.
    std::uint32_t n_fullband_upmix_signals = r.bits(4) + 1;
    if (n_fullband_upmix_signals == 16) {
        n_fullband_upmix_signals += variable_bits(r, 3);
    }
    info.n_fullband_upmix_signals = static_cast<int>(n_fullband_upmix_signals);
    info.upmix_objects = parse_bed_dyn_obj_assignment(r, info.n_fullband_upmix_signals);
    if (fs_index == 1 && r.bits(1)) {  // b_sf_multiplier
        info.sf_multiplier = static_cast<int>(r.bits(1));
    }
    if (r.bits(1)) {  // b_bitrate_info
        info.bitrate_kbps = bitrate_kbps(read_bitrate_indicator(r));
    }
    for (int i = 0; i < frame_rate_factor; ++i) {
        r.skip(1);  // b_audio_ndot
    }
    if (b_substreams_present) {
        info.substream_index = parse_substream_index_ref(r);
    }
    return info;
}

// --- §6.2.1.11 ac4_substream_info_obj ---------------------------------------

ObjSubstreamInfo parse_substream_info_obj(Reader& r, int fs_index, int frame_rate_factor,
                                           bool b_substreams_present) {
    ObjSubstreamInfo info;
    auto add = [&](ObjectKind kind, bool lfe) { info.objects.push_back({kind, lfe, false}); };

    constexpr std::array<int, 6> kNumObjects = {0, 1, 2, 3, 5, 7};
    // Table 60 (§6.3.2.10.2): codes 0-4 are b_lfe/1+b_lfe/2+b_lfe/3+b_lfe/
    // 5+b_lfe, 5-7 reserved - but the syntax table's own lookup is this
    // flat 6-entry array regardless, and b_lfe is folded in separately
    // below rather than by this array, so a "reserved" code still parses
    // (just with a count this parser cannot cross-check against the
    // semantics table's own account of it). Codes 6 and 7 fall past the end
    // of the array and name no objects - see count_for_code().
    const int num_objects = count_for_code(kNumObjects, r.bits(3));
    info.b_dynamic_objects = r.bits(1) != 0;
    if (info.b_dynamic_objects) {
        // No early return: fs_index/bitrate/b_audio_ndot/substream_index
        // below are read unconditionally, after this whole if/else - the
        // syntax table's braces close this branch well before them.
        const bool b_lfe = r.bits(1) != 0;
        for (int i = 0; i < num_objects; ++i) {
            if (b_lfe && i == 0) {
                add(ObjectKind::kBed, true);
            } else {
                add(ObjectKind::kDyn, false);
            }
        }
    } else if (r.bits(1)) {  // b_bed_objects
        if (r.bits(1)) {     // b_bed_start
            if (r.bits(1)) {  // b_ch_assign_code
                const int count = kBedChanAssignCountDirect[r.bits(3)];
                for (int i = 0; i < count; ++i) {
                    add(ObjectKind::kBed, i == 3);
                }
            } else if (r.bits(1)) {  // b_nonstd_bed_channel_assignment_flags_present
                const std::uint32_t flags = r.bits(17);
                for (int i = 0; i < 17; ++i) {
                    if ((flags >> i) & 1) {
                        add(ObjectKind::kBed, i == 3 || i == 16);
                    }
                }
            } else {
                const std::uint32_t flags = r.bits(10);
                for (int i = 0; i < 10; ++i) {
                    if ((flags >> i) & 1) {  // flag[9-i] - see parse_bed_dyn_obj_assignment()
                        for (int j = 0; j < kStdBedGroupSize[static_cast<std::size_t>(i)]; ++j) {
                            add(ObjectKind::kBed, i == 2 || i == 9);
                        }
                    }
                }
            }
        }
    } else if (r.bits(1)) {  // b_isf
        if (r.bits(1)) {     // b_isf_start
            const int n_isf = count_for_code(kIsfCounts, r.bits(3));
            for (int i = 0; i < n_isf; ++i) {
                add(ObjectKind::kIsf, false);
            }
        }
    } else {
        const int res_bytes = static_cast<int>(r.bits(4));
        r.skip(8 * res_bytes);
    }
    if (fs_index == 1 && r.bits(1)) {  // b_sf_multiplier
        info.sf_multiplier = static_cast<int>(r.bits(1));
    }
    if (r.bits(1)) {  // b_bitrate_info
        info.bitrate_kbps = bitrate_kbps(read_bitrate_indicator(r));
    }
    for (int i = 0; i < frame_rate_factor; ++i) {
        r.skip(1);  // b_audio_ndot
    }
    if (b_substreams_present) {
        info.substream_index = parse_substream_index_ref(r);
    }
    return info;
}

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
    group.b_channel_coded = r.bits(1) != 0;
    if (group.b_channel_coded) {
        for (std::uint32_t i = 0; i < n_lf_substreams; ++i) {
            // sus_ver only exists for bitstream_version == 1; the caller only
            // reaches this function for bitstream_version >= 2 (see parse_toc()'s
            // dispatch), where it is implicitly 1 (extended ac4_substream()
            // syntax) per §6.2.1.6.
            auto chan =
                parse_substream_info_chan(r, fs_index, frame_rate_factor, group.b_substreams_present);
            std::optional<int> hsf_ext_substream_index;
            if (b_hsf_ext) {
                hsf_ext_substream_index = parse_hsf_ext_substream_info(r, group.b_substreams_present);
            }
            GroupSubstream sub;
            sub.kind = GroupSubstream::Kind::kChan;
            sub.chan = std::move(chan);
            sub.hsf_ext_substream_index = hsf_ext_substream_index;
            group.substreams.push_back(std::move(sub));
            if (r.error()) {
                return group;  // the object-coded loop below already did this
            }
        }
    } else {
        if (r.bits(1)) {  // b_oamd_substream
            group.oamd = parse_oamd_substream_info(r, group.b_substreams_present);
        }
        for (std::uint32_t i = 0; i < n_lf_substreams; ++i) {
            GroupSubstream sub;
            if (r.bits(1)) {  // b_ajoc
                sub.kind = GroupSubstream::Kind::kAjoc;
                sub.ajoc = parse_substream_info_ajoc(r, fs_index, frame_rate_factor,
                                                      group.b_substreams_present);
            } else {
                sub.kind = GroupSubstream::Kind::kObj;
                sub.obj = parse_substream_info_obj(r, fs_index, frame_rate_factor,
                                                    group.b_substreams_present);
            }
            if (b_hsf_ext) {
                sub.hsf_ext_substream_index =
                    parse_hsf_ext_substream_info(r, group.b_substreams_present);
            }
            group.substreams.push_back(std::move(sub));
            if (r.error()) {
                return group;  // an ajoc's oamd_common_data() failed - caller checks r.error()
            }
        }
    }
    if (r.bits(1)) {  // b_content_type
        group.content_type = parse_content_type(r);
    }
    return group;
}

// --- §6.2.1.3 ac4_presentation_v1_info / §6.2.1.7 ac4_sgi_specifier --------

// How many ac4_sgi_specifier() elements §6.2.1.3 reads for presentation_config
// 0 to 4. Not the n_substream_groups it assigns: "Main + DE" (1) reads two
// specifiers and sets n_substream_groups to 1, and "Main + DE + Associated
// Audio" (4) reads three and sets 2 (verified on the rendered page 115).
constexpr std::array<int, 5> kV1ConfigGroupCounts = {2, 2, 2, 3, 3};  // presentation_config 0-4

// §6.2.1.7. `ac4_sgi_specifier()`'s own bitstream_version == 1 branch
// (inlining a whole ac4_substream_group_info() rather than a group_index
// reference) is unreachable here: parse_toc() only calls
// parse_presentation_v1_info() - and so this - for bitstream_version >= 2,
// per §6.2.1.1's own `if (bitstream_version <= 1) {legacy} else {v1}`
// dispatch. The one bitstream_version 1 route, §6.2.1.5's nested
// ac4_presentation_v1_info(), lies inside the bytes
// parse_presentation_config_ext_info() skips. Every group is referenced by
// index, resolved later against Toc::substream_groups.
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
    bool b_add_emdf_substreams = false;
    if (!b_single_substream_group && presentation_config == 6) {
        // An EMDF-only presentation: nothing but the loop below. It
        // references no substream group and transmits no
        // frame_rate_multiply_info(), so frame_rate_factor keeps its default.
        b_add_emdf_substreams = true;
    } else {
        if (bitstream_version != 1) {
            pres.md_compat = static_cast<int>(r.bits(3));
        }
        if (r.bits(1)) {  // b_presentation_id
            pres.presentation_id = static_cast<int>(variable_bits(r, 2));
        }
        pres.frame_rate_factor = parse_frame_rate_multiply_info(r, frame_rate_index);
        pres.frame_rate_fraction =
            parse_frame_rate_fractions_info(r, frame_rate_index, pres.frame_rate_factor);
        if (const EmdfInfo emdf = parse_emdf_info(r); emdf.payloads_substream_index) {
            pres.emdf_payloads_substream_indices.push_back(*emdf.payloads_substream_index);
        }
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
                    // Same unbounded-count shape as substream_index_table(): n
                    // passes through variable_bits(), so the reader running out
                    // is the only thing that ends this loop.
                    if (r.error()) {
                        return pres;
                    }
                }
            } else {
                parse_presentation_config_ext_info(r);
            }
        }
        pres.b_pre_virtualized = r.bits(1) != 0;
        b_add_emdf_substreams = r.bits(1) != 0;
        // ac4_presentation_substream_info() (§6.2.1.12)
        pres.b_alternative = r.bits(1) != 0;
        pres.b_pres_ndot = r.bits(1) != 0;
        pres.presentation_substream_index = parse_substream_index_ref(r);
    }
    if (b_add_emdf_substreams) {
        parse_add_emdf_substreams(r, pres.emdf_payloads_substream_indices);
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
            // n_substreams comes through variable_bits() and so has no
            // useful upper bound; without this the loop grows
            // substream_sizes off the end of the data, which a fuzzed frame
            // rode to a 2 GB allocation.
            if (r.error()) {
                return;
            }
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
    // A group_index escapes through variable_bits() (parse_sgi_specifier()),
    // so a reference can be INT_MAX itself, where + 1 would overflow. No
    // frame holds that many groups either way: parse_toc()'s group loop stops
    // when the reader runs out.
    return max_group_index == std::numeric_limits<int>::max() ? max_group_index
                                                              : max_group_index + 1;
}

// frame_rate_factor is frame-global in practice - see
// parse_substream_group_info()'s own comment - so every substream group takes
// it from the first presentation that transmits frame_rate_multiply_info().
// An EMDF-only presentation transmits none, so it is passed over. Its
// presentation_config is 6, a value no other presentation holds:
// parse_presentation_v1_info() leaves the field unset when
// b_single_substream_group is set.
int substream_group_frame_rate_factor(const std::vector<PresentationInfoV1>& presentations) {
    for (const auto& p : presentations) {
        if (p.presentation_config != 6) {
            return p.frame_rate_factor;
        }
    }
    return 1;
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
        // Summed unsigned for the same reason as parse_emdf_info()'s escapes;
        // parse_raw_frame() reads the int back as the unsigned value sent.
        std::uint32_t payload_base = r.bits(5) + 1;
        if (payload_base == 0x20) {
            payload_base += variable_bits(r, 3);
        }
        toc.payload_base = static_cast<int>(payload_base);
    }
    if (toc.bitstream_version <= 1) {
        // No reserve() on n_presentations, and the same r.error() check the
        // substream-group loop below already had: both counts come from the
        // bitstream, so reserving on one is an attacker-chosen allocation
        // (a fuzzed frame asked for 0x2000000100 bytes of
        // SubstreamGroupInfo), and a loop that does not stop when the reader
        // is exhausted keeps building elements out of nothing.
        for (int i = 0; i < toc.n_presentations; ++i) {
            toc.presentations_v0.push_back(
                parse_presentation_info_v0(r, fs_index, toc.frame_rate_index));
            if (const auto err = r.error()) {
                return std::unexpected(*err);
            }
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
        for (int i = 0; i < toc.n_presentations; ++i) {
            toc.presentations_v1.push_back(
                parse_presentation_v1_info(r, toc.bitstream_version, toc.frame_rate_index));
            if (const auto err = r.error()) {
                return std::unexpected(*err);
            }
        }
        const int total_groups = total_substream_groups(toc.presentations_v1);
        const int group_frame_rate_factor = substream_group_frame_rate_factor(toc.presentations_v1);
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
std::vector<bool> audio_substream_indices(const Toc& toc) {
    std::vector<bool> is_audio(static_cast<std::size_t>(toc.n_substreams), false);
    auto mark = [&](std::optional<int> idx) {
        if (idx && *idx >= 0 && static_cast<std::size_t>(*idx) < is_audio.size()) {
            is_audio[static_cast<std::size_t>(*idx)] = true;
        }
    };
    if (!toc.substream_groups.empty()) {
        for (const auto& group : toc.substream_groups) {
            for (const auto& sub : group.substreams) {
                switch (sub.kind) {
                    case GroupSubstream::Kind::kChan:
                        mark(sub.chan ? sub.chan->substream_index : std::nullopt);
                        break;
                    case GroupSubstream::Kind::kAjoc:
                        mark(sub.ajoc ? sub.ajoc->substream_index : std::nullopt);
                        break;
                    case GroupSubstream::Kind::kObj:
                        mark(sub.obj ? sub.obj->substream_index : std::nullopt);
                        break;
                }
            }
        }
    } else {
        for (const auto& pres : toc.presentations_v0) {
            for (const auto& [role, sub] : pres.substreams) {
                mark(sub.substream_index);
            }
        }
    }
    return is_audio;
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
    const auto audio_indices = audio_substream_indices(result.toc);
    // payload_base and substream_size[] are unsigned counts the stream chose,
    // stored as int. Both are read back here as the unsigned values that were
    // sent and checked against what is left of the frame in 64 bits. The
    // check used to be offset + size > frame size on size_t, with a size
    // above INT_MAX sign-extended from its int: a payload_base past the frame
    // plus such a size wrapped the sum back under the frame size, passed, and
    // handed parse_substream_header() a subspan starting past the end.
    const std::uint64_t frame_size = raw_ac4_frame.size();
    std::uint64_t offset =
        toc_bytes + std::uint64_t{static_cast<std::uint32_t>(result.toc.payload_base)};
    for (int index = 0; index < result.toc.n_substreams; ++index) {
        // §4.2.3.11 transmits substream_size[] only when b_size_present, and
        // that flag is read at all only when n_substreams == 1 (Table 14) -
        // so substream_sizes is either exactly n_substreams long or empty,
        // and empty means "one substream, size not transmitted". Its extent
        // is still unambiguous: raw_ac4_frame is one frame_size-bounded
        // frame, the shape scan() hands over, so the only substream runs
        // from payload_base to the end of it.
        //
        // n_substreams was indexed straight into substream_sizes before
        // this, which read element 0 of an empty vector - a null dereference
        // on any stream that set b_size_present to 0. Found by
        // fuzz/fuzz_ac4_parse.cpp on its first run; tests/ac4 had only ever
        // built the b_size_present = 1 shape.
        const bool size_transmitted = !result.toc.substream_sizes.empty();
        const std::uint64_t size =
            size_transmitted
                ? std::uint64_t{static_cast<std::uint32_t>(
                      result.toc.substream_sizes[static_cast<std::size_t>(index)])}
                : (offset <= frame_size ? frame_size - offset : 0);
        // substream_index_table()'s own sizes are trusted, self-declared
        // lengths (§4.3.3.12.4) - nothing earlier in parse_toc() cross-checks
        // them against how much data `raw_ac4_frame` actually holds, since
        // the TOC itself can be, and normally is, far smaller than the
        // frame. A caller-supplied span that ends before the last declared
        // substream is exactly the truncated-file case this parser exists
        // to report cleanly rather than emit a Substream with a byte range
        // that reaches past the data it was handed.
        if (offset > frame_size || size > frame_size - offset) {
            return std::unexpected(Error::kTruncated);
        }
        Substream sub;
        sub.offset = static_cast<std::size_t>(offset);
        sub.size = static_cast<std::size_t>(size);
        sub.is_audio = static_cast<std::size_t>(index) < audio_indices.size() &&
                       audio_indices[static_cast<std::size_t>(index)];
        if (sub.is_audio && size >= 3) {
            Reader sub_r(raw_ac4_frame.subspan(sub.offset, sub.size));
            sub.audio_size = parse_substream_header(sub_r);
        }
        result.substreams.push_back(sub);
        offset += size;
    }
    return result;
}


// --- Carriage (roadmap IM4's separable slice) -------------------------------

namespace {

// Annex E's DSI fields are written MSB-first into whole bytes, the same
// bit-packing discipline the parser reads with - small enough here that a
// local accumulator beats pulling a writer dependency into a module whose
// whole identity is depending on nothing.
class DsiWriter {
   public:
    void put(std::uint32_t value, int bits) {
        for (int bit = bits - 1; bit >= 0; --bit) {
            accumulator_ = static_cast<std::uint8_t>(
                (static_cast<std::uint32_t>(accumulator_) << 1) | ((value >> bit) & 1u));
            if (++filled_ == 8) {
                bytes_.push_back(static_cast<std::byte>(accumulator_));
                accumulator_ = 0;
                filled_ = 0;
            }
        }
    }
    void byte_align() {
        while (filled_ != 0) {
            put(0, 1);
        }
    }
    [[nodiscard]] std::vector<std::byte> take() {
        byte_align();
        return std::move(bytes_);
    }

   private:
    std::vector<std::byte> bytes_;
    std::uint8_t accumulator_ = 0;
    int filled_ = 0;
};

// The two Toc presentation lists only ever have one populated (the struct's
// own comment); this reads whichever it is.
[[nodiscard]] int first_presentation_version(const Toc& toc) {
    if (!toc.presentations_v1.empty()) {
        return toc.presentations_v1.front().presentation_version;
    }
    if (!toc.presentations_v0.empty()) {
        return toc.presentations_v0.front().presentation_version;
    }
    return 0;
}

[[nodiscard]] int first_md_compat(const Toc& toc) {
    if (!toc.presentations_v1.empty()) {
        return toc.presentations_v1.front().md_compat.value_or(0);
    }
    if (!toc.presentations_v0.empty()) {
        return toc.presentations_v0.front().md_compat.value_or(0);
    }
    return 0;
}

}  // namespace

std::vector<std::byte> build_dac4(const Toc& toc) {
    DsiWriter w;
    // ac4_dsi_v1 (Annex E.5).
    w.put(1, 3);  // ac4_dsi_version
    w.put(static_cast<std::uint32_t>(toc.bitstream_version), 7);
    w.put(toc.sample_rate_hz == 48000 ? 1u : 0u, 1);  // fs_index (Table 82)
    w.put(static_cast<std::uint32_t>(toc.frame_rate_index), 4);
    w.put(static_cast<std::uint32_t>(toc.n_presentations), 9);
    if (toc.bitstream_version > 1) {
        // b_program_id: the TOC-level program identifier is not parsed into
        // Toc (it rides ahead of the presentation list), so none is claimed.
        w.put(0, 1);
    }
    // ac4_bitrate_dsi (Annex E.7): mode 0 with both fields 0xFFFFFFFF -
    // "unknown", the honest value for a muxer handed frames rather than an
    // encoder's rate plan.
    w.put(0, 2);
    w.put(0xFFFFFFFFu, 32);
    w.put(0xFFFFFFFFu, 32);
    w.byte_align();

    // One entry per presentation: its version, and pres_bytes = 0 - see the
    // header's own comment for why the per-presentation DSI body is this
    // slice's stated boundary.
    const auto version_of = [&](int index) {
        if (!toc.presentations_v1.empty() &&
            index < static_cast<int>(toc.presentations_v1.size())) {
            return toc.presentations_v1[static_cast<std::size_t>(index)].presentation_version;
        }
        if (!toc.presentations_v0.empty() &&
            index < static_cast<int>(toc.presentations_v0.size())) {
            return toc.presentations_v0[static_cast<std::size_t>(index)].presentation_version;
        }
        return 0;
    };
    for (int p = 0; p < toc.n_presentations; ++p) {
        w.put(static_cast<std::uint32_t>(version_of(p)), 8);
        w.put(0, 8);  // pres_bytes
    }
    return w.take();
}

std::optional<std::uint32_t> samples_per_frame(const Toc& toc) {
    // Table 83/84. At 44,1 kHz only the 2048-sample frame exists; at 48 kHz
    // the 1000/1001-family entries with a NON-integer sample count per frame
    // (29,97 / 59,94 / 119,88 fps - the frame length alternates) have no
    // single answer and yield nullopt.
    if (toc.sample_rate_hz == 44100) {
        return toc.frame_rate_index == 13 ? std::optional<std::uint32_t>{2048} : std::nullopt;
    }
    switch (toc.frame_rate_index) {
        case 0: return 2002;   // 23,976 fps
        case 1: return 2000;   // 24
        case 2: return 1920;   // 25
        case 3: return std::nullopt;  // 29,97: 1601,6 - alternating
        case 4: return 1600;   // 30
        case 5: return 1001;   // 47,952
        case 6: return 1000;   // 48
        case 7: return 960;    // 50
        case 8: return std::nullopt;  // 59,94: 800,8 - alternating
        case 9: return 800;    // 60
        case 10: return 480;   // 100
        case 11: return std::nullopt;  // 119,88: 400,4 - alternating
        case 12: return 400;   // 120
        case 13: return 2048;  // the sample-rate-locked frame
        default: return std::nullopt;
    }
}

std::string rfc6381_codec_string(const Toc& toc) {
    // Annex E.13: two lowercase hex digits per field.
    constexpr std::string_view kHex = "0123456789abcdef";
    const auto pair = [&](int value) {
        std::string out;
        out.push_back(kHex[static_cast<std::size_t>((value >> 4) & 0xF)]);
        out.push_back(kHex[static_cast<std::size_t>(value & 0xF)]);
        return out;
    };
    return "ac-4." + pair(toc.bitstream_version) + "." +
           pair(first_presentation_version(toc)) + "." + pair(first_md_compat(toc));
}

}  // namespace ac4
