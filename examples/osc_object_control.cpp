// The OSC wire form of a live scene update, with no socket in sight.
//
// ac3::audio::LivePositionSource (src/audio, not part of this distributed
// library - see docs/library/index.md) owns the actual UDP listener behind
// `ac3cli live mode=atmos positions=osc:<port>` and the GUI's live room.
// Everything it does with a datagram once it has one, though, is these three
// calls: parse_osc_packet turns the bytes into per-field-optional updates,
// apply() merges one onto an object's current placement without disturbing
// whatever that object's gain law already set, and SceneCursor::push puts
// the result in force. That is the whole seam a third party embedding this
// library for their own show-control integration needs - this example
// stands in for the UDP datagram with a hand-built byte array instead.

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fmt/printf.h>
#include <string_view>
#include <utility>
#include <vector>

#include "ac3/oba/scene.hpp"
#include "ac3/oba/scene_osc.hpp"

namespace {

// A minimal, correct OSC 1.0 message: an address pattern (OSC-string), a
// Type Tag String (OSC-string starting ','), then one big-endian float32 per
// 'f' in the tag - see scene_osc.cpp's own header comment for the full
// grammar this is a hand-rolled encoder for. A real sender (TouchOSC, a DAW,
// a lighting console's OSC output) produces exactly these bytes; nothing
// here is specific to this project.
std::vector<std::byte> osc_xyz_message(std::string_view address, float x, float y, float z) {
    std::vector<std::byte> out;
    const auto append_string = [&out](std::string_view text) {
        for (const char c : text) {
            out.push_back(std::byte{static_cast<unsigned char>(c)});
        }
        out.push_back(std::byte{0});
        while (out.size() % 4 != 0) {
            out.push_back(std::byte{0});
        }
    };
    const auto append_float = [&out](float value) {
        const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
        out.push_back(std::byte{static_cast<unsigned char>((bits >> 24) & 0xFFU)});
        out.push_back(std::byte{static_cast<unsigned char>((bits >> 16) & 0xFFU)});
        out.push_back(std::byte{static_cast<unsigned char>((bits >> 8) & 0xFFU)});
        out.push_back(std::byte{static_cast<unsigned char>(bits & 0xFFU)});
    };
    append_string(address);
    append_string(",fff");
    append_float(x);
    append_float(y);
    append_float(z);
    return out;
}

}  // namespace

int main() {
    // Two objects, each with one static authored point - room centre and a
    // fixed gain, the sort of default a live session starts from before
    // anything on the network has addressed either object yet.
    auto built = ac3::oba::ObjectScene::create({
        {.name = "left",
         .automation = {{.time_s = 0.0, .position = {.x = 0.2, .y = 0.5, .z = 0.0}, .gain = 0.7}}},
        {.name = "right",
         .automation = {{.time_s = 0.0, .position = {.x = 0.8, .y = 0.5, .z = 0.0}, .gain = 0.7}}},
    });
    if (!built) {
        fmt::printf("ObjectScene::create failed: %s\n", built.error().message.c_str());
        return 1;
    }
    ac3::oba::SceneCursor cursor{std::move(*built)};

    fmt::printf("before any OSC: object 0 at x=%.2f (authored)\n",
                cursor.sample(0.0)[0].position.x);

    // Stands in for one UDP datagram addressed to /object/0/xyz, as a show-
    // control rig configured to speak this project's convention would send
    // it (docs/library/spatial-and-atmos.md's "OSC wire form").
    const auto datagram = osc_xyz_message("/object/0/xyz", 0.9F, 0.1F, 0.5F);

    ac3::oba::OscParseStats stats;
    for (const auto& update : ac3::oba::parse_osc_packet(datagram, &stats)) {
        if (update.release) {
            cursor.release(update.object);
            continue;
        }
        // The merge that keeps this object's authored gain (0.7 above)
        // rather than resetting it to a default-constructed placement's
        // 1.0 - see apply()'s own header comment for why this step exists
        // and cannot be skipped in favour of pushing `update` directly.
        const auto base = cursor.scene().evaluate(update.object, 0.0);
        if (const auto merged = ac3::oba::apply(update, base)) {
            cursor.push({.object = update.object, .placement = *merged});
        }
    }
    fmt::printf("packets rejected: %zu, messages dropped: %zu\n", stats.packets_rejected,
                stats.messages_dropped);

    const auto after = cursor.sample(0.0);
    fmt::printf("after one /object/0/xyz message: object 0 at x=%.2f y=%.2f z=%.2f, gain=%.2f\n",
                after[0].position.x, after[0].position.y, after[0].position.z, after[0].gain);
    fmt::printf("object 1 is untouched: x=%.2f (still authored)\n", after[1].position.x);

    // position.x came off the wire as a float32 (0.9F), so it is compared
    // with a tolerance rather than bit-exactly; gain was never touched by
    // the OSC message and still holds its authored double exactly.
    if (std::abs(after[0].position.x - 0.9) > 1e-6 || after[0].gain != 0.7) {
        fmt::printf("unexpected result\n");
        return 1;
    }
    return 0;
}
