#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/sendspin/crypto.hpp"
#include "ac3/sendspin/dialect.hpp"
#include "ac3/sendspin/handshake.hpp"
#include "ac3/sendspin/messages.hpp"
#include "ac3/sendspin/noise.hpp"
#include "ac3/sendspin/pairing_flow.hpp"
#include "ac3/sendspin/pairing_messages.hpp"
#include "ac3/sendspin/server_store.hpp"
#include "ac3/sendspin/websocket.hpp"

// A Sendspin server on a computer: every connection to its clients, whichever side dialled, and
// the groups that play to them (planning/hearth-reference-player.md, A4, the server half).
//
// The host listens for clients that dial it and advertises _sendspin-server._tcp, browses for
// players that advertise _sendspin._tcp and dials them, and runs a ServerSession under a
// SessionDriver for each connection. It decides each client's activation from the store: a paired
// client, or an approved one on the Sentinel that offers unpaired access, gets playback with
// player@v1; a client whose token the operator entered is re-handshaken to its pairing PSK and
// paired by it; any other waits with no activities until the operator pairs or approves it.
//
// Groups play one programme to several clients: each member gets the programme in the first of its
// formats the group can produce from the source (PCM or FLAC at any depth, Opus at 48 kHz), on one
// timeline started far enough ahead for the member that needs the most lead.
//
// Thread-safe. Events arrive on the host's own thread.

namespace ac3::sendspin {

struct ServerHostOptions {
    noise::KeyPair identity;
    std::string name = "Hearth";
    std::vector<std::string> languages{"en"};
    std::string address = "0.0.0.0";
    // The port to listen on for clients that dial: 8927 by default, 0 for any, none for none.
    std::optional<std::uint16_t> port = transport::websocket::kServerPort;
    bool advertise = true;
    bool browse = true;
    // The IPv4 interfaces to advertise and browse on; empty for every one.
    std::vector<std::string> mdns_interfaces;
};

// What the host knows of one connected client.
struct ClientView {
    crypto::Key32 client_key{};
    std::string client_id;
    std::string name;
    std::string peer;
    Dialect dialect = Dialect::kSpecification;
    handshake::PskCategory psk = handshake::PskCategory::kSentinel;
    // A client the store holds a record for, that could not use it (connection.md, Sentinel
    // Fallback): it needs pairing again.
    bool credential_mismatch = false;
    bool hello = false;
    bool offers_unpaired_access = false;
    std::vector<messages::PairMethod> pair_methods;
    bool pairing = false;
    bool wants_code = false;
    bool playing = false;
    bool available = false;
    std::optional<messages::PlayerState> player_state;
    std::optional<messages::PlayerSupport> player_support;
};

class ServerHostEvents {
   public:
    ServerHostEvents() = default;
    virtual ~ServerHostEvents() = default;
    ServerHostEvents(const ServerHostEvents&) = delete;
    ServerHostEvents& operator=(const ServerHostEvents&) = delete;
    ServerHostEvents(ServerHostEvents&&) = delete;
    ServerHostEvents& operator=(ServerHostEvents&&) = delete;

    // A client connected, or what the host knows of it changed.
    virtual void on_client(const ClientView& client) = 0;
    virtual void on_client_gone(const std::string& client_id) = 0;
    // The pairing attempt with a client waits for the operator's code: ServerHost::enter_code().
    virtual void on_pairing_code_wanted(const std::string& client_id) = 0;
    virtual void on_paired(const std::string& client_id) = 0;
    virtual void on_pairing_ended(const std::string& client_id, std::optional<pairing_messages::AbortReason> reason) = 0;
    virtual void on_log(std::string_view line) = 0;
};

class Group;

class ServerHost {
   public:
    [[nodiscard]] static std::expected<std::unique_ptr<ServerHost>, std::string> start(ServerHostOptions options,
                                                                                       ServerStore& store,
                                                                                       ServerHostEvents& events);
    ~ServerHost();
    ServerHost(const ServerHost&) = delete;
    ServerHost& operator=(const ServerHost&) = delete;
    ServerHost(ServerHost&&) = delete;
    ServerHost& operator=(ServerHost&&) = delete;

    [[nodiscard]] std::optional<std::uint16_t> port() const;
    [[nodiscard]] std::string server_id() const;

    // Dials a client at a ws:// URL, on the host's thread.
    void dial(const std::string& url);

    [[nodiscard]] std::vector<ClientView> clients() const;
    [[nodiscard]] std::optional<ClientView> client(const std::string& client_id) const;

    // The operator entered a client's SP:0 token: the client pairs by its pairing PSK as soon as
    // it is connected. False for text that is not such a token.
    bool enter_pairing_token(std::string_view token);
    // Pairs a connected client by a code method it offers.
    bool pair(const std::string& client_id, messages::PairMethod method, std::optional<messages::CodeFormat> format);
    bool enter_code(const std::string& client_id, const pairing_flow::Code& code);
    bool cancel_pairing(const std::string& client_id);
    // Approves a client for unpaired access, or withdraws the approval.
    bool approve(const std::string& client_id, bool approved);
    bool unpair(const std::string& client_id);

    [[nodiscard]] std::shared_ptr<Group> make_group(std::string name);

   private:
    friend class HostConnection;
    friend class HostBrowseListener;
    friend class Group;
    struct State;
    explicit ServerHost(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

// One programme to several clients on one timeline. A group must not outlive its host.
class Group {
   public:
    ~Group();
    Group(const Group&) = delete;
    Group& operator=(const Group&) = delete;
    Group(Group&&) = delete;
    Group& operator=(Group&&) = delete;

    [[nodiscard]] const std::string& id() const;

    // A client joins; it starts receiving at the next audio pushed once it can play.
    void add(const std::string& client_id);
    void remove(const std::string& client_id);

    // Starts a programme whose samples push() takes: interleaved at `source`'s bit depth, in 32
    // bits. `buffered` for a source that can be read ahead, which may start with more lead.
    bool start(const messages::AudioFormat& source, bool buffered);
    // Encodes and sends as many of the frames in `interleaved` as fit: none while a member's
    // player holds enough, so a caller paces a buffered source by trying again shortly. Returns
    // the number of frames taken.
    [[nodiscard]] std::size_t push(std::span<const std::int32_t> interleaved);
    // Ends the programme: the last units, then stream/end.
    void stop();

    // For tests and the engine: when the first frame plays, on the server clock, once started.
    [[nodiscard]] std::optional<std::int64_t> start_time() const;
    [[nodiscard]] std::size_t members_playing() const;

   private:
    friend class ServerHost;
    struct State;
    explicit Group(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
};

}  // namespace ac3::sendspin
