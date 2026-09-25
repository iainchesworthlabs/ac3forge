#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "platform/process.hpp"

#include "ac3/sendspin/handshake.hpp"
#include "ac3/sendspin/messages.hpp"
#include "ac3/sendspin/noise.hpp"
#include "ac3/sendspin/pairing_messages.hpp"
#include "ac3/sendspin/server_host.hpp"
#include "ac3/sendspin/server_store.hpp"
#include "sink.hpp"

// ServerHost's dialling against a test sink in process over loopback, with mDNS off: what the host
// says when a dial fails, that a second connection to a client it already holds does not report
// the client gone, and the case Hearth's Network page is built around - a sink another server
// already holds (Music Assistant keeps a connection to every Sendspin player it has found) refuses
// a connection that asks for nothing (connection.md, Multiple servers), while a dial to pair is
// admitted, takes the sink from that server, and pairs by the code the sink shows.

namespace {

namespace fs = std::filesystem;
namespace m = ac3::sendspin::messages;
namespace ss = ac3::sendspin;
namespace testsink = ac3::hearth::testsink;
using namespace std::chrono_literals;

std::string scratch_pid_suffix() { return ac3::test::platform::process_id(); }

class CodeLog final : public testsink::SinkLog {
   public:
    void line(std::string_view text) override {
        const std::lock_guard lock(mutex_);
        lines_.emplace_back(text);
        const std::size_t at = text.find("PAIRING CODE ");
        if (at != std::string_view::npos) {
            std::string digits;
            for (const char c : text.substr(at + 13)) {
                if (c >= '0' && c <= '9') {
                    digits.push_back(c);
                }
            }
            code_ = digits;
            ++codes_;
        }
    }

    std::optional<std::string> code() {
        const std::lock_guard lock(mutex_);
        return code_;
    }

    // How many codes the sink has shown.
    std::size_t codes() {
        const std::lock_guard lock(mutex_);
        return codes_;
    }

    // Everything the sink has said, for a failure's message.
    std::string all() {
        const std::lock_guard lock(mutex_);
        std::string out;
        for (const std::string& one : lines_) {
            out += one + " | ";
        }
        return out;
    }

   private:
    std::mutex mutex_;
    std::optional<std::string> code_;
    std::size_t codes_ = 0;
    std::vector<std::string> lines_;
};

// Everything a host reports, for a test to wait on.
class Events final : public ss::ServerHostEvents {
   public:
    void on_client(const ss::ClientView& client) override { note([&] { clients_[client.client_id] = client; }); }
    void on_client_gone(const std::string& client_id) override { note([&] { gone_.push_back(client_id); }); }
    void on_dial_failed(const std::string& url, bool answered) override {
        note([&] { failed_.emplace_back(url, answered); });
    }
    void on_client_goodbye(const std::string& client_id, m::GoodbyeReason reason) override {
        note([&] { goodbyes_[client_id] = reason; });
    }
    void on_pairing_code_wanted(const std::string& /*client_id*/) override {}
    void on_paired(const std::string& client_id) override { note([&] { paired_.push_back(client_id); }); }
    void on_pairing_ended(const std::string& /*client_id*/,
                          std::optional<ss::pairing_messages::AbortReason> /*reason*/) override {}
    void on_log(std::string_view line) override { note([&] { log_.emplace_back(line); }); }

    // Waits until `predicate(*this)` holds, under the lock, or `timeout` passes.
    template <class Predicate>
    bool wait(Predicate&& predicate, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return predicate(*this); });
    }

    // What `reader(*this)` returns, read under the lock.
    template <class Reader>
    auto read(Reader&& reader) {
        const std::lock_guard lock(mutex_);
        return reader(*this);
    }

    std::map<std::string, ss::ClientView> clients_;
    std::vector<std::string> gone_;
    std::vector<std::pair<std::string, bool>> failed_;
    std::map<std::string, m::GoodbyeReason> goodbyes_;
    std::vector<std::string> paired_;
    std::vector<std::string> log_;

   private:
    template <class Change>
    void note(Change&& change) {
        {
            const std::lock_guard lock(mutex_);
            change();
        }
        changed_.notify_all();
    }

    std::mutex mutex_;
    std::condition_variable changed_;
};

std::unique_ptr<ss::ServerHost> start_host(ss::ServerStore& store, Events& events, std::string name) {
    std::optional<ss::noise::KeyPair> identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    auto host = ss::ServerHost::start({.identity = *identity,
                                       .name = std::move(name),
                                       .languages = {"en"},
                                       .address = "127.0.0.1",
                                       .port = std::nullopt,
                                       .advertise = false,
                                       .browse = false,
                                       .mdns_interfaces = {}},
                                      store, events);
    REQUIRE(host.has_value());
    return std::move(*host);
}

std::unique_ptr<testsink::Sink> start_sink(const fs::path& directory, CodeLog& log) {
    testsink::SinkOptions options;
    options.name = "Held sink";
    options.address = "127.0.0.1";
    options.port = 0;
    options.state_directory = directory / "state";
    options.advertise = false;
    // As a Hearth sink: no unpaired access, a dynamic code.
    options.unpaired_access = false;
    options.code_method = testsink::CodeMethod::kDynamic;
    auto sink = testsink::Sink::start(options, log);
    REQUIRE(sink.has_value());
    return std::move(*sink);
}

}  // namespace

TEST_CASE("server host: a dial nothing answers is reported as failed", "[hearth][server-host][websocket]") {
    ss::MemoryServerStore store;
    Events events;
    const std::unique_ptr<ss::ServerHost> host = start_host(store, events, "Test host");
    // Port 1 on loopback: refused at once.
    const std::string url = "ws://127.0.0.1:1/sendspin";
    REQUIRE(host->dial_to_pair(url, m::PairMethod::kDynamicCode, m::CodeFormat::kDigits));
    REQUIRE(events.wait([&](const Events& e) { return !e.failed_.empty(); }, 15s));
    CHECK(events.read([](const Events& e) { return e.failed_.front(); }) == std::pair<std::string, bool>{url, false});
    // The pairing method the page cannot take, and no URL, are refused outright.
    CHECK_FALSE(host->dial_to_pair(url, m::PairMethod::kPairingPsk, std::nullopt));
    CHECK_FALSE(host->dial_to_pair("", m::PairMethod::kDynamicCode, m::CodeFormat::kDigits));
}

TEST_CASE("server host: a second connection to a client it already holds leaves the client connected",
          "[hearth][server-host][websocket]") {
    const fs::path scratch = fs::path{AC3FORGE_TEST_SCRATCH_DIR} / ("server_host_second_" + scratch_pid_suffix());
    fs::remove_all(scratch);
    CodeLog log;
    std::unique_ptr<testsink::Sink> sink = start_sink(scratch, log);
    const std::string client_id = sink->client_id();
    const std::string url = "ws://127.0.0.1:" + std::to_string(sink->port()) + "/sendspin";

    ss::MemoryServerStore store;
    Events events;
    std::unique_ptr<ss::ServerHost> host = start_host(store, events, "Test host");
    host->dial(url);
    REQUIRE(events.wait([&](const Events& e) { return e.clients_.contains(client_id); }, 15s));
    host->dial(url);
    REQUIRE(events.wait(
        [](const Events& e) {
            for (const std::string& line : e.log_) {
                if (line.find("a second connection, closed") != std::string::npos) {
                    return true;
                }
            }
            return false;
        },
        15s));
    // The second connection is gone; the client is not.
    std::this_thread::sleep_for(300ms);
    CHECK(events.read([](const Events& e) { return e.gone_.empty(); }));
    CHECK(host->client(client_id).has_value());

    // Once the sink itself goes, the client is gone, once.
    sink.reset();
    REQUIRE(events.wait([](const Events& e) { return !e.gone_.empty(); }, 15s));
    std::this_thread::sleep_for(300ms);
    CHECK(events.read([](const Events& e) { return e.gone_; }) == std::vector<std::string>{client_id});
    host.reset();
}

TEST_CASE("server host: a pairing attempt cancelled can be asked for again on the same connection",
          "[hearth][server-host][websocket]") {
    const fs::path scratch = fs::path{AC3FORGE_TEST_SCRATCH_DIR} / ("server_host_again_" + scratch_pid_suffix());
    fs::remove_all(scratch);
    CodeLog log;
    const std::unique_ptr<testsink::Sink> sink = start_sink(scratch, log);
    const std::string client_id = sink->client_id();
    const std::string url = "ws://127.0.0.1:" + std::to_string(sink->port()) + "/sendspin";

    ss::MemoryServerStore store;
    Events events;
    std::unique_ptr<ss::ServerHost> host = start_host(store, events, "Hearth");
    host->dial(url);
    REQUIRE(events.wait([&](const Events& e) { return e.clients_.contains(client_id); }, 15s));

    const auto attempt_running = [&](bool running) {
        return [&, running](const Events& e) {
            const auto found = e.clients_.find(client_id);
            return found != e.clients_.end() && found->second.pairing_attempt == running &&
                   found->second.wants_code == running;
        };
    };
    // Waits until the sink has shown `count` codes in all.
    const auto codes_shown = [&](std::size_t count) {
        const auto until = std::chrono::steady_clock::now() + 15s;
        while (log.codes() < count && std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(20ms);
        }
        return log.codes() >= count;
    };
    // The first attempt, reported as it waits for the code, then cancelled once the sink shows
    // its code (so that code is not taken for the next attempt's) - reported too.
    REQUIRE(host->pair(client_id, m::PairMethod::kDynamicCode, m::CodeFormat::kDigits));
    REQUIRE(events.wait(attempt_running(true), 15s));
    REQUIRE(codes_shown(1));
    REQUIRE(host->cancel_pairing(client_id));
    REQUIRE(events.wait(attempt_running(false), 15s));

    // Asked for again: a new attempt runs on the same connection, shows a new code, and pairs.
    REQUIRE(host->pair(client_id, m::PairMethod::kDynamicCode, m::CodeFormat::kDigits));
    REQUIRE(events.wait(attempt_running(true), 15s));
    REQUIRE(codes_shown(2));
    REQUIRE(host->enter_code(client_id, *log.code()));
    INFO("host: " << events.read([](const Events& e) {
        std::string all;
        for (const std::string& line : e.log_) {
            all += line + " | ";
        }
        return all;
    }));
    INFO("sink: " << log.all());
    REQUIRE(events.wait(
        [&](const Events& e) {
            const auto found = e.clients_.find(client_id);
            return found != e.clients_.end() && found->second.playing &&
                   found->second.psk == ss::handshake::PskCategory::kLongTerm;
        },
        20s));
    host.reset();
}

TEST_CASE("server host: a code that does not match is asked for again, and the view counts each request",
          "[hearth][server-host][websocket]") {
    const fs::path scratch = fs::path{AC3FORGE_TEST_SCRATCH_DIR} / ("server_host_rounds_" + scratch_pid_suffix());
    fs::remove_all(scratch);
    CodeLog log;
    const std::unique_ptr<testsink::Sink> sink = start_sink(scratch, log);
    const std::string client_id = sink->client_id();
    const std::string url = "ws://127.0.0.1:" + std::to_string(sink->port()) + "/sendspin";

    ss::MemoryServerStore store;
    Events events;
    std::unique_ptr<ss::ServerHost> host = start_host(store, events, "Hearth");
    host->dial(url);
    REQUIRE(events.wait([&](const Events& e) { return e.clients_.contains(client_id); }, 15s));
    CHECK(events.read([&](const Events& e) { return e.clients_.at(client_id).code_requests; }) == 0U);

    const auto asked = [&](std::uint32_t requests) {
        return [&, requests](const Events& e) {
            const auto found = e.clients_.find(client_id);
            return found != e.clients_.end() && found->second.wants_code && found->second.code_requests == requests;
        };
    };
    REQUIRE(host->pair(client_id, m::PairMethod::kDynamicCode, m::CodeFormat::kDigits));
    REQUIRE(events.wait(asked(1), 15s));
    const auto until = std::chrono::steady_clock::now() + 15s;
    while (!log.code() && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(20ms);
    }
    REQUIRE(log.code());
    const std::string right = *log.code();
    std::string wrong = right;
    wrong[0] = wrong[0] == '9' ? '0' : static_cast<char>(wrong[0] + 1);

    // Refused: the sink starts another round under the same code, and asks for it again - the
    // second request, which is how a caller that entered the code tells it did not match.
    REQUIRE(host->enter_code(client_id, wrong));
    INFO("sink: " << log.all());
    REQUIRE(events.wait(asked(2), 15s));
    REQUIRE(host->enter_code(client_id, right));
    REQUIRE(events.wait(
        [&](const Events& e) {
            const auto found = e.clients_.find(client_id);
            return found != e.clients_.end() && found->second.playing &&
                   found->second.psk == ss::handshake::PskCategory::kLongTerm;
        },
        20s));
    host.reset();
}

TEST_CASE("server host: a sink another server holds refuses a waiting connection and admits a dial to pair",
          "[hearth][server-host][websocket]") {
    const fs::path scratch = fs::path{AC3FORGE_TEST_SCRATCH_DIR} / ("server_host_held_" + scratch_pid_suffix());
    fs::remove_all(scratch);
    CodeLog log;
    const std::unique_ptr<testsink::Sink> sink = start_sink(scratch, log);
    const std::string client_id = sink->client_id();
    const std::string url = "ws://127.0.0.1:" + std::to_string(sink->port()) + "/sendspin";

    // The other server holds the sink first, asking for nothing: a Music Assistant that has found
    // it but neither paired it nor played to it.
    ss::MemoryServerStore other_store;
    Events other_events;
    std::unique_ptr<ss::ServerHost> other = start_host(other_store, other_events, "Other server");
    other->dial(url);
    REQUIRE(other_events.wait([&](const Events& e) { return e.clients_.contains(client_id); }, 15s));
    std::this_thread::sleep_for(300ms);
    REQUIRE(other_events.read([](const Events& e) { return e.goodbyes_.empty(); }));

    // A second server's connection asking for nothing is refused: concurrent_attempt, then gone.
    ss::MemoryServerStore store;
    Events events;
    std::unique_ptr<ss::ServerHost> host = start_host(store, events, "Hearth");
    host->dial(url);
    REQUIRE(events.wait([&](const Events& e) { return e.goodbyes_.contains(client_id); }, 15s));
    CHECK(events.read([&](const Events& e) { return e.goodbyes_.at(client_id); }) == m::GoodbyeReason::kConcurrentAttempt);
    REQUIRE(events.wait([&](const Events& e) { return !e.gone_.empty(); }, 15s));

    // A dial to pair is admitted: its first activation is the pairing, which outranks the other
    // server's connection and takes the sink from it.
    REQUIRE(host->dial_to_pair(url, m::PairMethod::kDynamicCode, m::CodeFormat::kDigits));
    REQUIRE(other_events.wait([&](const Events& e) { return e.goodbyes_.contains(client_id); }, 15s));
    CHECK(other_events.read([&](const Events& e) { return e.goodbyes_.at(client_id); }) ==
          m::GoodbyeReason::kAnotherServer);
    const auto shown = std::chrono::steady_clock::now() + 15s;
    while (!log.code() && std::chrono::steady_clock::now() < shown) {
        std::this_thread::sleep_for(20ms);
    }
    REQUIRE(log.code().has_value());
    const auto wanted = std::chrono::steady_clock::now() + 10s;
    bool entered = false;
    while (!entered && std::chrono::steady_clock::now() < wanted) {
        entered = host->enter_code(client_id, *log.code());
        if (!entered) {
            std::this_thread::sleep_for(20ms);
        }
    }
    REQUIRE(entered);
    REQUIRE(events.wait(
        [&](const Events& e) {
            const auto found = e.clients_.find(client_id);
            return found != e.clients_.end() && found->second.playing &&
                   found->second.psk == ss::handshake::PskCategory::kLongTerm;
        },
        20s));
    CHECK(store.paired(events.read([&](const Events& e) { return e.clients_.at(client_id).client_key; })));

    host.reset();
    other.reset();
}
