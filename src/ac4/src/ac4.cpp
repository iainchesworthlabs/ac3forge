#include "ac4/ac4.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <numeric>
#include <string>
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

// --- SyncFrameSplitter ----------------------------------------------------------

std::span<std::byte> SyncFrameSplitter::writable() noexcept {
    consume(handed_);
    handed_ = 0;
    return storage_.subspan(filled_);
}

void SyncFrameSplitter::commit(std::size_t bytes) noexcept {
    filled_ = std::min(filled_ + bytes, storage_.size());
}

void SyncFrameSplitter::consume(std::size_t count) noexcept {
    count = std::min(count, filled_);
    if (count == 0) {
        return;
    }
    std::memmove(storage_.data(), storage_.data() + count, filled_ - count);
    filled_ -= count;
    position_ += count;
}

SyncFrameSplitter::Result SyncFrameSplitter::next() noexcept {
    consume(handed_);
    handed_ = 0;
    const auto byte = [this](std::size_t i) { return std::to_integer<unsigned>(storage_[i]); };
    const auto is_sync = [&byte](std::size_t i) {
        return byte(i) == 0xAC && (byte(i + 1) == 0x40 || byte(i + 1) == 0x41);
    };
    // What is held when no whole frame is: more is needed, or at the end
    // the part of a frame left over is dropped.
    const auto wait = [this]() {
        if (!finished_) {
            return Result{.status = Status::kNeedMoreInput};
        }
        if (filled_ > 0 && !truncated_) {
            truncated_ = true;
            consume(filled_);
            return Result{.status = Status::kTruncated};
        }
        consume(filled_);
        return Result{.status = Status::kEndOfStream};
    };
    // Skips to the next sync word after the first byte held; with none held,
    // skips everything but a last byte of 0xAC, which may begin a sync word
    // the next read completes. Called with two bytes or more held.
    const auto skip = [&]() {
        std::size_t at = 1;
        while (at + 1 < filled_ && !is_sync(at)) {
            ++at;
        }
        const bool found = at + 1 < filled_;
        const std::size_t count = found || byte(at) == 0xAC ? at : at + 1;
        skipped_ += count;
        resynchronising_ = true;
        consume(count);
    };
    for (;;) {
        if (filled_ < 2) {
            return wait();
        }
        if (!is_sync(0)) {
            skip();
            continue;
        }
        if (filled_ < 4) {
            return wait();
        }
        const auto sync = static_cast<std::uint16_t>((byte(0) << 8U) | byte(1));
        std::size_t header = 4;
        std::size_t frame_size = (byte(2) << 8U) | byte(3);
        if (frame_size == 0xFFFF) {
            if (filled_ < 7) {
                return wait();
            }
            frame_size = (byte(4) << 16U) | (byte(5) << 8U) | byte(6);
            header = 7;
        }
        const bool has_crc = sync == 0xAC41;
        const std::size_t total = header + frame_size + (has_crc ? 2U : 0U);
        if (total > storage_.size()) {
            // A sync word found by skipping may be a frame's bits, whose size
            // means nothing: try the next one. A stream that starts on a sync
            // word does not get that doubt.
            if (resynchronising_) {
                skip();
                continue;
            }
            return Result{.status = Status::kBufferTooSmall};
        }
        if (filled_ < total) {
            return wait();
        }
        if (resynchronising_ && !finished_) {
            // Confirmed by the sync word that follows, where the storage can
            // hold it.
            if (total + 2 <= storage_.size()) {
                if (filled_ < total + 2) {
                    return Result{.status = Status::kNeedMoreInput};
                }
                if (!is_sync(total)) {
                    skip();
                    continue;
                }
            }
        }
        resynchronising_ = false;
        std::optional<bool> crc_ok;
        if (has_crc) {
            const auto want = static_cast<std::uint16_t>((byte(total - 2) << 8U) | byte(total - 1));
            crc_ok = crc16(std::span<const std::byte>(storage_).subspan(2, total - 4)) == want;
        }
        handed_ = total;
        return Result{
            .status = Status::kFrame,
            .frame = SyncFrame{
                .offset = position_,
                .sync_word = sync,
                .raw_ac4_frame = std::span<const std::byte>(storage_).subspan(header, frame_size),
                .crc_ok = crc_ok}};
    }
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
            ct.serialized_language_tag = true;
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
        info.brate_ind = read_bitrate_indicator(r);
        info.bitrate_kbps = bitrate_kbps(*info.brate_ind);
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
        info.brate_ind = read_bitrate_indicator(r);
        info.bitrate_kbps = bitrate_kbps(*info.brate_ind);
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
void parse_add_emdf_substreams(Reader& r, std::vector<int>& payloads_substream_indices,
                               std::vector<EmdfVersionKey>* versions = nullptr) {
    std::uint32_t n = r.bits(2);  // n_add_emdf_substreams
    if (n == 0) {
        n = variable_bits(r, 2) + 4;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        const EmdfInfo emdf = parse_emdf_info(r);
        if (emdf.payloads_substream_index) {
            payloads_substream_indices.push_back(*emdf.payloads_substream_index);
        }
        if (versions != nullptr) {
            versions->push_back({emdf.emdf_version, emdf.key_id});
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

// The loudspeakers the assignments name, as Table A.27 indexes them (see
// ObjectEntry::speaker).
constexpr int kL = 0, kR = 1, kC = 2, kLs = 3, kRs = 4, kLb = 5, kRb = 6, kTfl = 7, kTfr = 8,
              kTbl = 9, kTbr = 10, kLfe = 11, kTsl = 12, kTsr = 13, kLfe2 = 19, kLw = 26, kRw = 27;
// Tables 63 (A-JOC coded) and 62 (direct coded): bed_chan_assign_code's
// speakers, in the order objects take them.
constexpr std::array<std::array<int, 12>, 8> kBedChanAssignAjoc = {{
    {kL, kR},
    {kL, kR, kC},
    {kL, kR, kC, kLs, kRs},
    {kL, kR, kC, kLs, kRs, kTsl, kTsr},
    {kL, kR, kC, kLs, kRs, kTfl, kTfr, kTbl, kTbr},
    {kL, kR, kC, kLs, kRs, kLb, kRb},
    {kL, kR, kC, kLs, kRs, kLb, kRb, kTsl, kTsr},
    {kL, kR, kC, kLs, kRs, kLb, kRb, kTfl, kTfr, kTbl, kTbr},
}};
constexpr std::array<std::array<int, 12>, 8> kBedChanAssignDirect = {{
    {kL, kR},
    {kL, kR, kC},
    {kL, kR, kC, kLfe, kLs, kRs},
    {kL, kR, kC, kLfe, kLs, kRs, kTsl, kTsr},
    {kL, kR, kC, kLfe, kLs, kRs, kTfl, kTfr, kTbl, kTbr},
    {kL, kR, kC, kLfe, kLs, kRs, kLb, kRb},
    {kL, kR, kC, kLfe, kLs, kRs, kLb, kRb, kTsl, kTsr},
    {kL, kR, kC, kLfe, kLs, kRs, kLb, kRb, kTfl, kTfr, kTbl, kTbr},
}};
// Table 64: nonstd_bed_channel_assignment_flag[]'s channel order.
constexpr std::array<int, 17> kNonstdFlagSpeakers = {
    kL, kR, kC, kLfe, kLs, kRs, kLb, kRb, kTfl, kTfr, kTsl, kTsr, kTbl, kTbr, kLw, kRw, kLfe2};
// Table 65: std_bed_channel_assignment_flag[]'s, one or two speakers each.
constexpr std::array<std::array<int, 2>, 10> kStdFlagSpeakers = {{
    {kL, kR},
    {kC, -1},
    {kLfe, -1},
    {kLs, kRs},
    {kLb, kRb},
    {kTfl, kTfr},
    {kTsl, kTsr},
    {kTbl, kTbr},
    {kLw, kRw},
    {kLfe2, -1},
}};
// Table 66: nonstd_bed_channel_assignment's; 3 is reserved.
constexpr std::array<int, 16> kNonstdAssignmentSpeakers = {
    kL, kR, kC, -1, kLs, kRs, kLb, kRb, kTfl, kTfr, kTsl, kTsr, kTbl, kTbr, kLw, kRw};
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
    auto add = [&](ObjectKind kind, bool lfe) {
        objects.push_back({kind, lfe, true, std::nullopt});
    };
    auto add_bed = [&](int speaker) {
        objects.push_back({ObjectKind::kBed, false, true, speaker});
    };

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
        const std::uint32_t code = r.bits(3);
        const int count = kBedChanAssignCountAjoc[code];
        for (int i = 0; i < count; ++i) {
            add_bed(kBedChanAssignAjoc[code][static_cast<std::size_t>(i)]);
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
            const std::uint32_t assignment = r.bits(4);  // nonstd_bed_channel_assignment
            if (assignment != 3) {
                add_bed(kNonstdAssignmentSpeakers[assignment]);
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
                    add_bed(kNonstdFlagSpeakers[static_cast<std::size_t>(i)]);
                }
            }
        }
    } else {
        const std::uint32_t flags = r.bits(10);
        for (int i = 0; i < 10; ++i) {
            if ((flags >> i) & 1) {  // flag[9-i], same reasoning as above
                if (i != 2 && i != 9) {
                    for (int j = 0; j < kStdBedGroupSize[static_cast<std::size_t>(i)]; ++j) {
                        add_bed(kStdFlagSpeakers[static_cast<std::size_t>(i)]
                                                [static_cast<std::size_t>(j)]);
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
        info.brate_ind = read_bitrate_indicator(r);
        info.bitrate_kbps = bitrate_kbps(*info.brate_ind);
    }
    for (int i = 0; i < frame_rate_factor; ++i) {
        info.b_iframe.push_back(r.bits(1) != 0);  // b_audio_ndot
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
    auto add = [&](ObjectKind kind, bool lfe, std::optional<int> speaker) {
        info.objects.push_back({kind, lfe, false, speaker});
    };

    // Table 60 (§6.3.2.10.2): codes 0 to 4 give b_lfe, 1 + b_lfe, 2 + b_lfe,
    // 3 + b_lfe and 5 + b_lfe objects, and 5 to 7 are reserved. The syntax's
    // own array, [0, 1, 2, 3, 5, 7], gives code 5 seven objects, which no
    // channel element objs_to_channel_mode() names can carry, and loops the
    // dynamic objects over that count with the LFE among them, where Table 60
    // and audio_data_objs(), whose mono_data(1) precedes an element of
    // n_objects channels, count it on top: the table is read, the LFE first
    // (src/ac4dec/ERRATA.md, "n_objects_code and the LFE"). A reserved code
    // names no objects; nothing after it depends on the count.
    constexpr std::array<int, 5> kNumObjects = {0, 1, 2, 3, 5};
    const std::uint32_t n_objects_code = r.bits(3);
    if (n_objects_code < kNumObjects.size()) {
        info.num_objects = kNumObjects[n_objects_code];
    }
    const int num_objects = info.num_objects.value_or(0);
    info.b_dynamic_objects = r.bits(1) != 0;
    if (info.b_dynamic_objects) {
        // No early return: fs_index/bitrate/b_audio_ndot/substream_index
        // below are read unconditionally, after this whole if/else - the
        // syntax table's braces close this branch well before them.
        info.b_lfe = r.bits(1) != 0;
        if (info.b_lfe && info.num_objects) {
            add(ObjectKind::kBed, true, kLfe);
        }
        for (int i = 0; i < num_objects; ++i) {
            add(ObjectKind::kDyn, false, std::nullopt);
        }
    } else if (r.bits(1)) {  // b_bed_objects
        info.static_kind = ObjSubstreamInfo::Static::kBed;
        info.static_start = r.bits(1) != 0;  // b_bed_start
        if (info.static_start) {
            if (r.bits(1)) {  // b_ch_assign_code
                const std::uint32_t code = r.bits(3);
                const int count = kBedChanAssignCountDirect[code];
                for (int i = 0; i < count; ++i) {
                    add(ObjectKind::kBed, i == 3,
                        kBedChanAssignDirect[code][static_cast<std::size_t>(i)]);
                }
            } else if (r.bits(1)) {  // b_nonstd_bed_channel_assignment_flags_present
                const std::uint32_t flags = r.bits(17);
                for (int i = 0; i < 17; ++i) {
                    if ((flags >> i) & 1) {
                        add(ObjectKind::kBed, i == 3 || i == 16,
                            kNonstdFlagSpeakers[static_cast<std::size_t>(i)]);
                    }
                }
            } else {
                const std::uint32_t flags = r.bits(10);
                for (int i = 0; i < 10; ++i) {
                    if ((flags >> i) & 1) {  // flag[9-i] - see parse_bed_dyn_obj_assignment()
                        for (int j = 0; j < kStdBedGroupSize[static_cast<std::size_t>(i)]; ++j) {
                            add(ObjectKind::kBed, i == 2 || i == 9,
                                kStdFlagSpeakers[static_cast<std::size_t>(i)]
                                                [static_cast<std::size_t>(j)]);
                        }
                    }
                }
            }
        }
    } else if (r.bits(1)) {  // b_isf
        info.static_kind = ObjSubstreamInfo::Static::kIsf;
        info.static_start = r.bits(1) != 0;  // b_isf_start
        if (info.static_start) {
            const int n_isf = count_for_code(kIsfCounts, r.bits(3));
            for (int i = 0; i < n_isf; ++i) {
                add(ObjectKind::kIsf, false, std::nullopt);
            }
        }
    } else {
        info.static_kind = ObjSubstreamInfo::Static::kReserved;
        const int res_bytes = static_cast<int>(r.bits(4));
        r.skip(8 * res_bytes);
    }
    if (fs_index == 1 && r.bits(1)) {  // b_sf_multiplier
        info.sf_multiplier = static_cast<int>(r.bits(1));
    }
    if (r.bits(1)) {  // b_bitrate_info
        info.brate_ind = read_bitrate_indicator(r);
        info.bitrate_kbps = bitrate_kbps(*info.brate_ind);
    }
    for (int i = 0; i < frame_rate_factor; ++i) {
        info.b_iframe.push_back(r.bits(1) != 0);  // b_audio_ndot
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
    group.b_hsf_ext = b_hsf_ext;
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
        const EmdfInfo emdf = parse_emdf_info(r);
        if (emdf.payloads_substream_index) {
            pres.emdf_payloads_substream_indices.push_back(*emdf.payloads_substream_index);
        }
        pres.emdf = {emdf.emdf_version, emdf.key_id};
        if (r.bits(1)) {  // b_presentation_filter
            pres.enable_presentation = r.bits(1) != 0;
        }
        if (b_single_substream_group) {
            pres.group_refs.push_back(parse_sgi_specifier(r));
        } else {
            pres.b_multi_pid = r.bits(1) != 0;
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
    pres.b_add_emdf_substreams = b_add_emdf_substreams;
    if (b_add_emdf_substreams) {
        parse_add_emdf_substreams(r, pres.emdf_payloads_substream_indices, &pres.add_emdf);
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
        if (r.bits(1)) {  // b_program_id
            toc.short_program_id = static_cast<int>(r.bits(16));
            if (r.bits(1)) {  // b_program_uuid_present
                std::array<std::byte, 16> uuid{};
                for (std::byte& b : uuid) {
                    b = static_cast<std::byte>(r.bits(8));
                }
                toc.program_uuid = uuid;
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


// --- Carriage (AC-4 bitstream inspector's separable slice) -------------------------------

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

// Annex E.7's ac4_bitrate_dsi(): the mode wait_frames implies, the rate unknown.
void put_bitrate_dsi(DsiWriter& w, const Toc& toc) {
    std::uint32_t mode = 3;
    if (toc.wait_frames == 0) {
        mode = 1;
    } else if (toc.wait_frames && *toc.wait_frames >= 1 && *toc.wait_frames <= 6) {
        mode = 2;
    }
    w.put(mode, 2);
    w.put(0, 32);            // bit_rate: unknown
    w.put(0xFFFFFFFFu, 32);  // bit_rate_precision: unknown
}

// Annex E.10.3's audio channel groups of a channel mode (Table A.27's right
// column, group g at bit g). Pseudocode E.3 as printed sets no LFE, above
// ch_mode 10 no L/R or Ls/Rs and the centre as group 2, never the 9.X layouts'
// Lscr/Rscr, and 22.2's Tsl/Tsr only for one top pair; the groups here are the
// ones Table A.27 gives each mode, which is what DEE's muxer writes
// (src/ac4enc/ERRATA.md).
std::uint32_t channel_groups(int ch_mode, bool centre, bool four_back, int top_pairs) {
    std::uint32_t groups = 0;
    const auto set = [&groups](int g) { groups |= 1u << g; };
    const bool lfe = ch_mode == 4 || ch_mode == 6 || ch_mode == 8 || ch_mode == 10 || ch_mode == 12 ||
                     ch_mode == 14 || ch_mode == 15;
    if (ch_mode == 15) {
        for (const int g : {17, 15, 14, 13, 12, 11, 10, 9, 7, 5, 4, 3, 1}) {
            set(g);
        }
    }
    if (ch_mode == 13 || ch_mode == 14) {
        set(16);  // Lscr Rscr
    }
    if (ch_mode >= 11) {
        set(0);  // L R
        set(2);  // Ls Rs
        if (top_pairs == 1) {
            set(7);  // Tsl Tsr
        }
        if (top_pairs == 2) {
            set(4);  // Tfl Tfr
            set(5);  // Tbl Tbr
        }
        if (four_back) {
            set(3);  // Lb Rb
        }
        if (centre) {
            set(1);  // C
        }
    }
    if (ch_mode >= 2 && ch_mode <= 10) {
        if (ch_mode >= 3) {
            set(2);  // Ls Rs
        }
        if (ch_mode == 5 || ch_mode == 6) {
            set(3);  // Lb Rb
        }
        if (ch_mode == 7 || ch_mode == 8) {
            set(17);  // Lw Rw
        }
        if (ch_mode == 9 || ch_mode == 10) {
            set(4);  // Tfl Tfr
        }
        set(0);  // L R
        set(1);  // C
    }
    if (ch_mode == 1) {
        set(0);
    }
    if (ch_mode == 0) {
        set(1);
    }
    if (lfe) {
        set(6);
    }
    return groups;
}

// A reason build_dac4() writes nothing, a string literal (dac4_refusal()).
using Refusal = std::string_view;

// Part 2 clause 6.3.3.1.27's superset() over the channel modes, by the channels
// each mode holds in full: its channel groups with the centre, the four back
// channels and both top pairs present. The lowest mode holding every channel
// of both; -1 identity, superset(0, 1) is 1 as the clause says, and -1 where no
// mode holds both, as src/ac4dec/ERRATA.md ("The presentation substream") reads
// the six pairs the clause leaves without one.
[[nodiscard]] int superset(int a, int b) {
    if (a < 0) {
        return b;
    }
    if (b < 0) {
        return a;
    }
    if ((a == 0 && b == 1) || (a == 1 && b == 0)) {
        return 1;
    }
    const std::uint32_t wanted =
        channel_groups(a, true, true, 2) | channel_groups(b, true, true, 2);
    for (int mode = 0; mode <= 15; ++mode) {
        if ((channel_groups(mode, true, true, 2) & wanted) == wanted) {
            return mode;
        }
    }
    return -1;
}

// The same over Table 71's core modes 3 to 6 (5.0, 5.1, 5.0.2 and 5.1.2),
// each the one before with an LFE or a top pair added.
[[nodiscard]] int superset_core(int a, int b) {
    if (a < 0) {
        return b;
    }
    if (b < 0) {
        return a;
    }
    const bool lfe = a == 4 || a == 6 || b == 4 || b == 6;
    const bool top = a >= 5 || b >= 5;
    return 3 + (lfe ? 1 : 0) + (top ? 2 : 0);
}

// What Pseudocodes 25 and 26 and clauses 6.3.3.1.29 to 6.3.3.1.30 derive from
// every substream of the substream groups a presentation's specifiers name,
// each group once, as the decoder takes them (src/ac4dec/ERRATA.md,
// "presentation_config 1 and 4 read more specifiers than n_substream_groups").
struct PresentationShape {
    int ch_mode = -1;  // pres_ch_mode
    int core = -1;     // pres_ch_mode_core
    bool four_back = false;
    bool centre = false;
    int top_pairs = 0;
    // Whether every substream sends b_bitrate_info, and one at least does.
    bool bitrate_info = false;
};

std::expected<PresentationShape, Refusal> shape_of(const Toc& toc, const PresentationInfoV1& pres) {
    PresentationShape shape;
    bool objects = false;
    bool adaptive = false;
    bool any = false;
    bool every_rate = true;
    std::vector<bool> counted(toc.substream_groups.size(), false);
    for (const int ref : pres.group_refs) {
        if (ref < 0 || static_cast<std::size_t>(ref) >= toc.substream_groups.size()) {
            return std::unexpected(
                "a substream group the table of contents does not carry (b_multi_pid puts it in "
                "another elementary stream)");
        }
        const auto index = static_cast<std::size_t>(ref);
        if (counted[index]) {
            continue;
        }
        counted[index] = true;
        for (const GroupSubstream& s : toc.substream_groups[index].substreams) {
            any = true;
            if (s.kind == GroupSubstream::Kind::kChan && s.chan) {
                if (!s.chan->ch_mode) {
                    return std::unexpected("a substream of a channel mode the text reserves");
                }
                const int mode = *s.chan->ch_mode;
                shape.ch_mode = superset(shape.ch_mode, mode);
                // Table 71: the channel-coded rows.
                const int mode_core =
                    (mode == 11 || mode == 13) ? 5 : ((mode == 12 || mode == 14) ? 6 : -1);
                shape.core = superset_core(shape.core, mode_core);
                if (s.chan->original_content) {
                    const OriginalContent& content = *s.chan->original_content;
                    shape.four_back = shape.four_back || content.b_4_back_channels_present;
                    shape.centre = shape.centre || content.b_centre_present;
                    // Table 72, 2 winning where both rows hold.
                    const int pairs = content.top_channels_present == 3
                                          ? 2
                                          : (content.top_channels_present > 0 ? 1 : 0);
                    shape.top_pairs = std::max(shape.top_pairs, pairs);
                }
                every_rate = every_rate && s.chan->brate_ind.has_value();
            } else if (s.kind == GroupSubstream::Kind::kAjoc && s.ajoc) {
                objects = true;
                if (s.ajoc->b_static_dmx) {
                    shape.core = superset_core(shape.core, s.ajoc->b_lfe ? 4 : 3);
                } else {
                    adaptive = true;
                }
                every_rate = every_rate && s.ajoc->brate_ind.has_value();
            } else if (s.kind == GroupSubstream::Kind::kObj && s.obj) {
                objects = true;
                adaptive = true;
                every_rate = every_rate && s.obj->brate_ind.has_value();
            } else {
                return std::unexpected("a substream its group does not describe");
            }
        }
    }
    if (objects) {
        shape.ch_mode = -1;
    }
    if (adaptive) {
        shape.core = -1;
    }
    if (shape.core == shape.ch_mode) {
        shape.core = -1;
    }
    // Nothing contributes to a presentation without audio substreams, so it
    // sends no rate (src/ac4enc/ERRATA.md, "The bit rate and the indicators").
    shape.bitrate_info = any && every_rate;
    return shape;
}

// Part 1 Table E.5d: 0 at the base rate, 1 for twice it, 2 for four times.
[[nodiscard]] std::uint32_t dsi_sf_multiplier(const std::optional<int>& sf_multiplier) {
    return sf_multiplier ? static_cast<std::uint32_t>(*sf_multiplier + 1) : 0U;
}

// ac4_substream_group_dsi() (Annex E.11).
std::optional<Refusal> put_group_dsi(DsiWriter& w, const SubstreamGroupInfo& group) {
    if (group.substreams.size() > 255) {
        return "a substream group of more substreams than n_substreams' eight bits count";
    }
    if (group.content_type && group.content_type->serialized_language_tag) {
        return "a language tag sent in chunks, which one table of contents does not hold whole";
    }
    w.put(group.b_substreams_present ? 1U : 0U, 1);
    w.put(group.b_hsf_ext ? 1U : 0U, 1);
    w.put(group.b_channel_coded ? 1U : 0U, 1);
    w.put(static_cast<std::uint32_t>(group.substreams.size()), 8);
    for (const GroupSubstream& s : group.substreams) {
        std::optional<int> sf_multiplier;
        std::optional<int> brate_ind;
        if (group.b_channel_coded) {
            if (s.kind != GroupSubstream::Kind::kChan || !s.chan || !s.chan->ch_mode) {
                return "a channel-coded group whose substream is not a channel-coded one";
            }
            sf_multiplier = s.chan->sf_multiplier;
            brate_ind = s.chan->brate_ind;
        } else if (s.kind == GroupSubstream::Kind::kAjoc && s.ajoc) {
            sf_multiplier = s.ajoc->sf_multiplier;
            brate_ind = s.ajoc->brate_ind;
        } else if (s.kind == GroupSubstream::Kind::kObj && s.obj) {
            sf_multiplier = s.obj->sf_multiplier;
            brate_ind = s.obj->brate_ind;
        } else {
            return "an object-coded group whose substream is not an object one";
        }
        w.put(dsi_sf_multiplier(sf_multiplier), 2);
        w.put(brate_ind ? 1U : 0U, 1);  // b_substream_bitrate_indicator
        if (brate_ind) {
            w.put(static_cast<std::uint32_t>(*brate_ind), 5);
        }
        if (group.b_channel_coded) {
            // The channel groups of the substream's original content (NOTE 2).
            const OriginalContent content = s.chan->original_content.value_or(OriginalContent{});
            const int top_pairs =
                content.top_channels_present == 0 ? 0 : (content.top_channels_present == 3 ? 2 : 1);
            w.put(0, 6);  // reserved_zero
            w.put(channel_groups(*s.chan->ch_mode, content.b_centre_present,
                                 content.b_4_back_channels_present, top_pairs),
                  18);
            continue;
        }
        bool bed = false;
        bool dynamic = false;
        bool isf = false;
        w.put(s.ajoc ? 1U : 0U, 1);  // b_ajoc
        if (s.ajoc) {
            const AjocSubstreamInfo& ajoc = *s.ajoc;
            if (ajoc.n_fullband_dmx_signals < 1 || ajoc.n_fullband_dmx_signals > 16 ||
                ajoc.n_fullband_upmix_signals < 1 || ajoc.n_fullband_upmix_signals > 64) {
                return "an A-JOC substream of more upmix objects than six bits count";
            }
            w.put(ajoc.b_static_dmx ? 1U : 0U, 1);
            if (!ajoc.b_static_dmx) {
                // n_dmx_objects_minus1
                w.put(static_cast<std::uint32_t>(ajoc.n_fullband_dmx_signals - 1), 4);
            }
            // n_umx_objects_minus1
            w.put(static_cast<std::uint32_t>(ajoc.n_fullband_upmix_signals - 1), 6);
            // The upmix's objects (Table E.15): bed_dyn_obj_assignment() lists
            // its bed and ISF objects, and the signals it does not list are
            // dynamic (src/ac4enc/ERRATA.md, "An A-JOC substream's objects").
            int listed = 0;
            for (const ObjectEntry& object : ajoc.upmix_objects) {
                bed = bed || object.kind == ObjectKind::kBed;
                isf = isf || object.kind == ObjectKind::kIsf;
                listed += object.kind == ObjectKind::kDyn ? 0 : 1;
            }
            dynamic = ajoc.n_fullband_upmix_signals > listed;
        } else {
            // What ac4_substream_info_obj() sends.
            bed = s.obj->static_kind == ObjSubstreamInfo::Static::kBed;
            dynamic = s.obj->b_dynamic_objects;
            isf = s.obj->static_kind == ObjSubstreamInfo::Static::kIsf;
        }
        w.put(bed ? 1U : 0U, 1);      // b_substream_contains_bed_objects
        w.put(dynamic ? 1U : 0U, 1);  // b_substream_contains_dynamic_objects
        w.put(isf ? 1U : 0U, 1);      // b_substream_contains_ISF_objects
        w.put(0, 1);                  // reserved
    }
    w.put(group.content_type ? 1U : 0U, 1);  // b_content_type
    if (group.content_type) {
        w.put(static_cast<std::uint32_t>(group.content_type->content_classifier), 3);
        const auto& tag = group.content_type->language_tag;
        w.put(tag ? 1U : 0U, 1);  // b_language_indicator
        if (tag) {
            w.put(static_cast<std::uint32_t>(tag->size()), 6);
            for (const std::byte b : *tag) {
                w.put(std::to_integer<std::uint32_t>(b), 8);
            }
        }
    }
    return std::nullopt;
}

// The DSI's closing byte (E.10.1): de_indicator, immersive_audio_indicator and
// an extended presentation_id, written where the Toc carries the indicators.
void put_indicators(DsiWriter& w, const PresentationInfoV1& pres) {
    const int id = pres.presentation_id.value_or(0);
    w.put(pres.de_indicator.value_or(false) ? 1U : 0U, 1);
    w.put(pres.immersive_audio_indicator.value_or(false) ? 1U : 0U, 1);
    w.put(0, 4);                  // reserved
    w.put(id > 31 ? 1U : 0U, 1);  // b_extended_presentation_id
    w.put(id > 31 ? static_cast<std::uint32_t>(id) : 0U, id > 31 ? 9 : 1);
}

[[nodiscard]] bool has_indicators(const PresentationInfoV1& pres) {
    return pres.de_indicator.has_value() || pres.immersive_audio_indicator.has_value();
}

std::optional<Refusal> put_add_emdf(DsiWriter& w, const std::vector<EmdfVersionKey>& add_emdf) {
    if (add_emdf.size() > 127) {
        return "more additional EMDF substreams than n_add_emdf_substreams' seven bits count";
    }
    w.put(static_cast<std::uint32_t>(add_emdf.size()), 7);
    for (const EmdfVersionKey& emdf : add_emdf) {
        if (emdf.emdf_version < 0 || emdf.emdf_version > 31 || emdf.key_id < 0 ||
            emdf.key_id > 1023) {
            return "an EMDF version or key_id past the DSI's five or ten bits";
        }
        w.put(static_cast<std::uint32_t>(emdf.emdf_version), 5);
        w.put(static_cast<std::uint32_t>(emdf.key_id), 10);
    }
    return std::nullopt;
}

// ac4_presentation_v1_dsi() (Annex E.10) for one presentation of the table of
// contents.
std::expected<std::vector<std::byte>, Refusal> presentation_v1_dsi(const Toc& toc,
                                                                   const PresentationInfoV1& pres) {
    DsiWriter w;
    if (pres.presentation_config == 6) {
        // EMDF payloads alone: presentation_config_v1 6 implies
        // b_add_emdf_substreams, and there is no substream to describe.
        w.put(6, 5);
        if (const auto refused = put_add_emdf(w, pres.add_emdf)) {
            return std::unexpected(*refused);
        }
        w.put(0, 1);  // b_presentation_bitrate_info: nothing contributes
        w.put(0, 1);  // b_alternative
        w.byte_align();
        if (has_indicators(pres)) {
            put_indicators(w, pres);
        }
        return w.take();
    }
    // Table 53's configurations, each with the substream groups its
    // specifiers name; unset for a single substream group.
    std::size_t groups = 1;
    if (pres.presentation_config) {
        const int config = *pres.presentation_config;
        if (config < 0 || config > 5) {
            return std::unexpected(
                "a presentation_config the text reserves, whose presentation_config_ext_info() "
                "the table of contents skips");
        }
        groups = config <= 2 ? 2 : (config <= 4 ? 3 : pres.group_refs.size());
        if (config == 5 && (groups < 2 || groups > 9)) {
            return std::unexpected(
                "more substream groups than n_substream_groups_minus2's three bits count");
        }
    }
    if (pres.group_refs.size() != groups) {
        return std::unexpected("a presentation whose substream groups did not all read");
    }
    const std::expected<PresentationShape, Refusal> shape = shape_of(toc, pres);
    if (!shape) {
        return std::unexpected(shape.error());
    }
    const int id = pres.presentation_id.value_or(0);
    if (id < 0 || id > 511) {
        return std::unexpected("a presentation_id past extended_presentation_id's nine bits");
    }
    if (id > 31 && !has_indicators(pres)) {
        return std::unexpected(
            "a presentation_id above 31, which the DSI carries only beside the indicators, "
            "and the table of contents carries no indicators");
    }
    if (pres.emdf.emdf_version < 0 || pres.emdf.emdf_version > 31 || pres.emdf.key_id < 0 ||
        pres.emdf.key_id > 1023) {
        return std::unexpected("an EMDF version or key_id past the DSI's five or ten bits");
    }
    if (pres.b_alternative && !pres.alternative_info) {
        return std::unexpected(
            "an alternative presentation, whose name and targets its presentation substream "
            "carries, and the table of contents does not");
    }

    const std::uint32_t config_v1 =
        pres.presentation_config ? static_cast<std::uint32_t>(*pres.presentation_config) : 0x1FU;
    w.put(config_v1, 5);  // presentation_config_v1
    w.put(static_cast<std::uint32_t>(pres.md_compat.value_or(0)), 3);
    w.put(pres.presentation_id ? 1U : 0U, 1);  // b_presentation_id
    if (pres.presentation_id) {
        w.put(static_cast<std::uint32_t>(id & 0x1F), 5);
    }
    // Tables E.12 and E.13, from the factor and fraction the TOC gave.
    const int index = toc.frame_rate_index;
    std::uint32_t multiply = 0;
    if ((index >= 2 && index <= 4) || index == 0 || index == 1 || (index >= 7 && index <= 9)) {
        multiply = pres.frame_rate_factor == 2 ? 1U : (pres.frame_rate_factor == 4 ? 2U : 0U);
    }
    std::uint32_t fraction = 0;
    if (index >= 5 && index <= 12) {
        fraction = pres.frame_rate_fraction == 2 ? 1U : (pres.frame_rate_fraction == 4 ? 2U : 0U);
    }
    w.put(multiply, 2);
    w.put(fraction, 2);
    w.put(static_cast<std::uint32_t>(pres.emdf.emdf_version), 5);
    w.put(static_cast<std::uint32_t>(pres.emdf.key_id), 10);

    // The presentation's channel mode (Pseudocode 25) and its channel groups
    // (Pseudocode E.3, as Table A.27 gives them: src/ac4enc/ERRATA.md).
    w.put(shape->ch_mode >= 0 ? 1U : 0U, 1);  // b_presentation_channel_coded
    if (shape->ch_mode >= 0) {
        w.put(static_cast<std::uint32_t>(shape->ch_mode), 5);
        if (shape->ch_mode >= 11 && shape->ch_mode <= 14) {
            w.put(shape->four_back ? 1U : 0U, 1);
            w.put(static_cast<std::uint32_t>(shape->top_pairs), 2);
        }
        w.put(0, 6);  // reserved_zero
        w.put(channel_groups(shape->ch_mode, shape->centre, shape->four_back, shape->top_pairs),
              18);
    }
    // b_presentation_core_differs where the core mode (Pseudocode 26) is not
    // -1 (Table E.11 prints "is -1"; src/ac4enc/ERRATA.md), and Table E.14's
    // code for it.
    w.put(shape->core >= 0 ? 1U : 0U, 1);
    if (shape->core >= 0) {
        w.put(1, 1);  // b_presentation_core_channel_coded
        w.put(static_cast<std::uint32_t>(shape->core - 3), 2);
    }
    w.put(pres.enable_presentation ? 1U : 0U, 1);  // b_presentation_filter
    if (pres.enable_presentation) {
        w.put(*pres.enable_presentation ? 1U : 0U, 1);
        w.put(0, 8);  // n_filter_bytes
    }
    if (pres.presentation_config) {
        w.put(pres.b_multi_pid ? 1U : 0U, 1);
        if (*pres.presentation_config == 5) {
            w.put(static_cast<std::uint32_t>(groups - 2), 3);  // n_substream_groups_minus2
        }
    }
    // A group named twice is described twice, as its specifiers name it.
    for (const int ref : pres.group_refs) {
        if (const auto refused =
                put_group_dsi(w, toc.substream_groups[static_cast<std::size_t>(ref)])) {
            return std::unexpected(*refused);
        }
    }

    w.put(pres.b_pre_virtualized ? 1U : 0U, 1);
    w.put(pres.b_add_emdf_substreams ? 1U : 0U, 1);
    if (pres.b_add_emdf_substreams) {
        if (const auto refused = put_add_emdf(w, pres.add_emdf)) {
            return std::unexpected(*refused);
        }
    }
    w.put(shape->bitrate_info ? 1U : 0U, 1);  // b_presentation_bitrate_info
    if (shape->bitrate_info) {
        put_bitrate_dsi(w, toc);
    }
    w.put(pres.b_alternative ? 1U : 0U, 1);
    if (pres.b_alternative) {
        // alternative_info() (E.12): the name's bytes without the 0 the
        // presentation substream closes it with, and each target's level and
        // device categories, Table 67's four Booleans above the four bits
        // tdc_extension would add (src/ac4enc/ERRATA.md, "An alternative
        // presentation's dac4").
        const AlternativeInfo& alternative = *pres.alternative_info;
        if (alternative.name.size() > 0xFFFF || alternative.targets.empty() ||
            alternative.targets.size() > 31) {
            return std::unexpected(
                "an alternative presentation's name or targets past alternative_info()'s fields");
        }
        w.byte_align();
        w.put(static_cast<std::uint32_t>(alternative.name.size()), 16);
        for (const char c : alternative.name) {
            w.put(static_cast<std::uint32_t>(static_cast<unsigned char>(c)), 8);
        }
        w.put(static_cast<std::uint32_t>(alternative.targets.size()), 5);
        for (const AlternativeTarget& target : alternative.targets) {
            if (target.md_compat < 0 || target.md_compat > 7 || target.device_category < 0 ||
                target.device_category > 15) {
                return std::unexpected("an alternative presentation's target past its fields");
            }
            w.put(static_cast<std::uint32_t>(target.md_compat), 3);
            w.put(static_cast<std::uint32_t>(target.device_category) << 4U, 8);
        }
    }
    w.byte_align();
    if (has_indicators(pres)) {
        put_indicators(w, pres);
    }
    return w.take();
}

std::expected<std::vector<std::byte>, Refusal> dac4_of(const Toc& toc) {
    if (toc.bitstream_version < 2) {
        return std::unexpected(
            "a bitstream_version 0 or 1 table of contents, whose presentations Part 1 Annex E.4a's "
            "ac4_presentation_v0_dsi() describes");
    }
    if (toc.n_presentations < 0 || toc.n_presentations > 511) {
        return std::unexpected("more presentations than n_presentations' nine bits count");
    }
    if (static_cast<std::size_t>(toc.n_presentations) != toc.presentations_v1.size()) {
        return std::unexpected("a table of contents whose presentations did not all read");
    }
    DsiWriter w;
    // ac4_dsi_v1 (Annex E.6).
    w.put(1, 3);  // ac4_dsi_version
    w.put(static_cast<std::uint32_t>(toc.bitstream_version), 7);
    w.put(toc.sample_rate_hz == 48000 ? 1U : 0U, 1);  // fs_index (Table 82)
    w.put(static_cast<std::uint32_t>(toc.frame_rate_index), 4);
    w.put(static_cast<std::uint32_t>(toc.n_presentations), 9);
    // The program identifier, copied from the table of contents.
    w.put(toc.short_program_id ? 1U : 0U, 1);  // b_program_id
    if (toc.short_program_id) {
        w.put(static_cast<std::uint32_t>(*toc.short_program_id), 16);
        w.put(toc.program_uuid ? 1U : 0U, 1);  // b_uuid
        if (toc.program_uuid) {
            for (const std::byte b : *toc.program_uuid) {
                w.put(std::to_integer<std::uint32_t>(b), 8);
            }
        }
    }
    put_bitrate_dsi(w, toc);
    w.byte_align();

    for (const PresentationInfoV1& pres : toc.presentations_v1) {
        // A version 2 presentation's DSI is a skip area to this annex; DEE's
        // muxer fills it with the version 1 structure, and so does this.
        if (pres.presentation_version < 1 || pres.presentation_version > 255) {
            return std::unexpected(
                "a presentation_version 0 presentation in a version 2 table of contents");
        }
        const std::expected<std::vector<std::byte>, Refusal> body = presentation_v1_dsi(toc, pres);
        if (!body) {
            return std::unexpected(body.error());
        }
        if (body->size() > 255 + 0xFFFF) {
            return std::unexpected(
                "a presentation longer than pres_bytes and add_pres_bytes count");
        }
        w.put(static_cast<std::uint32_t>(pres.presentation_version), 8);
        if (body->size() >= 255) {
            w.put(255, 8);
            w.put(static_cast<std::uint32_t>(body->size() - 255), 16);
        } else {
            w.put(static_cast<std::uint32_t>(body->size()), 8);  // pres_bytes
        }
        for (const std::byte b : *body) {
            w.put(std::to_integer<std::uint32_t>(b), 8);
        }
    }
    return w.take();
}

}  // namespace

std::vector<std::byte> build_dac4(const Toc& toc) {
    std::expected<std::vector<std::byte>, Refusal> dac4 = dac4_of(toc);
    return dac4 ? std::move(*dac4) : std::vector<std::byte>{};
}

std::string_view dac4_refusal(const Toc& toc) {
    const std::expected<std::vector<std::byte>, Refusal> dac4 = dac4_of(toc);
    return dac4 ? std::string_view{} : dac4.error();
}

std::string_view cmaf_refusal(const Toc& toc) {
    // Part 2 Annex H.1.2.1's constraints, as one table of contents shows them.
    if (toc.bitstream_version != 2) {
        return "a bitstream_version other than 2";
    }
    if (toc.n_presentations > 64) {
        return "more than 64 presentations";
    }
    std::vector<int> ids;
    for (const PresentationInfoV1& pres : toc.presentations_v1) {
        if (pres.presentation_version != 1) {
            return "a presentation_version other than 1";
        }
        // 6.2.1.3 reads no b_presentation_id for an EMDF-only presentation.
        if (pres.presentation_config == 6) {
            return "a presentation of configuration 6, EMDF payloads alone, which has no field for "
                   "the presentation_id every presentation needs";
        }
        if (!pres.presentation_id) {
            return "a presentation without a presentation_id";
        }
        if (std::ranges::find(ids, *pres.presentation_id) != ids.end()) {
            return "two presentations with one presentation_id";
        }
        ids.push_back(*pres.presentation_id);
    }
    return {};
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

std::optional<MediaTiming> media_timing(const Toc& toc) {
    if (const auto samples = samples_per_frame(toc)) {
        return MediaTiming{.timescale = static_cast<std::uint32_t>(toc.sample_rate_hz),
                           .sample_delta = *samples};
    }
    if (toc.sample_rate_hz != 48000) {
        return std::nullopt;
    }
    switch (toc.frame_rate_index) {
        case 3: return MediaTiming{.timescale = 240000, .sample_delta = 8008};   // 29,97 fps
        case 8: return MediaTiming{.timescale = 240000, .sample_delta = 4004};   // 59,94
        case 11: return MediaTiming{.timescale = 240000, .sample_delta = 2002};  // 119,88
        default: return std::nullopt;
    }
}

std::optional<FrameRate> frame_rate(const Toc& toc) {
    const std::optional<MediaTiming> timing = media_timing(toc);
    if (!timing || toc.frame_rate_index < 0 || toc.frame_rate_index > 13) {
        return std::nullopt;
    }
    // Table 83's frame lengths at the internal rate (frame_len_base), index 13
    // being Table 84's 2 048 at either rate.
    constexpr std::array<int, 14> kFrameLength = {1920, 1920, 2048, 1536, 1536, 960, 960,
                                                  1024, 768,  768,  512,  384,  384, 2048};
    FrameRate rate;
    rate.frames_per_second =
        static_cast<double>(timing->timescale) / static_cast<double>(timing->sample_delta);
    rate.frame_length = kFrameLength[static_cast<std::size_t>(toc.frame_rate_index)];
    rate.internal_rate_hz = static_cast<double>(rate.frame_length) * rate.frames_per_second;
    return rate;
}

std::string rfc6381_codec_string(const Toc& toc) {
    // Annex E.13: two lowercase hex digits per field, the presentation's two
    // from the one a manifest describes the track by. The two Toc presentation
    // lists only ever have one populated (the struct's own comment).
    constexpr std::string_view kHex = "0123456789abcdef";
    const auto pair = [&](int value) {
        std::string out;
        out.push_back(kHex[static_cast<std::size_t>((value >> 4) & 0xF)]);
        out.push_back(kHex[static_cast<std::size_t>(value & 0xF)]);
        return out;
    };
    int version = 0;
    int md_compat = 0;
    if (const std::optional<std::size_t> index = signalled_presentation(toc)) {
        if (!toc.presentations_v1.empty()) {
            version = toc.presentations_v1[*index].presentation_version;
            md_compat = toc.presentations_v1[*index].md_compat.value_or(0);
        } else {
            version = toc.presentations_v0[*index].presentation_version;
            md_compat = toc.presentations_v0[*index].md_compat.value_or(0);
        }
    }
    return "ac-4." + pair(toc.bitstream_version) + "." + pair(version) + "." + pair(md_compat);
}

std::optional<std::size_t> signalled_presentation(const Toc& toc) {
    // Annex G.2.3's widest compatibility: the level fewest decoders fall
    // short of, the lowest md_compat (Part 2 Table 55, Part 1 Table 86), among
    // the presentations a decoder may select.
    std::optional<std::size_t> best;
    int best_level = 0;
    const auto consider = [&](std::size_t index, bool audio, std::optional<int> md_compat) {
        const int level = md_compat.value_or(0);
        if (audio && (!best || level < best_level)) {
            best = index;
            best_level = level;
        }
    };
    if (!toc.presentations_v1.empty()) {
        for (std::size_t i = 0; i < toc.presentations_v1.size(); ++i) {
            const PresentationInfoV1& p = toc.presentations_v1[i];
            // Configuration 6 carries EMDF payloads alone.
            const bool audio = p.presentation_config != 6 && !p.group_refs.empty();
            consider(i, audio && p.enable_presentation.value_or(true), p.md_compat);
        }
        return best.has_value() ? best : std::optional<std::size_t>{0};
    }
    if (toc.presentations_v0.empty()) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < toc.presentations_v0.size(); ++i) {
        const PresentationInfoV0& p = toc.presentations_v0[i];
        consider(i, !p.substreams.empty(), p.md_compat);
    }
    return best.has_value() ? best : std::optional<std::size_t>{0};
}

namespace {

// Table A.27: how many speakers each audio channel group holds, group 8 being
// the deprecated pair group 7 replaces.
constexpr std::array<int, 18> kGroupSpeakers = {2, 1, 2, 2, 2, 2, 1, 2, 2,
                                                1, 1, 1, 1, 2, 1, 1, 2, 2};

// Table G.1: presentation_v1_channel_groups[], group g at bit g, and the value
// of urn:mpeg:mpegB:cicp:ChannelConfiguration it maps to.
struct CicpRow {
    std::uint32_t groups;
    int value;
};
// clang-format off
constexpr std::array<CicpRow, 27> kCicp = {{
    {0x000002, 1},  {0x000001, 2},  {0x000003, 3},  {0x008003, 4},  {0x000007, 5},
    {0x000047, 6},  {0x020047, 7},  {0x008001, 9},  {0x000005, 10}, {0x008047, 11},
    {0x00004F, 12}, {0x02FF7F, 13}, {0x06FF6F, 13}, {0x000057, 14}, {0x040047, 14},
    {0x00145F, 15}, {0x04144F, 15}, {0x000077, 16}, {0x040067, 16}, {0x000A77, 17},
    {0x040A67, 17}, {0x000A7F, 18}, {0x040A6F, 18}, {0x00007F, 19}, {0x04006F, 19},
    {0x01007F, 20}, {0x05006F, 20},
}};
// clang-format on

// What the manifest functions read of signalled_presentation(): its audio
// channel groups as build_dac4() writes them (Annex E.10.3), or that it is
// object audio. Nothing for a bitstream_version below 2, or a presentation
// whose substreams the table of contents does not describe whole.
struct SignalledChannels {
    bool objects = false;
    std::uint32_t groups = 0;
};

std::optional<SignalledChannels> signalled_channels(const Toc& toc) {
    const std::optional<std::size_t> index = signalled_presentation(toc);
    if (toc.bitstream_version < 2 || !index || *index >= toc.presentations_v1.size()) {
        return std::nullopt;
    }
    const PresentationInfoV1& pres = toc.presentations_v1[*index];
    const std::expected<PresentationShape, Refusal> shape = shape_of(toc, pres);
    if (!shape) {
        return std::nullopt;
    }
    if (shape->ch_mode >= 0) {
        return SignalledChannels{.objects = false,
                                 .groups = channel_groups(shape->ch_mode, shape->centre,
                                                          shape->four_back, shape->top_pairs)};
    }
    // Pseudocode 25 leaves no channel mode for object audio, and for a
    // presentation without audio substreams, which is not object audio.
    for (const int ref : pres.group_refs) {
        for (const GroupSubstream& s :
             toc.substream_groups[static_cast<std::size_t>(ref)].substreams) {
            if (s.kind != GroupSubstream::Kind::kChan) {
                return SignalledChannels{.objects = true, .groups = 0};
            }
        }
    }
    return std::nullopt;
}

// Six hexadecimal digits, as G.3.3.2's examples write them.
std::string hex6(std::uint32_t value) {
    constexpr std::string_view kHex = "0123456789ABCDEF";
    std::string out(6, '0');
    for (int digit = 5; digit >= 0; --digit) {
        out[static_cast<std::size_t>(digit)] = kHex[value & 0xFU];
        value >>= 4;
    }
    return out;
}

// A substream's sf_multiplier, whichever kind it is.
std::optional<int> sf_multiplier_of(const GroupSubstream& s) {
    if (s.chan) {
        return s.chan->sf_multiplier;
    }
    if (s.ajoc) {
        return s.ajoc->sf_multiplier;
    }
    return s.obj ? s.obj->sf_multiplier : std::nullopt;
}

// A content_type()'s language tag's primary subtag: its bytes up to the first
// hyphen.
std::string primary_subtag(const std::vector<std::byte>& tag) {
    std::string out;
    for (const std::byte b : tag) {
        const char c = static_cast<char>(std::to_integer<unsigned char>(b));
        if (c == '-') {
            break;
        }
        out.push_back(c);
    }
    return out;
}

}  // namespace

std::optional<ManifestDescriptor> dash_channel_configuration(const Toc& toc) {
    const std::optional<SignalledChannels> channels = signalled_channels(toc);
    if (!channels) {
        return std::nullopt;
    }
    constexpr std::string_view kDolby = "tag:dolby.com,2015:dash:audio_channel_configuration:2015";
    if (channels->objects) {
        return ManifestDescriptor{.scheme_id_uri = std::string{kDolby}, .value = hex6(0x800000U)};
    }
    for (const CicpRow& row : kCicp) {
        if (row.groups == channels->groups) {
            return ManifestDescriptor{.scheme_id_uri = "urn:mpeg:mpegB:cicp:ChannelConfiguration",
                                      .value = std::to_string(row.value)};
        }
    }
    return ManifestDescriptor{.scheme_id_uri = std::string{kDolby},
                              .value = hex6(channels->groups)};
}

std::vector<ManifestDescriptor> dash_supplemental_properties(const Toc& toc) {
    std::vector<ManifestDescriptor> out;
    // G.3.2's frame rate, reduced: Table E.1's time scale over its
    // sample_delta, 240 000 / 8 008 being 30 000 / 1 001.
    if (const std::optional<MediaTiming> timing = media_timing(toc)) {
        const std::uint32_t divisor = std::gcd(timing->timescale, timing->sample_delta);
        const std::uint32_t num = timing->timescale / divisor;
        const std::uint32_t den = timing->sample_delta / divisor;
        out.push_back(ManifestDescriptor{
            .scheme_id_uri = "tag:dolby.com,2017:dash:audio_frame_rate:2017",
            .value =
                den == 1 ? std::to_string(num) : std::to_string(num) + "/" + std::to_string(den)});
    }
    const std::optional<std::size_t> index = signalled_presentation(toc);
    const bool virtualized =
        index.has_value() &&
        (!toc.presentations_v1.empty() ? toc.presentations_v1[*index].b_pre_virtualized
                                       : toc.presentations_v0[*index].b_pre_virtualized);
    if (virtualized) {
        out.push_back(ManifestDescriptor{
            .scheme_id_uri = "tag:dolby.com,2016:dash:virtualized_content:2016", .value = "1"});
    }
    return out;
}

std::optional<int> presentation_channel_count(const Toc& toc) {
    const std::optional<SignalledChannels> channels = signalled_channels(toc);
    if (!channels || channels->objects) {
        return std::nullopt;
    }
    int count = 0;
    for (std::size_t g = 0; g < kGroupSpeakers.size(); ++g) {
        if (((channels->groups >> g) & 1U) != 0) {
            count += kGroupSpeakers[g];
        }
    }
    return count;
}

std::string_view configuration_difference(const Toc& a, const Toc& b) {
    // Annex H.1.2.4's parameters, in its order.
    if (a.frame_rate_index != b.frame_rate_index) {
        return "frame_rate_index";
    }
    if (a.sample_rate_hz != b.sample_rate_hz) {
        return "fs_index";
    }
    if (a.n_presentations != b.n_presentations ||
        a.presentations_v1.size() != b.presentations_v1.size() ||
        a.presentations_v0.size() != b.presentations_v0.size()) {
        return "n_presentations";
    }
    for (std::size_t i = 0; i < a.presentations_v1.size(); ++i) {
        if (a.presentations_v1[i].presentation_config !=
            b.presentations_v1[i].presentation_config) {
            return "a presentation's b_single_substream_group or presentation_config";
        }
    }
    for (std::size_t i = 0; i < a.presentations_v0.size(); ++i) {
        if (a.presentations_v0[i].presentation_config !=
            b.presentations_v0[i].presentation_config) {
            return "a presentation's b_single_substream_group or presentation_config";
        }
    }
    if (a.substream_groups.size() != b.substream_groups.size()) {
        return "the substream groups";
    }
    for (std::size_t j = 0; j < a.substream_groups.size(); ++j) {
        const SubstreamGroupInfo& ga = a.substream_groups[j];
        const SubstreamGroupInfo& gb = b.substream_groups[j];
        if (ga.content_type.has_value() != gb.content_type.has_value()) {
            return "a substream group's content_type()";
        }
        if (ga.content_type) {
            const ContentType& ca = *ga.content_type;
            const ContentType& cb = *gb.content_type;
            if (ca.content_classifier != cb.content_classifier) {
                return "a substream group's content_classifier";
            }
            const bool language_a = ca.language_tag.has_value() || ca.serialized_language_tag;
            const bool language_b = cb.language_tag.has_value() || cb.serialized_language_tag;
            if (language_a != language_b ||
                ca.serialized_language_tag != cb.serialized_language_tag ||
                (ca.language_tag && cb.language_tag &&
                 primary_subtag(*ca.language_tag) != primary_subtag(*cb.language_tag))) {
                return "a substream group's language";
            }
        }
        if (ga.substreams.size() != gb.substreams.size()) {
            return "a substream group's substreams";
        }
        for (std::size_t k = 0; k < ga.substreams.size(); ++k) {
            const GroupSubstream& sa = ga.substreams[k];
            const GroupSubstream& sb = gb.substreams[k];
            if (sa.kind != sb.kind ||
                (sa.chan && sb.chan && sa.chan->channel_mode != sb.chan->channel_mode)) {
                return "a substream's channel_mode";
            }
            if (sf_multiplier_of(sa) != sf_multiplier_of(sb)) {
                return "a substream's sf_multiplier";
            }
        }
    }
    return {};
}

}  // namespace ac4
