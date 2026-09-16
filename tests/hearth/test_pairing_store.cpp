#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ac3/sendspin/crypto.hpp"
#include "ac3/sendspin/handshake_session.hpp"
#include "pairing_store.hpp"
#include "settings_model.hpp"

// Hearth's pairing records (apps/hearth/engine/pairing_store.hpp): a record
// is written the moment its pairing completes and read back by the next
// start; one the store would not write is not kept; a forgotten one is gone
// for good; what pairing.md keeps only for a session is kept in memory only.

using ac3::hearth::MemorySettingsStore;
using ac3::hearth::PairingRecordView;
using ac3::hearth::PairingStore;
using ac3::sendspin::crypto::Key32;
using ac3::sendspin::handshake::PskCategory;

namespace {

Key32 key(std::uint8_t fill) {
    Key32 out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::uint8_t>(fill + i);
    }
    return out;
}

// The date a test's records are stamped with.
std::string today() {
    return "2026-09-16";
}

}  // namespace

TEST_CASE("pairing store: a record outlives the store, and the key ring names it",
          "[hearth][pairing-store]") {
    MemorySettingsStore settings;
    {
        PairingStore store{settings, today};
        CHECK(store.records().empty());
        CHECK(store.choose(key(1)).category == PskCategory::kSentinel);
        store.set_client_name(key(1), "hearth-s3-kitchen");
        REQUIRE(store.store_record(key(1), key(101)));
        REQUIRE(store.store_record(key(2), key(102)));
        CHECK(store.paired(key(1)));
        CHECK(store.choose(key(1)).category == PskCategory::kLongTerm);
        CHECK(store.choose(key(1)).psk == key(101));
    }
    // Kept as QSettings writes an array, the keys in hex.
    const auto& written = settings.synced();
    CHECK(written.at("pairing/size") == "2");
    CHECK(written.at("pairing/1/client") ==
          "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20");
    CHECK(written.at("pairing/1/name") == "hearth-s3-kitchen");
    CHECK(written.at("pairing/1/paired") == "2026-09-16");
    CHECK(written.at("pairing/2/name").empty());

    // A new start reads them back, in the order they were made.
    MemorySettingsStore reopened{settings.synced()};
    const PairingStore again{reopened, today};
    const std::vector<PairingRecordView> expected{
        {.client_key = key(1), .name = "hearth-s3-kitchen", .paired_on = "2026-09-16"},
        {.client_key = key(2), .name = "", .paired_on = "2026-09-16"},
    };
    CHECK(again.records() == expected);
    CHECK(again.choose(key(2)).psk == key(102));
    CHECK(again.choose(key(2)).category == PskCategory::kLongTerm);

    // Pairing again replaces the record, which moves to the end.
    PairingStore store{reopened, [] { return std::string{"2026-09-17"}; }};
    REQUIRE(store.store_record(key(1), key(111)));
    const auto records = store.records();
    REQUIRE(records.size() == 2);
    CHECK(records[1].client_key == key(1));
    CHECK(records[1].paired_on == "2026-09-17");
    CHECK(store.choose(key(1)).psk == key(111));
}

TEST_CASE("pairing store: a record the store would not write is not kept",
          "[hearth][pairing-store]") {
    MemorySettingsStore settings;
    PairingStore store{settings, today};
    REQUIRE(store.store_record(key(1), key(101)));
    store.set_pairing_psk(key(2), key(50));
    store.set_approved(key(2), true);

    settings.set_sync_fails(true);
    CHECK_FALSE(store.store_record(key(2), key(102)));
    CHECK_FALSE(store.paired(key(2)));
    // The attempt's own state is left for the next try.
    CHECK(store.has_pairing_psk(key(2)));
    CHECK(store.approved(key(2)));
    // Pairing a client again, when the write fails, keeps its old record.
    CHECK_FALSE(store.store_record(key(1), key(111)));
    CHECK(store.choose(key(1)).psk == key(101));
    CHECK(store.records().size() == 1);

    // Once the store writes again, what it writes is what the process holds.
    settings.set_sync_fails(false);
    store.set_client_name(key(3), "Kitchen speaker");
    REQUIRE(store.store_record(key(3), key(103)));
    MemorySettingsStore reopened{settings.synced()};
    const PairingStore again{reopened, today};
    REQUIRE(again.records().size() == 2);
    CHECK(again.choose(key(1)).psk == key(101));
    CHECK_FALSE(again.paired(key(2)));
    CHECK(again.records()[1].name == "Kitchen speaker");
}

TEST_CASE("pairing store: a forgotten record is gone for good", "[hearth][pairing-store]") {
    MemorySettingsStore settings;
    {
        PairingStore store{settings, today};
        REQUIRE(store.store_record(key(1), key(101)));
        REQUIRE(store.store_record(key(2), key(102)));
        store.forget(key(1));
        store.forget(key(9));
        CHECK_FALSE(store.paired(key(1)));
        CHECK(store.choose(key(1)).category == PskCategory::kSentinel);
        CHECK(store.paired(key(2)));
    }
    MemorySettingsStore reopened{settings.synced()};
    const PairingStore again{reopened, today};
    REQUIRE(again.records().size() == 1);
    CHECK(again.records()[0].client_key == key(2));
    CHECK_FALSE(settings.synced().contains("pairing/2/client"));
}

TEST_CASE("pairing store: typed-in keys and approvals last only as long as the process",
          "[hearth][pairing-store]") {
    MemorySettingsStore settings;
    PairingStore store{settings, today};
    store.set_pairing_psk(key(1), key(50));
    CHECK(store.has_pairing_psk(key(1)));
    CHECK(store.choose(key(1)).category == PskCategory::kPairing);
    CHECK(store.choose(key(1)).psk == key(50));
    store.set_pairing_psk(key(1), std::nullopt);
    CHECK_FALSE(store.has_pairing_psk(key(1)));
    CHECK(store.choose(key(1)).category == PskCategory::kSentinel);

    store.set_pairing_psk(key(1), key(50));
    store.set_approved(key(1), true);
    CHECK(store.approved(key(1)));
    store.set_approved(key(3), false);
    CHECK_FALSE(store.approved(key(3)));
    // A written record outranks the typed-in key, and clears both.
    REQUIRE(store.store_record(key(1), key(101)));
    CHECK(store.choose(key(1)).category == PskCategory::kLongTerm);
    CHECK_FALSE(store.has_pairing_psk(key(1)));
    CHECK_FALSE(store.approved(key(1)));

    store.set_pairing_psk(key(2), key(60));
    store.set_approved(key(2), true);
    for (const auto& entry : settings.synced()) {
        INFO(entry.first);
        CHECK(entry.first.starts_with("pairing/"));
    }
    MemorySettingsStore reopened{settings.synced()};
    const PairingStore again{reopened, today};
    CHECK_FALSE(again.has_pairing_psk(key(2)));
    CHECK_FALSE(again.approved(key(2)));
}

TEST_CASE("pairing store: records that do not read are left out", "[hearth][pairing-store]") {
    const std::string good_client(64, 'a');
    const std::string good_psk = "00112233445566778899AABBCCDDEEFF00112233445566778899aabbccddeeff";
    MemorySettingsStore settings{{
        {"pairing/size", "7"},
        // Fine, upper and lower case both.
        {"pairing/1/client", good_client},
        {"pairing/1/psk", good_psk},
        // A key a digit short, a key with a non-hex digit, a missing key.
        {"pairing/2/client", std::string(63, 'b')},
        {"pairing/2/psk", good_psk},
        {"pairing/3/client", std::string(63, 'c') + "g"},
        {"pairing/3/psk", good_psk},
        {"pairing/4/client", std::string(64, 'd')},
        // A second record for the first client.
        {"pairing/5/client", good_client},
        {"pairing/5/psk", std::string(64, '0')},
        // Signs and spaces are not hex digits.
        {"pairing/6/client", "+" + std::string(63, 'e')},
        {"pairing/6/psk", good_psk},
        {"pairing/7/client", std::string(64, 'f')},
        {"pairing/7/psk", " " + std::string(63, '1')},
    }};
    const PairingStore store{settings, today};
    REQUIRE(store.records().size() == 1);
    Key32 psk{};
    for (std::size_t i = 0; i < psk.size(); ++i) {
        psk[i] = static_cast<std::uint8_t>((i % 16) * 0x11);
    }
    Key32 client{};
    client.fill(0xAA);
    CHECK(store.records()[0].client_key == client);
    CHECK(store.choose(client).psk == psk);

    // A count that does not read is none; one far past what was written
    // stops at the limit, and what is there still reads.
    const std::vector<std::pair<std::string, std::size_t>> counts{
        {"", 0}, {"x", 0}, {"-1", 0}, {"18446744073709551615", 1}};
    for (const auto& [size, records] : counts) {
        INFO(size);
        MemorySettingsStore odd{{{"pairing/size", size},
                                 {"pairing/1/client", good_client},
                                 {"pairing/1/psk", good_psk}}};
        const PairingStore read{odd, today};
        CHECK(read.records().size() == records);
    }
}

TEST_CASE("pairing store: a client's name reaches its record", "[hearth][pairing-store]") {
    MemorySettingsStore settings;
    PairingStore store{settings, today};
    REQUIRE(store.store_record(key(1), key(101)));
    CHECK(store.records()[0].name.empty());
    store.set_client_name(key(1), "hearth-s3-lounge");
    CHECK(store.records()[0].name == "hearth-s3-lounge");
    CHECK(settings.synced().at("pairing/1/name") == "hearth-s3-lounge");
    // A name heard before the pairing completes waits for it.
    store.set_client_name(key(2), "ac3hearth-testsink-1");
    CHECK(store.records().size() == 1);
    REQUIRE(store.store_record(key(2), key(102)));
    CHECK(store.records()[1].name == "ac3hearth-testsink-1");
}

TEST_CASE("pairing store: sessions look keys up while the window pairs and forgets",
          "[hearth][pairing-store][concurrency]") {
    MemorySettingsStore settings;
    PairingStore store{settings, today};
    std::atomic<bool> running{true};
    std::atomic<std::uint64_t> lookups{0};
    std::vector<std::jthread> sessions;
    for (int thread = 0; thread < 3; ++thread) {
        sessions.emplace_back([&store, &running, &lookups, thread] {
            while (running.load()) {
                const auto choice = store.choose(key(static_cast<std::uint8_t>(thread)));
                static_cast<void>(choice);
                static_cast<void>(store.paired(key(9)));
                lookups.fetch_add(1);
            }
        });
    }
    for (int round = 0; round < 200; ++round) {
        const auto client = key(static_cast<std::uint8_t>(round % 4));
        store.set_client_name(client, "sink " + std::to_string(round));
        REQUIRE(store.store_record(client, key(static_cast<std::uint8_t>(100 + (round % 50)))));
        static_cast<void>(store.records());
        if (round % 3 == 0) {
            store.forget(client);
        }
    }
    running.store(false);
    for (std::jthread& session : sessions) {
        session.join();
    }
    CHECK(lookups.load() > 0);
    MemorySettingsStore reopened{settings.synced()};
    const PairingStore again{reopened, today};
    CHECK(again.records() == store.records());
}
