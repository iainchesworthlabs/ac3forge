#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <functional>
#include <map>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/render/layout.hpp"
#include "ac3/sendspin/noise.hpp"
#include "engine_thread.hpp"
#include "network_group_sink.hpp"
#include "network_sinks.hpp"
#include "settings_model.hpp"

// Last: on Windows it brings windows.h, whose macros would break the headers above.
#include <httplib.h>

// The whole Network page against real Hearth sinks on this network, hidden (it needs the boards):
// the app's own NetworkSinks finds them over mDNS, pairs each by the code it shows on its own page
// (read from its GET /status, as a person reads it off the page), puts them in one group, and the
// app's own Engine plays an E-AC-3 programme to the group; every sink reports the bursts it played,
// and the pairings are withdrawn at the end (server/unpair), so each board is left as it was. Another
// server holding a sink (Music Assistant) is displaced by the pairing, and takes it back afterwards
// if it wants it.
//
//   AC3HEARTH_LIVE_SINKS    the sinks' mDNS instance names, separated by commas (hearth-47b39c,...);
//                           the case is skipped without it
//   AC3HEARTH_LIVE_SECONDS  how long the programme plays (default 10)
//
// ac3tests has no firewall exception of its own: it asks mDNS for nothing a Windows firewall stops
// (NetworkSinks' browser's replies come back as replies to its own queries).

namespace {

namespace ss = ac3::sendspin;
using namespace std::chrono_literals;

[[nodiscard]] std::optional<std::string> environment(const char* name) {
    const char* const value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string(value);
}

[[nodiscard]] std::vector<std::string> split(const std::string& text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = text.find(',', start);
        const std::string part = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!part.empty()) {
            out.push_back(part);
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return out;
}

// A board's GET /status, or nothing.
[[nodiscard]] std::optional<std::string> board_status(const std::string& address) {
    httplib::Client client("http://" + address);
    client.set_connection_timeout(3, 0);
    client.set_read_timeout(3, 0);
    const httplib::Result result = client.Get("/status");
    if (!result || result->status != 200) {
        return std::nullopt;
    }
    return result->body;
}

// The value of "key" in the status JSON's "sendspin" object, as its raw text: a string's
// characters, or a number's digits. Flat enough a search is all it needs.
[[nodiscard]] std::optional<std::string> sendspin_field(const std::string& status, const std::string& key) {
    const std::size_t section = status.find("\"sendspin\":{");
    if (section == std::string::npos) {
        return std::nullopt;
    }
    const std::string quoted = "\"" + key + "\":";
    const std::size_t at = status.find(quoted, section);
    if (at == std::string::npos) {
        return std::nullopt;
    }
    std::size_t value = at + quoted.size();
    std::string out;
    if (value < status.size() && status[value] == '"') {
        for (++value; value < status.size() && status[value] != '"'; ++value) {
            out.push_back(status[value]);
        }
        return out;
    }
    for (; value < status.size() && (std::isdigit(static_cast<unsigned char>(status[value])) != 0); ++value) {
        out.push_back(status[value]);
    }
    return out;
}

// `frames` E-AC-3 access units of a 440 Hz tone - test_engine_network_group.cpp's own programme.
std::vector<std::byte> eac3_stream(int frames) {
    ac3::eac3::FrameConfig config;
    config.bitrate_kbps = 192;
    config.acmod = ac3::Acmod::k2_0;
    ac3::eac3::FrameEncoder encoder{config};
    std::vector<std::byte> out;
    for (int f = 0; f < frames; ++f) {
        std::vector<float> samples(ac3::kSamplesPerFrame);
        for (std::size_t n = 0; n < samples.size(); ++n) {
            samples[n] = static_cast<float>(
                0.1 * std::sin(2.0 * std::numbers::pi * 440.0 *
                               static_cast<double>(n + (static_cast<std::size_t>(f) * 1536)) / 48000.0));
        }
        const std::vector<std::span<const float>> views(2, samples);
        const auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        out.insert(out.end(), frame->begin(), frame->end());
    }
    return out;
}

// Calls tick() and prints what changed, until `done` holds or `timeout` passes.
bool drive(ac3::hearth::NetworkSinks& sinks, const std::function<bool(const ac3::hearth::NetworkStatus&)>& done,
           std::chrono::seconds timeout) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    std::uint64_t seen = 0;
    while (true) {
        sinks.tick();
        for (const std::string& line : sinks.take_log()) {
            std::printf("  host: %s\n", line.c_str());
        }
        const ac3::hearth::NetworkStatus status = sinks.status();
        if (status.generation != seen) {
            seen = status.generation;
            for (const ac3::hearth::SinkFacts& facts : status.sinks) {
                std::printf("  %-16s %-8s %-28s %s\n", facts.id.c_str(),
                            facts.pair_state == ac3::hearth::PairState::kPaired ? "paired" : "unpaired",
                            ac3::hearth::link_text(facts).c_str(), facts.notice.c_str());
            }
            std::fflush(stdout);
        }
        if (done(status)) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= until) {
            return false;
        }
        std::this_thread::sleep_for(100ms);
    }
}

[[nodiscard]] const ac3::hearth::SinkFacts* row(const ac3::hearth::NetworkStatus& status, const std::string& id) {
    const auto found = std::find_if(status.sinks.begin(), status.sinks.end(),
                                    [&](const ac3::hearth::SinkFacts& facts) { return facts.id == id; });
    return found == status.sinks.end() ? nullptr : &*found;
}

// Withdraws this test's pairings however the case ends, a failed REQUIRE included, so no board is
// left holding a record for an identity nothing will use again.
class Unpair {
   public:
    explicit Unpair(ac3::hearth::NetworkSinks& sinks) : sinks_(&sinks) {}
    ~Unpair() {
        for (const std::string& id : paired_) {
            sinks_->forget_pairing(id);
        }
        if (!paired_.empty()) {
            // server/unpair goes out as the call returns; give each board a moment to act on it.
            std::this_thread::sleep_for(1s);
        }
    }
    Unpair(const Unpair&) = delete;
    Unpair& operator=(const Unpair&) = delete;
    Unpair(Unpair&&) = delete;
    Unpair& operator=(Unpair&&) = delete;

    void paired(const std::string& id) { paired_.push_back(id); }
    void clear() { paired_.clear(); }

   private:
    ac3::hearth::NetworkSinks* sinks_;
    std::vector<std::string> paired_;
};

}  // namespace

TEST_CASE("network sinks live: the sinks on this network are found, paired, played to as one group, and unpaired",
          "[.][hearth-network-live]") {
    const std::optional<std::string> named = environment("AC3HEARTH_LIVE_SINKS");
    if (!named) {
        SKIP("AC3HEARTH_LIVE_SINKS names no sinks");
    }
    const std::vector<std::string> wanted = split(*named);
    REQUIRE_FALSE(wanted.empty());
    const int seconds = environment("AC3HEARTH_LIVE_SECONDS") ? std::atoi(environment("AC3HEARTH_LIVE_SECONDS")->c_str()) : 10;
    REQUIRE(seconds > 0);

    ac3::hearth::MemorySettingsStore settings;
    ac3::hearth::PairingStore store{settings, [] { return std::string("live"); }};
    const auto identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    ac3::hearth::NetworkSinks sinks{*identity, "Hearth live test", store, /*request_firewall_exception=*/false};
    REQUIRE(sinks.started());

    // 1. Found over mDNS, and read: each has said hello at least once.
    std::printf("finding %zu sink(s)\n", wanted.size());
    REQUIRE(drive(
        sinks,
        [&](const ac3::hearth::NetworkStatus& status) {
            return std::all_of(wanted.begin(), wanted.end(), [&](const std::string& id) {
                const ac3::hearth::SinkFacts* facts = row(status, id);
                return facts != nullptr && !facts->roles.empty();
            });
        },
        60s));

    // 2. Paired, one at a time, by the code each shows on its own page.
    Unpair unpair(sinks);
    for (const std::string& id : wanted) {
        const ac3::hearth::NetworkStatus listed = sinks.status();
        const ac3::hearth::SinkFacts* const found = row(listed, id);
        REQUIRE(found != nullptr);
        const std::string address = found->address;
        std::printf("pairing %s at %s\n", id.c_str(), address.c_str());
        // A board's page keeps showing the code of an attempt that ended with its connection;
        // this attempt's is a new one.
        const std::optional<std::string> before = board_status(address);
        const std::string stale = before ? sendspin_field(*before, "pairing_code").value_or("") : "";
        sinks.select_sink(id);
        sinks.pair_sink(id);
        std::optional<std::string> code;
        REQUIRE(drive(
            sinks,
            [&](const ac3::hearth::NetworkStatus& status) {
                const ac3::hearth::SinkFacts* facts = row(status, id);
                if (facts == nullptr || !facts->wants_code) {
                    return false;
                }
                if (const std::optional<std::string> text = board_status(address)) {
                    code = sendspin_field(*text, "pairing_code");
                }
                return code.has_value() && code->size() == 6 && *code != stale;
            },
            45s));
        std::printf("  %s shows %s\n", id.c_str(), code->c_str());
        sinks.submit_pairing_code(id, *code);
        unpair.paired(id);
        REQUIRE(drive(
            sinks,
            [&](const ac3::hearth::NetworkStatus& status) {
                const ac3::hearth::SinkFacts* facts = row(status, id);
                return facts != nullptr && facts->pair_state == ac3::hearth::PairState::kPaired &&
                       facts->link == ac3::hearth::SinkLink::kConnected && facts->ac3forge_support.has_value();
            },
            45s));
    }

    // 3. One group, every sink a member, each with its clock synchronised.
    const std::string group_id = sinks.create_group("Live");
    REQUIRE_FALSE(group_id.empty());
    for (const std::string& id : wanted) {
        sinks.add_group_member(group_id, id);
    }
    REQUIRE(drive(
        sinks,
        [&](const ac3::hearth::NetworkStatus& status) {
            return std::all_of(wanted.begin(), wanted.end(), [&](const std::string& id) {
                const ac3::hearth::SinkFacts* facts = row(status, id);
                return facts != nullptr && facts->clock_converged;
            });
        },
        45s));

    // 4. The app's own engine plays to the group.
    const int frames = seconds * 48000 / 1536;
    const std::vector<std::byte> programme = eac3_stream(frames);
    const ac3::hearth::ItemLoader loader =
        [&programme](const std::string& path) -> std::expected<ac3::hearth::LoadedItem, std::string> {
        if (path != "programme") {
            return std::unexpected("no such item: " + path);
        }
        return ac3::hearth::LoadedItem{.bytes = programme};
    };
    const auto layout = ac3::render::OutputLayout::parse("2.0");
    REQUIRE(layout.has_value());
    ac3::hearth::EngineOutputs outputs{
        .group = ac3::hearth::make_group_sink([&sinks](const std::string& id) { return sinks.group(id); })};
    {
        ac3::hearth::Engine engine(std::move(outputs), loader, *layout, ac3::hearth::DecoderSettings{},
                                   ac3::hearth::EngineTiming{});
        engine.set_output_preferences(ac3::hearth::OutputPreferences{.pinned = ac3::hearth::OutputMode::kNetworkGroup,
                                                                     .follow_sink = true,
                                                                     .group_name = group_id,
                                                                     .group_ready = true});
        engine.add({ac3::hearth::QueueItem{.path = "programme", .title = "Live programme"}});
        engine.play();
        std::printf("playing %d s to the group\n", seconds);
        REQUIRE(drive(
            sinks,
            [&](const ac3::hearth::NetworkStatus&) {
                const ac3::hearth::EngineStatus status = engine.status();
                return status.state == ac3::hearth::TransportState::kStopped && !status.history.empty();
            },
            std::chrono::seconds(seconds + 60)));
        const ac3::hearth::EngineStatus finished = engine.status();
        INFO("output_reason: " << finished.output_reason << " / note: " << finished.note << " / error: "
                               << finished.error);
        REQUIRE(finished.history.size() == 1);
        CHECK(finished.history.front().frames == finished.history.front().expected_frames);
    }

    // 5. Every sink played the programme's bursts, as it reports over this computer's own connection
    //    (its client/state counters - its page's counters are the last stream's, whoever sent it).
    const auto counters = [&](const ac3::hearth::NetworkStatus& status, const std::string& id) {
        const ac3::hearth::SinkFacts* facts = row(status, id);
        return facts != nullptr && facts->ac3forge_state ? std::optional(facts->ac3forge_state->counters) : std::nullopt;
    };
    (void)drive(
        sinks,
        [&](const ac3::hearth::NetworkStatus& status) {
            return std::all_of(wanted.begin(), wanted.end(), [&](const std::string& id) {
                const auto reported = counters(status, id);
                return reported && reported->bursts_played >= static_cast<std::uint64_t>(frames);
            });
        },
        15s);
    const ac3::hearth::NetworkStatus played = sinks.status();
    for (const std::string& id : wanted) {
        const auto reported = counters(played, id);
        REQUIRE(reported.has_value());
        std::printf("  %s played %llu of %d bursts: %llu underruns, %llu late, %llu dropped, %llu invalid\n", id.c_str(),
                    static_cast<unsigned long long>(reported->bursts_played), frames,
                    static_cast<unsigned long long>(reported->underruns),
                    static_cast<unsigned long long>(reported->late_chunks),
                    static_cast<unsigned long long>(reported->dropped_chunks),
                    static_cast<unsigned long long>(reported->invalid_chunks));
        CHECK(reported->bursts_played == static_cast<std::uint64_t>(frames));
        CHECK(reported->invalid_chunks == 0);
    }

    // 6. Each board forgets this test's pairing.
    unpair.clear();
    for (const std::string& id : wanted) {
        sinks.forget_pairing(id);
    }
    CHECK(drive(
        sinks,
        [&](const ac3::hearth::NetworkStatus& status) {
            return std::all_of(wanted.begin(), wanted.end(), [&](const std::string& id) {
                const ac3::hearth::SinkFacts* facts = row(status, id);
                return facts != nullptr && facts->pair_state != ac3::hearth::PairState::kPaired;
            });
        },
        30s));
}
