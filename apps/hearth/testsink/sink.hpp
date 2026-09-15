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
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ac3/sendspin/arbiter.hpp"
#include "ac3/sendspin/discovery.hpp"
#include "ac3/sendspin/messages.hpp"
#include "ac3/sendspin/pairing_flow.hpp"
#include "ac3/sendspin/session.hpp"
#include "ac3/sendspin/session_driver.hpp"
#include "ac3/sendspin/websocket.hpp"
#include "store.hpp"

// ac3hearth-testsink: src/sendspin's player half as a program (planning/hearth-reference-player.md,
// The test sink). A Sendspin client that waits for servers: it listens on a WebSocket, advertises
// _sendspin._tcp, pairs by its pairing PSK and a dynamic or static code, admits servers as the
// specification ranks them, and decodes each player@v1 stream it plays, PCM, FLAC or Opus, to a
// WAV file with a play-time log.
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
    };
    [[nodiscard]] Totals totals() const;

   private:
    friend class Connection;

    Sink(SinkOptions options, SinkLog& log, std::unique_ptr<Store> store);
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

    std::mutex posted_mutex_;
    std::condition_variable posted_changed_;
    std::deque<std::function<void()>> posted_;
    bool stopping_ = false;
    std::thread worker_;

    std::unique_ptr<sendspin::transport::websocket::Listener> listener_;
    std::unique_ptr<sendspin::discovery::Advertiser> advertiser_;
};

}  // namespace ac3::hearth::testsink
