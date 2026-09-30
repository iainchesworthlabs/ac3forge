// A Sendspin player's pairing records and its servers' names, tested on the
// host (ac3forge/pairing_records.hpp): the order a board keeps its records in,
// which record a new pairing evicts, and the two blobs NVS holds.
//
// What can be wrong here is silent on a board until the day it matters: the
// record evicted is the server that plays every day, or one a connection is
// using right now (pairing.md forbids that one), or a firmware update reads the
// records it wrote before as something else.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac3forge/pairing_records.hpp"

using ac3forge::PairingKey;
using ac3forge::PairingRecords;
using ac3forge::ServerNames;

namespace {

PairingKey key(std::uint8_t id) {
    PairingKey k{};
    k.fill(id);
    return k;
}

// The servers in the order the records hold them, the least recently used
// first, by the byte each key is filled with.
template <std::size_t N>
std::vector<int> order(const PairingRecords<N>& records) {
    std::vector<int> ids;
    for (std::size_t i = 0; i < records.size(); ++i) {
        ids.push_back(records[i].server_key[0]);
    }
    return ids;
}

}  // namespace

TEST_CASE("pairing records: a pairing is the most recently used, and replaces its server's earlier one",
          "[io][pairing_records]") {
    PairingRecords<3> records;
    CHECK_FALSE(records.add(key(1), key(11), {}).has_value());
    CHECK_FALSE(records.add(key(2), key(12), {}).has_value());
    CHECK_FALSE(records.add(key(3), key(13), {}).has_value());
    CHECK(order(records) == std::vector<int>{1, 2, 3});
    // Server 1 pairs again: one record for it, last, with the new PSK.
    CHECK_FALSE(records.add(key(1), key(21), {}).has_value());
    CHECK(order(records) == std::vector<int>{2, 3, 1});
    CHECK(records[2].psk == key(21));
    CHECK(records[2].seen);
}

TEST_CASE("pairing records: at the capacity the least recently used goes, not the oldest pairing",
          "[io][pairing_records]") {
    PairingRecords<3> records;
    (void)records.add(key(1), key(11), {});
    (void)records.add(key(2), key(12), {});
    (void)records.add(key(3), key(13), {});
    // Server 1 - the first to pair - connects again, and is the one in use.
    CHECK(records.touch(key(1)));
    CHECK(order(records) == std::vector<int>{2, 3, 1});
    CHECK(records.add(key(4), key(14), {}) == std::optional<PairingKey>(key(2)));
    CHECK(order(records) == std::vector<int>{3, 1, 4});
    CHECK_FALSE(records.find(key(2)).has_value());
}

TEST_CASE("pairing records: a record an open connection rests on is never the one evicted",
          "[io][pairing_records]") {
    PairingRecords<3> records;
    (void)records.add(key(1), key(11), {});
    (void)records.add(key(2), key(12), {});
    (void)records.add(key(3), key(13), {});
    const std::array<PairingKey, 2> open{key(1), key(2)};
    CHECK(records.add(key(4), key(14), open) == std::optional<PairingKey>(key(3)));
    CHECK(order(records) == std::vector<int>{1, 2, 4});
    // Were every record in use - a board holds fewer connections than
    // records, so it never is - the pairing still stores: the least recently
    // used goes.
    const std::array<PairingKey, 3> all{key(1), key(2), key(4)};
    CHECK(records.add(key(5), key(15), all) == std::optional<PairingKey>(key(1)));
    CHECK(order(records) == std::vector<int>{2, 4, 5});
}

TEST_CASE("pairing records: an admitted connection moves its record last, and any marks it seen",
          "[io][pairing_records]") {
    PairingRecords<4> records;
    std::array<std::uint8_t, PairingRecords<4>::kBlobBytes> blob{};
    PairingRecords<4> written;
    (void)written.add(key(1), key(11), {});
    (void)written.add(key(2), key(12), {});
    (void)written.add(key(3), key(13), {});
    records.decode(std::span<const std::uint8_t>(blob).first(written.encode(blob)));
    // Read from flash: nothing seen yet this boot.
    for (std::size_t i = 0; i < records.size(); ++i) {
        CHECK_FALSE(records[i].seen);
    }
    // A connection the board refused: its record is seen, and nothing moves.
    records.mark_seen(key(1));
    CHECK(records[0].seen);
    CHECK(order(records) == std::vector<int>{1, 2, 3});
    records.mark_seen(key(9));
    // Already the most recently used: seen, and nothing to write.
    CHECK_FALSE(records.touch(key(3)));
    CHECK(records[2].seen);
    CHECK(order(records) == std::vector<int>{1, 2, 3});
    // Moved, so there is.
    CHECK(records.touch(key(1)));
    CHECK(order(records) == std::vector<int>{2, 3, 1});
    CHECK(records[2].seen);
    CHECK(records[2].psk == key(11));
    CHECK_FALSE(records[0].seen);
    // A server with no record touches nothing.
    CHECK_FALSE(records.touch(key(9)));
    CHECK(order(records) == std::vector<int>{2, 3, 1});
}

TEST_CASE("pairing records: removing one keeps the rest in their order", "[io][pairing_records]") {
    PairingRecords<4> records;
    (void)records.add(key(1), key(11), {});
    (void)records.add(key(2), key(12), {});
    (void)records.add(key(3), key(13), {});
    CHECK(records.remove(key(2)));
    CHECK_FALSE(records.remove(key(2)));
    CHECK(order(records) == std::vector<int>{1, 3});
    CHECK(records[1].psk == key(13));
    records.clear();
    CHECK(records.size() == 0);
}

TEST_CASE("pairing records: the blob is the layout a board has written since Hearth B3", "[io][pairing_records]") {
    STATIC_REQUIRE(PairingRecords<8>::kBlobBytes == 512);
    PairingRecords<8> records;
    (void)records.add(key(1), key(11), {});
    (void)records.add(key(2), key(12), {});
    std::array<std::uint8_t, PairingRecords<8>::kBlobBytes> blob{};
    const std::size_t length = records.encode(blob);
    REQUIRE(length == 128);
    // The server's key, then the long-term PSK, least recently used first.
    CHECK(blob[0] == 1);
    CHECK(blob[31] == 1);
    CHECK(blob[32] == 11);
    CHECK(blob[63] == 11);
    CHECK(blob[64] == 2);
    CHECK(blob[96] == 12);

    PairingRecords<8> read;
    read.decode(std::span<const std::uint8_t>(blob).first(length));
    CHECK(order(read) == std::vector<int>{1, 2});
    CHECK(read[1].psk == key(12));
    // Half a record at the end is left out.
    read.decode(std::span<const std::uint8_t>(blob).first(length + 20));
    CHECK(read.size() == 2);
    read.decode(std::span<const std::uint8_t>(blob).first(100));
    CHECK(order(read) == std::vector<int>{1});
    // More records than the capacity: the first ones fit.
    std::vector<std::uint8_t> many(5 * 64);
    for (std::size_t i = 0; i < 5; ++i) {
        many[i * 64] = static_cast<std::uint8_t>(i + 1);
    }
    PairingRecords<3> small;
    small.decode(many);
    CHECK(small.size() == 3);
    CHECK(small[2].server_key[0] == 3);
}

TEST_CASE("server names: set, renamed, forgotten, and kept only for records held", "[io][pairing_records]") {
    ServerNames<3> names;
    CHECK(names.find(key(1)).empty());
    CHECK(names.set(key(1), "Music Assistant (d5369777-music-assistant)"));
    CHECK_FALSE(names.set(key(1), "Music Assistant (d5369777-music-assistant)"));
    CHECK(names.find(key(1)) == "Music Assistant (d5369777-music-assistant)");
    CHECK(names.set(key(2), "Hearth on the desk"));
    CHECK(names.set(key(2), "Hearth in the study"));
    CHECK(names.find(key(2)) == "Hearth in the study");
    // An empty name forgets the one it had.
    CHECK(names.set(key(2), ""));
    CHECK(names.find(key(2)).empty());
    CHECK_FALSE(names.set(key(2), ""));
    CHECK(names.set(key(3), "Three"));
    CHECK(names.set(key(4), "Four"));
    CHECK(names.size() == 3);
    // A record gone since: its name goes at the next write.
    CHECK(names.keep([](const PairingKey& k) { return k != key(3); }));
    CHECK(names.find(key(3)).empty());
    CHECK_FALSE(names.keep([](const PairingKey&) { return true; }));
    CHECK(names.size() == 2);
}

TEST_CASE("server names: the blob, and what another firmware might have written in it", "[io][pairing_records]") {
    STATIC_REQUIRE(ServerNames<8>::kBlobBytes == 640);
    ServerNames<8> names;
    (void)names.set(key(1), "Music Assistant");
    (void)names.set(key(2), "Hearth");
    std::array<std::uint8_t, ServerNames<8>::kBlobBytes> blob{};
    const std::size_t length = names.encode(blob);
    REQUIRE(length == 160);
    // The server's key, then the name, NUL-padded to 48 bytes.
    CHECK(blob[0] == 1);
    CHECK(blob[32] == 'M');
    CHECK(blob[32 + 15] == 0);
    CHECK(blob[80] == 2);

    ServerNames<8> read;
    read.decode(std::span<const std::uint8_t>(blob).first(length));
    CHECK(read.find(key(1)) == "Music Assistant");
    CHECK(read.find(key(2)) == "Hearth");
    // A name with no NUL in its 48 bytes, or with bytes no UTF-8 has, reads as
    // what fits of it.
    for (std::size_t i = 32; i < 80; ++i) {
        blob[i] = 'x';
    }
    blob[80 + 32] = 0xFF;
    read.decode(std::span<const std::uint8_t>(blob).first(length));
    CHECK(read.find(key(1)) == std::string(47, 'x'));
    CHECK(read.find(key(2)) == "earth");
}

TEST_CASE("server names: a name is cut between characters, without control characters", "[io][pairing_records]") {
    std::array<char, 8> out{};
    CHECK(ac3forge::fit_server_name("abc", out) == 3);
    // A tab and a newline are left out; so are bytes that begin no whole
    // character.
    CHECK(ac3forge::fit_server_name("a\tb\nc", out) == 3);
    CHECK(std::string_view(out.data(), 3) == "abc");
    CHECK(ac3forge::fit_server_name("a\x80\xC3", out) == 1);
    // Eight bytes of room: "Küche" is six, and "é" would need two more after
    // "Küche ", so the name stops before it rather than split it.
    const std::size_t n = ac3forge::fit_server_name("K\xC3\xBC" "che \xC3\xA9t\xC3\xA9", out);
    CHECK(std::string_view(out.data(), n) == "K\xC3\xBC" "che ");
    // A stored name leaves a byte for its NUL: 47 characters of 50.
    ServerNames<1> names;
    (void)names.set(key(1), std::string(50, 'y'));
    CHECK(names.find(key(1)).size() == 47);
}
