// Improv Wi-Fi, serial transport: the packet format, and nothing else.
//
// https://www.improv-wifi.com/serial/ - a client (a browser page over Web
// Serial, or the ESPHome tooling) hands a device its Wi-Fi credentials over
// the same serial port the console uses. It is how a Hearth sink joins a
// network without being rebuilt: planning/hearth-reference-player.md, B2.
//
// Header only, free of ESP-IDF and of this project's own audio types, for the
// same reason ac3forge/interleave.hpp and ac3forge/sink_plan.hpp are: the
// arithmetic is exactly the kind a host test can hold to a byte, and the
// peripheral around it is not. tests/io/test_improv.cpp builds it on the host.
//
// A packet is
//
//   'I' 'M' 'P' 'R' 'O' 'V'   the header, six bytes
//   version                   1
//   type                      PacketType below
//   length                    of the data that follows, 0-255
//   data                      `length` bytes
//   checksum                  one byte
//
// THE CHECKSUM RULE IS NOT IN THE SPECIFICATION. The page above lists the
// byte and says nothing about how it is computed. Every implementation this
// has to talk to - ESPHome's improv_serial component and the improv-wifi
// Arduino library - sums every byte from the header through the last data
// byte, modulo 256, so that is what this does; a client that disagreed would
// reject the first packet, which is what the board run in B2's exit checks.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace ac3forge::improv {

inline constexpr std::array<std::uint8_t, 6> kHeader{'I', 'M', 'P', 'R', 'O', 'V'};
inline constexpr std::uint8_t kVersion = 1;
// Header, version, type, length, checksum - what a packet costs beyond its data.
inline constexpr std::size_t kOverhead = kHeader.size() + 4;
inline constexpr std::size_t kMaxData = 255;
inline constexpr std::size_t kMaxPacket = kOverhead + kMaxData;

enum class PacketType : std::uint8_t {
    current_state = 0x01,  // device to client
    error_state = 0x02,    // device to client
    rpc = 0x03,            // client to device
    rpc_result = 0x04,     // device to client
};

enum class State : std::uint8_t {
    stopped = 0x00,
    ready = 0x02,  // authorized, waiting to be told a network
    provisioning = 0x03,
    provisioned = 0x04,
};

enum class Error : std::uint8_t {
    none = 0x00,
    invalid_packet = 0x01,
    unknown_command = 0x02,
    cannot_connect = 0x03,
    bad_hostname = 0x05,
    unknown = 0xFF,
};

enum class Command : std::uint8_t {
    wifi_settings = 0x01,
    current_state = 0x02,
    device_info = 0x03,
    scan = 0x04,
    hostname = 0x05,
    device_name = 0x06,
    network_state = 0x07,
};

// One command as it arrived. `ssid` and `password` point into the reader's own
// buffer and are valid until the next byte is fed to it; they are empty for
// every command but wifi_settings.
struct Rpc {
    Command command{};
    std::string_view ssid;
    std::string_view password;
};

[[nodiscard]] inline std::uint8_t checksum(std::span<const std::uint8_t> bytes) {
    unsigned sum = 0;
    for (const std::uint8_t byte : bytes) {
        sum += byte;
    }
    return static_cast<std::uint8_t>(sum & 0xFFU);
}

// Bytes in, one RPC out. Fixed storage, no allocation, and no way to be left
// half-consumed by a client that stops mid-packet: anything that is not a
// packet slides out of the buffer as the next byte arrives.
class Reader {
   public:
    // Returns the command once its last byte has arrived, and nothing until
    // then. A packet with a bad checksum, an unknown version or a length the
    // buffer cannot hold is dropped, and `dropped()` counts it - a caller that
    // wants to answer with Error::invalid_packet can watch that count.
    [[nodiscard]] std::optional<Rpc> feed(std::uint8_t byte) {
        if (filled_ < kHeader.size()) {
            // Still matching the header: a byte that does not continue it
            // starts the match again, so 'I','I','M','P',... still works.
            if (byte == kHeader[filled_]) {
                buffer_[filled_++] = byte;
            } else {
                filled_ = (byte == kHeader[0]) ? 1 : 0;
                buffer_[0] = kHeader[0];
            }
            return std::nullopt;
        }
        buffer_[filled_++] = byte;
        if (filled_ < kOverhead - 1) {  // version, type and length not all in yet
            return std::nullopt;
        }
        const std::size_t length = buffer_[kHeader.size() + 2];
        if (filled_ < kOverhead + length) {
            return std::nullopt;
        }
        const auto packet = std::span<const std::uint8_t>(buffer_.data(), filled_);
        filled_ = 0;
        return decode(packet);
    }

    [[nodiscard]] std::size_t dropped() const { return dropped_; }

   private:
    [[nodiscard]] std::optional<Rpc> decode(std::span<const std::uint8_t> packet) {
        const std::size_t length = packet[kHeader.size() + 2];
        if (checksum(packet.first(packet.size() - 1)) != packet.back() ||
            packet[kHeader.size()] != kVersion ||
            packet[kHeader.size() + 1] != static_cast<std::uint8_t>(PacketType::rpc)) {
            ++dropped_;
            return std::nullopt;
        }
        const auto data = packet.subspan(kOverhead - 1, length);
        if (data.size() < 2) {
            ++dropped_;
            return std::nullopt;
        }
        Rpc rpc{};
        rpc.command = static_cast<Command>(data[0]);
        // data[1] is the command's own data length, which must fit what the
        // packet said: a client that disagrees with itself is not one to act
        // on.
        const std::size_t inner = data[1];
        if (inner + 2 > data.size()) {
            ++dropped_;
            return std::nullopt;
        }
        if (rpc.command == Command::wifi_settings) {
            const auto body = data.subspan(2, inner);
            if (body.empty()) {
                ++dropped_;
                return std::nullopt;
            }
            const std::size_t ssid_length = body[0];
            if (1 + ssid_length >= body.size()) {
                ++dropped_;
                return std::nullopt;
            }
            const std::size_t password_length = body[1 + ssid_length];
            if (2 + ssid_length + password_length > body.size()) {
                ++dropped_;
                return std::nullopt;
            }
            rpc.ssid = std::string_view(reinterpret_cast<const char*>(body.data() + 1), ssid_length);
            rpc.password = std::string_view(
                reinterpret_cast<const char*>(body.data() + 2 + ssid_length), password_length);
        }
        return rpc;
    }

    std::array<std::uint8_t, kMaxPacket> buffer_{};
    std::size_t filled_ = 0;
    std::size_t dropped_ = 0;
};

// The three packets a device sends. Each writes into the caller's buffer and
// returns how many bytes it wrote, or 0 if the buffer is too small - nothing
// is written in that case.
[[nodiscard]] inline std::size_t write_packet(PacketType type, std::span<const std::uint8_t> data,
                                              std::span<std::uint8_t> out) {
    if (data.size() > kMaxData || out.size() < kOverhead + data.size()) {
        return 0;
    }
    std::size_t at = 0;
    for (const std::uint8_t byte : kHeader) {
        out[at++] = byte;
    }
    out[at++] = kVersion;
    out[at++] = static_cast<std::uint8_t>(type);
    out[at++] = static_cast<std::uint8_t>(data.size());
    for (const std::uint8_t byte : data) {
        out[at++] = byte;
    }
    out[at] = checksum(out.first(at));
    return at + 1;
}

[[nodiscard]] inline std::size_t write_state(State state, std::span<std::uint8_t> out) {
    const std::array<std::uint8_t, 1> data{static_cast<std::uint8_t>(state)};
    return write_packet(PacketType::current_state, data, out);
}

[[nodiscard]] inline std::size_t write_error(Error error, std::span<std::uint8_t> out) {
    const std::array<std::uint8_t, 1> data{static_cast<std::uint8_t>(error)};
    return write_packet(PacketType::error_state, data, out);
}

// An RPC result: the command being answered, then the strings, each with its
// own length byte. The first string of a wifi_settings result is the URL a
// client sends the user to - this device's own web page.
[[nodiscard]] inline std::size_t write_result(Command command,
                                              std::span<const std::string_view> strings,
                                              std::span<std::uint8_t> out) {
    std::array<std::uint8_t, kMaxData> data{};
    std::size_t at = 2;  // command and its data length, filled in below
    for (const std::string_view text : strings) {
        if (text.size() > 0xFF || at + 1 + text.size() > data.size()) {
            return 0;
        }
        data[at++] = static_cast<std::uint8_t>(text.size());
        for (const char c : text) {
            data[at++] = static_cast<std::uint8_t>(c);
        }
    }
    data[0] = static_cast<std::uint8_t>(command);
    data[1] = static_cast<std::uint8_t>(at - 2);
    return write_packet(PacketType::rpc_result, std::span<const std::uint8_t>(data.data(), at), out);
}

}  // namespace ac3forge::improv
