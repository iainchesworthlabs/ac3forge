#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/sendspin/dialect.hpp"
#include "ac3/sendspin/json.hpp"
#include "ac3/sendspin/messages.hpp"

// The core messages' readers (src/sendspin/src/messages.cpp): what a server reads from a
// client once the handshake is done (client/hello, client/state, client/time,
// client/goodbye) and what a player reads from a server (server/hello, server/activate,
// server/time, server/command, stream/start, stream/clear, stream/end, group/update), each
// in both dialects. Every reader sees the payload of every input, whatever its type says.
//
// Whatever reads must write back out, and that text must read and write back to itself: one
// pass settles what the writer leaves out, such as an activity or a codec it does not
// recognise, and after it the writer and reader agree. A second read that fails is not a
// finding, since the first read can hold values no writer is meant to be given, such as a
// pairing activation whose method it did not recognise.

namespace {

namespace m = ac3::sendspin::messages;
namespace json = ac3::sendspin::json;
using ac3::sendspin::Dialect;

// Reads `text` as a message and hands its payload to `read`, which returns an expected.
template <class Read>
[[nodiscard]] auto read_text(const std::string& text, const Read& read) {
    std::vector<json::Token> tokens;
    json::Document document;
    using Result = decltype(read(json::Value{}));
    if (!document.parse(text, tokens, 4096)) {
        return Result(std::unexpected(m::MessageError::kMalformed));
    }
    const auto envelope = m::read_envelope(document);
    if (!envelope) {
        return Result(std::unexpected(m::MessageError::kMalformed));
    }
    return read(envelope->payload);
}

// The settling check for one message type.
template <class Read, class Write>
void settle(json::Value payload, const Read& read, const Write& write) {
    const auto first = read(payload);
    if (!first) {
        return;
    }
    const std::string written = write(*first);
    const auto second = read_text(written, read);
    if (second && write(*second) != written) {
        std::abort();
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text{reinterpret_cast<const char*>(data), size};
    std::vector<json::Token> tokens;
    json::Document document;
    if (!document.parse(text, tokens, 4096)) {
        return 0;
    }
    const auto envelope = m::read_envelope(document);
    if (!envelope) {
        return 0;
    }
    const json::Value payload = envelope->payload;
    (void)m::client_hello_dialect(payload);

    settle(payload, m::read_server_hello, m::write_server_hello);
    settle(payload, m::read_client_time, m::write_client_time);
    settle(payload, m::read_server_time, m::write_server_time);
    settle(payload, m::read_stream_start, m::write_stream_start);
    settle(payload, m::read_stream_clear, m::write_stream_clear);
    settle(payload, m::read_stream_end, m::write_stream_end);
    settle(payload, m::read_client_goodbye, m::write_client_goodbye);

    for (const Dialect dialect : {Dialect::kSpecification, Dialect::kAiosendspin911}) {
        settle(
            payload, [dialect](json::Value p) { return m::read_client_hello(p, dialect); },
            [dialect](const m::ClientHello& hello) { return m::write_client_hello(hello, dialect); });
        settle(
            payload, [dialect](json::Value p) { return m::read_activate(p, dialect); },
            [dialect](const m::Activate& activate) { return m::write_activate(activate, dialect); });
        settle(
            payload, [dialect](json::Value p) { return m::read_client_state(p, dialect); },
            [dialect](const m::ClientState& state) { return m::write_client_state(state, dialect); });
        settle(
            payload, [dialect](json::Value p) { return m::read_server_command(p, dialect); },
            [dialect](const m::ServerCommand& command) { return m::write_server_command(command, dialect); });
        settle(
            payload, [dialect](json::Value p) { return m::read_group_update(p, dialect); },
            [](const m::GroupUpdate& update) { return m::write_group_update(update); });
    }
    return 0;
}
