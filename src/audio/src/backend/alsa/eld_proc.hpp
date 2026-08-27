#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ac3/audio/sink_capabilities.hpp"

// Parsing the text ALSA's HD-audio driver exposes at
// /proc/asound/card<N>/eld#<dev>.<port> for an HDMI/DisplayPort output -
// monitor_present, eld_valid, sad_count, and one block per Short Audio
// Descriptor (sad<i>_coding_type/_channels/_rates/_bits/_max_bitrate). The
// kernel has already decoded the raw CEA-861 bytes into these fields, so
// there is no byte layout to get wrong here - only text to read.
//
// Header-only and proc-file-content-in, struct-out, in the same spirit as
// device_names.hpp: it can be tested with a synthesized fixture on a machine
// with no ALSA, no sound card and no /proc/asound at all - see
// tests/backend/alsa/test_alsa_eld_parsing.cpp. sink_capabilities.cpp is the
// (untestable-here) other half: finding which file to read.

namespace ac3::alsa {

namespace detail {

// The value inside a "[0x...]" prefix, e.g. "[0x1] LPCM" -> 1. Used for the
// coding type rather than the trailing name ("LPCM", "AC-3", ...) because the
// numeric code is the actual CEA-861 value and does not depend on exactly how
// a given kernel driver spells the name.
[[nodiscard]] inline std::optional<unsigned> hex_in_brackets(std::string_view value) {
    const auto open = value.find("[0x");
    if (open == std::string_view::npos) {
        return std::nullopt;
    }
    const auto digits_start = open + 3;
    auto pos = digits_start;
    while (pos < value.size() && std::isxdigit(static_cast<unsigned char>(value[pos]))) {
        ++pos;
    }
    if (pos == digits_start) {
        return std::nullopt;
    }
    unsigned result = 0;
    for (auto i = digits_start; i < pos; ++i) {
        const char c = value[i];
        const unsigned digit = c <= '9' ? static_cast<unsigned>(c - '0')
                                        : static_cast<unsigned>((c | 0x20) - 'a') + 10;
        result = result * 16 + digit;
    }
    return result;
}

// Every decimal integer substring in `value`, in order.
[[nodiscard]] inline std::vector<std::uint32_t> decimal_numbers(std::string_view value) {
    std::vector<std::uint32_t> numbers;
    std::size_t i = 0;
    while (i < value.size()) {
        if (std::isdigit(static_cast<unsigned char>(value[i])) == 0) {
            ++i;
            continue;
        }
        std::size_t j = i;
        std::uint64_t n = 0;
        while (j < value.size() && std::isdigit(static_cast<unsigned char>(value[j])) != 0) {
            n = n * 10 + static_cast<std::uint64_t>(value[j] - '0');
            ++j;
        }
        numbers.push_back(static_cast<std::uint32_t>(n));
        i = j;
    }
    return numbers;
}

// True when `value`'s first decimal number is non-zero - used for the
// bare-integer fields (monitor_present, eld_valid).
[[nodiscard]] inline bool truthy(std::string_view value) {
    const auto numbers = decimal_numbers(value);
    return !numbers.empty() && numbers.front() != 0;
}

// Decimal numbers appearing AFTER a leading "[0x..]" bitmap prefix, e.g.
// "[0x1ee0] 32000 44100 48000" -> {32000, 44100, 48000}. Used for the
// trailing rate list rather than decimal_numbers() alone, which would
// otherwise misread the hex digits inside the brackets themselves (that
// "[0x1ee0]" prefix contains a literal '1' and a literal '0') as spurious
// values of their own - read from the already-expanded decimal text, not the
// bitmap, so a mistake in this file cannot invert a bit and silently claim a
// rate the sink never advertised.
[[nodiscard]] inline std::vector<std::uint32_t> decimal_numbers_after_bracket(
    std::string_view value) {
    const auto close = value.find(']');
    return decimal_numbers(close == std::string_view::npos ? value : value.substr(close + 1));
}

// CEA-861 Table 34's coding-type nibble, collapsed to what this project acts
// on. 1 = LPCM, 2 = AC-3, 10 (0xA) = Dolby Digital Plus (E-AC-3); everything
// else (DTS, AAC, MAT/TrueHD, ...) is not a format this codebase produces or
// consumes, so it is tracked only as "not one of the three" below.
enum class CodingType : std::uint8_t { kLpcm, kAc3, kEac3, kOther };

[[nodiscard]] inline CodingType coding_type_from_code(unsigned code) {
    switch (code) {
        case 1: return CodingType::kLpcm;
        case 2: return CodingType::kAc3;
        case 10: return CodingType::kEac3;
        default: return CodingType::kOther;
    }
}

// The SAD index a "sad<N>_<field>" key names, and the field name past it -
// e.g. "sad12_channels" -> (12, "channels"). Neither part of a pair when
// `key` does not start with "sad" followed by at least one digit.
[[nodiscard]] inline std::optional<std::pair<std::size_t, std::string_view>> split_sad_key(
    std::string_view key) {
    if (!key.starts_with("sad")) {
        return std::nullopt;
    }
    std::size_t i = 3;
    while (i < key.size() && std::isdigit(static_cast<unsigned char>(key[i])) != 0) {
        ++i;
    }
    if (i == 3 || i >= key.size() || key[i] != '_') {
        return std::nullopt;
    }
    std::size_t index = 0;
    for (std::size_t d = 3; d < i; ++d) {
        index = index * 10 + static_cast<std::size_t>(key[d] - '0');
    }
    return std::pair{index, key.substr(i + 1)};
}

}  // namespace detail

// One line's key ("sad0_coding_type") and the rest of the line as-is, split
// on the first run of whitespace. `line` is a single line, no trailing '\n'.
[[nodiscard]] inline std::pair<std::string_view, std::string_view> split_eld_line(
    std::string_view line) {
    const auto key_end = line.find_first_of(" \t");
    if (key_end == std::string_view::npos) {
        return {line, {}};
    }
    const auto value_start = line.find_first_not_of(" \t", key_end);
    if (value_start == std::string_view::npos) {
        return {line.substr(0, key_end), {}};
    }
    return {line.substr(0, key_end), line.substr(value_start)};
}

[[nodiscard]] inline std::expected<ac3::audio::SinkAudioCapabilities, ac3::audio::EdidError>
parse_eld_proc_text(std::string_view contents) {
    using ac3::audio::EdidError;
    using ac3::audio::SinkAudioCapabilities;

    bool saw_monitor_present = false;
    bool saw_eld_valid = false;
    bool monitor_present = true;
    bool eld_valid = true;
    SinkAudioCapabilities caps;
    // Which coding type each SAD index turned out to be, filled in as each
    // index's own "sad<i>_coding_type" line is seen. channels/rates are only
    // folded into `caps` for an index once it is known to be LPCM - an AC-3
    // or E-AC-3 SAD has its own _channels/_rates fields too (different units:
    // a bitmask of legal bitrates rather than bit depths), and merging those
    // in as if they were LPCM's would misreport what plain PCM can do.
    std::vector<detail::CodingType> sad_types;

    const auto note_type = [&](std::size_t index, detail::CodingType type) {
        if (index >= sad_types.size()) {
            sad_types.resize(index + 1, detail::CodingType::kOther);
        }
        sad_types[index] = type;
    };

    std::size_t pos = 0;
    while (pos <= contents.size()) {
        const auto eol = contents.find('\n', pos);
        const auto line = contents.substr(
            pos, eol == std::string_view::npos ? std::string_view::npos : eol - pos);
        pos = eol == std::string_view::npos ? contents.size() + 1 : eol + 1;
        if (line.empty()) {
            continue;
        }
        const auto [key, value] = split_eld_line(line);

        if (key == "monitor_present") {
            saw_monitor_present = true;
            monitor_present = detail::truthy(value);
            continue;
        }
        if (key == "eld_valid") {
            saw_eld_valid = true;
            eld_valid = detail::truthy(value);
            continue;
        }
        const auto sad = detail::split_sad_key(key);
        if (!sad) {
            continue;
        }
        const auto [index, field] = *sad;

        if (field == "coding_type") {
            const auto code = detail::hex_in_brackets(value);
            if (!code) {
                continue;
            }
            const auto type = detail::coding_type_from_code(*code);
            note_type(index, type);
            switch (type) {
                case detail::CodingType::kLpcm: caps.pcm = true; break;
                case detail::CodingType::kAc3: caps.ac3 = true; break;
                case detail::CodingType::kEac3: caps.eac3 = true; break;
                case detail::CodingType::kOther: break;
            }
            continue;
        }
        const bool is_lpcm =
            index < sad_types.size() && sad_types[index] == detail::CodingType::kLpcm;
        if (!is_lpcm) {
            continue;
        }
        if (field == "channels") {
            const auto numbers = detail::decimal_numbers(value);
            if (!numbers.empty() && numbers.front() <= 0xffff) {
                caps.max_pcm_channels = static_cast<std::uint16_t>(
                    std::max<std::uint32_t>(caps.max_pcm_channels, numbers.front()));
            }
        } else if (field == "rates") {
            for (const auto rate : detail::decimal_numbers_after_bracket(value)) {
                if (std::find(caps.pcm_sample_rates_hz.begin(), caps.pcm_sample_rates_hz.end(),
                              rate) == caps.pcm_sample_rates_hz.end()) {
                    caps.pcm_sample_rates_hz.push_back(rate);
                }
            }
        }
    }

    if (!saw_monitor_present && !saw_eld_valid) {
        return std::unexpected(EdidError::kParseFailed);
    }
    if (!monitor_present || !eld_valid) {
        return std::unexpected(EdidError::kNoEdid);
    }
    std::sort(caps.pcm_sample_rates_hz.begin(), caps.pcm_sample_rates_hz.end());
    return caps;
}

}  // namespace ac3::alsa
