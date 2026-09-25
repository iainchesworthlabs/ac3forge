#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

#include "ac3/sendspin/noise.hpp"
#include "server_identity.hpp"
#include "settings_model.hpp"

// This computer's Sendspin server identity, kept in the settings
// (apps/hearth/engine/server_identity.hpp): made once, then the same on every
// start, so a sink's pairing record - bound to the identity that made it - still
// matches after a restart.

using ac3::hearth::kServerIdentityKey;
using ac3::hearth::load_or_make_server_identity;
using ac3::hearth::MemorySettingsStore;

TEST_CASE("server identity: made once, then read back the same on the next start", "[hearth][server-identity]") {
    MemorySettingsStore first;
    const auto made = load_or_make_server_identity(first);
    REQUIRE(made.has_value());
    // Written through to what a later start reads.
    REQUIRE(first.synced().contains(std::string(kServerIdentityKey)));
    CHECK(first.synced().at(std::string(kServerIdentityKey)).size() == 64);

    // A later start, reading what the first wrote.
    MemorySettingsStore later(first.synced());
    const auto again = load_or_make_server_identity(later);
    REQUIRE(again.has_value());
    CHECK(again->public_key() == made->public_key());
    CHECK(again->private_key() == made->private_key());
}

TEST_CASE("server identity: a value that does not read as a key is replaced", "[hearth][server-identity]") {
    for (const std::string& damaged :
         std::vector<std::string>{"", "not hex at all", std::string(63, 'a'), std::string(64, 'g')}) {
        MemorySettingsStore store({{std::string(kServerIdentityKey), damaged}});
        const auto made = load_or_make_server_identity(store);
        REQUIRE(made.has_value());
        const std::string kept = store.synced().at(std::string(kServerIdentityKey));
        CHECK(kept != damaged);
        CHECK(kept.size() == 64);
    }
}

TEST_CASE("server identity: a store that will not save still gives this run an identity",
          "[hearth][server-identity]") {
    MemorySettingsStore store;
    store.set_sync_fails(true);
    const auto made = load_or_make_server_identity(store);
    REQUIRE(made.has_value());
    CHECK_FALSE(store.synced().contains(std::string(kServerIdentityKey)));
}

TEST_CASE("server identity: two stores make two identities", "[hearth][server-identity]") {
    MemorySettingsStore one;
    MemorySettingsStore other;
    const auto a = load_or_make_server_identity(one);
    const auto b = load_or_make_server_identity(other);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(a->public_key() != b->public_key());
}
