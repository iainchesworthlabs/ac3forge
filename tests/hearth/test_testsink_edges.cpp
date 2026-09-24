#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "platform/process.hpp"

#include "ac3/sendspin/ac3forge_player.hpp"
#include "ac3/sendspin/handshake.hpp"
#include "ac3/sendspin/messages.hpp"
#include "ac3/sendspin/noise.hpp"
#include "ac3/sendspin/pairing.hpp"
#include "ac3/sendspin/pairing_messages.hpp"
#include "ac3/sendspin/server_session.hpp"
#include "ac3/sendspin/session_driver.hpp"
#include "ac3/sendspin/websocket.hpp"
#include "sink.hpp"

// ac3hearth-testsink's edges, beside test_testsink.cpp's end-to-end case: what Sink::start
// refuses, and what a server session dialled straight at the sink over loopback (mDNS off) sees
// the sink do and log for each thing it is sent - a group name, volume and mute, a stream and a
// burst stream cleared, an extension-role command, a second server displacing the first, and
// the three pairing methods, including a code shown as a QR token, a wrong code, an attempt
// cancelled from either end and the static code's window opened by the operator.
//
// It dials, so under ThreadSanitizer it needs what tests/sendspin/test_websocket.cpp says.

namespace {

namespace fs = std::filesystem;
namespace m = ac3::sendspin::messages;
namespace hs = ac3::sendspin::handshake;
namespace ac = ac3::sendspin::ac3forge;
namespace testsink = ac3::hearth::testsink;
namespace websocket = ac3::sendspin::transport::websocket;
using ac3::sendspin::crypto::Key32;
using namespace std::chrono_literals;

std::string scratch_pid_suffix() {
    return ac3::test::platform::process_id();
}

// A fresh scratch directory for one case.
fs::path scratch(std::string_view leaf) {
    const fs::path dir =
        fs::path{AC3FORGE_TEST_SCRATCH_DIR} / ("hearth_testsink_edges_" + scratch_pid_suffix()) / std::string(leaf);
    fs::remove_all(dir);
    return dir;
}

// Every line the sink logs, for a case to wait on.
class Lines final : public testsink::SinkLog {
   public:
    void line(std::string_view text) override {
        {
            const std::lock_guard lock(mutex_);
            lines_.emplace_back(text);
        }
        changed_.notify_all();
    }

    bool wait(std::string_view needle, std::chrono::milliseconds timeout = 10s) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return has_locked(needle); });
    }

    [[nodiscard]] std::size_t count(std::string_view needle) {
        const std::lock_guard lock(mutex_);
        return static_cast<std::size_t>(
            std::ranges::count_if(lines_, [&](const std::string& l) { return l.find(needle) != std::string::npos; }));
    }

    [[nodiscard]] bool has(std::string_view needle) {
        const std::lock_guard lock(mutex_);
        return has_locked(needle);
    }

    // The digits of the last code the sink showed after "PAIRING CODE ", separators dropped.
    [[nodiscard]] std::optional<std::string> code() {
        const std::lock_guard lock(mutex_);
        for (auto it = lines_.rbegin(); it != lines_.rend(); ++it) {
            const std::size_t at = it->find("PAIRING CODE ");
            if (at != std::string::npos) {
                return it->substr(at + 13);
            }
        }
        return std::nullopt;
    }

   private:
    [[nodiscard]] bool has_locked(std::string_view needle) const {
        return std::ranges::any_of(lines_, [&](const std::string& l) { return l.find(needle) != std::string::npos; });
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::string> lines_;
};

class Keys final : public hs::ServerKeyring {
   public:
    hs::PskChoice choice = hs::sentinel_choice();
    [[nodiscard]] hs::PskChoice choose(const Key32& /*client_key*/) const override { return choice; }
};

class Events final : public ac3::sendspin::ServerListener {
   public:
    void on_hello(const m::ClientHello& /*hello*/) override { update([&] { ++hellos; }); }
    void on_state(const m::ClientState& state) override {
        update([&] {
            ++states;
            available = state.available;
            if (state.player && state.player->volume) {
                volume = *state.player->volume;
            }
        });
    }
    void on_goodbye(m::GoodbyeReason reason) override { update([&] { goodbye = reason; }); }
    void on_leave() override {}
    void on_pairing_held_back(const std::optional<std::string>& /*message*/) override {
        update([&] { held_back = true; });
    }
    void on_pairing_code_wanted() override { update([&] { code_wanted = true; }); }
    bool on_paired(const Key32& /*client_key*/, const Key32& psk) override {
        update([&] { long_term_psk = psk; });
        return true;
    }
    void on_pairing_ended(std::optional<ac3::sendspin::pairing_messages::AbortReason> reason) override {
        update([&] {
            ended = true;
            abort = reason;
        });
    }

    template <class Predicate>
    bool wait(Predicate&& predicate, std::chrono::milliseconds timeout = 10s) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, std::forward<Predicate>(predicate));
    }

    int hellos = 0;
    int states = 0;
    bool available = false;
    std::int32_t volume = -1;
    bool held_back = false;
    bool code_wanted = false;
    bool ended = false;
    std::optional<ac3::sendspin::pairing_messages::AbortReason> abort;
    std::optional<m::GoodbyeReason> goodbye;
    std::optional<Key32> long_term_psk;

   private:
    template <class Update>
    void update(Update&& change) {
        {
            const std::lock_guard lock(mutex_);
            change();
        }
        changed_.notify_all();
    }

    std::mutex mutex_;
    std::condition_variable changed_;
};

ac3::sendspin::noise::KeyPair generated() {
    std::optional<ac3::sendspin::noise::KeyPair> pair = ac3::sendspin::noise::KeyPair::generate();
    REQUIRE(pair.has_value());
    return *pair;
}

testsink::SinkOptions options_in(const fs::path& dir) {
    testsink::SinkOptions options;
    options.name = "Edge sink";
    options.address = "127.0.0.1";
    options.port = 0;
    options.state_directory = dir / "state";
    options.output_directory = dir / "out";
    options.advertise = false;
    options.codecs = {m::Codec::kPcm};
    options.unpaired_access = true;
    return options;
}

// One server dialled at a sink: its session, its events and the driver that runs them.
struct Dialled {
    ac3::sendspin::SteadyClock clock;
    Keys keys;
    Events events;
    ac3::sendspin::ServerSession server;
    std::unique_ptr<ac3::sendspin::SessionDriver> driver;

    Dialled(const testsink::Sink& sink, const ac3::sendspin::noise::KeyPair& identity,
            std::optional<hs::PskChoice> choice = std::nullopt)
        : server({.identity = identity, .name = "Edge server", .languages = {}, .max_message_bytes = 1 << 22}, keys,
                 events, clock) {
        if (choice) {
            keys.choice = *choice;
        }
        auto dialled = websocket::connect("ws://127.0.0.1:" + std::to_string(sink.port()) + "/sendspin");
        REQUIRE(dialled.has_value());
        driver = std::make_unique<ac3::sendspin::SessionDriver>(
            std::move(*dialled), ac3::sendspin::DrivenSession{
                                     .receive = [this](const auto& frame) { return server.receive(frame); },
                                     .tick = [this] { return server.tick(); },
                                     .next_tick_us = [this] { return server.next_tick_us(); },
                                     .ended = {}});
        driver->start();
        REQUIRE(events.wait([&] { return events.hellos >= 1; }));
    }

    ~Dialled() {
        driver->close();
        driver->join();
    }
    Dialled(const Dialled&) = delete;
    Dialled& operator=(const Dialled&) = delete;
    Dialled(Dialled&&) = delete;
    Dialled& operator=(Dialled&&) = delete;

    template <class Call>
    bool call(Call&& work) {
        return driver->call(std::forward<Call>(work)).has_value();
    }

    bool play(std::vector<std::string> roles) {
        return call([&] {
            return server.activate(
                {.activities = {m::Activity::kPlayback}, .active_roles = std::move(roles), .pairing = std::nullopt});
        });
    }

    bool pair(m::PairMethod method, std::optional<m::CodeFormat> format, std::int32_t pin_length) {
        return call([&] {
            return server.activate({.activities = {m::Activity::kPairing},
                                    .active_roles = std::vector<std::string>{},
                                    .pairing = m::PairingActivation{
                                        .method = method, .format = format, .pin_length = pin_length, .languages = {}}});
        });
    }
};

// Polls `predicate` until it holds or `timeout` passes.
template <class Predicate>
bool eventually(Predicate&& predicate, std::chrono::milliseconds timeout = 10s) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= until) {
            return false;
        }
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

}  // namespace

TEST_CASE("test sink edges: start refuses what it cannot run with, and says which", "[hearth][testsink]") {
    const fs::path dir = scratch("refusals");
    Lines log;

    SECTION("no state directory") {
        auto options = options_in(dir);
        options.state_directory.clear();
        const auto sink = testsink::Sink::start(options, log);
        REQUIRE_FALSE(sink.has_value());
        CHECK(sink.error() == "a state directory is required");
    }
    SECTION("a static code that is not eight digits") {
        auto options = options_in(dir);
        options.code_method = testsink::CodeMethod::kStatic;
        for (const std::string code : {"1234567", "1234567a", "123456789"}) {
            options.static_code = code;
            const auto sink = testsink::Sink::start(options, log);
            REQUIRE_FALSE(sink.has_value());
            CHECK(sink.error() == "a static pairing code is eight digits");
        }
    }
    SECTION("a layout the renderer cannot parse") {
        auto options = options_in(dir);
        options.layout = "not a layout";
        const auto sink = testsink::Sink::start(options, log);
        REQUIRE_FALSE(sink.has_value());
        CHECK(sink.error() == "not a speaker layout: not a layout");
    }
    SECTION("an output directory that cannot be created") {
        auto options = options_in(dir);
        fs::create_directories(dir);
        std::ofstream{dir / "a-file"} << "x";
        options.output_directory = dir / "a-file" / "out";
        const auto sink = testsink::Sink::start(options, log);
        REQUIRE_FALSE(sink.has_value());
        CHECK(sink.error().starts_with("cannot create "));
    }
    SECTION("a port another sink holds") {
        auto first = testsink::Sink::start(options_in(dir / "first"), log);
        REQUIRE(first.has_value());
        auto options = options_in(dir / "second");
        options.port = (*first)->port();
        const auto second = testsink::Sink::start(options, log);
        REQUIRE_FALSE(second.has_value());
        CHECK(second.error() == "cannot listen on 127.0.0.1:" + std::to_string(options.port));
    }
}

TEST_CASE("test sink edges: the operator's pairing actions are logged even with no server connected",
          "[hearth][testsink]") {
    const fs::path dir = scratch("operator");
    Lines log;
    auto sink = testsink::Sink::start(options_in(dir), log);
    REQUIRE(sink.has_value());
    (*sink)->open_window();
    (*sink)->reset_rounds();
    (*sink)->cancel_pairing();
    CHECK(log.has("pairing window open for five minutes"));
    CHECK(log.has("round limit reset"));
    CHECK((*sink)->totals().connections == 0);
}

TEST_CASE("test sink edges: a player's group name, commands and a cleared stream are logged",
          "[hearth][testsink][websocket]") {
    const fs::path dir = scratch("player");
    Lines log;
    auto sink = testsink::Sink::start(options_in(dir), log);
    REQUIRE(sink.has_value());
    const auto identity = generated();
    Dialled server(**sink, identity);
    REQUIRE(server.play({"player@v1"}));
    REQUIRE(log.wait("activated for playback"));
    REQUIRE(server.events.wait([&] { return server.events.available; }, 15s));

    REQUIRE(server.call([&] {
        return server.server.update_group({.playback_state = m::PlaybackState::kPlaying, .group_id = "g1", .group_name = "Kitchen"});
    }));
    CHECK(log.wait("group Kitchen"));

    REQUIRE(server.call([&] {
        return server.server.command({.command = m::PlayerCommand::kVolume, .volume = 42, .mute = false, .output_delay_ms = 0});
    }));
    CHECK(log.wait("volume 42"));
    REQUIRE(server.call([&] {
        return server.server.command({.command = m::PlayerCommand::kMute, .volume = 0, .mute = true, .output_delay_ms = 0});
    }));
    CHECK(log.wait("] muted"));
    REQUIRE(server.call([&] {
        return server.server.command({.command = m::PlayerCommand::kMute, .volume = 0, .mute = false, .output_delay_ms = 0});
    }));
    CHECK(log.wait("unmuted"));
    // The new state goes back to the server from the sink's own thread.
    CHECK(server.events.wait([&] { return server.events.volume == 42; }));
    // The sink does not list an output delay, so a conforming server does not send one.
    CHECK_FALSE(server.call([&] {
        return server.server.command(
            {.command = m::PlayerCommand::kSetOutputDelay, .volume = 0, .mute = false, .output_delay_ms = 30});
    }));

    const m::AudioFormat format{.codec = m::Codec::kPcm, .channels = 2, .sample_rate = 48000, .bit_depth = 16};
    REQUIRE(server.call([&] { return server.server.start_stream({.format = format, .codec_header = {}}); }));
    CHECK(log.wait("stream PCM 48000 Hz 16-bit 2 ch to "));
    REQUIRE(server.call([&] { return server.server.clear_stream(); }));
    CHECK(log.wait("stream cleared"));
    REQUIRE(server.call([&] { return server.server.end_stream(); }));
    CHECK(log.wait("stream ended after 0 chunks"));
}

TEST_CASE("test sink edges: the extension role's stream cleared and its commands are logged",
          "[hearth][testsink][websocket]") {
    const fs::path dir = scratch("extension");
    Lines log;
    auto options = options_in(dir);
    options.output_directory.clear();  // count, write nothing
    auto sink = testsink::Sink::start(options, log);
    REQUIRE(sink.has_value());
    const auto identity = generated();
    Dialled server(**sink, identity);
    REQUIRE(server.play({std::string(ac::kRole)}));
    REQUIRE(log.wait("activated for playback"));
    REQUIRE(server.events.wait([&] { return server.events.available; }, 15s));

    REQUIRE(server.call(
        [&] { return server.server.start_burst_stream({.data_type = ac::DataType::kAc3, .sample_rate = 48000}); }));
    CHECK(log.wait("burst stream AC-3 48000 Hz to 7.1.4"));
    REQUIRE(server.call([&] { return server.server.clear_burst_stream(); }));
    CHECK(log.wait("burst stream cleared"));
    REQUIRE(server.call([&] { return server.server.end_burst_stream(); }));
    CHECK(log.wait("burst stream ended after 0 bursts"));

    ac::CommandMessage volume;
    volume.command = ac::Command::kVolume;
    volume.volume = 17;
    REQUIRE(server.call([&] { return server.server.ac3forge_command(volume); }));
    CHECK(log.wait("volume 17"));
    ac::CommandMessage mute;
    mute.command = ac::Command::kMute;
    mute.mute = true;
    REQUIRE(server.call([&] { return server.server.ac3forge_command(mute); }));
    CHECK(log.wait("] muted"));
    mute.mute = false;
    REQUIRE(server.call([&] { return server.server.ac3forge_command(mute); }));
    CHECK(log.wait("unmuted"));
    ac::CommandMessage delay;
    delay.command = ac::Command::kSetOutputDelay;
    delay.output_delay_ms = 20;
    CHECK_FALSE(server.call([&] { return server.server.ac3forge_command(delay); }));  // not listed
}

TEST_CASE("test sink edges: a second server taking the sink for playback displaces the first",
          "[hearth][testsink][websocket]") {
    const fs::path dir = scratch("displace");
    Lines log;
    auto sink = testsink::Sink::start(options_in(dir), log);
    REQUIRE(sink.has_value());
    const auto first_identity = generated();
    const auto second_identity = generated();
    Dialled first(**sink, first_identity);
    REQUIRE(first.play({"player@v1"}));
    REQUIRE(log.wait("[1] activated for playback"));
    Dialled second(**sink, second_identity);
    REQUIRE(second.play({"player@v1"}));
    const bool displaced = log.wait("[1] displaced by another server");
    if (displaced) {
        CHECK(first.events.wait([&] { return first.events.goodbye.has_value(); }));
        CHECK(first.events.goodbye == m::GoodbyeReason::kAnotherServer);
        CHECK(log.wait("[1] closed"));
    } else {
        // The sink's arbiter kept the first: the second is refused instead.
        CHECK(log.wait("[2] refused, another server holds the sink"));
    }
    CHECK(eventually([&] { return (*sink)->totals().connections >= 1; }));
}

TEST_CASE("test sink edges: a dynamic code is shown as a QR token or grouped digits, and a wrong code costs a round and a cancel ends the attempt",
          "[hearth][testsink][websocket]") {
    const fs::path dir = scratch("dynamic");
    Lines log;
    auto options = options_in(dir);
    options.unpaired_access = false;
    auto sink = testsink::Sink::start(options, log);
    REQUIRE(sink.has_value());
    const auto identity = generated();

    {
        Dialled server(**sink, identity);
        REQUIRE(server.pair(m::PairMethod::kDynamicCode, m::CodeFormat::kQrCode, 0));
        REQUIRE(log.wait("PAIRING CODE SP:"));
        const auto token = log.code();
        REQUIRE(token.has_value());
        CHECK(ac3::sendspin::pairing::decode_token(*token).has_value());
        // The operator gives up on the device.
        (*sink)->cancel_pairing();
        CHECK(log.wait("pairing cancelled"));
        CHECK(server.events.wait([&] { return server.events.ended; }));
    }
    {
        Dialled server(**sink, identity);
        REQUIRE(server.pair(m::PairMethod::kDynamicCode, m::CodeFormat::kDigits, 8));
        // The code is grouped in two halves, whatever length the sink settled on.
        REQUIRE(eventually([&] {
            const auto code = log.code();
            return code && code->find('-') != std::string::npos;
        }));
        REQUIRE(server.events.wait([&] { return server.events.code_wanted; }));
        const std::string shown = *log.code();
        std::string wrong = shown;
        std::erase(wrong, '-');
        wrong[0] = wrong[0] == '0' ? '1' : '0';
        const std::size_t shown_before = log.count("[2] PAIRING CODE ");
        REQUIRE(server.call([&] { return server.server.enter_code(ac3::sendspin::pairing_flow::Code{wrong}); }));
        // A wrong code costs a round, not the attempt: the sink shows a code again.
        CHECK(eventually([&] { return log.count("[2] PAIRING CODE ") > shown_before; }));
        CHECK_FALSE(log.has("[2] paired with server"));
    }
    {
        Dialled server(**sink, identity);
        REQUIRE(server.pair(m::PairMethod::kDynamicCode, m::CodeFormat::kDigits, 6));
        REQUIRE(server.events.wait([&] { return server.events.code_wanted; }));
        REQUIRE(server.call([&] { return server.server.cancel_pairing(); }));
        CHECK(log.wait("[3] pairing cancelled"));
    }
}

TEST_CASE("test sink edges: a static code waits for the operator's window and then pairs",
          "[hearth][testsink][websocket]") {
    const fs::path dir = scratch("static");
    Lines log;
    auto options = options_in(dir);
    options.unpaired_access = false;
    options.code_method = testsink::CodeMethod::kStatic;
    options.static_code = "13572468";
    auto sink = testsink::Sink::start(options, log);
    REQUIRE(sink.has_value());
    const auto identity = generated();
    Dialled server(**sink, identity);

    REQUIRE(server.pair(m::PairMethod::kStaticCode, std::nullopt, 0));
    REQUIRE(log.wait("pairing waits for the operator"));
    CHECK(server.events.wait([&] { return server.events.held_back; }));

    (*sink)->open_window();
    REQUIRE(server.events.wait([&] { return server.events.code_wanted; }));
    REQUIRE(server.call([&] { return server.server.enter_code(ac3::sendspin::pairing_flow::Code{std::string("13572468")}); }));
    REQUIRE(log.wait("paired with server "));
    CHECK(server.events.wait([&] { return server.events.long_term_psk.has_value(); }));
    // Once the attempt is over, a reset of the round limit has nothing to resume.
    (*sink)->reset_rounds();
    CHECK(log.wait("round limit reset"));
}

TEST_CASE("test sink edges: a server that unpairs is forgotten, and the sink says so", "[hearth][testsink][websocket]") {
    const fs::path dir = scratch("unpair");
    Lines log;
    auto options = options_in(dir);
    options.unpaired_access = false;
    auto sink = testsink::Sink::start(options, log);
    REQUIRE(sink.has_value());
    const auto token = ac3::sendspin::pairing::decode_pairing_psk_token((*sink)->pairing_token());
    REQUIRE(token.has_value());
    const auto identity = generated();
    std::optional<Key32> long_term;
    {
        Dialled pairing(**sink, identity, hs::PskChoice{.psk = token->pairing_psk, .category = hs::PskCategory::kPairing});
        REQUIRE(pairing.pair(m::PairMethod::kPairingPsk, std::nullopt, 0));
        REQUIRE(log.wait("paired with server "));
        REQUIRE(pairing.events.wait([&] { return pairing.events.long_term_psk.has_value(); }));
        long_term = pairing.events.long_term_psk;
    }
    // Reached again under the long-term PSK, which is the only session an unpair is heard on.
    Dialled server(**sink, identity, hs::PskChoice{.psk = *long_term, .category = hs::PskCategory::kLongTerm});
    REQUIRE(server.driver->inspect([&] { return server.server.psk_category(); }) == hs::PskCategory::kLongTerm);
    REQUIRE(server.play({"player@v1"}));
    REQUIRE(log.wait("[2] activated for playback"));
    REQUIRE(server.call([&] { return server.server.unpair(); }));
    CHECK(log.wait("unpaired by server "));
    CHECK(server.events.wait([&] { return server.events.goodbye == m::GoodbyeReason::kUnpaired; }));
}
