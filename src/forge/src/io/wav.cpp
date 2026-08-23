#include "ac3/io/wav.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <fstream>
#include <ios>
#include <istream>
#include <iterator>
#include <numeric>
#include <optional>
#include <ostream>
#include "ac3/core/tables.hpp"
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ac3::io {

namespace {

// WAVE_FORMAT_EXTENSIBLE dwChannelMask bits (mmreg.h's SPEAKER_*). A WAV
// frame is interleaved in increasing bit order, which is what makes these
// values - rather than A/52's coded order - decide where a channel sits in
// the file. SPEAKER_BACK_CENTER is the standard mono-surround position, so
// acmods 2/1 and 3/1 are as well served by this convention as any other.
constexpr std::uint32_t kFrontLeft = 0x1;
constexpr std::uint32_t kFrontRight = 0x2;
constexpr std::uint32_t kFrontCenter = 0x4;
constexpr std::uint32_t kLowFrequency = 0x8;
constexpr std::uint32_t kBackLeft = 0x10;
constexpr std::uint32_t kBackRight = 0x20;
constexpr std::uint32_t kBackCenter = 0x100;

// Each acmod's full-bandwidth channels, in A/52 Table 5.8 coded order, as the
// speaker each one feeds. Indexed by acmod; 1+1 is deliberately empty because
// it is two programmes rather than a soundfield (see wav_channel_order). The
// LFE is not here: it is coded last whatever the acmod, and is appended by
// the caller with kLowFrequency.
constexpr std::array<std::array<std::uint32_t, 5>, 8> kAcmodSpeakers = {{
    {},                                                              // 1+1
    {kFrontCenter},                                                  // 1/0
    {kFrontLeft, kFrontRight},                                       // 2/0
    {kFrontLeft, kFrontCenter, kFrontRight},                         // 3/0
    {kFrontLeft, kFrontRight, kBackCenter},                          // 2/1
    {kFrontLeft, kFrontCenter, kFrontRight, kBackCenter},            // 3/1
    {kFrontLeft, kFrontRight, kBackLeft, kBackRight},                // 2/2
    {kFrontLeft, kFrontCenter, kFrontRight, kBackLeft, kBackRight},  // 3/2
}};

std::uint16_t read_u16(std::span<const char> data, std::size_t at) {
    return static_cast<std::uint16_t>(static_cast<std::uint8_t>(data[at]) |
                                      (static_cast<std::uint8_t>(data[at + 1]) << 8));
}

std::uint32_t read_u32(std::span<const char> data, std::size_t at) {
    return static_cast<std::uint32_t>(read_u16(data, at)) |
           (static_cast<std::uint32_t>(read_u16(data, at + 2)) << 16);
}

// std::ostream rather than std::ofstream specifically: the same header-
// writing code below now runs against either a real file or an in-memory/
// stdout stream (see the write_wav_f32(std::ostream&, ...) overload).
void put_u16(std::ostream& out, std::uint16_t value) {
    out.write(reinterpret_cast<const char*>(&value), 2);
}

void put_u32(std::ostream& out, std::uint32_t value) {
    out.write(reinterpret_cast<const char*>(&value), 4);
}

std::size_t find_chunk(std::string_view view, std::string_view tag) {
    return view.find(tag);
}

// The parse itself, shared by the path and istream overloads below - both
// read their whole source into memory first (see either overload's own
// comment for why), so this is the one place that actually walks the bytes.
std::expected<WavData, WavError> parse_wav(const std::vector<char>& raw) {
    const std::string_view view{raw.data(), raw.size()};
    if (raw.size() < 44 || view.substr(0, 4) != "RIFF" || view.substr(8, 4) != "WAVE") {
        return std::unexpected(WavError::kNotRiffWave);
    }
    const auto fmt_at = find_chunk(view, "fmt ");
    const auto data_at = find_chunk(view, "data");
    if (fmt_at == std::string_view::npos || data_at == std::string_view::npos) {
        return std::unexpected(WavError::kNotRiffWave);
    }

    const std::span<const char> bytes{raw};
    auto format = read_u16(bytes, fmt_at + 8);
    const auto channel_count = read_u16(bytes, fmt_at + 10);
    const auto sample_rate = read_u32(bytes, fmt_at + 12);
    const auto bits = read_u16(bytes, fmt_at + 22);
    if (format == 0xFFFE) {
        // WAVE_FORMAT_EXTENSIBLE: the real tag is the first two bytes of the
        // SubFormat GUID in the extension.
        format = read_u16(bytes, fmt_at + 32);
    }
    const bool is_float = format == 3 && bits == 32;
    const bool is_pcm16 = format == 1 && bits == 16;
    if (channel_count == 0 || (!is_float && !is_pcm16)) {
        return std::unexpected(WavError::kUnsupportedFormat);
    }

    const auto declared = read_u32(bytes, data_at + 4);
    const std::size_t payload_at = data_at + 8;
    const std::size_t available = raw.size() > payload_at ? raw.size() - payload_at : 0;
    const std::size_t payload = std::min<std::size_t>(declared, available);
    const std::size_t stride = static_cast<std::size_t>(channel_count) * bits / 8;
    if (stride == 0) {
        return std::unexpected(WavError::kUnsupportedFormat);
    }

    WavData result;
    result.sample_rate = sample_rate;
    const std::size_t frames = payload / stride;
    result.channels.assign(channel_count, std::vector<float>(frames));
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::uint16_t ch = 0; ch < channel_count; ++ch) {
            const std::size_t at = payload_at + frame * stride + static_cast<std::size_t>(ch) * bits / 8;
            if (is_float) {
                float value = 0.0f;
                std::memcpy(&value, raw.data() + at, sizeof(value));
                result.channels[ch][frame] = value;
            } else {
                const auto sample = static_cast<std::int16_t>(read_u16(bytes, at));
                result.channels[ch][frame] = static_cast<float>(sample) / 32768.0f;
            }
        }
    }
    return result;
}

}  // namespace

std::string_view describe(WavError error) {
    switch (error) {
        case WavError::kCannotOpen: return "cannot open file";
        case WavError::kNotRiffWave: return "not a RIFF/WAVE file";
        case WavError::kUnsupportedFormat: return "unsupported WAV format (need PCM16 or float32)";
        case WavError::kTruncated: return "truncated WAV data";
    }
    return "unknown error";
}

std::expected<WavData, WavError> read_wav(const std::string& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return std::unexpected(WavError::kCannotOpen);
    }
    return read_wav(in);
}

std::expected<WavData, WavError> read_wav(std::istream& in) {
    if (!in) {
        return std::unexpected(WavError::kCannotOpen);
    }
    const std::vector<char> raw{std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>()};
    return parse_wav(raw);
}

std::optional<Ac3Layout> ac3_layout_for(std::size_t wav_channels) {
    // WAV order per channel count, mapped onto the A/52 Table 5.8 array. Only
    // the counts an acmod can express appear; 5.1 is the interesting one,
    // where every channel but L moves.
    switch (wav_channels) {
        case 1:  // FC                      -> C
            return Ac3Layout{.acmod = Acmod::k1_0, .lfe = false, .wav_index = {0}};
        case 2:  // FL FR                   -> L R
            return Ac3Layout{.acmod = Acmod::k2_0, .lfe = false, .wav_index = {0, 1}};
        case 3:  // FL FR FC                -> L C R
            return Ac3Layout{.acmod = Acmod::k3_0, .lfe = false, .wav_index = {0, 2, 1}};
        case 4:  // FL FR BL BR             -> L R SL SR
            return Ac3Layout{.acmod = Acmod::k2_2, .lfe = false, .wav_index = {0, 1, 2, 3}};
        case 5:  // FL FR FC BL BR          -> L C R SL SR
            return Ac3Layout{.acmod = Acmod::k3_2, .lfe = false, .wav_index = {0, 2, 1, 3, 4}};
        case 6:  // FL FR FC LFE BL BR      -> L C R SL SR LFE
            return Ac3Layout{.acmod = Acmod::k3_2, .lfe = true, .wav_index = {0, 2, 1, 4, 5, 3}};
        default: return std::nullopt;
    }
}

std::vector<std::size_t> wav_channel_order(Acmod acmod, bool lfe) {
    const auto fullbw = static_cast<std::size_t>(fullbw_channel_count(acmod));
    const auto count = fullbw + (lfe ? 1u : 0u);
    std::vector<std::size_t> order(count);

    if (acmod == Acmod::kDualMono) {
        // 1+1 is the one acmod with no speaker positions to sort: it carries
        // two independent programmes (Ch1, Ch2) rather than a soundfield, so
        // there is nothing for dwChannelMask to say about it. The channels go
        // out in the order the codec holds them.
        std::iota(order.begin(), order.end(), std::size_t{0});
        return order;
    }

    // Pair each coded channel with its speaker's mask bit and sort. Sorting is
    // the whole rule: a WAV frame is interleaved in increasing dwChannelMask
    // bit order, so the bit *is* the slot index once the set is known. That
    // also puts the LFE where WAV wants it (bit 3, straight after FC) rather
    // than where A/52 codes it (always last), which is the difference for
    // every acmod below that has an LFE.
    std::vector<std::pair<std::uint32_t, std::size_t>> slots;
    slots.reserve(count);
    const auto& speakers = kAcmodSpeakers[static_cast<std::uint8_t>(acmod)];
    for (std::size_t ch = 0; ch < fullbw; ++ch) {
        slots.emplace_back(speakers[ch], ch);
    }
    if (lfe) {
        slots.emplace_back(kLowFrequency, fullbw);
    }
    std::ranges::sort(slots);
    for (std::size_t slot = 0; slot < count; ++slot) {
        order[slot] = slots[slot].second;
    }
    return order;
}

std::expected<void, WavError> write_wav_f32(const std::string& path,
                                            std::span<const std::vector<float>> channels,
                                            std::uint32_t sample_rate,
                                            std::span<const std::size_t> channel_order) {
    // Checked here too, rather than left solely to the std::ostream& overload
    // below: an empty `channels` must not touch the filesystem at all (no
    // truncated file left behind at `path`), so this has to fail before
    // ofstream's constructor - which opens (and truncates) the file - ever
    // runs.
    if (channels.empty()) {
        return std::unexpected(WavError::kTruncated);
    }
    std::ofstream out{path, std::ios::binary};
    if (!out) {
        return std::unexpected(WavError::kCannotOpen);
    }
    return write_wav_f32(out, channels, sample_rate, channel_order);
}

std::expected<void, WavError> write_wav_f32(std::ostream& out,
                                            std::span<const std::vector<float>> channels,
                                            std::uint32_t sample_rate,
                                            std::span<const std::size_t> channel_order) {
    if (channels.empty()) {
        return std::unexpected(WavError::kTruncated);
    }
    if (!out) {
        return std::unexpected(WavError::kCannotOpen);
    }
    std::vector<std::size_t> order;
    if (channel_order.empty()) {
        order.resize(channels.size());
        for (std::size_t i = 0; i < order.size(); ++i) {
            order[i] = i;
        }
    } else {
        order.assign(channel_order.begin(), channel_order.end());
    }

    const auto count = static_cast<std::uint16_t>(order.size());
    const auto frames = static_cast<std::uint32_t>(channels.front().size());
    const std::uint32_t data_bytes = frames * count * 4;

    // data_bytes is computed above, from `channels` alone, before a single
    // byte goes out - so unlike ac3::io::WavStreamWriter (built for a live
    // capture whose length isn't known until the session ends), this never
    // needs to seek back and patch the header once the truth is known. That
    // makes it exactly as safe on an unseekable stream (a pipe to `-`) as it
    // is on a plain file - see docs/cli/commands.md's "-" convention.
    out.write("RIFF", 4);
    put_u32(out, 36 + data_bytes);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    put_u32(out, 16);
    put_u16(out, 3);  // IEEE float
    put_u16(out, count);
    put_u32(out, sample_rate);
    put_u32(out, sample_rate * count * 4);
    put_u16(out, static_cast<std::uint16_t>(count * 4));
    put_u16(out, 32);
    out.write("data", 4);
    put_u32(out, data_bytes);
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        for (const auto source : order) {
            const float value = channels[source][frame];
            out.write(reinterpret_cast<const char*>(&value), 4);
        }
    }
    if (!out) {
        return std::unexpected(WavError::kCannotOpen);
    }
    return {};
}

std::expected<void, WavError> write_wav_pcm16_raw(const std::string& path,
                                                  std::span<const std::byte> payload,
                                                  std::uint32_t sample_rate,
                                                  std::uint16_t channels) {
    std::ofstream out{path, std::ios::binary};
    if (!out) {
        return std::unexpected(WavError::kCannotOpen);
    }
    const auto data_bytes = static_cast<std::uint32_t>(payload.size());
    const std::uint32_t block_align = static_cast<std::uint32_t>(channels) * 2;

    out.write("RIFF", 4);
    put_u32(out, 36 + data_bytes);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    put_u32(out, 16);
    put_u16(out, 1);  // PCM
    put_u16(out, channels);
    put_u32(out, sample_rate);
    put_u32(out, sample_rate * block_align);
    put_u16(out, static_cast<std::uint16_t>(block_align));
    put_u16(out, 16);
    out.write("data", 4);
    put_u32(out, data_bytes);
    out.write(reinterpret_cast<const char*>(payload.data()),
              static_cast<std::streamsize>(payload.size()));
    return {};
}

}  // namespace ac3::io
