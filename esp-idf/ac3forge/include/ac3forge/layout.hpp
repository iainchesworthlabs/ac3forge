#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/decoder/output.hpp"
#include "ac3/spatial/spatial.hpp"

// The speakers a player has, one per output slot.
//
// A player is configured for the room it is in, not for the stream it is sent:
// the stream says what was coded, this says where it should come out, and
// ac3forge/render.hpp turns one into the other a block at a time. Free of
// ESP-IDF, like interleave.hpp beside it and for the same reason - this is the
// part that can be tested on the host (tests/io/test_layout.cpp), and the part
// where a wrong index puts the centre channel in a subwoofer.
//
// Two ways to say it, both in one string, because planning/esp32-player.md's
// decision 7 wants names for the installations that have one and a list for
// the ones that do not:
//
//   A NAME, "F.L.H": F full-bandwidth speakers on the listener's ring, L
//   low-frequency feeds, H height speakers. "2.0", "5.1", "7.1", "5.1.2",
//   "5.1.4", "7.1.4", "9.1.4", "9.2.4", "5.0.4" and so on. F is 1 (C), 2
//   (L R), 3 (L C R), 4 (L R Ls Rs), 5 (L C R Ls Rs), 7 (5 plus Lrs Rrs) or 9
//   (7 plus Lw Rw); L is 0, 1 (LFE) or 2 (LFE and LFE2); H is 0, 2 (Vhl Vhr),
//   4 (Vhl Vhr Lts Rts) or 6 (Vhl Vhr Vhc Lts Rts Ts). The slots come out in
//   that order - ring, heights, LFE - which for a name is Table E2.5's own
//   order with the LFE-type locations last: "5.1" is L C R Ls Rs LFE, the
//   AC-3 order, NOT the L R C LFE Ls Rs a WAV file uses. A DAC is wired to
//   slots, so a board that wants the other order writes a list.
//
//   A LIST, comma-separated, one token per slot in slot order:
//     a Table E2.5 location name    L C R Ls Rs Lc Rc Lrs Rrs Cs Ts Lsd Rsd
//                                   Lw Rw Vhl Vhr Vhc Lts Rts LFE LFE2
//                                   (case-insensitive)
//     azimuth/elevation in degrees  "30/0", "-110/0", "45/45"; azimuth is
//                                   counterclockwise from the front, so left
//                                   is positive (ITU-R BS.775, and
//                                   ac3::spatial's convention); elevation is
//                                   above the listener's plane
//     lfe                           a low-frequency feed
//     -                             a slot the bus has and no speaker is on;
//                                   written as silence
//   "L,R,C,LFE,Ls,Rs" is a 5.1 DAC wired in WAV order; "30/0,-30/0,lfe" is
//   2.1 by angles. A location token remembers its location, so a coded
//   channel at that location reaches the slot exactly rather than through
//   the panner; an angle token is placed by geometry alone.
//
//   A location or angle token may carry one or more ':'-separated suffixes,
//   in any order:
//     :small                       this full-bandwidth speaker cannot
//                                   reproduce the bottom two octaves;
//                                   LayoutRenderer redirects its bass to the
//                                   LFE feed instead (see render.hpp). Only
//                                   valid on a speaker, and only when the
//                                   layout has an LFE feed to send the bass
//                                   to - there is nowhere else for it to go.
//     :height, :top, :upfiring      which physical thing realizes a height
//                                   position - Vhl, Vhr, Vhc, Lts or Rts
//                                   only, the height channels a Dolby-style
//                                   5.1.x/7.1.x installation actually has.
//                                   ":top" is a true in-ceiling speaker and
//                                   moves this slot's elevation to 90
//                                   degrees, ITU-R BS.2051's Top tier,
//                                   overriding the location's own nominal
//                                   ~45-degree Upper-tier angle. ":height"
//                                   (a wall-mounted, angled speaker) and
//                                   ":upfiring" (a Dolby "Atmos-enabled"
//                                   module bouncing sound off the ceiling)
//                                   both leave the angle exactly where it
//                                   is: BS.2051 has one Upper tier, not a
//                                   separate one for each way of physically
//                                   reaching it, so today the two render
//                                   identically and exist as labels a
//                                   configuration and /status can carry
//                                   truthfully. An angle token accepts these
//                                   too, purely as a label - the degrees you
//                                   typed always win over what a suffix
//                                   would otherwise imply.
//   "L:small,R:small,C,LFE,Ls,Rs" is a 5.1 DAC with small fronts;
//   "Vhl:top,Vhr:top,Lts,Rts,L,C,R,Ls,Rs,LFE" is a 7.1.4 room with in-ceiling
//   front heights and wall-mounted rears.
//
// The NAME form takes the same three realization suffixes too, applied to
// every height slot the name expands to: "7.1.4:top" is a 7.1.4 room where
// all four heights are in-ceiling. There is no per-slot control this way -
// that needs the list form - but it is the one-token spelling for the
// ordinary case of one installation, one realization.
//
// Where a named location sits is ac3::spatial::direction_of's answer, which
// depends on the company it keeps: Ls and Rs are at +-110 degrees on a 5.1
// ring and move to +-90 when a layout also has rear surrounds (Lrs Rrs), the
// way ITU-R BS.2051 lays 7.1 out. That is why the directions are resolved
// once, over the whole layout, rather than per slot as the tokens arrive.

namespace ac3forge {

struct Speaker {
    enum class Kind : std::uint8_t {
        kEmpty,    // a slot nothing is connected to; written as zeros
        kSpeaker,  // a full-bandwidth speaker at `direction`
        kLfe,      // a low-frequency feed; the bed's LFE, never panned audio
    };
    // What physically realizes a height-tier speaker - see the header
    // comment's ":height"/":top"/":upfiring" suffixes. Meaningless, and left
    // at kDefault, on anything that isn't one of the five Dolby height
    // locations (Vhl, Vhr, Vhc, Lts, Rts).
    enum class Realization : std::uint8_t {
        kDefault,   // this location's own nominal angle, untouched
        kHeight,    // wall-mounted, angled - numerically the same as kDefault
        kTop,       // true in-ceiling: elevation forced to 90 degrees
        kUpFiring,  // Dolby "Atmos-enabled" module - numerically the same as
                    // kHeight; BS.2051 draws no separate tier for it
    };
    Kind kind = Kind::kEmpty;
    // A full-bandwidth speaker whose bass LayoutRenderer should redirect to
    // the LFE feed rather than send here. Only meaningful on kSpeaker, and
    // only valid when the layout has an LFE feed - see OutputLayout::listed().
    bool small = false;
    Realization realization = Realization::kDefault;
    // Deliberately placed right after `kind`, ahead of `direction`: `kind`
    // alone leaves alignment padding before `direction` (whose Direction
    // holds two doubles) on any ABI, so these two one-byte fields land in
    // padding that already existed rather than growing Speaker. Putting them
    // after `location` instead measured as free on x86-64 MSVC (padding
    // Direction's own eight-byte alignment already left at the end of the
    // struct), but that was this one ABI's padding, not a portable
    // guarantee - a narrower alignment for double elsewhere could leave
    // none there. Growing Speaker is exactly what boot-looped the ESP32
    // example once already, kMaxSlots copies of it held by value on a tight
    // FreeRTOS stack - see kTextBytes's own comment for that incident.
    ac3::spatial::Direction direction{};
    // The Table E2.5 location this slot was named by, when it was. A coded
    // channel of the same location goes to this slot with unit gain; a slot
    // placed by angle alone has none and takes what the panner gives it.
    std::optional<ac3::eac3::chanmap::Location> location = std::nullopt;
};

class OutputLayout {
   public:
    using Location = ac3::eac3::chanmap::Location;

    // Sixteen is §E3.8.2's cap on a rendered programme, the most one TDM line
    // carries at 32 bits, and the panner's own ring limit.
    static constexpr std::size_t kMaxSlots = 16;
    // The text a layout keeps of itself, for logs and /status. Longer input is
    // still parsed - parsing reads the caller's string directly, never this
    // buffer - only the echo is cut, gracefully, at whatever this holds.
    // Left at 96 deliberately even though ":small"/":top"/":height"/
    // ":upfiring" suffixes can make a fully spelled-out list longer than
    // that: raising it to 224 to fit a worst-case sixteen-slot list once
    // measured on the board (QEMU, 2026-09-12) - PlayerConfig holds an
    // OutputLayout by value on the ESP-IDF example's main task, whose stack
    // is tight enough that the extra 128 bytes boot-looped it with a stack
    // overflow before a single request was served. A realistic decorated
    // list - a handful of small/re-tiered slots, not all sixteen at once -
    // fits well inside 96 regardless (a 7.1.4 room with two small fronts and
    // two in-ceiling heights is under 50 characters); only a pathological
    // list that names and decorates every slot loses its tail in the echo,
    // which is a truncation, not a defect - text() is a report, not the
    // configuration itself, which OutputLayout has already parsed in full
    // by the time anything reads it back.
    static constexpr std::size_t kTextBytes = 96;

    OutputLayout() = default;

    // "2.0": what every player before this one played.
    [[nodiscard]] static OutputLayout stereo() { return *named("2.0"); }

    // A name or a list, as the header describes. std::nullopt for anything
    // else, including an empty string, a name with a count this cannot place,
    // a list with more than kMaxSlots entries or a token it does not know.
    [[nodiscard]] static std::optional<OutputLayout> parse(std::string_view text) {
        const std::string_view trimmed = trim(text);
        if (trimmed.empty()) {
            return std::nullopt;
        }
        if (auto by_name = named(trimmed)) {
            return by_name;
        }
        return listed(trimmed);
    }

    // The name form only.
    [[nodiscard]] static std::optional<OutputLayout> named(std::string_view name) {
        const std::string_view original = trim(name);
        std::string_view trimmed = original;
        // An optional trailing modifier applied to every height slot the
        // name expands to - "7.1.4:top" - see the header comment.
        Speaker::Realization realization = Speaker::Realization::kDefault;
        if (const std::size_t colon = trimmed.rfind(':'); colon != std::string_view::npos) {
            const auto found = realization_named(trim(trimmed.substr(colon + 1)));
            if (!found) {
                return std::nullopt;  // an unrecognised trailing token
            }
            realization = *found;
            trimmed = trim(trimmed.substr(0, colon));
        }
        // F.L or F.L.H, every field a single digit.
        std::array<int, 3> fields = {-1, -1, 0};
        std::size_t field = 0;
        for (const char c : trimmed) {
            if (c >= '0' && c <= '9') {
                if (field >= fields.size() || fields[field] >= 0) {
                    return std::nullopt;  // two digits in one field
                }
                fields[field] = c - '0';
            } else if (c == '.') {
                if (field >= fields.size() || fields[field] < 0) {
                    return std::nullopt;
                }
                ++field;
                if (field < fields.size()) {
                    fields[field] = -1;
                }
            } else {
                return std::nullopt;
            }
        }
        if (field < 1 || field > 2 || fields[0] < 0 || fields[1] < 0 || fields[2] < 0) {
            return std::nullopt;
        }
        std::array<Location, kMaxSlots> locations{};
        std::size_t count = 0;
        const auto add = [&](std::initializer_list<Location> more) {
            for (const Location location : more) {
                if (count < locations.size()) {
                    locations[count++] = location;
                }
            }
        };
        switch (fields[0]) {
            case 1: add({Location::kCentre}); break;
            case 2: add({Location::kLeft, Location::kRight}); break;
            case 3: add({Location::kLeft, Location::kCentre, Location::kRight}); break;
            case 4:
                add({Location::kLeft, Location::kRight, Location::kLeftSurround,
                     Location::kRightSurround});
                break;
            case 5:
                add({Location::kLeft, Location::kCentre, Location::kRight, Location::kLeftSurround,
                     Location::kRightSurround});
                break;
            case 7:
                add({Location::kLeft, Location::kCentre, Location::kRight, Location::kLeftSurround,
                     Location::kRightSurround, Location::kLrs, Location::kRrs});
                break;
            case 9:
                add({Location::kLeft, Location::kCentre, Location::kRight, Location::kLeftSurround,
                     Location::kRightSurround, Location::kLrs, Location::kRrs, Location::kLw,
                     Location::kRw});
                break;
            default: return std::nullopt;
        }
        switch (fields[2]) {
            case 0: break;
            case 2: add({Location::kVhl, Location::kVhr}); break;
            case 4: add({Location::kVhl, Location::kVhr, Location::kLts, Location::kRts}); break;
            case 6:
                add({Location::kVhl, Location::kVhr, Location::kVhc, Location::kLts, Location::kRts,
                     Location::kTs});
                break;
            default: return std::nullopt;
        }
        switch (fields[1]) {
            case 0: break;
            case 1: add({Location::kLfe}); break;
            case 2: add({Location::kLfe, Location::kLfe2}); break;
            default: return std::nullopt;
        }
        auto out = from_locations(std::span<const Location>(locations.data(), count));
        if (!out) {
            return std::nullopt;
        }
        if (realization != Speaker::Realization::kDefault) {
            bool touched_any = false;
            for (std::size_t i = 0; i < out->count_; ++i) {
                Speaker& speaker = out->speakers_[i];
                if (speaker.location.has_value() && is_realizable_height(*speaker.location)) {
                    speaker.realization = realization;
                    touched_any = true;
                }
            }
            if (!touched_any) {
                return std::nullopt;  // e.g. "5.1:top" - no height slots to realize
            }
            out->resolve_directions();  // re-applies elevation with the realization set
        }
        out->set_text(original);
        return out;
    }

    // Slots in the order given, one per location. std::nullopt for more than
    // kMaxSlots or a location repeated.
    [[nodiscard]] static std::optional<OutputLayout> from_locations(
        std::span<const Location> locations) {
        if (locations.empty() || locations.size() > kMaxSlots) {
            return std::nullopt;
        }
        OutputLayout out;
        for (const Location location : locations) {
            if (out.index_of(location) >= 0) {
                return std::nullopt;
            }
            Speaker& speaker = out.speakers_[out.count_++];
            speaker.location = location;
            speaker.kind = is_lfe(location) ? Speaker::Kind::kLfe : Speaker::Kind::kSpeaker;
        }
        out.resolve_directions();
        out.set_text_from_slots();
        return out;
    }

    [[nodiscard]] std::size_t slots() const { return count_; }
    [[nodiscard]] const Speaker& slot(std::size_t index) const { return speakers_[index]; }
    [[nodiscard]] std::span<const Speaker> speakers() const {
        return std::span<const Speaker>(speakers_.data(), count_);
    }

    [[nodiscard]] std::size_t speaker_count() const { return count(Speaker::Kind::kSpeaker); }
    [[nodiscard]] std::size_t lfe_count() const { return count(Speaker::Kind::kLfe); }

    // Any speaker in the upper layer, by the panner's own threshold - the
    // case where the bed cannot serve and objects are worth reconstructing.
    [[nodiscard]] bool has_height() const {
        for (const Speaker& speaker : speakers()) {
            if (speaker.kind == Speaker::Kind::kSpeaker &&
                speaker.direction.elevation_deg >= ac3::spatial::kHeightThresholdDeg) {
                return true;
            }
        }
        return false;
    }

    // Any speaker marked ":small" - see LayoutRenderer's bass management.
    [[nodiscard]] bool has_small() const {
        for (const Speaker& speaker : speakers()) {
            if (speaker.small) {
                return true;
            }
        }
        return false;
    }

    // Which slot a location is on, or -1.
    [[nodiscard]] int index_of(Location location) const {
        for (std::size_t i = 0; i < count_; ++i) {
            if (speakers_[i].location == location) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    // When the decoder's own §7.8 output stage serves this layout: two
    // full-bandwidth speakers and nothing else is `stereo` (kLoRo or kLtRt,
    // the caller's choice), one is kMono. Anything wider, or anything with an
    // LFE or a height, is rendered as coded through ac3forge/render.hpp,
    // because §7.8 has no fold that keeps an LFE or places a height.
    [[nodiscard]] std::optional<ac3::DownmixTarget> fold(ac3::DownmixTarget stereo) const {
        if (lfe_count() != 0 || has_height()) {
            return std::nullopt;
        }
        switch (speaker_count()) {
            case 1: return ac3::DownmixTarget::kMono;
            case 2: return stereo;
            default: return std::nullopt;
        }
    }

    // The text this was parsed from, or the list form of what it holds.
    [[nodiscard]] std::string_view text() const { return std::string_view{text_.data()}; }

    // The slots something is connected to - a speaker or a low-frequency
    // feed, not a "-" - with bit n for slot n.
    [[nodiscard]] std::uint16_t connected_slots() const {
        std::uint16_t mask = 0;
        for (std::size_t i = 0; i < count_; ++i) {
            if (speakers_[i].kind != Speaker::Kind::kEmpty) {
                mask = static_cast<std::uint16_t>(mask | (1U << i));
            }
        }
        return mask;
    }

    // A slot's name as a speaker list writes it: its location ("Lrs"), "lfe"
    // for a low-frequency feed with none, azimuth/elevation ("30/0") for a
    // speaker placed by angle, "-" for an empty slot. Into `out`,
    // NUL-terminated and cut to fit; returns the length written.
    std::size_t slot_name(std::size_t index, std::span<char> out) const {
        if (out.empty()) {
            return 0;
        }
        const Speaker& speaker = speakers_[index];
        std::array<char, 32> angle{};
        std::string_view name = "-";
        if (speaker.location.has_value()) {
            name = ac3::eac3::chanmap::name(*speaker.location);
        } else if (speaker.kind == Speaker::Kind::kLfe) {
            name = "lfe";
        } else if (speaker.kind == Speaker::Kind::kSpeaker) {
            const int written = std::snprintf(angle.data(), angle.size(), "%g/%g",
                                              speaker.direction.azimuth_deg,
                                              speaker.direction.elevation_deg);
            name = std::string_view{angle.data(), written > 0 ? static_cast<std::size_t>(written) : 0U};
        }
        const std::size_t n = name.size() < out.size() - 1 ? name.size() : out.size() - 1;
        for (std::size_t i = 0; i < n; ++i) {
            out[i] = name[i];
        }
        out[n] = '\0';
        return n;
    }

    // The names of the slots in `slots` (bit n for slot n), comma-separated,
    // into `out`, NUL-terminated. A name that would not fit whole is left out,
    // with every one after it.
    void names_of(std::uint16_t slots, std::span<char> out) const {
        if (out.empty()) {
            return;
        }
        std::size_t used = 0;
        out[0] = '\0';
        for (std::size_t i = 0; i < count_; ++i) {
            if ((slots & (1U << i)) == 0) {
                continue;
            }
            std::array<char, 32> name{};
            const std::size_t n = slot_name(i, name);
            if (used + n + (used > 0 ? 1 : 0) + 1 > out.size()) {
                break;
            }
            if (used > 0) {
                out[used++] = ',';
            }
            for (std::size_t k = 0; k < n; ++k) {
                out[used++] = name[k];
            }
            out[used] = '\0';
        }
    }

   private:
    static bool is_lfe(Location location) {
        return location == Location::kLfe || location == Location::kLfe2;
    }

    // The five Dolby-style height locations a ":height"/":top"/":upfiring"
    // suffix may re-tier - see the header comment. Ts (a true overhead
    // centre-rear, already at 90 degrees) is deliberately not among them:
    // it has no "which physical thing realizes it" question to answer.
    static bool is_realizable_height(Location location) {
        switch (location) {
            case Location::kVhl:
            case Location::kVhr:
            case Location::kVhc:
            case Location::kLts:
            case Location::kRts: return true;
            default: return false;
        }
    }

    static std::optional<Speaker::Realization> realization_named(std::string_view token) {
        if (equals_ignoring_case(token, "height")) {
            return Speaker::Realization::kHeight;
        }
        if (equals_ignoring_case(token, "top")) {
            return Speaker::Realization::kTop;
        }
        if (equals_ignoring_case(token, "upfiring")) {
            return Speaker::Realization::kUpFiring;
        }
        return std::nullopt;
    }

    static std::string_view trim(std::string_view s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' ||
                              s.front() == '\n')) {
            s.remove_prefix(1);
        }
        while (!s.empty() &&
               (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
            s.remove_suffix(1);
        }
        return s;
    }

    static bool equals_ignoring_case(std::string_view a, std::string_view b) {
        if (a.size() != b.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i) {
            const char x = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] + 32) : a[i];
            const char y = b[i] >= 'A' && b[i] <= 'Z' ? static_cast<char>(b[i] + 32) : b[i];
            if (x != y) {
                return false;
            }
        }
        return true;
    }

    static std::optional<Location> location_named(std::string_view token) {
        for (int i = 0; i < ac3::eac3::chanmap::kMaxChannels; ++i) {
            const auto location = static_cast<Location>(i);
            if (equals_ignoring_case(token, ac3::eac3::chanmap::name(location))) {
                return location;
            }
        }
        return std::nullopt;
    }

    // A decimal number from the whole of `token`, or std::nullopt.
    static std::optional<double> number(std::string_view token) {
        std::array<char, 32> buffer{};
        if (token.empty() || token.size() >= buffer.size()) {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < token.size(); ++i) {
            buffer[i] = token[i];
        }
        char* end = nullptr;
        const double value = std::strtod(buffer.data(), &end);
        if (end != buffer.data() + token.size()) {
            return std::nullopt;
        }
        return value;
    }

    [[nodiscard]] static std::optional<OutputLayout> listed(std::string_view text) {
        OutputLayout out;
        std::string_view rest = text;
        for (;;) {
            const std::size_t comma = rest.find(',');
            const std::string_view token =
                trim(comma == std::string_view::npos ? rest : rest.substr(0, comma));
            if (token.empty() || out.count_ >= kMaxSlots) {
                return std::nullopt;
            }
            // Peel ':'-separated suffixes off the right, in any order, each
            // at most once - see the header comment. What is left is the
            // base token: a location name, an angle pair, "-" or "lfe".
            std::string_view base = token;
            bool small = false;
            Speaker::Realization realization = Speaker::Realization::kDefault;
            for (;;) {
                const std::size_t colon = base.rfind(':');
                if (colon == std::string_view::npos) {
                    break;
                }
                const std::string_view suffix = trim(base.substr(colon + 1));
                if (equals_ignoring_case(suffix, "small")) {
                    if (small) {
                        return std::nullopt;  // ":small" twice
                    }
                    small = true;
                } else if (const auto found = realization_named(suffix)) {
                    if (realization != Speaker::Realization::kDefault) {
                        return std::nullopt;  // two realization suffixes
                    }
                    realization = *found;
                } else {
                    return std::nullopt;  // an unrecognised suffix
                }
                base = trim(base.substr(0, colon));
                if (base.empty()) {
                    return std::nullopt;
                }
            }
            Speaker speaker;
            if (base == "-") {
                if (small || realization != Speaker::Realization::kDefault) {
                    return std::nullopt;  // a suffix on an empty slot means nothing
                }
                speaker.kind = Speaker::Kind::kEmpty;
            } else if (const auto location = location_named(base)) {
                // "lfe" and "LFE2" arrive here too: they are Table E2.5
                // locations, and keep their names like any other.
                if (out.index_of(*location) >= 0) {
                    return std::nullopt;
                }
                const bool lfe = is_lfe(*location);
                if (lfe && (small || realization != Speaker::Realization::kDefault)) {
                    return std::nullopt;  // an LFE feed has no bass to redirect or re-tier
                }
                if (realization != Speaker::Realization::kDefault &&
                    !is_realizable_height(*location)) {
                    return std::nullopt;  // only a height location can be re-tiered
                }
                speaker.location = *location;
                speaker.kind = lfe ? Speaker::Kind::kLfe : Speaker::Kind::kSpeaker;
                speaker.small = small;
                speaker.realization = realization;
            } else {
                const std::size_t slash = base.find('/');
                if (slash == std::string_view::npos) {
                    return std::nullopt;
                }
                const auto azimuth = number(trim(base.substr(0, slash)));
                const auto elevation = number(trim(base.substr(slash + 1)));
                if (!azimuth || !elevation || *elevation < -90.0 || *elevation > 90.0) {
                    return std::nullopt;
                }
                speaker.kind = Speaker::Kind::kSpeaker;
                speaker.direction = {.azimuth_deg = *azimuth, .elevation_deg = *elevation};
                speaker.small = small;
                // A realization suffix on an explicit angle is a label only
                // - the degrees already given win over anything it implies.
                speaker.realization = realization;
            }
            out.speakers_[out.count_++] = speaker;
            if (comma == std::string_view::npos) {
                break;
            }
            rest = rest.substr(comma + 1);
        }
        if (out.speaker_count() == 0 && out.lfe_count() == 0) {
            return std::nullopt;  // a bus of empty slots is not a layout
        }
        if (out.has_small() && out.lfe_count() == 0) {
            return std::nullopt;  // nowhere to send a small speaker's redirected bass
        }
        out.resolve_directions();
        out.set_text(text);
        return out;
    }

    // Named locations get their directions here, once the whole set is known
    // - see the header on why Ls and Rs move when rears are present.
    void resolve_directions() {
        const bool has_rears = index_of(Location::kLrs) >= 0;
        const bool has_side_discrete = index_of(Location::kLsd) >= 0;
        for (std::size_t i = 0; i < count_; ++i) {
            Speaker& speaker = speakers_[i];
            if (speaker.location.has_value() && speaker.kind == Speaker::Kind::kSpeaker) {
                speaker.direction =
                    ac3::spatial::direction_of(*speaker.location, has_rears, has_side_discrete);
                if (speaker.realization == Speaker::Realization::kTop) {
                    speaker.direction.elevation_deg = 90.0;
                }
            }
        }
    }

    [[nodiscard]] std::size_t count(Speaker::Kind kind) const {
        std::size_t n = 0;
        for (const Speaker& speaker : speakers()) {
            if (speaker.kind == kind) {
                ++n;
            }
        }
        return n;
    }

    void set_text(std::string_view text) {
        const std::size_t n = text.size() < kTextBytes - 1 ? text.size() : kTextBytes - 1;
        for (std::size_t i = 0; i < n; ++i) {
            text_[i] = text[i];
        }
        text_[n] = '\0';
    }

    // The list form of what the slots hold, for a layout built from locations
    // rather than parsed.
    void set_text_from_slots() {
        std::size_t used = 0;
        for (std::size_t i = 0; i < count_; ++i) {
            const Speaker& speaker = speakers_[i];
            std::string_view token = "-";
            if (speaker.location.has_value()) {
                token = ac3::eac3::chanmap::name(*speaker.location);
            } else if (speaker.kind == Speaker::Kind::kLfe) {
                token = "lfe";
            }
            if (used + token.size() + 2 >= kTextBytes) {
                break;
            }
            if (i > 0) {
                text_[used++] = ',';
            }
            for (const char c : token) {
                text_[used++] = c;
            }
        }
        text_[used] = '\0';
    }

    std::array<Speaker, kMaxSlots> speakers_{};
    std::size_t count_ = 0;
    std::array<char, kTextBytes> text_{};
};

}  // namespace ac3forge
