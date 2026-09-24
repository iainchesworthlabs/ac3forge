#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ac3/render/layout.hpp"
#include "ac3/sendspin/arbiter.hpp"
#include "ac3/sendspin/discovery.hpp"
#include "ac3/sendspin/messages.hpp"
#include "ac3/sendspin/pairing_flow.hpp"
#include "ac3/sendspin/session.hpp"
#include "ac3/sendspin/session_driver.hpp"
#include "ac3/sendspin/state_roles.hpp"
#include "ac3/sendspin/stream_roles.hpp"
#include "ac3/sendspin/websocket.hpp"
#include "store.hpp"

// ac3hearth-testsink: src/sendspin's player half as a program (planning/hearth-reference-player.md,
// The test sink). A Sendspin client that waits for servers: it listens on a WebSocket, advertises
// _sendspin._tcp, pairs by its pairing PSK and a dynamic or static code, admits servers as the
// specification ranks them, and decodes each stream it plays to a WAV file with a play-time log:
// PCM, FLAC or Opus over player@v1, and AC-3 or E-AC-3, objects included, over _ac3forge_player@v1
// (planning/hearth-sendspin-extension.md), rendered to its speaker layout. It can list the other
// roles too, keeping what a server sends them and sending controller commands.
//
// Several sinks run side by side in one process or several, with distinct names, ports and state
// directories.

namespace ac3::hearth::testsink {

enum class CodeMethod : std::uint8_t {
    kNone,
    kDynamic,
    kStatic,
};

struct SinkOptions {
    std::string name = "Hearth test sink";
    std::string address = "0.0.0.0";
    std::uint16_t port = sendspin::transport::websocket::kClientPort;
    // The identity, pairing PSK and pairing records.
    std::filesystem::path state_directory;
    // WAV files and play-time logs; empty to count what is played and write nothing.
    std::filesystem::path output_directory;
    bool unpaired_access = false;
    // The codecs offered, most preferred first: each as stereo at 48 kHz 16-bit, and PCM and FLAC
    // also at 44.1 kHz 16-bit and 48 kHz 24-bit.
    std::vector<sendspin::messages::Codec> codecs{sendspin::messages::Codec::kPcm, sendspin::messages::Codec::kFlac,
                                                  sendspin::messages::Codec::kOpus};
    // Offer _ac3forge_player@v1 before player@v1, as a Hearth sink does.
    bool extension_role = true;
    // Lists the Settings command in the extension role's state and applies
    // a settings command by reporting its revision (logged "settings N
    // applied") - for a server-side test of a sink that takes settings.
    // Off by default: the sink manages nothing (see Sink's config).
    bool accept_settings = false;
    // The speaker layout the extension role's streams are rendered to, in ac3::render::OutputLayout's
    // grammar.
    std::string layout = "7.1.4";
    // Roles beyond the playback roles to list, for testing a server's: any of controller@v1,
    // metadata@v1, color@v1, artwork@v1 and visualizer@v1. What they receive is kept (Sink::roles).
    std::vector<std::string> other_roles;
    // artwork@v1's channels, and visualizer@v1's request, when those roles are listed.
    sendspin::artwork::Channels artwork_channels;
    sendspin::visualizer::State visualizer_request;
    CodeMethod code_method = CodeMethod::kDynamic;
    // Eight digits, for CodeMethod::kStatic.
    std::string static_code;
    bool advertise = true;
    // The IPv4 interfaces to advertise on; empty for every one.
    std::vector<std::string> mdns_interfaces;
    std::size_t max_connections = 4;
};

// What a sink reports, one line at a time, from any of its threads.
class SinkLog {
   public:
    SinkLog() = default;
    virtual ~SinkLog() = default;
    SinkLog(const SinkLog&) = delete;
    SinkLog& operator=(const SinkLog&) = delete;
    SinkLog(SinkLog&&) = delete;
    SinkLog& operator=(SinkLog&&) = delete;

    virtual void line(std::string_view text) = 0;
};

class Connection;

class Sink {
   public:
    [[nodiscard]] static std::expected<std::unique_ptr<Sink>, std::string> start(SinkOptions options, SinkLog& log);
    ~Sink();
    Sink(const Sink&) = delete;
    Sink& operator=(const Sink&) = delete;
    Sink(Sink&&) = delete;
    Sink& operator=(Sink&&) = delete;

    [[nodiscard]] std::uint16_t port() const;
    // client_id, and the SP:0 token that pairs this sink by its pairing PSK.
    [[nodiscard]] std::string client_id() const;
    [[nodiscard]] std::string pairing_token() const;

    // The operator's actions on the device: a gesture opening the static code's window, a reset
    // of the round limit, and cancelling the attempt in progress.
    void open_window();
    void reset_rounds();
    void cancel_pairing();

    // Totals over every connection, for tests.
    struct Totals {
        std::uint32_t connections = 0;
        std::uint32_t streams = 0;
        std::uint64_t chunks = 0;
        std::uint64_t frames = 0;
        // _ac3forge_player@v1's.
        std::uint32_t burst_streams = 0;
        std::uint64_t bursts = 0;
        std::uint64_t burst_frames = 0;
    };
    [[nodiscard]] Totals totals() const;

    // What the other roles have received, over every connection: the server/state messages and the
    // latest state of each state role, none once cleared; the last image on each artwork channel,
    // empty once cleared; and the visualizer streams and frames.
    struct Roles {
        std::uint32_t states = 0;
        std::optional<sendspin::metadata::State> metadata;
        std::optional<sendspin::controller::State> controller;
        std::optional<sendspin::color::State> colors;
        std::uint32_t artwork_streams = 0;
        std::map<std::size_t, std::vector<std::uint8_t>> images;
        std::optional<sendspin::visualizer::StreamStart> visualizer;
        std::uint64_t visualizer_frames = 0;
    };
    [[nodiscard]] Roles roles() const;
    // A controller@v1 command, sent on every connection where the role is active.
    void send_controller_command(const sendspin::controller::CommandMessage& command);

   private:
    friend class Connection;

    Sink(SinkOptions options, render::OutputLayout layout, SinkLog& log, std::unique_ptr<Store> store);
    void accept(std::unique_ptr<sendspin::transport::Connection> transport);
    // Runs `work` on the sink's own thread, where connections may be called and destroyed.
    void post(std::function<void()> work);
    void run_posted();
    // Tells connection `id` that another server has taken the sink.
    void displace(sendspin::Arbiter::Id id);
    // Forgets connection `id`, whose connection has ended.
    void remove(sendspin::Arbiter::Id id);
    void for_each_connection(const std::function<void(Connection&)>& visit);
    void log(std::string_view text);

    SinkOptions options_;
    render::OutputLayout layout_;
    SinkLog* log_;
    std::unique_ptr<Store> store_;
    sendspin::SteadyClock clock_;
    sendspin::Arbiter arbiter_;
    // Every connection's session lock, which also guards the pairing state they share.
    std::shared_ptr<std::mutex> session_lock_ = std::make_shared<std::mutex>();
    sendspin::pairing_flow::ClientPairingState pairing_state_;

    mutable std::mutex connections_mutex_;
    std::map<sendspin::Arbiter::Id, std::shared_ptr<Connection>> connections_;
    std::uint64_t next_id_ = 1;
    Totals ended_totals_;

    mutable std::mutex roles_mutex_;
    Roles roles_;

    std::mutex posted_mutex_;
    std::condition_variable posted_changed_;
    std::deque<std::function<void()>> posted_;
    bool stopping_ = false;
    std::thread worker_;

    std::unique_ptr<sendspin::transport::websocket::Listener> listener_;
    std::unique_ptr<sendspin::discovery::Advertiser> advertiser_;
};

}  // namespace ac3::hearth::testsink
