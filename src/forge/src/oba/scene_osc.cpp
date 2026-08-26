#include "ac3/oba/scene_osc.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

// OSC 1.0, transcribed from the published specification
// (opensoundcontrol.org, "OSC 1.0 Specification") one grammar element at a
// time, each cited by the section it comes from - the "OSC Packets" section
// defines the packet/message/bundle split this file's three readers below
// implement; "OSC Data Types" defines int32/float32/OSC-string; "OSC
// Message" and "OSC Bundle" define how they compose. What this file does
// NOT implement is scene_osc.hpp's own header comment.

namespace ac3::oba {

namespace {

// --- OSC Data Types ---------------------------------------------------------

// int32/float32: 4 bytes, big-endian ("network byte order" - "OSC Data
// Types"). Assembled by hand rather than via std::byteswap: NDK r26's
// bundled libc++ and the macOS wheel's deployment target are both missing
// pieces of <bit>/<charconv> this project has hit before (CONTRIBUTING.md's
// code-conventions section) - hand assembly has no such exposure and no
// endianness assumption about the host at all.
std::optional<std::uint32_t> read_u32_bits(std::span<const std::byte> buf, std::size_t& pos) {
    if (pos + 4 > buf.size()) {
        return std::nullopt;
    }
    std::uint32_t bits = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        bits = (bits << 8) | std::to_integer<std::uint32_t>(buf[pos + i]);
    }
    pos += 4;
    return bits;
}

// Two's complement is well-defined for this cast since C++20 ([conv.integral]),
// so this is a value-preserving reinterpretation, not implementation-defined
// behaviour - the -Wsign-conversion trap this project's own warnings set
// would otherwise flag is that the WIRE size fields below are read as this
// signed type and validated (>= 0, a multiple of 4) BEFORE ever reaching a
// static_cast<std::size_t>, never the other way around.
std::optional<std::int32_t> read_i32(std::span<const std::byte> buf, std::size_t& pos) {
    const auto bits = read_u32_bits(buf, pos);
    if (!bits) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(*bits);
}

std::optional<float> read_f32(std::span<const std::byte> buf, std::size_t& pos) {
    const auto bits = read_u32_bits(buf, pos);
    if (!bits) {
        return std::nullopt;
    }
    return std::bit_cast<float>(*bits);
}

// OSC-string: ASCII, NUL-terminated, padded with 0-3 further NULs so the
// terminator-plus-padding always lands on a 4-byte boundary ("OSC Data
// Types"). The terminator must be found WITHIN `buf` - an unterminated
// string is exactly the kind of malformed input this parser must not read
// past looking for.
std::optional<std::string_view> read_osc_string(std::span<const std::byte> buf, std::size_t& pos) {
    std::size_t i = pos;
    while (i < buf.size() && buf[i] != std::byte{0}) {
        ++i;
    }
    if (i >= buf.size()) {
        return std::nullopt;  // ran off the end with no NUL - not a valid OSC-string
    }
    const std::size_t len = i - pos;
    const std::size_t padded = ((len + 1 + 3) / 4) * 4;  // +1 for the terminator itself
    if (pos + padded > buf.size()) {
        return std::nullopt;
    }
    // std::byte and char share an object representation (both are exactly
    // one byte, [basic.types.general]); this is the standard way to view a
    // byte range as text, the same relationship std::to_integer already
    // exploits above.
    const std::string_view text{reinterpret_cast<const char*>(buf.data() + pos), len};
    pos += padded;
    return text;
}

constexpr std::size_t kMaxBundleDepth = 8;
// Sanity bound on an object index a packet can name, matching
// scene.cpp's own kMaxObjectIndex for the keyframe grammar (1023, far above
// TS 103 420's real 16-object cap) - keeps a hostile index from being
// anything other than a plain rejected message, never a reason to allocate.
constexpr std::size_t kMaxObjectIndex = 1023;

bool is_bundle_tag(std::span<const std::byte> element) {
    constexpr std::array<char, 8> kTag{'#', 'b', 'u', 'n', 'd', 'l', 'e', '\0'};
    if (element.size() < kTag.size()) {
        return false;
    }
    for (std::size_t i = 0; i < kTag.size(); ++i) {
        if (element[i] != std::byte{static_cast<unsigned char>(kTag[i])}) {
            return false;
        }
    }
    return true;
}

enum class AddressKind : std::uint8_t { kXyz, kGain, kLfe, kRelease };

// /object/<n>/{xyz,gain,lfe,release} - matched LITERALLY (scene_osc.hpp's
// own "deliberately narrow" note: no OSC address-pattern glob matching).
// <n> is 0-based, matching scene order and the keyframe grammar's own
// object_index column.
std::optional<std::pair<std::size_t, AddressKind>> match_address(std::string_view address) {
    constexpr std::string_view kPrefix = "/object/";
    if (!address.starts_with(kPrefix)) {
        return std::nullopt;
    }
    const std::string_view rest = address.substr(kPrefix.size());
    const auto slash = rest.find('/');
    if (slash == std::string_view::npos || slash == 0) {
        return std::nullopt;
    }
    const std::string_view index_text = rest.substr(0, slash);
    const std::string_view suffix = rest.substr(slash);

    std::size_t object = 0;
    const auto conv =
        std::from_chars(index_text.data(), index_text.data() + index_text.size(), object);
    if (conv.ec != std::errc{} || conv.ptr != index_text.data() + index_text.size() ||
        object > kMaxObjectIndex) {
        return std::nullopt;
    }
    if (suffix == "/xyz") {
        return std::pair{object, AddressKind::kXyz};
    }
    if (suffix == "/gain") {
        return std::pair{object, AddressKind::kGain};
    }
    if (suffix == "/lfe") {
        return std::pair{object, AddressKind::kLfe};
    }
    if (suffix == "/release") {
        return std::pair{object, AddressKind::kRelease};
    }
    return std::nullopt;
}

// Parses one OSC Message (address, Type Tag String, arguments) from `msg` -
// either the whole top-level packet, or one bundle element's contents - and
// calls `emit(update)` for a message this project's own address space
// understands. Only 'f' (float32) and 'i' (int32, widened losslessly) are
// supported argument types, capped at 3 (the most any address here takes,
// /xyz's x/y/z) - a type tag needing more, or spelling any other type char,
// drops the whole message rather than partially applying it. Returns
// whatever `emit` returned (false = the sink is full and wants no more), or
// true when nothing was emitted (a dropped message is never a reason to
// stop the walk).
template <typename Sink>
bool process_message(std::span<const std::byte> msg, OscParseStats& stats, Sink&& emit) {
    std::size_t pos = 0;
    const auto address = read_osc_string(msg, pos);
    if (!address || address->empty() || address->front() != '/') {
        ++stats.messages_dropped;
        return true;
    }
    const auto type_tag = read_osc_string(msg, pos);
    // OSC 1.0 permits an absent Type Tag String for pre-1.0 compatibility;
    // this parser does not guess argument types, so it drops instead
    // (scene_osc.hpp's own "deliberately narrow" note).
    if (!type_tag || type_tag->empty() || type_tag->front() != ',') {
        ++stats.messages_dropped;
        return true;
    }

    std::array<double, 3> args{};
    std::size_t argc = 0;
    bool bad = false;
    for (std::size_t i = 1; i < type_tag->size(); ++i) {
        if (argc >= args.size()) {
            bad = true;  // more arguments than any address here takes
            break;
        }
        const char tag = (*type_tag)[i];
        if (tag == 'f') {
            const auto v = read_f32(msg, pos);
            if (!v || !std::isfinite(*v)) {
                bad = true;
                break;
            }
            args[argc++] = static_cast<double>(*v);
        } else if (tag == 'i') {
            const auto v = read_i32(msg, pos);
            if (!v) {
                bad = true;
                break;
            }
            args[argc++] = static_cast<double>(*v);  // always finite
        } else {
            bad = true;  // 'd'/'s'/'b'/anything else: unsupported
            break;
        }
    }
    if (bad) {
        ++stats.messages_dropped;
        return true;
    }

    const auto matched = match_address(*address);
    if (!matched) {
        ++stats.messages_dropped;
        return true;
    }
    const auto& [object, kind] = *matched;

    SceneOscUpdate update;
    update.object = object;
    switch (kind) {
        case AddressKind::kXyz:
            if (argc != 3) {
                ++stats.messages_dropped;
                return true;
            }
            update.position = Position{.x = std::clamp(args[0], 0.0, 1.0),
                                       .y = std::clamp(args[1], 0.0, 1.0),
                                       .z = std::clamp(args[2], -1.0, 1.0)};
            break;
        case AddressKind::kGain:
            if (argc != 1) {
                ++stats.messages_dropped;
                return true;
            }
            update.gain = std::clamp(args[0], 0.0, 1.0);
            break;
        case AddressKind::kLfe:
            if (argc != 1) {
                ++stats.messages_dropped;
                return true;
            }
            update.lfe_send = std::clamp(args[0], 0.0, 1.0);
            break;
        case AddressKind::kRelease:
            if (argc != 0) {
                ++stats.messages_dropped;
                return true;
            }
            update.release = true;
            break;
    }
    return emit(update);
}

// Walks an OSC Bundle's elements - int32 size, contents, repeated - to
// arbitrary width but a HARD-CAPPED depth (kMaxBundleDepth), ITERATIVELY:
// an explicit fixed-size stack of "bytes not yet consumed at this level"
// frames, indexed by an integer top-of-stack rather than function
// recursion, so docs/threat-model.md's "no recursive descent anywhere in
// the parsers" stays true of this parser by construction rather than by
// convention. A malformed element's framing (a negative or non-multiple-of-4
// size, or a size claiming more bytes than remain) ends THAT level's walk -
// whatever was already parsed at this level and any level below it is kept -
// and a bundle nested past the cap is dropped whole rather than descended
// into. `body` is the bytes after "#bundle\0" and the (discarded) time tag.
template <typename Sink>
void walk_bundle_body(std::span<const std::byte> body, OscParseStats& stats, Sink&& emit) {
    struct Frame {
        std::span<const std::byte> remaining;
    };
    std::array<Frame, kMaxBundleDepth> stack{};
    stack[0] = Frame{.remaining = body};
    int top = 0;
    bool keep_going = true;

    while (top >= 0 && keep_going) {
        if (stack[static_cast<std::size_t>(top)].remaining.empty()) {
            --top;
            continue;
        }
        auto& frame = stack[static_cast<std::size_t>(top)];
        std::size_t pos = 0;
        const auto size = read_i32(frame.remaining, pos);
        if (!size || *size < 0 || (*size % 4) != 0 ||
            static_cast<std::size_t>(*size) > frame.remaining.size() - pos) {
            ++stats.packets_rejected;
            frame.remaining = {};  // stop this level; keep what already parsed
            continue;
        }
        const auto element_size = static_cast<std::size_t>(*size);
        const auto element = frame.remaining.subspan(pos, element_size);
        frame.remaining = frame.remaining.subspan(pos + element_size);

        if (!element.empty() && element.front() == std::byte{'/'}) {
            keep_going = process_message(element, stats, emit);
        } else if (is_bundle_tag(element)) {
            constexpr std::size_t kBundleHeaderSize = 8 + 8;  // "#bundle\0" + time tag
            if (element.size() < kBundleHeaderSize) {
                ++stats.packets_rejected;
            } else if (top + 1 >= static_cast<int>(kMaxBundleDepth)) {
                ++stats.packets_rejected;  // too deep - dropped whole, not descended
            } else {
                ++top;
                stack[static_cast<std::size_t>(top)] =
                    Frame{.remaining = element.subspan(kBundleHeaderSize)};
            }
        } else {
            ++stats.packets_rejected;  // neither a message nor a bundle
        }
    }
}

template <typename Sink>
void walk_osc_packet(std::span<const std::byte> packet, OscParseStats& stats, Sink&& emit) {
    if (!packet.empty() && packet.front() == std::byte{'/'}) {
        process_message(packet, stats, emit);
        return;
    }
    constexpr std::size_t kBundleHeaderSize = 8 + 8;
    if (is_bundle_tag(packet) && packet.size() >= kBundleHeaderSize) {
        walk_bundle_body(packet.subspan(kBundleHeaderSize), stats, emit);
        return;
    }
    ++stats.packets_rejected;
}

}  // namespace

std::vector<SceneOscUpdate> parse_osc_packet(std::span<const std::byte> packet,
                                             OscParseStats* stats) {
    OscParseStats local{};
    OscParseStats& s = stats ? *stats : local;
    std::vector<SceneOscUpdate> out;
    walk_osc_packet(packet, s, [&out](const SceneOscUpdate& update) {
        out.push_back(update);
        return true;
    });
    return out;
}

std::size_t parse_osc_packet_into(std::span<const std::byte> packet,
                                  std::span<SceneOscUpdate> out, OscParseStats* stats) {
    OscParseStats local{};
    OscParseStats& s = stats ? *stats : local;
    std::size_t count = 0;
    walk_osc_packet(packet, s, [&out, &count](const SceneOscUpdate& update) {
        out[count++] = update;
        return count < out.size();
    });
    return count;
}

std::optional<ObjectPlacement> apply(const SceneOscUpdate& update, const ObjectPlacement& base) {
    if (!update.position) {
        return std::nullopt;
    }
    ObjectPlacement result = base;
    result.position = *update.position;
    if (update.gain) {
        result.gain = *update.gain;
    }
    if (update.lfe_send) {
        result.lfe_send = *update.lfe_send;
    }
    return result;
}

}  // namespace ac3::oba
