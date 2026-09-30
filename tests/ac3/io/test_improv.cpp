// ac3forge/improv.hpp: the Improv Wi-Fi serial packet format, on the host.
//
// The board this runs on has no serial client attached in CI, and the bytes
// are the whole of the interoperability: a checksum computed over the wrong
// span, or a length byte read from the wrong place, is a board that ignores
// every browser that tries to provision it. So the packets are built and
// taken apart here, byte by byte, the way tests/io/test_interleave.cpp does
// for the slot conversions.

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac3forge/improv.hpp"

namespace {

namespace improv = ac3forge::improv;

// A packet as a client would send it: header, version, type, length, data,
// and the checksum over everything before it.
std::vector<std::uint8_t> packet(improv::PacketType type, std::vector<std::uint8_t> data) {
    std::vector<std::uint8_t> bytes(improv::kHeader.begin(), improv::kHeader.end());
    bytes.push_back(improv::kVersion);
    bytes.push_back(static_cast<std::uint8_t>(type));
    bytes.push_back(static_cast<std::uint8_t>(data.size()));
    bytes.insert(bytes.end(), data.begin(), data.end());
    bytes.push_back(improv::checksum(bytes));
    return bytes;
}

// The data half of a wifi_settings command: the command, its own length, then
// each string with a length byte.
std::vector<std::uint8_t> wifi_settings(std::string_view ssid, std::string_view password) {
    std::vector<std::uint8_t> body;
    body.push_back(static_cast<std::uint8_t>(ssid.size()));
    body.insert(body.end(), ssid.begin(), ssid.end());
    body.push_back(static_cast<std::uint8_t>(password.size()));
    body.insert(body.end(), password.begin(), password.end());

    std::vector<std::uint8_t> data{static_cast<std::uint8_t>(improv::Command::wifi_settings),
                                   static_cast<std::uint8_t>(body.size())};
    data.insert(data.end(), body.begin(), body.end());
    return data;
}

// Every byte through the reader, returning whatever it made of them.
std::optional<improv::Rpc> read(improv::Reader& reader, const std::vector<std::uint8_t>& bytes) {
    std::optional<improv::Rpc> got;
    for (const std::uint8_t byte : bytes) {
        if (auto rpc = reader.feed(byte)) {
            got = rpc;
        }
    }
    return got;
}

}  // namespace

TEST_CASE("a wifi settings command arrives with its two strings", "[io][improv]") {
    improv::Reader reader;
    const auto got = read(reader, packet(improv::PacketType::rpc, wifi_settings("kitchen", "hunter2")));
    REQUIRE(got.has_value());
    CHECK(got->command == improv::Command::wifi_settings);
    CHECK(got->ssid == "kitchen");
    CHECK(got->password == "hunter2");
    CHECK(reader.dropped() == 0);
}

TEST_CASE("an empty password is a password", "[io][improv]") {
    // An open network. The length byte is there and zero, which is not the
    // same as the string being absent.
    improv::Reader reader;
    const auto got = read(reader, packet(improv::PacketType::rpc, wifi_settings("guest", "")));
    REQUIRE(got.has_value());
    CHECK(got->ssid == "guest");
    CHECK(got->password.empty());
}

TEST_CASE("a command with no strings of its own still arrives", "[io][improv]") {
    improv::Reader reader;
    const auto got = read(reader, packet(improv::PacketType::rpc,
                                         {static_cast<std::uint8_t>(improv::Command::device_info), 0}));
    REQUIRE(got.has_value());
    CHECK(got->command == improv::Command::device_info);
    CHECK(got->ssid.empty());
}

TEST_CASE("junk before a packet is skipped, and a false start does not eat the real one",
          "[io][improv]") {
    improv::Reader reader;
    std::vector<std::uint8_t> bytes{0x00, 0xFF, 'I', 'I', 'M'};  // 'I' twice: the second starts it
    const auto real = packet(improv::PacketType::rpc, wifi_settings("attic", "p"));
    bytes.insert(bytes.end(), real.begin(), real.end());
    const auto got = read(reader, bytes);
    REQUIRE(got.has_value());
    CHECK(got->ssid == "attic");
    CHECK(reader.dropped() == 0);
}

TEST_CASE("a packet whose checksum does not add up is dropped and counted", "[io][improv]") {
    improv::Reader reader;
    auto bytes = packet(improv::PacketType::rpc, wifi_settings("kitchen", "hunter2"));
    bytes.back() ^= 0xFF;
    CHECK_FALSE(read(reader, bytes).has_value());
    CHECK(reader.dropped() == 1);

    // And the reader is ready for the next one rather than stuck mid-packet.
    const auto got = read(reader, packet(improv::PacketType::rpc, wifi_settings("kitchen", "x")));
    REQUIRE(got.has_value());
    CHECK(got->password == "x");
}

TEST_CASE("a wifi settings payload that lies about its lengths is dropped", "[io][improv]") {
    improv::Reader reader;
    // An SSID length past the end of the command's own data.
    std::vector<std::uint8_t> data{static_cast<std::uint8_t>(improv::Command::wifi_settings), 3,
                                   200, 'a', 'b'};
    CHECK_FALSE(read(reader, packet(improv::PacketType::rpc, data)).has_value());
    CHECK(reader.dropped() == 1);
}

TEST_CASE("a packet that is not an RPC is not one to act on", "[io][improv]") {
    improv::Reader reader;
    // A device's own current-state packet, looped back: right shape, wrong
    // direction.
    CHECK_FALSE(read(reader, packet(improv::PacketType::current_state, {0x02})).has_value());
    CHECK(reader.dropped() == 1);
}

TEST_CASE("the state and error packets are the bytes the specification lists", "[io][improv]") {
    std::array<std::uint8_t, improv::kMaxPacket> out{};
    const std::size_t written = improv::write_state(improv::State::provisioned, out);
    REQUIRE(written == improv::kOverhead + 1);
    CHECK(std::string_view(reinterpret_cast<const char*>(out.data()), 6) == "IMPROV");
    CHECK(out[6] == 1);     // version
    CHECK(out[7] == 0x01);  // current state
    CHECK(out[8] == 1);     // one byte of data
    CHECK(out[9] == 0x04);  // provisioned
    CHECK(out[10] == improv::checksum(std::span<const std::uint8_t>(out.data(), 10)));

    const std::size_t error = improv::write_error(improv::Error::cannot_connect, out);
    REQUIRE(error == improv::kOverhead + 1);
    CHECK(out[7] == 0x02);  // error state
    CHECK(out[9] == 0x03);  // unable to connect
}

TEST_CASE("an RPC result carries the command and its strings, each with a length",
          "[io][improv]") {
    std::array<std::uint8_t, improv::kMaxPacket> out{};
    const std::array<std::string_view, 1> strings{"http://10.0.0.7/"};
    const std::size_t written = improv::write_result(improv::Command::wifi_settings, strings, out);
    REQUIRE(written > 0);
    CHECK(out[7] == 0x04);  // rpc result
    CHECK(out[9] == static_cast<std::uint8_t>(improv::Command::wifi_settings));
    CHECK(out[10] == 1 + strings[0].size());  // the command's own data length
    CHECK(out[11] == strings[0].size());
    CHECK(std::string_view(reinterpret_cast<const char*>(out.data() + 12), strings[0].size()) ==
          strings[0]);
    CHECK(out[written - 1] ==
          improv::checksum(std::span<const std::uint8_t>(out.data(), written - 1)));
}

TEST_CASE("a packet that will not fit the caller's buffer writes nothing", "[io][improv]") {
    std::array<std::uint8_t, 4> tiny{};
    CHECK(improv::write_state(improv::State::ready, tiny) == 0);
    CHECK(tiny[0] == 0);
}
