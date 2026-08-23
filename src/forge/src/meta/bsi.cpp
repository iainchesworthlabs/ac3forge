#include "ac3/meta/bsi.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <system_error>

#include "ac3/core/tables.hpp"

namespace ac3::meta {

namespace {

// One token spelling per enumerator, in enumerator order, so a table lookup
// serves both describe() and parse_* without the two being able to disagree
// about which name means which code.
template <typename T, std::size_t N>
bool lookup(std::string_view text, const std::array<std::string_view, N>& names, T& out) {
    for (std::size_t i = 0; i < N; ++i) {
        if (names[i] == text) {
            out = static_cast<T>(i);
            return true;
        }
    }
    return false;
}

constexpr std::array<std::string_view, 3> kSurroundModeTokens = {"none", "off", "on"};
constexpr std::array<std::string_view, 4> kSurroundExModeTokens = {"none", "off", "ex", "pliiz"};
constexpr std::array<std::string_view, 3> kHeadphoneModeTokens = {"none", "off", "on"};
constexpr std::array<std::string_view, 2> kAdConverterTokens = {"standard", "hdcd"};
constexpr std::array<std::string_view, 3> kRoomTypeTokens = {"none", "large", "small"};
constexpr std::array<std::string_view, 8> kBsmodTokens = {
    "cm", "me", "vi", "hi", "dialogue", "commentary", "emergency", "voiceover"};

// A whole non-negative integer, with no leading sign, whitespace or trailing
// text - from_chars alone accepts a partial parse, which would let "12abc"
// through as 12.
bool parse_int(std::string_view text, int& out) {
    if (text.empty()) {
        return false;
    }
    int value = 0;
    const auto* const end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, value);
    if (result.ec != std::errc{} || result.ptr != end || value < 0) {
        return false;
    }
    out = value;
    return true;
}

}  // namespace

bool valid_bsi_info(const BsiInfo& value) {
    const auto production_ok = [](const std::optional<AudioProduction>& production) {
        return !production || (production->mixlevel >= 0 && production->mixlevel <= 31);
    };
    if (!production_ok(value.audprod) || !production_ok(value.audprod2)) {
        return false;
    }
    if (value.timecod1) {
        const auto& t = *value.timecod1;
        if (t.hours < 0 || t.hours > 23 || t.minutes < 0 || t.minutes > 59 ||
            t.eight_seconds < 0 || t.eight_seconds > 7) {
            return false;
        }
    }
    if (value.timecod2) {
        const auto& t = *value.timecod2;
        if (t.seconds < 0 || t.seconds > 7 || t.frames < 0 || t.frames > 29 ||
            t.sixty_fourths < 0 || t.sixty_fourths > 63) {
            return false;
        }
    }
    return true;
}

bool valid_alternate_bsi(const AlternateBsi& value) {
    // xbsi1's five fields are all enumerators of exactly their field's width,
    // and xbsi2's are too - bar the surround levels, whose reserved codes
    // valid_surround_mix_level() already refuses on the E-AC-3 path and which
    // Tables D2.4/D2.6 reserve identically here.
    return !value.mix || (valid_surround_mix_level(value.mix->ltrtsurmixlev) &&
                          valid_surround_mix_level(value.mix->lorosurmixlev));
}

std::string_view describe(BitstreamMode value, Acmod acmod) {
    switch (value) {
        case BitstreamMode::kCompleteMain:
            return "main audio service: complete main (CM)";
        case BitstreamMode::kMusicAndEffects:
            return "main audio service: music and effects (ME)";
        case BitstreamMode::kVisuallyImpaired:
            return "associated service: visually impaired (VI)";
        case BitstreamMode::kHearingImpaired:
            return "associated service: hearing impaired (HI)";
        case BitstreamMode::kDialogue:
            return "associated service: dialogue (D)";
        case BitstreamMode::kCommentary:
            return "associated service: commentary (C)";
        case BitstreamMode::kEmergency:
            return "associated service: emergency (E)";
        case BitstreamMode::kVoiceOverOrKaraoke:
            // Table 5.7's one row where the same code means two services and
            // acmod is the only thing telling them apart.
            return acmod == Acmod::k1_0 ? "associated service: voice over (VO)"
                                        : "main audio service: karaoke";
    }
    return "main audio service: complete main (CM)";
}

std::string_view describe(SurroundMode value) {
    switch (value) {
        case SurroundMode::kNotIndicated:
            return "not indicated";
        case SurroundMode::kNotDolbySurround:
            return "not Dolby Surround encoded";
        case SurroundMode::kDolbySurround:
            return "Dolby Surround encoded";
    }
    return "not indicated";
}

std::string_view describe(SurroundExMode value) {
    switch (value) {
        case SurroundExMode::kNotIndicated:
            return "not indicated";
        case SurroundExMode::kNotSurroundEx:
            return "not Surround EX / Pro Logic IIx / IIz encoded";
        case SurroundExMode::kSurroundEx:
            return "Dolby Surround EX or Pro Logic IIx encoded";
        case SurroundExMode::kProLogicIIz:
            return "Dolby Pro Logic IIz encoded";
    }
    return "not indicated";
}

std::string_view describe(HeadphoneMode value) {
    switch (value) {
        case HeadphoneMode::kNotIndicated:
            return "not indicated";
        case HeadphoneMode::kNotDolbyHeadphone:
            return "not Dolby Headphone encoded";
        case HeadphoneMode::kDolbyHeadphone:
            return "Dolby Headphone encoded";
    }
    return "not indicated";
}

std::string_view describe(AdConverterType value) {
    switch (value) {
        case AdConverterType::kStandard:
            return "standard";
        case AdConverterType::kHdcd:
            return "HDCD";
    }
    return "standard";
}

std::string_view describe(RoomType value) {
    switch (value) {
        case RoomType::kNotIndicated:
            return "not indicated";
        case RoomType::kLargeRoomXCurve:
            return "large room, X curve monitor";
        case RoomType::kSmallRoomFlat:
            return "small room, flat monitor";
    }
    return "not indicated";
}

bool parse_bsmod(std::string_view text, BitstreamMode& out) {
    if (lookup(text, kBsmodTokens, out)) {
        return true;
    }
    // Table 5.7 is one of the few places where the raw code is what a
    // broadcast spec quotes, so the number is a first-class spelling here
    // rather than a fallback nobody documents.
    int code = 0;
    if (!parse_int(text, code) || code > 7) {
        return false;
    }
    out = static_cast<BitstreamMode>(code);
    return true;
}

bool parse_surround_mode(std::string_view text, SurroundMode& out) {
    return lookup(text, kSurroundModeTokens, out);
}

bool parse_surround_ex_mode(std::string_view text, SurroundExMode& out) {
    return lookup(text, kSurroundExModeTokens, out);
}

bool parse_headphone_mode(std::string_view text, HeadphoneMode& out) {
    return lookup(text, kHeadphoneModeTokens, out);
}

bool parse_ad_converter(std::string_view text, AdConverterType& out) {
    return lookup(text, kAdConverterTokens, out);
}

bool parse_room_type(std::string_view text, RoomType& out) {
    return lookup(text, kRoomTypeTokens, out);
}

bool parse_timecode(std::string_view text, TimeCodeCoarse& coarse, TimeCodeFine& fine) {
    // HH:MM:SS[:FF[.N]]. Split on ':' first, then peel the optional '.N' off
    // the frame field - the fraction rides the frame rather than being a
    // fifth colon-separated part, because it is a fraction OF that frame.
    std::array<std::string_view, 4> parts{};
    std::size_t count = 0;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto colon = text.find(':', start);
        const auto piece = text.substr(start, colon == std::string_view::npos
                                                  ? std::string_view::npos
                                                  : colon - start);
        if (count == parts.size()) {
            return false;
        }
        parts[count++] = piece;
        if (colon == std::string_view::npos) {
            break;
        }
        start = colon + 1;
    }
    if (count < 3) {
        return false;
    }

    int hours = 0;
    int minutes = 0;
    int seconds = 0;
    if (!parse_int(parts[0], hours) || !parse_int(parts[1], minutes) ||
        !parse_int(parts[2], seconds)) {
        return false;
    }
    if (hours > 23 || minutes > 59 || seconds > 59) {
        return false;
    }

    int frames = 0;
    int sixty_fourths = 0;
    if (count == 4) {
        auto frame_text = parts[3];
        const auto dot = frame_text.find('.');
        if (dot != std::string_view::npos) {
            if (!parse_int(frame_text.substr(dot + 1), sixty_fourths) || sixty_fourths > 63) {
                return false;
            }
            frame_text = frame_text.substr(0, dot);
        }
        if (!parse_int(frame_text, frames) || frames > 29) {
            return false;
        }
    }

    // §5.4.2.27/28: the split is at 8 seconds, the coarse half counting whole
    // buckets and the fine half the remainder within one.
    coarse = {.hours = hours, .minutes = minutes, .eight_seconds = seconds / 8};
    fine = {.seconds = seconds % 8, .frames = frames, .sixty_fourths = sixty_fourths};
    return true;
}

std::string format_timecode(const TimeCodeCoarse& coarse, const TimeCodeFine& fine) {
    return std::format("{:02}:{:02}:{:02}:{:02}.{}", coarse.hours, coarse.minutes,
                       coarse.eight_seconds * 8 + fine.seconds, fine.frames, fine.sixty_fourths);
}

}  // namespace ac3::meta
