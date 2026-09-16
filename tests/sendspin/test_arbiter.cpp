#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <vector>

#include "ac3/sendspin/arbiter.hpp"
#include "ac3/sendspin/crypto.hpp"
#include "ac3/sendspin/messages.hpp"

// Admission between servers, as connection.md's Multiple servers (server-initiated) sets it:
// the ranking, its three exceptions, and what a later activation or a closed connection
// changes.

namespace {

using ac3::sendspin::Arbiter;
using ac3::sendspin::crypto::Key32;
using Rank = Arbiter::Rank;
namespace m = ac3::sendspin::messages;

Key32 server(std::uint8_t id) {
    Key32 key{};
    key.fill(id);
    return key;
}

bool admitted(const Arbiter::Verdict& verdict) {
    return verdict.admit && !verdict.displaced;
}

bool displaces(const Arbiter::Verdict& verdict, Arbiter::Id id) {
    return verdict.admit && verdict.displaced == std::optional<Arbiter::Id>(id);
}

bool rejected(const Arbiter::Verdict& verdict) {
    return !verdict.admit && !verdict.displaced;
}

}  // namespace

TEST_CASE("arbiter: the rank of an activity set", "[sendspin][arbiter]") {
    CHECK(Arbiter::rank_of({}) == Rank::kNone);
    CHECK(Arbiter::rank_of({m::Activity::kPairing}) == Rank::kPairing);
    CHECK(Arbiter::rank_of({m::Activity::kPlayback}) == Rank::kPlayback);
    CHECK(Arbiter::rank_of({m::Activity::kOther, m::Activity::kPlayback}) == Rank::kPlayback);
}

TEST_CASE("arbiter: equal or higher displaces the holder, lower is rejected", "[sendspin][arbiter]") {
    Arbiter arbiter;
    CHECK(admitted(arbiter.activation(1, server(1), Rank::kPairing, true)));
    CHECK(arbiter.admitted() == std::optional<Arbiter::Id>(1));
    CHECK(rejected(arbiter.activation(2, server(2), Rank::kNone, true)));
    CHECK(displaces(arbiter.activation(3, server(3), Rank::kPairing, true), 1));
    CHECK(displaces(arbiter.activation(4, server(4), Rank::kPlayback, true), 3));
    CHECK(displaces(arbiter.activation(5, server(5), Rank::kPlayback, true), 4));
    CHECK(rejected(arbiter.activation(6, server(6), Rank::kNone, true)));
    CHECK(arbiter.admitted() == std::optional<Arbiter::Id>(5));
    // The last server to hold the admitted connection with playback.
    CHECK(arbiter.last_playback() == std::optional<Key32>(server(5)));
    // A rejected or displaced connection is no longer held, whatever it declares next.
    CHECK(rejected(arbiter.activation(4, server(4), Rank::kPlayback, false)));
}

TEST_CASE("arbiter: a pairing attempt in progress is not displaced", "[sendspin][arbiter]") {
    Arbiter arbiter;
    CHECK(admitted(arbiter.activation(1, server(1), Rank::kPairing, true)));
    arbiter.attempt(1, true);
    CHECK(rejected(arbiter.activation(2, server(2), Rank::kPlayback, true)));
    CHECK(rejected(arbiter.activation(3, server(3), Rank::kPairing, true)));
    // Once it ends, it is displaced like any holder.
    arbiter.attempt(1, false);
    CHECK(displaces(arbiter.activation(4, server(4), Rank::kPlayback, true), 1));
}

TEST_CASE("arbiter: with nothing declared on either, the last-playback server wins", "[sendspin][arbiter]") {
    Arbiter arbiter(server(9));
    CHECK(admitted(arbiter.activation(1, server(1), Rank::kNone, true)));
    // Another server with nothing declared: the holder stays.
    CHECK(rejected(arbiter.activation(2, server(2), Rank::kNone, true)));
    // The last-playback server takes the client from one that is not.
    CHECK(displaces(arbiter.activation(3, server(9), Rank::kNone, true), 1));
    // And is not displaced by another connection from itself.
    CHECK(rejected(arbiter.activation(4, server(9), Rank::kNone, true)));
}

TEST_CASE("arbiter: one pairing connection beside the playback holder", "[sendspin][arbiter]") {
    Arbiter arbiter;
    CHECK(admitted(arbiter.activation(1, server(1), Rank::kPlayback, true)));
    CHECK(admitted(arbiter.activation(2, server(2), Rank::kPairing, true)));
    CHECK(arbiter.beside() == std::optional<Arbiter::Id>(2));
    // A second pairing connection ranks below the playback holder.
    CHECK(rejected(arbiter.activation(3, server(3), Rank::kPairing, true)));
    // Pairing again keeps it beside.
    CHECK(admitted(arbiter.activation(2, server(2), Rank::kPairing, false)));

    SECTION("dropping pairing for nothing, it is rejected against the playback holder") {
        CHECK(rejected(arbiter.activation(2, server(2), Rank::kNone, false)));
        CHECK_FALSE(arbiter.beside().has_value());
        CHECK(arbiter.admitted() == std::optional<Arbiter::Id>(1));
    }
    SECTION("dropping pairing for playback, it displaces the holder") {
        CHECK(displaces(arbiter.activation(2, server(2), Rank::kPlayback, false), 1));
        CHECK(arbiter.admitted() == std::optional<Arbiter::Id>(2));
        CHECK(arbiter.last_playback() == std::optional<Key32>(server(2)));
    }
    SECTION("the holder closes, and the client holds the pairing connection") {
        arbiter.ended(1);
        CHECK(arbiter.admitted() == std::optional<Arbiter::Id>(2));
        CHECK_FALSE(arbiter.beside().has_value());
    }
    SECTION("an incoming playback connection displaces the holder and leaves it beside") {
        CHECK(displaces(arbiter.activation(4, server(4), Rank::kPlayback, true), 1));
        CHECK(arbiter.beside() == std::optional<Arbiter::Id>(2));
    }
}

TEST_CASE("arbiter: after a re-handshake, a held connection's first activation is not arbitrated", "[sendspin][arbiter]") {
    Arbiter arbiter;
    CHECK(admitted(arbiter.activation(1, server(1), Rank::kPairing, true)));
    // Paired: the re-handshake starts the session's activations again.
    CHECK(admitted(arbiter.activation(1, server(1), Rank::kPlayback, true)));
    CHECK(arbiter.admitted() == std::optional<Arbiter::Id>(1));
    CHECK(arbiter.last_playback() == std::optional<Key32>(server(1)));
}

TEST_CASE("arbiter: later activations change the rank without arbitration", "[sendspin][arbiter]") {
    Arbiter arbiter;
    CHECK(admitted(arbiter.activation(1, server(1), Rank::kNone, true)));
    CHECK_FALSE(arbiter.last_playback().has_value());
    CHECK(admitted(arbiter.activation(1, server(1), Rank::kPlayback, false)));
    CHECK(arbiter.last_playback() == std::optional<Key32>(server(1)));
    // Now a playback holder, it has pairing beside it.
    CHECK(admitted(arbiter.activation(2, server(2), Rank::kPairing, true)));
    CHECK(arbiter.beside() == std::optional<Arbiter::Id>(2));
    // Dropping to nothing keeps the last-playback server as it was.
    CHECK(admitted(arbiter.activation(1, server(1), Rank::kNone, false)));
    CHECK(arbiter.last_playback() == std::optional<Key32>(server(1)));
}
