#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <span>
#include <string_view>
#include <vector>

#include "ac3/oba/atmos.hpp"
#include "ac3/oba/scene.hpp"
#include "ac3/oba/scene_osc.hpp"

namespace {

using Catch::Matchers::WithinAbs;

// --- Hand-built OSC bytes, matching scene_osc.cpp's own reader exactly, so
// a bug in that reader would have to also be reproduced here to hide. ---

void append_osc_string(std::vector<std::byte>& out, std::string_view text) {
    for (const char c : text) {
        out.push_back(std::byte{static_cast<unsigned char>(c)});
    }
    out.push_back(std::byte{0});
    while (out.size() % 4 != 0) {
        out.push_back(std::byte{0});
    }
}

void append_u32(std::vector<std::byte>& out, std::uint32_t v) {
    out.push_back(std::byte{static_cast<unsigned char>((v >> 24) & 0xFFU)});
    out.push_back(std::byte{static_cast<unsigned char>((v >> 16) & 0xFFU)});
    out.push_back(std::byte{static_cast<unsigned char>((v >> 8) & 0xFFU)});
    out.push_back(std::byte{static_cast<unsigned char>(v & 0xFFU)});
}

void append_i32(std::vector<std::byte>& out, std::int32_t v) {
    append_u32(out, static_cast<std::uint32_t>(v));
}

void append_f32(std::vector<std::byte>& out, float v) { append_u32(out, std::bit_cast<std::uint32_t>(v)); }

std::vector<std::byte> osc_message_f(std::string_view address, std::string_view typetag,
                                     std::initializer_list<float> floats) {
    std::vector<std::byte> out;
    append_osc_string(out, address);
    append_osc_string(out, typetag);
    for (const float f : floats) {
        append_f32(out, f);
    }
    return out;
}

std::vector<std::byte> osc_message_i(std::string_view address, std::string_view typetag,
                                     std::initializer_list<std::int32_t> ints) {
    std::vector<std::byte> out;
    append_osc_string(out, address);
    append_osc_string(out, typetag);
    for (const std::int32_t i : ints) {
        append_i32(out, i);
    }
    return out;
}

std::vector<std::byte> osc_bundle(std::initializer_list<std::vector<std::byte>> elements) {
    std::vector<std::byte> out;
    append_osc_string(out, "#bundle");  // exactly 8 bytes ("#bundle" + NUL); no rounding needed
    for (int i = 0; i < 8; ++i) {
        out.push_back(std::byte{0});  // time tag - discarded by the parser, value irrelevant
    }
    for (const auto& element : elements) {
        append_i32(out, static_cast<std::int32_t>(element.size()));
        out.insert(out.end(), element.begin(), element.end());
    }
    return out;
}

ac3::oba::ObjectScene must_create(std::vector<ac3::oba::SceneObject> objects,
                                  const ac3::oba::Orientation& orientation = {}) {
    auto scene = ac3::oba::ObjectScene::create(std::move(objects), orientation);
    REQUIRE(scene.has_value());
    return std::move(*scene);
}

}  // namespace

// ---------------------------------------------------------------------------
// Messages: the shapes this project's own address space understands
// ---------------------------------------------------------------------------

TEST_CASE("parse_osc_packet reads a well-formed xyz message", "[oba][scene][osc]") {
    const auto bytes = osc_message_f("/object/2/xyz", ",fff", {0.25F, 0.75F, -0.5F});
    ac3::oba::OscParseStats stats;
    const auto updates = ac3::oba::parse_osc_packet(bytes, &stats);
    REQUIRE(updates.size() == 1);
    CHECK(updates[0].object == 2);
    REQUIRE(updates[0].position.has_value());
    CHECK_THAT(updates[0].position->x, WithinAbs(0.25, 1e-9));
    CHECK_THAT(updates[0].position->y, WithinAbs(0.75, 1e-9));
    CHECK_THAT(updates[0].position->z, WithinAbs(-0.5, 1e-9));
    CHECK_FALSE(updates[0].gain.has_value());
    CHECK_FALSE(updates[0].lfe_send.has_value());
    CHECK(stats.messages_dropped == 0);
    CHECK(stats.packets_rejected == 0);
}

TEST_CASE("parse_osc_packet reads gain, lfe and release", "[oba][scene][osc]") {
    SECTION("gain") {
        const auto updates = ac3::oba::parse_osc_packet(osc_message_f("/object/0/gain", ",f", {0.5F}));
        REQUIRE(updates.size() == 1);
        REQUIRE(updates[0].gain.has_value());
        CHECK_THAT(*updates[0].gain, WithinAbs(0.5, 1e-9));
        CHECK_FALSE(updates[0].position.has_value());
    }
    SECTION("lfe") {
        const auto updates = ac3::oba::parse_osc_packet(osc_message_f("/object/0/lfe", ",f", {0.2F}));
        REQUIRE(updates.size() == 1);
        REQUIRE(updates[0].lfe_send.has_value());
        // 0.2 has no exact float32 representation, unlike the 0.25/0.5-style
        // values used elsewhere in this file - the wire value round-trips
        // through float32, not double, so the comparison tolerance has to be
        // wider than a bit-exact check.
        CHECK_THAT(*updates[0].lfe_send, WithinAbs(0.2, 1e-6));
    }
    SECTION("release takes no arguments") {
        const auto updates = ac3::oba::parse_osc_packet(osc_message_f("/object/3/release", ",", {}));
        REQUIRE(updates.size() == 1);
        CHECK(updates[0].object == 3);
        CHECK(updates[0].release);
    }
}

TEST_CASE("int32 arguments widen losslessly wherever float is accepted", "[oba][scene][osc]") {
    const auto updates =
        ac3::oba::parse_osc_packet(osc_message_i("/object/0/xyz", ",iii", {1, 0, -1}));
    REQUIRE(updates.size() == 1);
    REQUIRE(updates[0].position.has_value());
    CHECK(updates[0].position->x == 1.0);
    CHECK(updates[0].position->y == 0.0);
    CHECK(updates[0].position->z == -1.0);
}

TEST_CASE("out-of-range but finite values clamp rather than drop", "[oba][scene][osc]") {
    const auto updates =
        ac3::oba::parse_osc_packet(osc_message_f("/object/0/xyz", ",fff", {5.0F, -5.0F, 9.0F}));
    REQUIRE(updates.size() == 1);
    REQUIRE(updates[0].position.has_value());
    CHECK(updates[0].position->x == 1.0);  // x clamps to [0,1]
    CHECK(updates[0].position->y == 0.0);  // y clamps to [0,1]
    CHECK(updates[0].position->z == 1.0);  // z clamps to [-1,1]
}

// ---------------------------------------------------------------------------
// Rejection: a malformed control-rate update is dropped, never fatal
// ---------------------------------------------------------------------------

TEST_CASE("malformed or unrecognised messages are dropped and counted, not fatal",
          "[oba][scene][osc]") {
    ac3::oba::OscParseStats stats;

    SECTION("unknown address") {
        const auto updates =
            ac3::oba::parse_osc_packet(osc_message_f("/mixer/1/level", ",f", {0.5F}), &stats);
        CHECK(updates.empty());
        CHECK(stats.messages_dropped == 1);
    }
    SECTION("wrong argument count for the address") {
        const auto updates =
            ac3::oba::parse_osc_packet(osc_message_f("/object/0/xyz", ",ff", {0.1F, 0.2F}), &stats);
        CHECK(updates.empty());
        CHECK(stats.messages_dropped == 1);
    }
    SECTION("unsupported argument type (string)") {
        std::vector<std::byte> bytes;
        append_osc_string(bytes, "/object/0/gain");
        append_osc_string(bytes, ",s");
        append_osc_string(bytes, "loud");
        const auto updates = ac3::oba::parse_osc_packet(bytes, &stats);
        CHECK(updates.empty());
        CHECK(stats.messages_dropped == 1);
    }
    SECTION("non-finite float argument") {
        const auto updates = ac3::oba::parse_osc_packet(
            osc_message_f("/object/0/gain", ",f", {std::numeric_limits<float>::quiet_NaN()}),
            &stats);
        CHECK(updates.empty());
        CHECK(stats.messages_dropped == 1);
    }
    SECTION("object index past the sanity cap") {
        const auto updates = ac3::oba::parse_osc_packet(
            osc_message_f("/object/99999/gain", ",f", {0.5F}), &stats);
        CHECK(updates.empty());
        CHECK(stats.messages_dropped == 1);
    }
    SECTION("no type tag string at all (pre-1.0 compatibility) is not guessed at") {
        std::vector<std::byte> bytes;
        append_osc_string(bytes, "/object/0/gain");
        const auto updates = ac3::oba::parse_osc_packet(bytes, &stats);
        CHECK(updates.empty());
        CHECK(stats.messages_dropped == 1);
    }
}

TEST_CASE("truncated and unterminated packets are rejected without reading out of bounds",
          "[oba][scene][osc]") {
    ac3::oba::OscParseStats stats;

    SECTION("a type tag promising an argument the packet does not carry") {
        std::vector<std::byte> bytes;
        append_osc_string(bytes, "/object/0/xyz");
        append_osc_string(bytes, ",fff");
        append_f32(bytes, 0.1F);
        append_f32(bytes, 0.2F);
        // third float missing entirely
        const auto updates = ac3::oba::parse_osc_packet(bytes, &stats);
        CHECK(updates.empty());
        CHECK(stats.messages_dropped == 1);
    }
    SECTION("an address string with no terminating NUL anywhere in the packet") {
        std::vector<std::byte> bytes;
        for (const char c : std::string_view{"/object/0/xyz"}) {
            bytes.push_back(std::byte{static_cast<unsigned char>(c)});
        }
        // deliberately no NUL, no padding, nothing after it
        const auto updates = ac3::oba::parse_osc_packet(bytes, &stats);
        CHECK(updates.empty());
        // Classified by the FIRST byte: this datagram starts '/', so it is a
        // message-shaped attempt whose address could not be read, not a
        // datagram whose top-level shape was unrecognised - messages_dropped,
        // not packets_rejected (see OscParseStats' own doc comment).
        CHECK(stats.messages_dropped == 1);
    }
    SECTION("a completely empty packet") {
        const auto updates = ac3::oba::parse_osc_packet(std::span<const std::byte>{}, &stats);
        CHECK(updates.empty());
        CHECK(stats.packets_rejected == 1);
    }
    SECTION("a packet that is neither a message nor a bundle") {
        std::vector<std::byte> bytes{std::byte{'x'}, std::byte{'y'}, std::byte{'z'}, std::byte{0}};
        const auto updates = ac3::oba::parse_osc_packet(bytes, &stats);
        CHECK(updates.empty());
        CHECK(stats.packets_rejected == 1);
    }
}

// ---------------------------------------------------------------------------
// Bundles: iterative, depth-capped, order-preserving
// ---------------------------------------------------------------------------

TEST_CASE("a bundle's messages all arrive, in order", "[oba][scene][osc]") {
    const auto bundle = osc_bundle({
        osc_message_f("/object/0/xyz", ",fff", {0.1F, 0.2F, 0.3F}),
        osc_message_f("/object/1/xyz", ",fff", {0.4F, 0.5F, 0.6F}),
        osc_message_f("/object/0/gain", ",f", {0.9F}),
    });
    const auto updates = ac3::oba::parse_osc_packet(bundle);
    REQUIRE(updates.size() == 3);
    CHECK(updates[0].object == 0);
    CHECK(updates[0].position.has_value());
    CHECK(updates[1].object == 1);
    CHECK(updates[2].object == 0);
    CHECK(updates[2].gain.has_value());
}

TEST_CASE("a bundle nested inside a bundle is walked too", "[oba][scene][osc]") {
    const auto inner = osc_bundle({osc_message_f("/object/2/gain", ",f", {0.3F})});
    const auto outer = osc_bundle({
        osc_message_f("/object/0/gain", ",f", {0.1F}),
        inner,
    });
    const auto updates = ac3::oba::parse_osc_packet(outer);
    REQUIRE(updates.size() == 2);
    CHECK(updates[0].object == 0);
    CHECK(updates[1].object == 2);
}

TEST_CASE("a malformed bundle element ends that level's walk, keeping what came before",
          "[oba][scene][osc]") {
    ac3::oba::OscParseStats stats;
    std::vector<std::byte> bundle;
    append_osc_string(bundle, "#bundle");
    for (int i = 0; i < 8; ++i) {
        bundle.push_back(std::byte{0});
    }
    const auto good = osc_message_f("/object/0/gain", ",f", {0.5F});
    append_i32(bundle, static_cast<std::int32_t>(good.size()));
    bundle.insert(bundle.end(), good.begin(), good.end());

    SECTION("a negative element size") {
        append_i32(bundle, -4);
        const auto updates = ac3::oba::parse_osc_packet(bundle, &stats);
        REQUIRE(updates.size() == 1);
        CHECK(updates[0].object == 0);
        CHECK(stats.packets_rejected == 1);
    }
    SECTION("an element size that is not a multiple of 4") {
        append_i32(bundle, 5);
        bundle.resize(bundle.size() + 5);  // bytes it claims but the walk must not trust
        const auto updates = ac3::oba::parse_osc_packet(bundle, &stats);
        REQUIRE(updates.size() == 1);
        CHECK(stats.packets_rejected == 1);
    }
    SECTION("an element size larger than the bytes remaining") {
        append_i32(bundle, 4096);
        const auto updates = ac3::oba::parse_osc_packet(bundle, &stats);
        REQUIRE(updates.size() == 1);
        CHECK(stats.packets_rejected == 1);
    }
}

TEST_CASE("a bundle nested past the depth cap is dropped whole, not descended into",
          "[oba][scene][osc]") {
    // Nine levels deep - one past the 8-level cap - each wrapping the next.
    auto innermost = osc_message_f("/object/0/gain", ",f", {0.5F});
    std::vector<std::byte> current = innermost;
    for (int depth = 0; depth < 9; ++depth) {
        current = osc_bundle({current});
    }
    ac3::oba::OscParseStats stats;
    const auto updates = ac3::oba::parse_osc_packet(current, &stats);
    CHECK(updates.empty());
    CHECK(stats.packets_rejected >= 1);
}

// ---------------------------------------------------------------------------
// The allocation-free form
// ---------------------------------------------------------------------------

TEST_CASE("parse_osc_packet_into stops once the caller's storage is full", "[oba][scene][osc]") {
    const auto bundle = osc_bundle({
        osc_message_f("/object/0/gain", ",f", {0.1F}),
        osc_message_f("/object/1/gain", ",f", {0.2F}),
        osc_message_f("/object/2/gain", ",f", {0.3F}),
    });
    std::array<ac3::oba::SceneOscUpdate, 2> out{};
    const auto count = ac3::oba::parse_osc_packet_into(bundle, out);
    CHECK(count == 2);
    CHECK(out[0].object == 0);
    CHECK(out[1].object == 1);
}

// ---------------------------------------------------------------------------
// apply(): the merge that keeps the gain law and rotates exactly once
// ---------------------------------------------------------------------------

TEST_CASE("apply merges a wire update onto a base placement", "[oba][scene][osc]") {
    const ac3::oba::ObjectPlacement base{
        .position = {.x = 0.5, .y = 0.5, .z = 0.0}, .gain = 0.7, .lfe_send = 0.1};

    SECTION("no position: nothing to push yet") {
        const ac3::oba::SceneOscUpdate update{.gain = 0.3};
        CHECK_FALSE(ac3::oba::apply(update, base).has_value());
    }
    SECTION("position only: base's gain and lfe_send carry through") {
        const ac3::oba::SceneOscUpdate update{.position = ac3::oba::Position{.x = 0.9}};
        const auto merged = ac3::oba::apply(update, base);
        REQUIRE(merged.has_value());
        CHECK(merged->position.x == 0.9);
        CHECK(merged->gain == base.gain);
        CHECK(merged->lfe_send == base.lfe_send);
    }
    SECTION("position, gain and lfe all set: all three override") {
        const ac3::oba::SceneOscUpdate update{
            .position = ac3::oba::Position{.x = 0.2}, .gain = 0.4, .lfe_send = 0.6};
        const auto merged = ac3::oba::apply(update, base);
        REQUIRE(merged.has_value());
        CHECK(merged->position.x == 0.2);
        CHECK(merged->gain == 0.4);
        CHECK(merged->lfe_send == 0.6);
    }
}

TEST_CASE("apply through a live cursor rotates the wire position exactly once",
          "[oba][scene][osc]") {
    // A non-identity orientation, so a double rotation would be visibly
    // different from a single one - this is the regression scene.hpp's own
    // "The live half" comment and scene_osc.hpp's apply() warn about: a
    // pushed placement's position is rotated by SceneCursor::sample_into, so
    // whatever apply() hands push() must be UNROTATED wire coordinates, not
    // scene().evaluate()'s already-rotated ones.
    auto scene = must_create(
        {{.name = "a", .automation = {{.time_s = 0.0, .position = {.x = 0.5, .y = 0.0, .z = 0.0},
                                       .gain = 0.7}}}},
        ac3::oba::orientation_from_degrees(90, 0, 0));
    ac3::oba::SceneCursor cursor{std::move(scene)};

    const ac3::oba::Position wire{.x = 0.5, .y = 0.0, .z = 0.0};
    const auto base = cursor.scene().evaluate(0, 0.0);  // already rotated - must NOT be re-rotated
    const ac3::oba::SceneOscUpdate update{.position = wire};
    const auto merged = ac3::oba::apply(update, base);
    REQUIRE(merged.has_value());
    // apply() itself must hand back the UNROTATED wire position, not base's.
    CHECK(merged->position.x == wire.x);
    CHECK(merged->position.y == wire.y);

    REQUIRE(cursor.push({.object = 0, .placement = *merged}));
    const auto sampled = cursor.sample(0.0)[0];

    // The correct answer: the wire position rotated exactly once.
    const auto expected = ac3::oba::rotate(wire, ac3::oba::orientation_from_degrees(90, 0, 0));
    CHECK_THAT(sampled.position.x, WithinAbs(expected.x, 1e-12));
    CHECK_THAT(sampled.position.y, WithinAbs(expected.y, 1e-12));
    // Gain rode through from base, unrelated to rotation.
    CHECK(sampled.gain == 0.7);

    // What a double rotation would have produced, so this test would fail if
    // apply() ever started from base's (already-rotated) position instead.
    const auto double_rotated =
        ac3::oba::rotate(base.position, ac3::oba::orientation_from_degrees(90, 0, 0));
    CHECK(sampled.position.x != double_rotated.x);
}

// ---------------------------------------------------------------------------
// End to end: a drain loop shaped like LivePositionSource::drain_into
// ---------------------------------------------------------------------------

TEST_CASE("a realistic drain loop applies OSC updates to the right objects", "[oba][scene][osc]") {
    auto scene = must_create({
        {.name = "a", .automation = {{.time_s = 0.0, .position = {.x = 0.1}, .gain = 0.5}}},
        {.name = "b", .automation = {{.time_s = 0.0, .position = {.x = 0.9}, .gain = 0.6}}},
    });
    ac3::oba::SceneCursor cursor{std::move(scene)};

    const auto bundle = osc_bundle({
        osc_message_f("/object/1/xyz", ",fff", {0.3F, 0.4F, 0.0F}),
        osc_message_f("/object/1/gain", ",f", {0.2F}),
    });

    // What LivePositionSource's mailbox does before ever calling apply(): two
    // messages for the SAME object within one packet accumulate field by
    // field (position from the first, gain from the second) rather than each
    // being applied independently - a naive per-message apply() would apply
    // the xyz message against the object's still-authored gain, push that,
    // then find the gain-only message has no position to push at all (see
    // apply()'s own header comment) and silently lose the gain change. Same
    // shape, one pending slot per object rather than per message.
    std::map<std::size_t, ac3::oba::SceneOscUpdate> pending;
    for (const auto& update : ac3::oba::parse_osc_packet(bundle)) {
        if (update.release) {
            cursor.release(update.object);
            pending.erase(update.object);
            continue;
        }
        auto& slot = pending[update.object];
        slot.object = update.object;
        if (update.position) {
            slot.position = update.position;
        }
        if (update.gain) {
            slot.gain = update.gain;
        }
        if (update.lfe_send) {
            slot.lfe_send = update.lfe_send;
        }
    }
    for (const auto& [object, update] : pending) {
        const auto base = cursor.scene().evaluate(object, 0.0);
        if (const auto merged = ac3::oba::apply(update, base)) {
            CHECK(cursor.push({.object = object, .placement = *merged}));
        }
    }

    CHECK_FALSE(cursor.is_live(0));  // object 0 was never addressed
    CHECK(cursor.sample(0.0)[0].position.x == 0.1);

    REQUIRE(cursor.is_live(1));
    const auto driven = cursor.sample(0.0)[1];
    CHECK_THAT(driven.position.x, WithinAbs(0.3, 1e-6));
    CHECK_THAT(driven.gain, WithinAbs(0.2, 1e-6));
}
