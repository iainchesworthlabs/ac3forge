// ac3hearth-testserver: a Sendspin server for Hearth's board and CI tests
// (planning/hearth-reference-player.md, B3 and B4).
//
// It dials each player it is given, pairs it, sends it settings, plays one AC-3 or E-AC-3
// programme to all of them as a group over _ac3forge_player@v1, and reports what each said: its
// counters, its decoder's findings and its levels, and for a board, what its GET /status said
// while it played - above all where it put the programme's first frame on the server's clock,
// which is how two boards are held to playing within a millisecond of each other.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "ac3/iec61937/iec61937.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/sendspin/ac3forge_player.hpp"
#include "ac3/sendspin/handshake.hpp"
#include "ac3/sendspin/json.hpp"
#include "ac3/sendspin/messages.hpp"
#include "ac3/sendspin/pairing_messages.hpp"
#include "ac3/sendspin/server_host.hpp"
#include "server_store.hpp"
#include "sink.hpp"

// Last: on Windows it brings windows.h, whose macros (small, among others) would break the
// headers above.
#include <httplib.h>

namespace {

namespace fs = std::filesystem;
namespace ss = ac3::sendspin;
namespace m = ss::messages;
namespace ac = ss::ac3forge;
namespace json = ss::json;
namespace testsink = ac3::hearth::testsink;
using namespace std::chrono_literals;
using SteadyClock = std::chrono::steady_clock;

constexpr std::string_view kUsage = R"(usage: ac3hearth-testserver [options] --play FILE --player URL [player options]...

A Sendspin server for Hearth's tests. It dials each player, pairs it, sends it
settings, plays one AC-3 or E-AC-3 programme to all of them as a group over
_ac3forge_player@v1, and writes a report of what each said about it.

  --player URL         a player to dial, such as ws://192.168.1.40:8928/sendspin;
                       the options below apply to the last one given
    --label TEXT       what the report calls it (default: the name it gives)
    --token SP:0...    its pairing token, from its console
    --code-log FILE    pair by the code it shows, read from the first
                       "PAIRING CODE" line written to FILE after asking
    --layout LAYOUT    settings with this speaker layout before the play
    --trim-db LIST     and these trims, one per output in dB, separated by
                       commas: -6,-6 plays a two-output board 6 dB down
    --status URL       its GET /status, read once a second while it plays,
                       such as http://192.168.1.40/status
    --hold             only connect, and stay connected unpaired, as a
                       second server such as Music Assistant does: it is not
                       paired or played to, and may be displaced

  --play FILE          the programme, an .ac3 or .ec3 file
  --seconds N          play the file over and over for at least N seconds
                       (default: once through)
  --sink DIRECTORY     also play to a test sink in this process, which writes
                       its WAV and play-time log to DIRECTORY/out
  --sink-layout LAYOUT the test sink's speaker layout (default 2.0)
  --state DIRECTORY    the server's identity and pairing records
                       (default ./hearth-testserver-state)
  --name NAME          the name players show (default "Hearth test server")
  --timeout SECONDS    how long a player may take to connect, pair and take
                       its settings (default 60)
  --report FILE        the report, as JSON (default: standard output)

The exit status is 0 when every player played the whole programme, 1 for a
command line or a file that cannot be used, 2 when a player did not connect,
pair or take its settings, and 3 when a player went away, or reported an
error, during the play.
)";

constexpr int kExitUsage = 1;
constexpr int kExitSetup = 2;
constexpr int kExitPlay = 3;

constexpr std::uint32_t kSampleRate = 48000;
// The samples in every burst (planning/hearth-sendspin-extension.md, Burst chunks).
constexpr std::int64_t kSamplesPerBurst = 1536;
// How long after its last burst has played a programme ends: past the players' output queues.
constexpr std::int64_t kEndMarginUs = 500'000;

std::mutex g_log_mutex;
const SteadyClock::time_point g_started = SteadyClock::now();

void note(std::string_view text) {
    const std::lock_guard lock(g_log_mutex);
    const double seconds = std::chrono::duration<double>(SteadyClock::now() - g_started).count();
    std::array<char, 16> stamp{};
    std::snprintf(stamp.data(), stamp.size(), "%9.3f", seconds);
    std::cerr << stamp.data() << " " << text << std::endl;
}

struct PlayerSpec {
    std::string url{};
    std::string label{};
    std::optional<std::string> token{};
    std::optional<fs::path> code_log{};
    std::optional<std::string> layout{};
    std::optional<std::vector<double>> trim_db{};
    std::optional<std::string> status_url{};
    bool hold = false;
    bool in_process = false;
};

// What one board's GET /status said, sampled once a second.
struct StatusSample {
    double seconds = 0.0;
    std::optional<std::int64_t> origin_server_us{};
    std::string playing{};
    std::int64_t underruns = 0;
    std::int64_t error_us = 0;
    std::int64_t worst_error_us = 0;
    std::int64_t decode_stack_free = 0;
    std::int64_t server_stack_free = 0;
};

struct PlayerRun {
    PlayerSpec spec{};
    std::string client_id{};
    std::string name{};
    std::string dialect{};
    std::string paired_by{};
    std::int64_t settings_revision = 0;
    std::optional<std::string> failure{};
    std::optional<ss::ClientView> last_view{};
    // What the player reported while it played: its decoder's findings, and its levels.
    std::optional<ac::DecoderReport> decoder{};
    std::vector<ac::Level> levels{};
    std::vector<StatusSample> samples{};
    std::optional<std::string> status_error{};
};

class Events final : public ss::ServerHostEvents {
   public:
    void on_client(const ss::ClientView& client) override {
        const std::lock_guard lock(mutex_);
        if (!clients_.contains(client.client_id)) {
            note("client " + client.client_id.substr(0, 8) + " (" + client.name + ") from " +
                (client.url.empty() ? client.peer : client.url));
        }
        clients_[client.client_id] = client;
    }
    void on_client_gone(const std::string& client_id) override {
        note("client " + client_id.substr(0, 8) + " gone");
        const std::lock_guard lock(mutex_);
        gone_.push_back(client_id);
    }
    void on_pairing_code_wanted(const std::string& client_id) override {
        note("client " + client_id.substr(0, 8) + " waits for its code");
    }
    void on_paired(const std::string& client_id) override { note("client " + client_id.substr(0, 8) + " paired"); }
    void on_pairing_ended(const std::string& client_id, std::optional<ss::pairing_messages::AbortReason> reason) override {
        note("client " + client_id.substr(0, 8) + " pairing ended" + (reason ? " (aborted)" : ""));
        const std::lock_guard lock(mutex_);
        pairing_failed_.push_back(client_id);
    }
    void on_log(std::string_view line) override { note(line); }

    [[nodiscard]] bool gone(const std::string& client_id) {
        const std::lock_guard lock(mutex_);
        return std::find(gone_.begin(), gone_.end(), client_id) != gone_.end();
    }
    [[nodiscard]] bool pairing_failed(const std::string& client_id) {
        const std::lock_guard lock(mutex_);
        return std::find(pairing_failed_.begin(), pairing_failed_.end(), client_id) != pairing_failed_.end();
    }

   private:
    std::mutex mutex_;
    std::map<std::string, ss::ClientView> clients_;
    std::vector<std::string> gone_;
    std::vector<std::string> pairing_failed_;
};

class SinkLog final : public testsink::SinkLog {
   public:
    void line(std::string_view text) override { note("sink: " + std::string(text)); }
};

[[nodiscard]] std::optional<std::vector<std::byte>> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    const std::string bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    std::vector<std::byte> out(bytes.size());
    std::transform(bytes.begin(), bytes.end(), out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}

[[nodiscard]] std::optional<long> parse_number(std::string_view text) {
    long value = 0;
    if (text.empty() || text.size() > 9) {
        return std::nullopt;
    }
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        value = (value * 10) + (c - '0');
    }
    return value;
}

// The burst's two little-endian words at `at` in IEC 61937 carrier bytes.
[[nodiscard]] std::uint16_t word_at(const std::vector<std::byte>& words, std::size_t at) {
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(words[at]) |
                                      (std::to_integer<unsigned>(words[at + 1]) << 8U));
}

// The programme's bursts, made as they are needed: `passes` times through the file's access units
// on one timeline, each burst with the Pc and Pd ac3::iec61937 writes, the units it carries, and the
// programme frame of its first sample.
class BurstSource {
   public:
    BurstSource(const ac3::io::ScannedStream& stream, int passes)
        : stream_(&stream), passes_(passes), pass_samples_(ac3::io::stream_duration_samples(stream)) {}

    [[nodiscard]] std::optional<ss::Group::Burst> next(std::string& error) {
        const bool eac3 = stream_->kind != ac3::io::StreamKind::kAc3;
        payload_.clear();
        std::optional<std::int64_t> frame;
        while (pass_ < passes_) {
            if (unit_ >= stream_->access_units.size()) {
                unit_ = 0;
                ++pass_;
                continue;
            }
            const std::span<const std::byte> unit = stream_->access_units[unit_];
            const std::optional<ac3::io::AccessUnitTiming> timing = ac3::io::access_unit_timing(*stream_, unit_);
            ++unit_;
            if (!timing) {
                error = "an access unit has no timing";
                return std::nullopt;
            }
            if (!frame) {
                frame = static_cast<std::int64_t>((pass_samples_ * static_cast<std::uint64_t>(pass_)) + timing->start_sample);
            }
            std::transform(unit.begin(), unit.end(), std::back_inserter(payload_),
                           [](std::byte b) { return std::to_integer<std::uint8_t>(b); });
            if (!eac3) {
                const auto burst = ac3::iec61937::wrap_frame(unit);
                if (!burst) {
                    error = "an AC-3 frame could not be wrapped";
                    return std::nullopt;
                }
                return ss::Group::Burst{.pc = word_at(*burst, 4), .pd = word_at(*burst, 6), .payload = payload_, .frame = *frame};
            }
            const auto burst = packer_.push(unit);
            if (!burst) {
                error = "an E-AC-3 access unit could not be packed";
                return std::nullopt;
            }
            if (*burst) {
                return ss::Group::Burst{.pc = word_at(**burst, 4), .pd = word_at(**burst, 6), .payload = payload_, .frame = *frame};
            }
        }
        return std::nullopt;
    }

   private:
    const ac3::io::ScannedStream* stream_;
    int passes_;
    std::uint64_t pass_samples_;
    int pass_ = 0;
    std::size_t unit_ = 0;
    ac3::iec61937::Eac3BurstPacker packer_;
    std::vector<std::uint8_t> payload_;
};

// The digits of the first "PAIRING CODE" line in `path` past byte `from`.
[[nodiscard]] std::optional<std::string> code_in(const fs::path& path, std::uintmax_t from) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    in.seekg(static_cast<std::streamoff>(from));
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t at = line.find("PAIRING CODE ");
        if (at == std::string::npos) {
            continue;
        }
        std::string digits;
        for (const char c : line.substr(at + 13)) {
            if (c >= '0' && c <= '9') {
                digits.push_back(c);
            } else if (c != '-' && c != ' ') {
                break;
            }
        }
        if (digits.size() >= 6) {
            return digits;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::uintmax_t size_of(const fs::path& path) {
    std::error_code error;
    const std::uintmax_t size = fs::file_size(path, error);
    return error ? 0 : size;
}

[[nodiscard]] std::string psk_text(ss::handshake::PskCategory psk) {
    switch (psk) {
        case ss::handshake::PskCategory::kLongTerm:
            return "long-term";
        case ss::handshake::PskCategory::kPairing:
            return "pairing";
        case ss::handshake::PskCategory::kSentinel:
            return "sentinel";
    }
    return "unknown";
}

// One GET /status, and what its "sendspin" object says.
[[nodiscard]] std::optional<StatusSample> read_status(const std::string& url, double seconds, std::string& error) {
    const std::size_t scheme = url.find("://");
    const std::size_t path_at = url.find('/', scheme == std::string::npos ? 0 : scheme + 3);
    const std::string origin = path_at == std::string::npos ? url : url.substr(0, path_at);
    const std::string path = path_at == std::string::npos ? "/status" : url.substr(path_at);
    httplib::Client client(origin);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(2, 0);
    const httplib::Result result = client.Get(path);
    if (!result) {
        error = "no answer from " + url;
        return std::nullopt;
    }
    if (result->status != 200) {
        error = url + " answered " + std::to_string(result->status);
        return std::nullopt;
    }
    json::Document document;
    std::vector<json::Token> tokens;
    if (!document.parse(result->body, tokens, 16384)) {
        error = url + " sent no JSON";
        return std::nullopt;
    }
    const json::Value sendspin = document.root()["sendspin"];
    if (!sendspin.is_object()) {
        error = url + " has no Sendspin player";
        return std::nullopt;
    }
    StatusSample sample;
    sample.seconds = seconds;
    sample.origin_server_us = sendspin["origin_server_us"].as_int();
    sample.playing = sendspin["playing"].as_string().value_or("");
    sample.underruns = sendspin["underruns"].as_int().value_or(0);
    sample.error_us = sendspin["error_us"].as_int().value_or(0);
    sample.worst_error_us = sendspin["worst_error_us"].as_int().value_or(0);
    sample.decode_stack_free = sendspin["decode_stack_free"].as_int().value_or(0);
    sample.server_stack_free = sendspin["server_stack_free"].as_int().value_or(0);
    return sample;
}

// Connects, pairs and configures one player; its client_id once it plays the extension role with
// the settings asked for, or a failure.
[[nodiscard]] bool bring_up(ss::ServerHost& host, Events& events, PlayerRun& run, SteadyClock::time_point deadline) {
    const auto fail = [&](std::string why) {
        run.failure = std::move(why);
        note(run.spec.label + ": " + *run.failure);
        return false;
    };
    // Connected and said hello.
    std::optional<ss::ClientView> view;
    while (!view) {
        for (const ss::ClientView& client : host.clients()) {
            if (client.url == run.spec.url) {
                view = client;
            }
        }
        if (!view) {
            if (SteadyClock::now() >= deadline) {
                return fail("did not connect");
            }
            std::this_thread::sleep_for(50ms);
        }
    }
    run.client_id = view->client_id;
    run.name = view->name;
    if (run.spec.label.empty()) {
        run.spec.label = view->name;
    }
    run.dialect = view->dialect == ss::Dialect::kAiosendspin911 ? "aiosendspin 9.1.1" : "specification";
    if (run.spec.hold) {
        note(run.spec.label + ": connected, and held without pairing (" + psk_text(view->psk) + ")");
        return true;
    }
    run.paired_by = run.spec.token ? "token" : view->psk == ss::handshake::PskCategory::kLongTerm ? "record" : "";

    // Paired: by its record, by its token (entered before dialling), or by the code it shows.
    const auto playing = [&](const ss::ClientView& client) {
        return client.playing && client.bursts && client.available &&
               client.psk == ss::handshake::PskCategory::kLongTerm && client.ac3forge_state.has_value();
    };
    if (view->psk != ss::handshake::PskCategory::kLongTerm && !run.spec.token) {
        if (!run.spec.code_log) {
            return fail("is not paired, and has no --token or --code-log");
        }
        const std::uintmax_t from = size_of(*run.spec.code_log);
        if (!host.pair(run.client_id, m::PairMethod::kDynamicCode, m::CodeFormat::kDigits)) {
            return fail("cannot be asked to pair by a code");
        }
        run.paired_by = "code";
        note(run.spec.label + ": asked to show a code; reading " + run.spec.code_log->string());
        std::optional<std::string> code;
        while (!code) {
            code = code_in(*run.spec.code_log, from);
            if (!code) {
                if (SteadyClock::now() >= deadline) {
                    return fail("showed no code in " + run.spec.code_log->string());
                }
                std::this_thread::sleep_for(100ms);
            }
        }
        note(run.spec.label + ": entering the code it shows");
        bool entered = false;
        while (!entered) {
            entered = host.enter_code(run.client_id, *code);
            if (!entered) {
                if (SteadyClock::now() >= deadline || events.pairing_failed(run.client_id)) {
                    return fail("did not take its code");
                }
                std::this_thread::sleep_for(50ms);
            }
        }
    }
    while (true) {
        const std::optional<ss::ClientView> now = host.client(run.client_id);
        if (now && playing(*now)) {
            view = now;
            break;
        }
        if (events.pairing_failed(run.client_id) && run.paired_by == "code") {
            return fail("pairing failed");
        }
        if (SteadyClock::now() >= deadline) {
            if (!now) {
                return fail("went away before it played");
            }
            return fail(std::string("is not playing _ac3forge_player@v1 on a long-term PSK (psk ") + psk_text(now->psk) +
                        (now->playing ? ", playing" : "") + (now->bursts ? ", bursts" : "") +
                        (now->available ? ", available" : "") + ")");
        }
        std::this_thread::sleep_for(50ms);
    }
    note(run.spec.label + ": playing _ac3forge_player@v1 (" + run.dialect + ")");

    // Settings, for a player that takes them.
    if (run.spec.layout) {
        const std::vector<ac::Command>& commands = view->ac3forge_state->supported_commands;
        if (std::find(commands.begin(), commands.end(), ac::Command::kSettings) == commands.end()) {
            return fail("does not take settings");
        }
        const std::int64_t revision =
            std::max<std::int64_t>(view->ac3forge_state->settings_revision + 1,
                                   std::chrono::duration_cast<std::chrono::seconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count());
        ac::CommandMessage command;
        command.command = ac::Command::kSettings;
        command.settings.revision = revision;
        command.settings.layout = *run.spec.layout;
        command.settings.trim_db = run.spec.trim_db;
        if (!host.ac3forge_command(run.client_id, command)) {
            return fail("refused settings for layout " + *run.spec.layout + " before sending them");
        }
        while (true) {
            const std::optional<ss::ClientView> now = host.client(run.client_id);
            if (now && now->ac3forge_state) {
                const ac::State& state = *now->ac3forge_state;
                if (state.settings_error && state.settings_error->revision == revision) {
                    return fail("refused its settings: " + state.settings_error->why);
                }
                if (state.settings_revision == revision) {
                    break;
                }
            }
            if (SteadyClock::now() >= deadline) {
                return fail("did not report its settings as applied");
            }
            std::this_thread::sleep_for(50ms);
        }
        run.settings_revision = revision;
        note(run.spec.label + ": took layout " + *run.spec.layout + " (revision " + std::to_string(revision) + ")");
    }
    return true;
}

void write_report(std::ostream& out, const std::string& server_name, const std::string& server_id,
                  const fs::path& programme, const ac3::io::ScannedStream& stream, std::uint64_t bursts_sent,
                  std::optional<std::int64_t> start_server_us, double played_seconds, const std::vector<PlayerRun>& runs,
                  const std::string& result) {
    std::string text;
    json::Writer w(text);
    w.begin_object();
    w.key("server").begin_object().member("name", server_name).member("id", server_id).end_object();
    w.key("programme").begin_object();
    w.member("file", programme.generic_string());
    w.member("codec", stream.kind == ac3::io::StreamKind::kAc3 ? "AC-3" : "E-AC-3");
    w.member("channels", stream.channels);
    w.member("bursts", bursts_sent);
    w.key("seconds").number(played_seconds, 1);
    w.key("start_server_us");
    start_server_us ? w.integer(*start_server_us) : w.null();
    w.end_object();

    w.key("players").begin_array();
    for (const PlayerRun& run : runs) {
        w.begin_object();
        w.member("label", run.spec.label);
        w.member("url", run.spec.url);
        w.member("client_id", run.client_id);
        w.member("name", run.name);
        w.member("dialect", run.dialect);
        w.member("paired_by", run.paired_by);
        w.key("layout");
        run.spec.layout ? w.string(*run.spec.layout) : w.null();
        w.member("settings_revision", run.settings_revision);
        w.key("failure");
        run.failure ? w.string(*run.failure) : w.null();
        if (run.last_view && run.last_view->ac3forge_state) {
            const ac::State& state = *run.last_view->ac3forge_state;
            w.key("counters").begin_object();
            w.member("bursts_played", state.counters.bursts_played);
            w.member("underruns", state.counters.underruns);
            w.member("late_chunks", state.counters.late_chunks);
            w.member("dropped_chunks", state.counters.dropped_chunks);
            w.member("invalid_chunks", state.counters.invalid_chunks);
            w.end_object();
            w.key("settings_error");
            state.settings_error ? w.string(state.settings_error->why) : w.null();
        }
        w.key("decoder");
        if (run.decoder) {
            w.begin_object();
            w.member("acmod", run.decoder->acmod);
            w.member("lfe", run.decoder->lfe);
            w.member("substreams", run.decoder->substreams);
            w.member("objects", run.decoder->objects);
            w.member("objects_placed", run.decoder->objects_placed);
            w.key("dialnorm").number(run.decoder->dialnorm, 1);
            w.end_object();
        } else {
            w.null();
        }
        w.key("levels").begin_array();
        for (const ac::Level& level : run.levels) {
            w.begin_object();
            w.member("output", level.output);
            w.key("peak_db").number(level.peak_db, 1);
            w.key("rms_db").number(level.rms_db, 1);
            w.end_object();
        }
        w.end_array();
        if (run.spec.status_url) {
            w.key("status").begin_object();
            w.member("url", *run.spec.status_url);
            w.member("samples", static_cast<std::int64_t>(run.samples.size()));
            w.key("error");
            run.status_error ? w.string(*run.status_error) : w.null();
            // Where the board put the first frame, against where the group put it.
            std::optional<std::int64_t> low;
            std::optional<std::int64_t> high;
            std::int64_t underruns = 0;
            std::int64_t worst = 0;
            std::optional<std::int64_t> decode_stack;
            std::optional<std::int64_t> server_stack;
            for (const StatusSample& sample : run.samples) {
                underruns = std::max(underruns, sample.underruns);
                if (std::llabs(sample.worst_error_us) > std::llabs(worst)) {
                    worst = sample.worst_error_us;
                }
                if (sample.decode_stack_free > 0) {
                    decode_stack = std::min(decode_stack.value_or(sample.decode_stack_free), sample.decode_stack_free);
                }
                if (sample.server_stack_free > 0) {
                    server_stack = std::min(server_stack.value_or(sample.server_stack_free), sample.server_stack_free);
                }
                if (!sample.origin_server_us || !start_server_us) {
                    continue;
                }
                const std::int64_t off = *sample.origin_server_us - *start_server_us;
                low = std::min(low.value_or(off), off);
                high = std::max(high.value_or(off), off);
            }
            w.key("origin_error_us").begin_object();
            w.key("min");
            low ? w.integer(*low) : w.null();
            w.key("max");
            high ? w.integer(*high) : w.null();
            w.end_object();
            w.member("underruns", underruns);
            w.member("worst_error_us", worst);
            w.key("decode_stack_free");
            decode_stack ? w.integer(*decode_stack) : w.null();
            w.key("server_stack_free");
            server_stack ? w.integer(*server_stack) : w.null();
            w.end_object();
        }
        w.end_object();
    }
    w.end_array();

    // The boards against each other: at each second every board with a play reported, the
    // difference between the latest and earliest first-frame times.
    std::optional<std::int64_t> spread;
    std::int64_t compared = 0;
    const std::vector<const PlayerRun*> boards = [&] {
        std::vector<const PlayerRun*> found;
        for (const PlayerRun& run : runs) {
            if (run.spec.status_url) {
                found.push_back(&run);
            }
        }
        return found;
    }();
    if (boards.size() >= 2) {
        const std::size_t count = boards.front()->samples.size();
        for (std::size_t i = 0; i < count; ++i) {
            std::optional<std::int64_t> low;
            std::optional<std::int64_t> high;
            bool all = true;
            for (const PlayerRun* board : boards) {
                if (i >= board->samples.size() || !board->samples[i].origin_server_us ||
                    board->samples[i].playing == "idle") {
                    all = false;
                    break;
                }
                const std::int64_t origin = *board->samples[i].origin_server_us;
                low = std::min(low.value_or(origin), origin);
                high = std::max(high.value_or(origin), origin);
            }
            if (all && low && high) {
                spread = std::max(spread.value_or(0), *high - *low);
                ++compared;
            }
        }
    }
    w.key("boards").begin_object();
    w.member("compared_seconds", compared);
    w.key("spread_us");
    spread ? w.integer(*spread) : w.null();
    w.end_object();
    w.member("result", result);
    w.end_object();
    out << text << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<PlayerSpec> players;
    fs::path programme;
    std::optional<long> seconds;
    std::optional<fs::path> sink_directory;
    std::string sink_layout = "2.0";
    fs::path state_directory = "hearth-testserver-state";
    std::string server_name = "Hearth test server";
    long timeout_seconds = 60;
    std::optional<fs::path> report_path;

    const std::vector<std::string_view> arguments(argv + 1, argv + argc);
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string_view argument = arguments[i];
        if (argument == "--help" || argument == "-h") {
            std::cout << kUsage;
            return EXIT_SUCCESS;
        }
        if (argument == "--hold") {
            if (players.empty() || players.back().in_process) {
                std::cerr << "--hold follows a --player\n";
                return kExitUsage;
            }
            players.back().hold = true;
            continue;
        }
        if (i + 1 >= arguments.size()) {
            std::cerr << argument << " needs a value\n";
            return kExitUsage;
        }
        const std::string value(arguments[++i]);
        const auto player = [&]() -> PlayerSpec* {
            if (players.empty() || players.back().in_process) {
                std::cerr << argument << " follows a --player\n";
                return nullptr;
            }
            return &players.back();
        };
        if (argument == "--player") {
            players.push_back({.url = value});
        } else if (argument == "--label" || argument == "--token" || argument == "--code-log" ||
                   argument == "--layout" || argument == "--trim-db" || argument == "--status") {
            PlayerSpec* spec = player();
            if (spec == nullptr) {
                return kExitUsage;
            }
            if (argument == "--label") {
                spec->label = value;
            } else if (argument == "--token") {
                spec->token = value;
            } else if (argument == "--code-log") {
                spec->code_log = fs::path(value);
            } else if (argument == "--layout") {
                spec->layout = value;
            } else if (argument == "--trim-db") {
                std::vector<double> trims;
                std::string_view list = value;
                while (!list.empty()) {
                    const std::size_t comma = list.find(',');
                    const std::string item(list.substr(0, comma));
                    char* end = nullptr;
                    const double trim = std::strtod(item.c_str(), &end);
                    if (item.empty() || end != item.c_str() + item.size()) {
                        std::cerr << "--trim-db takes numbers of dB separated by commas\n";
                        return kExitUsage;
                    }
                    trims.push_back(trim);
                    list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
                }
                spec->trim_db = std::move(trims);
            } else {
                spec->status_url = value;
            }
        } else if (argument == "--play") {
            programme = value;
        } else if (argument == "--seconds") {
            seconds = parse_number(value);
            if (!seconds || *seconds == 0) {
                std::cerr << "--seconds takes a whole number of seconds\n";
                return kExitUsage;
            }
        } else if (argument == "--sink") {
            sink_directory = fs::path(value);
        } else if (argument == "--sink-layout") {
            sink_layout = value;
        } else if (argument == "--state") {
            state_directory = value;
        } else if (argument == "--name") {
            server_name = value;
        } else if (argument == "--timeout") {
            const std::optional<long> parsed = parse_number(value);
            if (!parsed || *parsed == 0) {
                std::cerr << "--timeout takes a whole number of seconds\n";
                return kExitUsage;
            }
            timeout_seconds = *parsed;
        } else if (argument == "--report") {
            report_path = fs::path(value);
        } else {
            std::cerr << "unknown option " << argument << "\n\n" << kUsage;
            return kExitUsage;
        }
    }
    if (programme.empty() || (players.empty() && !sink_directory)) {
        std::cerr << kUsage;
        return kExitUsage;
    }

    const std::optional<std::vector<std::byte>> bytes = read_file(programme);
    if (!bytes) {
        std::cerr << "cannot read " << programme.string() << "\n";
        return kExitUsage;
    }
    const std::expected<ac3::io::ScannedStream, ac3::io::ScanError> stream = ac3::io::scan(*bytes);
    if (!stream || stream->access_units.empty()) {
        std::cerr << programme.string() << " is not an AC-3 or E-AC-3 stream\n";
        return kExitUsage;
    }
    if (stream->sample_rate != ac3::SampleRate::k48000) {
        std::cerr << programme.string() << " is not at 48 kHz, which is all a Hearth sink plays\n";
        return kExitUsage;
    }
    const std::uint64_t pass_samples = ac3::io::stream_duration_samples(*stream);
    const int passes =
        seconds ? static_cast<int>(((static_cast<std::uint64_t>(*seconds) * kSampleRate) + pass_samples - 1) / pass_samples) : 1;

    auto store = ac3::hearth::testserver::FileServerStore::open(state_directory);
    if (!store) {
        std::cerr << "ac3hearth-testserver: " << store.error() << "\n";
        return kExitUsage;
    }
    Events events;
    auto host = ss::ServerHost::start({.identity = (*store)->identity(),
                                       .name = server_name,
                                       .languages = {"en"},
                                       .address = "127.0.0.1",
                                       .port = std::nullopt,
                                       .advertise = false,
                                       .browse = false,
                                       .mdns_interfaces = {}},
                                      **store, events);
    if (!host) {
        std::cerr << "ac3hearth-testserver: " << host.error() << "\n";
        return kExitUsage;
    }
    note("server " + (*host)->server_id().substr(0, 8) + " (\"" + server_name + "\")");

    // A test sink in this process, as the reference the boards' levels are held to.
    SinkLog sink_log;
    std::unique_ptr<testsink::Sink> sink;
    if (sink_directory) {
        testsink::SinkOptions options;
        options.name = "Reference sink";
        options.address = "127.0.0.1";
        options.port = 0;
        options.state_directory = *sink_directory / "state";
        options.output_directory = *sink_directory / "out";
        options.advertise = false;
        options.codecs = {m::Codec::kPcm};
        options.layout = sink_layout;
        auto started = testsink::Sink::start(options, sink_log);
        if (!started) {
            std::cerr << "ac3hearth-testserver: the test sink: " << started.error() << "\n";
            return kExitUsage;
        }
        sink = std::move(*started);
        players.push_back({.url = "ws://127.0.0.1:" + std::to_string(sink->port()) + "/sendspin",
                           .label = "reference sink",
                           .token = sink->pairing_token(),
                           .in_process = true});
    }

    std::vector<PlayerRun> runs;
    for (const PlayerSpec& spec : players) {
        if (spec.token && !(*host)->enter_pairing_token(*spec.token)) {
            std::cerr << "not a pairing token: " << *spec.token << "\n";
            return kExitUsage;
        }
        runs.push_back({.spec = spec});
        (*host)->dial(spec.url);
    }

    std::string server_id = (*host)->server_id();
    const auto finish = [&](int status, const std::string& result, std::optional<std::int64_t> start, std::uint64_t sent,
                            double played) {
        for (PlayerRun& run : runs) {
            if (!run.client_id.empty()) {
                if (std::optional<ss::ClientView> view = (*host)->client(run.client_id)) {
                    run.last_view = std::move(view);
                }
            }
        }
        if (report_path) {
            std::ofstream out(*report_path, std::ios::binary | std::ios::trunc);
            write_report(out, server_name, server_id, programme, *stream, sent, start, played, runs, result);
        } else {
            write_report(std::cout, server_name, server_id, programme, *stream, sent, start, played, runs, result);
        }
        note(result);
        return status;
    };

    // Every player connected, paired and configured, together within the timeout.
    const SteadyClock::time_point deadline = SteadyClock::now() + std::chrono::seconds(timeout_seconds);
    for (PlayerRun& run : runs) {
        if (!bring_up(**host, events, run, deadline)) {
            return finish(kExitSetup, "failed: " + run.spec.label + " " + *run.failure, std::nullopt, 0, 0.0);
        }
    }

    // Held players only: nothing is played, and the connections are kept for the programme's
    // length, for another server to find the players taken.
    const bool any_played = std::any_of(runs.begin(), runs.end(), [](const PlayerRun& run) { return !run.spec.hold; });
    if (!any_played) {
        const auto hold_for = std::chrono::seconds(seconds.value_or(10));
        note("holding " + std::to_string(runs.size()) + " connection(s) for " +
             std::to_string(static_cast<long>(hold_for.count())) + " s");
        std::this_thread::sleep_for(hold_for);
        for (PlayerRun& run : runs) {
            run.failure = events.gone(run.client_id) ? std::optional<std::string>("displaced or closed") : std::nullopt;
        }
        return finish(EXIT_SUCCESS, "pass", std::nullopt, 0, 0.0);
    }

    std::shared_ptr<ss::Group> group = (*host)->make_group("Test group");
    for (const PlayerRun& run : runs) {
        if (!run.spec.hold) {
            group->add(run.client_id);
        }
    }
    const ac::DataType data_type =
        stream->kind == ac3::io::StreamKind::kAc3 ? ac::DataType::kAc3 : ac::DataType::kEac3;
    if (!group->start({.pcm = std::nullopt,
                       .bursts = ac::StreamStart{.data_type = data_type, .sample_rate = static_cast<std::int32_t>(kSampleRate)},
                       .buffered = true})) {
        return finish(kExitSetup, "failed: the group did not start", std::nullopt, 0, 0.0);
    }
    const double programme_seconds = static_cast<double>(pass_samples) * passes / kSampleRate;
    note("playing " + programme.filename().string() + " " + std::to_string(passes) + " time(s) through, " +
        std::to_string(static_cast<long>(programme_seconds)) + " s, to " + std::to_string(runs.size()) + " player(s)");

    // The boards' status, once a second, on a thread of its own.
    std::atomic<bool> playing{true};
    std::mutex samples_mutex;
    const SteadyClock::time_point play_started = SteadyClock::now();
    std::thread poller([&] {
        SteadyClock::time_point next = SteadyClock::now();
        while (playing.load()) {
            next += 1s;
            const double at = std::chrono::duration<double>(SteadyClock::now() - play_started).count();
            for (PlayerRun& run : runs) {
                if (!run.spec.status_url) {
                    continue;
                }
                std::string error;
                std::optional<StatusSample> sample = read_status(*run.spec.status_url, at, error);
                const std::lock_guard lock(samples_mutex);
                if (sample) {
                    run.samples.push_back(std::move(*sample));
                } else {
                    run.status_error = error;
                    run.samples.push_back({.seconds = at});
                }
            }
            std::this_thread::sleep_until(next);
        }
    });

    BurstSource source(*stream, passes);
    std::uint64_t sent = 0;
    // The programme frame after the last burst sent.
    std::int64_t end_frame = 0;
    std::string failure;
    std::optional<ss::Group::Burst> burst;
    std::string error;
    // Each player still there, and what it last said of its decoder and levels.
    const auto check_players = [&] {
        for (PlayerRun& run : runs) {
            if (run.spec.hold) {
                continue;
            }
            const std::optional<ss::ClientView> view = (*host)->client(run.client_id);
            if (events.gone(run.client_id) || !view) {
                failure = run.spec.label + " went away during the play";
                continue;
            }
            if (view->ac3forge_state) {
                if (view->ac3forge_state->decoder) {
                    run.decoder = view->ac3forge_state->decoder;
                }
                if (view->ac3forge_state->levels && !view->ac3forge_state->levels->empty()) {
                    run.levels = *view->ac3forge_state->levels;
                }
            }
        }
    };
    const SteadyClock::time_point play_deadline =
        SteadyClock::now() + std::chrono::milliseconds(static_cast<std::int64_t>(programme_seconds * 1500.0)) + 60s;
    SteadyClock::time_point next_check = SteadyClock::now();
    while (failure.empty()) {
        if (!burst) {
            burst = source.next(error);
            if (!burst) {
                break;
            }
        }
        if (group->push_burst(*burst)) {
            ++sent;
            end_frame = burst->frame + kSamplesPerBurst;
            burst.reset();
        } else {
            std::this_thread::sleep_for(5ms);
        }
        if (SteadyClock::now() >= next_check) {
            next_check = SteadyClock::now() + 250ms;
            check_players();
            if (SteadyClock::now() >= play_deadline) {
                failure = "the play took too long: the group stopped taking bursts";
            }
        }
    }
    if (failure.empty() && !error.empty()) {
        failure = error;
    }
    const std::optional<std::int64_t> start_server_us = group->start_time();
    // The group sends ahead of play, and stream/end has a player drop what it has not played yet
    // (planning/hearth-sendspin-extension.md, stream/end): the end waits for the last burst to have
    // played, and for the players' output queues after it.
    if (failure.empty() && start_server_us) {
        const std::int64_t played_out_us = *start_server_us + (end_frame * 1'000'000 / kSampleRate) + kEndMarginUs;
        const ss::SteadyClock server_clock;
        while (failure.empty() && server_clock.now_us() < played_out_us) {
            std::this_thread::sleep_for(250ms);
            check_players();
        }
    }
    group->stop();

    // Every player has played every burst, or has stopped counting.
    if (failure.empty()) {
        const SteadyClock::time_point settle = SteadyClock::now() + 30s;
        std::map<std::string, std::pair<std::uint64_t, SteadyClock::time_point>> last;
        while (SteadyClock::now() < settle) {
            bool done = true;
            for (const PlayerRun& run : runs) {
                const std::optional<ss::ClientView> view =
                    run.spec.hold ? std::nullopt : (*host)->client(run.client_id);
                if (!view || !view->ac3forge_state) {
                    continue;
                }
                const ac::Counters& counters = view->ac3forge_state->counters;
                const std::uint64_t accounted = counters.bursts_played + counters.late_chunks + counters.dropped_chunks;
                auto& [count, since] = last[run.client_id];
                if (accounted != count) {
                    count = accounted;
                    since = SteadyClock::now();
                }
                if (accounted < sent && SteadyClock::now() - since < 3s) {
                    done = false;
                }
            }
            if (done) {
                break;
            }
            std::this_thread::sleep_for(100ms);
        }
        std::this_thread::sleep_for(1500ms);
    }
    playing.store(false);
    poller.join();
    const double played = std::chrono::duration<double>(SteadyClock::now() - play_started).count();

    // What each player reported of the play.
    for (PlayerRun& run : runs) {
        if (run.spec.hold) {
            continue;
        }
        const std::optional<ss::ClientView> view = (*host)->client(run.client_id);
        if (!view || !view->ac3forge_state) {
            if (failure.empty()) {
                failure = run.spec.label + " reported nothing at the end";
            }
            continue;
        }
        const ac::Counters& counters = view->ac3forge_state->counters;
        note(run.spec.label + ": " + std::to_string(counters.bursts_played) + " bursts played of " +
            std::to_string(sent) + ", " + std::to_string(counters.underruns) + " underruns, " +
            std::to_string(counters.late_chunks) + " late, " + std::to_string(counters.dropped_chunks) + " dropped, " +
            std::to_string(counters.invalid_chunks) + " invalid");
        if (failure.empty() && counters.invalid_chunks > 0) {
            failure = run.spec.label + " found invalid chunks";
        }
        if (failure.empty() && counters.bursts_played + counters.late_chunks + counters.dropped_chunks < sent) {
            failure = run.spec.label + " did not account for every burst sent";
        }
        // The whole programme, played: a burst dropped for want of room, or
        // too late to play, is a gap in what was heard.
        if (failure.empty() && (counters.late_chunks > 0 || counters.dropped_chunks > 0)) {
            failure = run.spec.label + " did not play every burst: " + std::to_string(counters.late_chunks) +
                      " late, " + std::to_string(counters.dropped_chunks) + " dropped with no room for them";
        }
        if (failure.empty() && counters.underruns > 0) {
            failure = run.spec.label + " ran out of audio " + std::to_string(counters.underruns) + " time(s)";
        }
    }
    group.reset();
    if (!failure.empty()) {
        return finish(kExitPlay, "failed: " + failure, start_server_us, sent, played);
    }
    return finish(EXIT_SUCCESS, "pass", start_server_us, sent, played);
}
