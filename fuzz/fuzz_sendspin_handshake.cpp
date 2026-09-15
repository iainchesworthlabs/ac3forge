#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>

#include "ac3/sendspin/handshake.hpp"

// The handshake phase's message parsers (src/sendspin/src/handshake.cpp): what a
// server reads from a client that has not authenticated at all (client/init),
// what a client reads before the Noise handshake binds anything (server/init,
// server/error, noise/handshake), and the two payloads decrypted from the Noise
// messages. Every parser sees the same input.
//
// Whatever parses must write back out and parse to the same value, so the writer
// and reader agree on each message.

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    namespace hs = ac3::sendspin::handshake;
    const std::string_view text{reinterpret_cast<const char*>(data), size};
    const std::span<const std::uint8_t> bytes{data, size};

    if (const auto init = hs::parse_client_init(text)) {
        const auto again = hs::parse_client_init(hs::write_client_init(*init));
        if (!again || again->client_key != init->client_key || again->suite != init->suite) {
            std::abort();
        }
    }
    if (const auto init = hs::parse_server_init(text)) {
        const auto again = hs::parse_server_init(hs::write_server_init(*init));
        if (!again || again->server_key != init->server_key) {
            std::abort();
        }
    }
    if (const auto error = hs::parse_server_error(text)) {
        if (hs::parse_server_error(hs::write_server_error(*error)) != error) {
            std::abort();
        }
    }
    if (const auto message = hs::parse_noise_handshake(text)) {
        if (hs::parse_noise_handshake(hs::write_noise_handshake(*message)) != message) {
            std::abort();
        }
    }
    if (const auto reference = hs::parse_message_1_payload(bytes)) {
        if (reference->category) {
            const std::string written = hs::write_message_1_payload(reference->psk_id, *reference->category);
            const auto again = hs::parse_message_1_payload(
                std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(written.data()),
                                              written.size()));
            if (!again || again->psk_id != reference->psk_id || again->category != reference->category) {
                std::abort();
            }
        }
    }
    (void)hs::parse_message_2_payload(bytes);
    return 0;
}
