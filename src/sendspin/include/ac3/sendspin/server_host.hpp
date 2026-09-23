#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/sendspin/ac3forge_player.hpp"
#include "ac3/sendspin/crypto.hpp"
#include "ac3/sendspin/dialect.hpp"
#include "ac3/sendspin/handshake.hpp"
#include "ac3/sendspin/messages.hpp"
#include "ac3/sendspin/noise.hpp"
#include "ac3/sendspin/pairing_flow.hpp"
#include "ac3/sendspin/pairing_messages.hpp"
#include "ac3/sendspin/server_store.hpp"
#include "ac3/sendspin/state_roles.hpp"
#include "ac3/sendspin/stream_roles.hpp"
#include "ac3/sendspin/websocket.hpp"

// A Sendspin server on a computer: every connection to its clients, whichever side dialled, and
// the groups that play to them (planning/hearth-reference-player.md, A4, the server half).
//
// The host listens for clients that dial it and advertises _sendspin-server._tcp, browses for
// players that advertise _sendspin._tcp and dials them, and runs a ServerSession under a
// SessionDriver for each connection. It decides each client's activation from the store: a paired
// client, or an approved one on the Sentinel that offers unpaired access, gets playback with
// player@v1; a client whose token the operator entered is re-handshaken to its pairing PSK and
// paired by it; any other waits with no activities until the operator pairs or approves it. A
// paired client that offers _ac3forge_player@v1 gets that role instead of player@v1
// (planning/hearth-sendspin-extension.md, The role _ac3forge_player@v1).
//
// Groups play one programme to several clients: a member playing player@v1 gets the programme's
// PCM in the first of its formats the group can produce from it (PCM or FLAC at any depth, Opus at
// 48 kHz), and a member playing _ac3forge_player@v1 gets the coded stream's bursts, all on one
// timeline started far enough ahead for the member that needs the most lead.
//
// The other roles are activated by policy (planning/hearth-sendspin-extension.md, Other roles):
// controller@v1, metadata@v1 and color@v1 for any client that lists them, artwork@v1 and
// visualizer@v1 for a client that lists them and is not aiosendspin 9.1.1, and source@v1 only for a
// client the operator has allowed it. A group gives its members' roles the programme's metadata,
// colours, transport, artwork and visualizer frames, and applies a controller's volume and mute to
// its players.
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
    // The URL the host dialled for the client; empty for a client that dialled the host.
    std::string url;
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
    // A playback role is active: player@v1, or _ac3forge_player@v1 when `bursts`.
    bool playing = false;
    bool bursts = false;
    bool available = false;
    std::optional<messages::PlayerState> player_state;
    std::optional<messages::PlayerSupport> player_support;
    std::optional<ac3forge::State> ac3forge_state;
    std::optional<ac3forge::Support> ac3forge_support;
    // The roles the client lists and the roles active on its connection, with the other roles'
    // support and client/state objects.
    std::vector<std::string> supported_roles;
    std::vector<std::string> active_roles;
    std::optional<visualizer::Support> visualizer_support;
    std::optional<source::Support> source_support;
    std::optional<artwork::Channels> artwork_state;
    std::optional<visualizer::State> visualizer_state;
    std::optional<source::State> source_state;
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
    // The client sent client/goodbye, its own reason for the disconnect that follows shortly as
    // on_client_gone(): kAnotherServer when a connection of equal or higher rank displaced this
    // one, kConcurrentAttempt when this one's own activation was rejected because another
    // server's pairing attempt was already in progress on the client (Arbiter's own protection
    // for an attempt underway, arbiter.hpp) - both readable as "something else is using this
    // client" without this host ever seeing that rival connection itself (issue #876). The other
    // reasons (shutdown, restart, user request, unauthorized, pairing required, unpaired) are the
    // client's own choice or this host's, and already knowable from the call that caused them.
    virtual void on_client_goodbye(const std::string& /*client_id*/, messages::GoodbyeReason /*reason*/) {}
    // The pairing attempt with a client waits for the operator's code: ServerHost::enter_code().
    virtual void on_pairing_code_wanted(const std::string& client_id) = 0;
    virtual void on_paired(const std::string& client_id) = 0;
    virtual void on_pairing_ended(const std::string& client_id, std::optional<pairing_messages::AbortReason> reason) = 0;
    virtual void on_log(std::string_view line) = 0;

    // A controller@v1 command from a member of group `group_id` that is the engine's to carry out:
    // play, pause, stop, next, previous, repeat, shuffle and seeks. The group applies volume and
    // mute itself.
    virtual void on_controller_command(const std::string& /*group_id*/, const std::string& /*client_id*/,
                                       const controller::CommandMessage& /*command*/) {}
    // source@v1: a client's input stream began or changed format, one chunk captured from
    // `timestamp_us` on the server clock, and its end.
    virtual void on_source_stream_start(const std::string& /*client_id*/, const messages::ClientStreamStart& /*start*/) {}
    virtual void on_source_audio(const std::string& /*client_id*/, std::int64_t /*timestamp_us*/,
                                 std::span<const std::uint8_t> /*frame*/) {}
    virtual void on_source_stream_end(const std::string& /*client_id*/) {}
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
    // Sends a client playing _ac3forge_player@v1 a command its state lists, settings checked
    // against its support object first (ServerSession::ac3forge_command). False when the client is
    // not connected or the session refuses it.
    bool ac3forge_command(const std::string& client_id, const ac3forge::CommandMessage& command);
    // Approves a client for unpaired access, or withdraws the approval.
    bool approve(const std::string& client_id, bool approved);
    bool unpair(const std::string& client_id);
    // Lets a client that lists source@v1 have the role, which the host activates for no client
    // without this, paired or not (roles/source/v1.md, Unpaired access), or withdraws it.
    bool allow_source(const std::string& client_id, bool allowed);
    // Asks a source to stream, or to stop.
    bool start_source(const std::string& client_id);
    bool stop_source(const std::string& client_id);

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

    struct Programme {
        // The PCM push() takes, for members playing player@v1: interleaved at its bit depth, in
        // 32 bits. Nothing for a programme only members playing _ac3forge_player@v1 can play.
        std::optional<messages::AudioFormat> pcm;
        // The coded stream push_burst() takes, for members playing _ac3forge_player@v1.
        std::optional<ac3forge::StreamStart> bursts;
        // A source that can be read ahead, which may start with more lead.
        bool buffered = false;
    };
    // Starts a programme. With both forms, frame n of the PCM is sample n of the stream as the
    // library decodes it, at the same sample rate.
    bool start(const Programme& programme);
    // Encodes and sends the frames in `interleaved`, or none while it is too early for them or a
    // member's player holds enough, so a caller paces a buffered source by trying again shortly.
    // Returns the number of frames taken.
    [[nodiscard]] std::size_t push(std::span<const std::int32_t> interleaved);

    struct Burst {
        // The burst's Pc and Pd as ac3::iec61937 writes them, and the elementary-stream bytes they
        // describe.
        std::uint16_t pc = 0;
        std::uint16_t pd = 0;
        std::span<const std::uint8_t> payload;
        // The programme frame that is the burst's first decoded sample
        // (planning/hearth-sendspin-extension.md, Timing).
        std::int64_t frame = 0;
    };
    // Sends one burst, of 1,536 samples, to every member playing _ac3forge_player@v1; false, taking
    // nothing, on the same terms as push().
    [[nodiscard]] bool push_burst(const Burst& burst);
    // Ends the programme: the last units, then stream/end.
    void stop();

    // What the group shows to members with the other roles. Each reaches the members whose role is
    // active now, and a member when it joins or its role becomes active; a member that leaves, or is
    // left when the group goes, has its state roles cleared and its artwork and visualizer streams
    // ended.
    //
    // metadata@v1's state.
    void set_metadata(std::optional<metadata::State> state);
    // color@v1's state, moved to the contrast the role requires (color::with_contrast) first.
    void set_colors(std::optional<color::State> state);

    // What the engine can do with the programme, for controller@v1: the commands it carries out
    // among play, pause, stop, next, previous, the repeat and shuffle commands and the seeks, with
    // its repeat, shuffle and seek range. The group adds volume and mute while a member supports
    // them, with the group volume and mute its players give, and carries out those two itself.
    struct Transport {
        std::vector<controller::Command> commands;
        controller::Repeat repeat = controller::Repeat::kOff;
        bool shuffle = false;
        std::optional<std::int64_t> seek_max_ms;
    };
    void set_transport(std::optional<Transport> transport);

    // artwork@v1: the image for a channel's source, encoded in its format at exactly its size,
    // scaled to fit and padded with black, never cropped (roles/artwork/v1.md); nothing when there
    // is none. Called with the group's lock held, on whichever thread set or needs the image.
    using ArtworkImage =
        std::function<std::optional<std::vector<std::uint8_t>>(artwork::Source, artwork::Format, std::int32_t, std::int32_t)>;
    // The programme's artwork from `timestamp_us` on the server clock; an empty function clears it.
    void set_artwork(std::int64_t timestamp_us, ArtworkImage image);

    // visualizer@v1: the types the engine analyses the programme for, at up to `rate_max` frames a
    // second, and whether its beats mark downbeats; then each frame, to the members that asked for
    // its type. A frame a member's stream cannot take now is not sent to it.
    void set_visualizer(std::vector<visualizer::Type> types, std::int32_t rate_max, bool tracks_downbeats);
    void push_visualizer(const visualizer::Frame& frame);

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
