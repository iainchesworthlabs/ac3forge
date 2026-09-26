#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "platform/process.hpp"

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/decoder/output.hpp"
#include "ac3/iec61937/iec61937.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/render/layout.hpp"
#include "ac3/render/render.hpp"
#include "ac3/render/serving.hpp"
#include "ac3/sendspin/ac3forge_player.hpp"
#include "ac3/sendspin/crypto.hpp"
#include "ac3/sendspin/handshake.hpp"
#include "ac3/sendspin/messages.hpp"
#include "ac3/sendspin/noise.hpp"
#include "ac3/sendspin/pairing_messages.hpp"
#include "ac3/sendspin/server_host.hpp"
#include "ac3/sendspin/server_store.hpp"
#include "ac3/sendspin/state_roles.hpp"
#include "ac3/sendspin/stream_roles.hpp"
#include "ac4/ac4.hpp"
#include "ac4dec/decoder.hpp"
#include "burst_output.hpp"
#include "sink.hpp"

// A ServerHost and two test sinks in process over loopback WebSockets, with mDNS off: the host
// dials both and plays one programme to them as a group. Approved for unpaired access, they take
// player@v1, PCM to one and FLAC to the other; paired by their tokens, they take
// _ac3forge_player@v1, and the programme is the Dolby Encoding Engine's E-AC-3 JOC fixture in
// bursts, rendered to their speaker layout with its objects. Each sink's WAV holds exactly what a
// local decode and render of the programme gives, and every chunk's logged play time puts the
// programme's first frame at the same local time on both sinks, within 1 ms: the group plays in
// step (planning/hearth-reference-player.md, A4's exit). A hidden case, [hearth-soak], plays the
// E-AC-3 programme for ten minutes. Sinks that list the other roles get the group's metadata,
// colours, transport, artwork and visualizer frames, and a controller's volume and mute reach every
// player in the group.
//
// It dials, so under ThreadSanitizer it needs what tests/sendspin/test_websocket.cpp says.

namespace {

namespace fs = std::filesystem;
namespace m = ac3::sendspin::messages;
namespace testsink = ac3::hearth::testsink;
using namespace std::chrono_literals;

// See tests/cli/test_cli.cpp's own scratch_dir comment for why every
// TEST_CASE below folds this into its scratch leaf, on top of
// AC3FORGE_TEST_SCRATCH_DIR's build-tree rooting.
std::string scratch_pid_suffix() { return ac3::test::platform::process_id(); }

class QuietLog final : public testsink::SinkLog {
   public:
    void line(std::string_view text) override {
        const std::lock_guard lock(mutex_);
        const std::size_t at = text.find("PAIRING CODE ");
        if (at != std::string_view::npos) {
            std::string digits;
            for (const char c : text.substr(at + 13)) {
                if (c >= '0' && c <= '9') {
                    digits.push_back(c);
                }
            }
            code_ = digits;
        }
    }

    // The last dynamic pairing code a sink showed, as digits.
    std::optional<std::string> code() {
        const std::lock_guard lock(mutex_);
        return code_;
    }

   private:
    std::mutex mutex_;
    std::optional<std::string> code_;
};

class HostEvents final : public ac3::sendspin::ServerHostEvents {
   public:
    void on_client(const ac3::sendspin::ClientView& client) override {
        {
            const std::lock_guard lock(mutex_);
            clients_[client.client_id] = client;
        }
        changed_.notify_all();
    }
    void on_client_gone(const std::string& /*client_id*/) override {}
    void on_client_goodbye(const std::string& client_id, ac3::sendspin::messages::GoodbyeReason reason) override {
        {
            const std::lock_guard lock(mutex_);
            goodbyes_[client_id] = reason;
        }
        changed_.notify_all();
    }
    void on_pairing_code_wanted(const std::string& /*client_id*/) override {}
    void on_paired(const std::string& /*client_id*/) override {}
    void on_pairing_ended(const std::string& /*client_id*/,
                          std::optional<ac3::sendspin::pairing_messages::AbortReason> /*reason*/) override {}
    void on_log(std::string_view /*line*/) override {}

    struct Command {
        std::string group_id;
        std::string client_id;
        ac3::sendspin::controller::CommandMessage command;
    };
    void on_controller_command(const std::string& group_id, const std::string& client_id,
                               const ac3::sendspin::controller::CommandMessage& command) override {
        const std::lock_guard lock(mutex_);
        commands_.push_back({.group_id = group_id, .client_id = client_id, .command = command});
    }

    template <class Predicate>
    bool wait(Predicate&& predicate, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return predicate(clients_); });
    }

    // The controller commands the host has passed on, in order.
    std::vector<Command> commands() {
        const std::lock_guard lock(mutex_);
        return commands_;
    }

    // client/goodbye's own reason, once on_client_goodbye() has heard one for this client_id.
    std::optional<ac3::sendspin::messages::GoodbyeReason> goodbye(const std::string& client_id) {
        const std::lock_guard lock(mutex_);
        const auto found = goodbyes_.find(client_id);
        return found == goodbyes_.end() ? std::nullopt : std::optional(found->second);
    }

    bool wait_for_goodbye(const std::string& client_id, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return goodbyes_.contains(client_id); });
    }

   private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::map<std::string, ac3::sendspin::ClientView> clients_;
    std::vector<Command> commands_;
    std::map<std::string, ac3::sendspin::messages::GoodbyeReason> goodbyes_;
};

// Polls `predicate` until it holds or `timeout` passes.
template <class Predicate>
bool eventually(Predicate&& predicate, std::chrono::milliseconds timeout) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= until) {
            return false;
        }
        std::this_thread::sleep_for(20ms);
    }
    return true;
}

std::unique_ptr<testsink::Sink> start_sink(const fs::path& directory, std::string name, m::Codec codec, QuietLog& log,
                                          bool unpaired_access = true) {
    testsink::SinkOptions options;
    options.name = std::move(name);
    options.address = "127.0.0.1";
    options.port = 0;
    options.state_directory = directory / "state";
    options.output_directory = directory / "out";
    options.advertise = false;
    options.unpaired_access = unpaired_access;
    options.codecs = {codec};
    auto sink = testsink::Sink::start(options, log);
    REQUIRE(sink.has_value());
    return std::move(*sink);
}

// The local time each logged chunk puts the stream's first frame at.
std::vector<double> first_frame_times(const fs::path& log) {
    std::vector<double> times;
    std::ifstream in(log);
    std::string line;
    std::getline(in, line);
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string local;
        std::string first;
        if (!std::getline(fields, local, ',') || !std::getline(fields, first, ',') || local == "clear") {
            continue;
        }
        times.push_back(std::stod(local) - (std::stod(first) * 1'000'000.0 / 48000.0));
    }
    return times;
}

// The only file in `directory` whose name starts with `prefix` and ends with `extension`.
fs::path only_file(const fs::path& directory, std::string_view prefix, std::string_view extension) {
    std::vector<fs::path> found;
    for (const fs::directory_entry& entry : fs::directory_iterator(directory)) {
        const std::string name = entry.path().filename().string();
        if (name.starts_with(prefix) && name.ends_with(extension)) {
            found.push_back(entry.path());
        }
    }
    REQUIRE(found.size() == 1);
    return found.front();
}

std::vector<std::byte> read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    const std::string bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    std::vector<std::byte> out(bytes.size());
    std::transform(bytes.begin(), bytes.end(), out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}

struct PackedBurst {
    std::uint16_t pc = 0;
    std::uint16_t pd = 0;
    std::vector<std::uint8_t> payload;
    std::int64_t frame = 0;
};

// The bursts ac3::iec61937::Eac3BurstPacker makes of `stream`'s access units, `passes` times over as
// one programme: each with the Pc and Pd the packer writes, the access units it holds, and the
// programme frame of its first sample.
std::vector<PackedBurst> pack_bursts(const ac3::io::ScannedStream& stream, int passes) {
    ac3::iec61937::Eac3BurstPacker packer;
    std::vector<PackedBurst> bursts;
    const std::uint64_t pass_samples = ac3::io::stream_duration_samples(stream);
    PackedBurst pending;
    for (int pass = 0; pass < passes; ++pass) {
        for (std::size_t i = 0; i < stream.access_units.size(); ++i) {
            const std::span<const std::byte> unit = stream.access_units[i];
            if (pending.payload.empty()) {
                const std::optional<ac3::io::AccessUnitTiming> timing = ac3::io::access_unit_timing(stream, i);
                REQUIRE(timing.has_value());
                pending.frame = static_cast<std::int64_t>((pass_samples * static_cast<std::uint64_t>(pass)) +
                                                          timing->start_sample);
            }
            std::transform(unit.begin(), unit.end(), std::back_inserter(pending.payload),
                           [](std::byte b) { return std::to_integer<std::uint8_t>(b); });
            const auto burst = packer.push(unit);
            REQUIRE(burst.has_value());
            if (*burst) {
                // The carrier burst's words are little-endian: Pa, Pb, Pc, Pd, then the payload.
                const std::vector<std::byte>& words = **burst;
                const auto word = [&](std::size_t at) {
                    return static_cast<std::uint16_t>(std::to_integer<unsigned>(words[at]) |
                                                      (std::to_integer<unsigned>(words[at + 1]) << 8U));
                };
                pending.pc = word(4);
                pending.pd = word(6);
                bursts.push_back(std::move(pending));
                pending = PackedBurst{};
            }
        }
    }
    return bursts;
}

// A local decode and render of `stream`'s access units, `passes` times over, as the test sink's
// BurstOutput decodes and renders (burst_output.hpp): each block of `layout`'s slots to `consume`.
template <class Consume>
void decode_and_render(const ac3::io::ScannedStream& stream, int passes, const ac3::render::OutputLayout& layout,
                       Consume&& consume) {
    const ac3::render::Serving serving =
        ac3::render::serve(layout, ac3::DownmixTarget::kLoRo, ac3::render::ObjectsPolicy::kAuto);
    REQUIRE_FALSE(serving.fold.has_value());
    ac3::DecoderConfig config;
    config.output.mode = ac3::OperatingMode::kLine;
    ac3::render::configure_decoder(serving, config);
    ac3::Eac3Decoder decoder(config);
    ac3::render::LayoutRenderer renderer(layout);
    const std::size_t slots = layout.slots();
    std::vector<std::array<float, ac3::kSamplesPerBlock>> block(slots);
    std::vector<std::span<float>> spans;
    for (std::array<float, ac3::kSamplesPerBlock>& slot : block) {
        spans.emplace_back(slot);
    }
    // Each unit's bed, taken by its first block whichever call delivers it.
    std::deque<ac3::eac3::chanmap::Layout> beds;
    for (int pass = 0; pass < passes; ++pass) {
        for (const std::span<const std::byte> unit : stream.access_units) {
            const std::expected<ac3::io::ScannedStream, ac3::io::ScanError> scanned = ac3::io::scan(unit);
            REQUIRE(scanned.has_value());
            beds.push_back(ac3::eac3::chanmap::expand(scanned->channel_map));
            const auto decoded = decoder.decode_access_unit_by_block(unit, [&](const ac3::PcmBlock& pcm) {
                if (pcm.index == 0) {
                    renderer.set_bed(beds.front());
                    beds.pop_front();
                    if (serving.reconstruct) {
                        renderer.set_objects(pcm.object_metadata, pcm.objects.size());
                    }
                }
                renderer.render(pcm, serving.reconstruct, 1.0F, spans);
                consume(std::span<const std::array<float, ac3::kSamplesPerBlock>>(block),
                        pcm.channels.empty() ? std::size_t{0} : pcm.channels.front().size());
            });
            REQUIRE(decoded.has_value());
        }
    }
}

// Plays the Dolby Encoding Engine's E-AC-3 JOC fixture `passes` times over to two test sinks paired
// by their tokens, as one programme over _ac3forge_player@v1 rendered to `layout_text`. Then each
// sink's WAV must be a local decode and render of the programme, sample for sample, and every
// burst's logged play time must put the first frame at the same local time on both, within 1 ms.
void play_joc_programme(const fs::path& scratch, const std::string& layout_text, int passes) {
    fs::remove_all(scratch);
    const std::optional<ac3::render::OutputLayout> layout = ac3::render::OutputLayout::parse(layout_text);
    REQUIRE(layout.has_value());
    const std::vector<std::byte> fixture = read_bytes(AC3FORGE_GOLDEN_OBJECT_DIR "/dee_joc_514.ec3");
    const std::expected<ac3::io::ScannedStream, ac3::io::ScanError> stream = ac3::io::scan(fixture);
    REQUIRE(stream.has_value());
    const std::vector<PackedBurst> bursts = pack_bursts(*stream, passes);
    REQUIRE(bursts.size() == stream->access_units.size() * static_cast<std::size_t>(passes));

    QuietLog log;
    const auto make_sink = [&](const fs::path& directory, std::string name) {
        testsink::SinkOptions options;
        options.name = std::move(name);
        options.address = "127.0.0.1";
        options.port = 0;
        options.state_directory = directory / "state";
        options.output_directory = directory / "out";
        options.advertise = false;
        options.codecs = {m::Codec::kPcm};
        options.layout = layout_text;
        auto started = testsink::Sink::start(options, log);
        REQUIRE(started.has_value());
        return std::move(*started);
    };
    const std::unique_ptr<testsink::Sink> kitchen = make_sink(scratch / "kitchen", "Kitchen");
    const std::unique_ptr<testsink::Sink> lounge = make_sink(scratch / "lounge", "Lounge");

    std::optional<ac3::sendspin::noise::KeyPair> identity = ac3::sendspin::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    ac3::sendspin::MemoryServerStore store;
    HostEvents events;
    auto host = ac3::sendspin::ServerHost::start(
        {.identity = *identity, .name = "Test host", .languages = {"en"}, .address = "127.0.0.1", .port = std::nullopt,
         .advertise = false, .browse = false, .mdns_interfaces = {}},
        store, events);
    REQUIRE(host.has_value());
    REQUIRE((*host)->enter_pairing_token(kitchen->pairing_token()));
    REQUIRE((*host)->enter_pairing_token(lounge->pairing_token()));
    (*host)->dial("ws://127.0.0.1:" + std::to_string(kitchen->port()) + "/sendspin");
    (*host)->dial("ws://127.0.0.1:" + std::to_string(lounge->port()) + "/sendspin");

    // Paired, both play the extension role once their clocks converge.
    REQUIRE(events.wait(
        [](const auto& clients) {
            return clients.size() == 2 && std::all_of(clients.begin(), clients.end(), [](const auto& entry) {
                       return entry.second.playing && entry.second.bursts && entry.second.available &&
                              entry.second.psk == ac3::sendspin::handshake::PskCategory::kLongTerm;
                   });
        },
        30s));

    std::shared_ptr<ac3::sendspin::Group> group = (*host)->make_group("Downstairs");
    for (const ac3::sendspin::ClientView& client : (*host)->clients()) {
        group->add(client.client_id);
    }
    REQUIRE(group->start({.pcm = std::nullopt,
                          .bursts = ac3::sendspin::ac3forge::StreamStart{.data_type = ac3::sendspin::ac3forge::DataType::kEac3,
                                                                         .sample_rate = 48000},
                          .buffered = true}));

    // As fast as the group takes them.
    std::size_t next = 0;
    const auto deadline = std::chrono::steady_clock::now() + 30s +
                          std::chrono::milliseconds(static_cast<std::int64_t>(bursts.size()) * 32 * 3 / 2);
    while (next < bursts.size() && std::chrono::steady_clock::now() < deadline) {
        const PackedBurst& burst = bursts[next];
        if (group->push_burst({.pc = burst.pc, .pd = burst.pd, .payload = burst.payload, .frame = burst.frame})) {
            ++next;
        } else {
            std::this_thread::sleep_for(5ms);
        }
    }
    REQUIRE(next == bursts.size());
    CHECK(group->members_playing() == 2);
    // What each sink's decoder found reaches the host in its state.
    const bool reported = events.wait(
        [](const auto& clients) {
            return clients.size() == 2 && std::all_of(clients.begin(), clients.end(), [](const auto& entry) {
                       const std::optional<ac3::sendspin::ac3forge::State>& state = entry.second.ac3forge_state;
                       return state && state->decoder && state->decoder->objects > 0 && state->decoder->objects_placed;
                   });
        },
        10s);
    if (!reported) {
        for (const ac3::sendspin::ClientView& client : (*host)->clients()) {
            const std::optional<ac3::sendspin::ac3forge::State>& state = client.ac3forge_state;
            UNSCOPED_INFO(client.name << ": decoder reported " << (state && state->decoder) << ", objects "
                                      << (state && state->decoder ? state->decoder->objects : -1));
        }
    }
    CHECK(reported);
    group->stop();

    // Both sinks have every burst.
    const auto received = [](const testsink::Sink& sink) { return sink.totals().bursts; };
    const auto until = std::chrono::steady_clock::now() + 30s;
    while ((received(*kitchen) < bursts.size() || received(*lounge) < bursts.size()) &&
           std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(20ms);
    }
    REQUIRE(received(*kitchen) == bursts.size());
    REQUIRE(received(*lounge) == bursts.size());
    // A group must not outlive its host.
    group.reset();
    host->reset();

    // Each WAV is the local decode and render, sample for sample, both read a block at a time
    // against one decode.
    struct Played {
        ac3::io::WavStreamReader wav;
        std::vector<std::vector<float>> samples;
        std::vector<std::span<float>> spans;
        std::uint64_t different = 0;
    };
    std::array<Played, 2> played;
    const std::array<fs::path, 2> directories{scratch / "kitchen", scratch / "lounge"};
    for (std::size_t i = 0; i < played.size(); ++i) {
        REQUIRE(played[i].wav.open(only_file(directories[i] / "out", "bursts-", ".wav").string()).has_value());
        REQUIRE(static_cast<std::size_t>(played[i].wav.channels()) == layout->slots());
        played[i].samples.assign(layout->slots(), std::vector<float>(ac3::kSamplesPerBlock));
        played[i].spans.assign(played[i].samples.begin(), played[i].samples.end());
    }
    std::uint64_t frames = 0;
    decode_and_render(*stream, passes, *layout,
                      [&](std::span<const std::array<float, ac3::kSamplesPerBlock>> block, std::size_t n) {
                          for (Played& sink_played : played) {
                              const std::expected<std::size_t, ac3::io::WavError> got =
                                  sink_played.wav.read_planar(sink_played.spans, n);
                              REQUIRE(got.has_value());
                              REQUIRE(*got == n);
                              for (std::size_t slot = 0; slot < block.size(); ++slot) {
                                  for (std::size_t t = 0; t < n; ++t) {
                                      sink_played.different += block[slot][t] == sink_played.samples[slot][t] ? 0U : 1U;
                                  }
                              }
                          }
                          frames += n;
                      });
    CHECK(frames > 0);
    for (const Played& sink_played : played) {
        CHECK(sink_played.different == 0);
        CHECK(sink_played.wav.frame_count() == frames);
    }

    // Every burst on both sinks puts the first frame at the same local time, within 1 ms.
    std::vector<double> times = first_frame_times(only_file(scratch / "kitchen" / "out", "bursts-", ".times.csv"));
    const std::vector<double> lounge_times = first_frame_times(only_file(scratch / "lounge" / "out", "bursts-", ".times.csv"));
    CHECK(times.size() == bursts.size());
    CHECK(lounge_times.size() == bursts.size());
    times.insert(times.end(), lounge_times.begin(), lounge_times.end());
    const auto [earliest, latest] = std::minmax_element(times.begin(), times.end());
    CHECK(*latest - *earliest < 1000.0);
}

}  // namespace

TEST_CASE("group: two test sinks play one programme in step, in PCM and FLAC", "[hearth][group][websocket]") {
    const fs::path scratch = fs::path{AC3FORGE_TEST_SCRATCH_DIR} / ("hearth_group_" + scratch_pid_suffix());
    fs::remove_all(scratch);
    QuietLog log;
    const std::unique_ptr<testsink::Sink> kitchen = start_sink(scratch / "kitchen", "Kitchen", m::Codec::kPcm, log);
    const std::unique_ptr<testsink::Sink> lounge = start_sink(scratch / "lounge", "Lounge", m::Codec::kFlac, log);

    std::optional<ac3::sendspin::noise::KeyPair> identity = ac3::sendspin::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    ac3::sendspin::MemoryServerStore store;
    HostEvents events;
    auto host = ac3::sendspin::ServerHost::start(
        {.identity = *identity, .name = "Test host", .languages = {"en"}, .address = "127.0.0.1", .port = std::nullopt,
         .advertise = false, .browse = false, .mdns_interfaces = {}},
        store, events);
    REQUIRE(host.has_value());
    (*host)->dial("ws://127.0.0.1:" + std::to_string(kitchen->port()) + "/sendspin");
    (*host)->dial("ws://127.0.0.1:" + std::to_string(lounge->port()) + "/sendspin");

    REQUIRE(events.wait([](const auto& clients) { return clients.size() == 2; }, 15s));
    for (const ac3::sendspin::ClientView& client : (*host)->clients()) {
        CHECK_FALSE(client.playing);
        REQUIRE((*host)->approve(client.client_id, true));
    }
    // Approved, both play once their clocks converge.
    REQUIRE(events.wait(
        [](const auto& clients) {
            return clients.size() == 2 && std::all_of(clients.begin(), clients.end(), [](const auto& entry) {
                       return entry.second.playing && entry.second.available;
                   });
        },
        20s));

    std::shared_ptr<ac3::sendspin::Group> group = (*host)->make_group("Downstairs");
    for (const ac3::sendspin::ClientView& client : (*host)->clients()) {
        group->add(client.client_id);
    }
    const m::AudioFormat source{.codec = m::Codec::kPcm, .channels = 2, .sample_rate = 48000, .bit_depth = 16};
    REQUIRE(group->start({.pcm = source, .bursts = std::nullopt, .buffered = true}));

    // Two seconds of a tone, pushed in blocks as fast as the group takes them.
    std::vector<std::int32_t> programme;
    for (int frame = 0; frame < 96000; ++frame) {
        programme.push_back(static_cast<std::int32_t>(std::lround(9000.0 * std::sin(frame * 0.0575))));
        programme.push_back(static_cast<std::int32_t>(std::lround(9000.0 * std::sin(frame * 0.131))));
    }
    std::size_t offset = 0;
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (offset < programme.size() && std::chrono::steady_clock::now() < deadline) {
        const std::size_t block = std::min<std::size_t>(4800 * 2, programme.size() - offset);
        const std::size_t taken = group->push(std::span<const std::int32_t>(programme).subspan(offset, block));
        if (taken == 0) {
            std::this_thread::sleep_for(10ms);
        }
        offset += taken * 2;
    }
    REQUIRE(offset == programme.size());
    CHECK(group->members_playing() == 2);
    group->stop();

    // Both sinks have played it all.
    const auto played = [](const testsink::Sink& sink) { return sink.totals().frames; };
    const auto until = std::chrono::steady_clock::now() + 10s;
    while ((played(*kitchen) < 96000 || played(*lounge) < 96000) && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(20ms);
    }
    REQUIRE(played(*kitchen) == 96000);
    REQUIRE(played(*lounge) == 96000);
    // A group must not outlive its host.
    group.reset();
    host->reset();

    // Each WAV is the programme, sample for sample.
    for (const fs::path& directory : {scratch / "kitchen", scratch / "lounge"}) {
        const auto wav = ac3::io::read_wav((directory / "out" / "stream-1-1.wav").string());
        REQUIRE(wav.has_value());
        REQUIRE(wav->frame_count() == 96000);
        std::size_t different = 0;
        for (std::size_t frame = 0; frame < 96000; ++frame) {
            for (std::size_t channel = 0; channel < 2; ++channel) {
                const float wanted = static_cast<float>(programme[(frame * 2) + channel]) / 32768.0F;
                different += wav->channels[channel][frame] == wanted ? 0U : 1U;
            }
        }
        CHECK(different == 0);
    }

    // Every chunk on both sinks puts the first frame at the same local time, within 1 ms.
    std::vector<double> times = first_frame_times(scratch / "kitchen" / "out" / "stream-1-1.times.csv");
    const std::vector<double> lounge_times = first_frame_times(scratch / "lounge" / "out" / "stream-1-1.times.csv");
    REQUIRE_FALSE(times.empty());
    REQUIRE_FALSE(lounge_times.empty());
    times.insert(times.end(), lounge_times.begin(), lounge_times.end());
    const auto [earliest, latest] = std::minmax_element(times.begin(), times.end());
    CHECK(*latest - *earliest < 1000.0);
}

TEST_CASE("group: a host pairs one test sink by its token and another by a dynamic code", "[hearth][group][websocket]") {
    const fs::path scratch = fs::path{AC3FORGE_TEST_SCRATCH_DIR} / ("hearth_pairing_" + scratch_pid_suffix());
    fs::remove_all(scratch);
    QuietLog token_log;
    QuietLog code_log;
    const std::unique_ptr<testsink::Sink> by_token = start_sink(scratch / "token", "By token", m::Codec::kPcm, token_log, false);
    const std::unique_ptr<testsink::Sink> by_code = start_sink(scratch / "code", "By code", m::Codec::kPcm, code_log, false);

    std::optional<ac3::sendspin::noise::KeyPair> identity = ac3::sendspin::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    ac3::sendspin::MemoryServerStore store;
    HostEvents events;
    auto host = ac3::sendspin::ServerHost::start(
        {.identity = *identity, .name = "Test host", .languages = {"en"}, .address = "127.0.0.1", .port = std::nullopt,
         .advertise = false, .browse = false, .mdns_interfaces = {}},
        store, events);
    REQUIRE(host.has_value());

    // The operator enters the first sink's token before the host has even met it.
    REQUIRE((*host)->enter_pairing_token(by_token->pairing_token()));
    CHECK_FALSE((*host)->enter_pairing_token("SP:0NOTATOKEN"));
    (*host)->dial("ws://127.0.0.1:" + std::to_string(by_token->port()) + "/sendspin");
    (*host)->dial("ws://127.0.0.1:" + std::to_string(by_code->port()) + "/sendspin");

    const auto playing = [](const std::string& id) {
        return [id](const auto& clients) {
            const auto found = clients.find(id);
            return found != clients.end() && found->second.playing &&
                   found->second.psk == ac3::sendspin::handshake::PskCategory::kLongTerm;
        };
    };
    REQUIRE(events.wait(playing(by_token->client_id()), 20s));

    // The second waits unpaired until the operator pairs it by the code it shows.
    REQUIRE(events.wait([&](const auto& clients) { return clients.contains(by_code->client_id()); }, 15s));
    const std::optional<ac3::sendspin::ClientView> waiting = (*host)->client(by_code->client_id());
    REQUIRE(waiting.has_value());
    CHECK_FALSE(waiting->playing);
    REQUIRE((*host)->pair(by_code->client_id(), m::PairMethod::kDynamicCode, m::CodeFormat::kDigits));
    const auto shown = std::chrono::steady_clock::now() + 15s;
    while (!code_log.code() && std::chrono::steady_clock::now() < shown) {
        std::this_thread::sleep_for(20ms);
    }
    REQUIRE(code_log.code().has_value());
    const auto wanted = std::chrono::steady_clock::now() + 10s;
    bool entered = false;
    while (!entered && std::chrono::steady_clock::now() < wanted) {
        entered = (*host)->enter_code(by_code->client_id(), *code_log.code());
        if (!entered) {
            std::this_thread::sleep_for(20ms);
        }
    }
    REQUIRE(entered);
    REQUIRE(events.wait(playing(by_code->client_id()), 20s));

    host->reset();
}

TEST_CASE("group: unpairing a sink delivers client/goodbye's own reason to the host",
          "[hearth][group][websocket]") {
    // server/unpair (ServerHost::unpair()) makes a paired test sink drop its record and answer
    // with client/goodbye kUnpaired (player_session.cpp's own "server/unpair" handler) - a real,
    // encrypted round trip that proves ServerListener::on_goodbye() now reaches
    // ServerHostEvents::on_client_goodbye() (HostConnection::on_goodbye(), server_host.cpp) rather
    // than being discarded, as issue #876 found it. The reasons NetworkSinks actually reads
    // meaning into (kAnotherServer, kConcurrentAttempt) are player_session.cpp's own admission
    // outcomes, covered end to end already by tests/sendspin/test_sessions.cpp's "the owner
    // rejects an activation, or another server displaces the connection" - this test is only
    // for the plumbing between here and there, which kUnpaired reaches just as directly.
    const fs::path scratch = fs::path{AC3FORGE_TEST_SCRATCH_DIR} / ("hearth_goodbye_" + scratch_pid_suffix());
    fs::remove_all(scratch);
    QuietLog log;
    const std::unique_ptr<testsink::Sink> sink = start_sink(scratch, "Study", m::Codec::kPcm, log, false);

    std::optional<ac3::sendspin::noise::KeyPair> identity = ac3::sendspin::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    ac3::sendspin::MemoryServerStore store;
    HostEvents events;
    auto host = ac3::sendspin::ServerHost::start(
        {.identity = *identity, .name = "Test host", .languages = {"en"}, .address = "127.0.0.1", .port = std::nullopt,
         .advertise = false, .browse = false, .mdns_interfaces = {}},
        store, events);
    REQUIRE(host.has_value());
    REQUIRE((*host)->enter_pairing_token(sink->pairing_token()));
    (*host)->dial("ws://127.0.0.1:" + std::to_string(sink->port()) + "/sendspin");

    REQUIRE(events.wait(
        [&](const auto& clients) {
            const auto found = clients.find(sink->client_id());
            return found != clients.end() && found->second.playing &&
                   found->second.psk == ac3::sendspin::handshake::PskCategory::kLongTerm;
        },
        20s));

    REQUIRE((*host)->unpair(sink->client_id()));
    REQUIRE(events.wait_for_goodbye(sink->client_id(), 15s));
    CHECK(events.goodbye(sink->client_id()) == m::GoodbyeReason::kUnpaired);

    host->reset();
}

TEST_CASE("group: test sinks' other roles get the group's metadata, colours, transport, artwork and visualizer",
          "[hearth][group][websocket][roles]") {
    namespace ss = ac3::sendspin;
    namespace controller = ac3::sendspin::controller;
    const fs::path scratch = fs::path{AC3FORGE_TEST_SCRATCH_DIR} / ("hearth_roles_" + scratch_pid_suffix());
    fs::remove_all(scratch);
    QuietLog log;
    const auto make_sink = [&](const fs::path& directory, std::string sink_name, std::vector<std::string> listed) {
        testsink::SinkOptions options;
        options.name = std::move(sink_name);
        options.address = "127.0.0.1";
        options.port = 0;
        options.state_directory = directory / "state";
        options.advertise = false;
        options.unpaired_access = true;
        options.codecs = {m::Codec::kPcm};
        options.other_roles = std::move(listed);
        options.artwork_channels.channels = {
            {.source = ss::artwork::Source::kAlbum, .format = ss::artwork::Format::kJpeg, .width = 300, .height = 300}};
        options.visualizer_request = {.types = {ss::visualizer::Type::kLoudness, ss::visualizer::Type::kBeat},
                                      .rate_max = 30,
                                      .spectrum = std::nullopt};
        auto started = testsink::Sink::start(options, log);
        REQUIRE(started.has_value());
        return std::move(*started);
    };
    // The kitchen pairs and plays the extension role with every other role but source; the lounge,
    // approved unpaired, plays player@v1 with a controller.
    const std::unique_ptr<testsink::Sink> kitchen =
        make_sink(scratch / "kitchen", "Kitchen", {"controller@v1", "metadata@v1", "color@v1", "artwork@v1", "visualizer@v1"});
    const std::unique_ptr<testsink::Sink> lounge = make_sink(scratch / "lounge", "Lounge", {"controller@v1"});
    const std::string kitchen_id = kitchen->client_id();
    const std::string lounge_id = lounge->client_id();

    std::optional<ss::noise::KeyPair> identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    ss::MemoryServerStore store;
    HostEvents events;
    auto host = ss::ServerHost::start(
        {.identity = *identity, .name = "Test host", .languages = {"en"}, .address = "127.0.0.1", .port = std::nullopt,
         .advertise = false, .browse = false, .mdns_interfaces = {}},
        store, events);
    REQUIRE(host.has_value());
    REQUIRE((*host)->enter_pairing_token(kitchen->pairing_token()));
    (*host)->dial("ws://127.0.0.1:" + std::to_string(kitchen->port()) + "/sendspin");
    (*host)->dial("ws://127.0.0.1:" + std::to_string(lounge->port()) + "/sendspin");
    REQUIRE(events.wait([&](const auto& clients) { return clients.contains(lounge_id); }, 15s));
    REQUIRE((*host)->approve(lounge_id, true));

    const auto active = [](const ss::ClientView& client, std::string_view role) {
        return std::find(client.active_roles.begin(), client.active_roles.end(), role) != client.active_roles.end();
    };
    REQUIRE(events.wait(
        [&](const auto& clients) {
            const auto k = clients.find(kitchen_id);
            const auto l = clients.find(lounge_id);
            return k != clients.end() && l != clients.end() && k->second.bursts && k->second.available &&
                   k->second.artwork_state && k->second.visualizer_state && active(k->second, controller::kRole) &&
                   active(k->second, ss::metadata::kRole) && active(k->second, ss::color::kRole) &&
                   active(k->second, ss::artwork::kRole) && active(k->second, ss::visualizer::kRole) &&
                   l->second.playing && !l->second.bursts && l->second.available && active(l->second, controller::kRole) &&
                   !active(l->second, ss::metadata::kRole);
        },
        30s));

    std::shared_ptr<ss::Group> group = (*host)->make_group("Downstairs");
    group->add(kitchen_id);
    group->add(lounge_id);

    // Metadata reaches the member with the role, and no other.
    ss::metadata::State metadata;
    metadata.title = "Spring";
    metadata.artist = "Hearth";
    metadata.album = "Seasons";
    metadata.year = 2026;
    metadata.track = 3;
    metadata.progress = ss::metadata::Progress{.track_progress_ms = 1000, .track_duration_ms = 180000, .playback_speed = 1000};
    group->set_metadata(metadata);
    REQUIRE(eventually([&] { return kitchen->roles().metadata == metadata; }, 10s));
    CHECK_FALSE(lounge->roles().metadata.has_value());

    // Colours go out at the contrast the role requires.
    const ss::color::State colours{.timestamp = 0,
                                   .background_dark = ss::color::Rgb{.r = 90, .g = 90, .b = 100},
                                   .background_light = std::nullopt,
                                   .primary = ss::color::Rgb{.r = 200, .g = 40, .b = 40},
                                   .accent = std::nullopt,
                                   .on_dark = ss::color::Rgb{.r = 120, .g = 120, .b = 120},
                                   .on_light = std::nullopt};
    REQUIRE_FALSE(ss::color::meets_contrast(colours));
    group->set_colors(colours);
    REQUIRE(eventually([&] { return kitchen->roles().colors == ss::color::with_contrast(colours); }, 10s));
    CHECK(ss::color::meets_contrast(*kitchen->roles().colors));

    // The transport, with the group's volume and mute from both players.
    group->set_transport(ss::Group::Transport{
        .commands = {controller::Command::kPlay, controller::Command::kPause, controller::Command::kNext,
                     controller::Command::kPrevious},
        .repeat = controller::Repeat::kOff,
        .shuffle = false,
        .seek_max_ms = std::nullopt});
    const auto shows = [](const testsink::Sink& sink, std::int32_t volume, bool muted) {
        const std::optional<controller::State> state = sink.roles().controller;
        const auto lists = [&](controller::Command command) {
            return std::find(state->supported_commands.begin(), state->supported_commands.end(), command) !=
                   state->supported_commands.end();
        };
        return state && state->volume == volume && state->muted == muted && lists(controller::Command::kVolume) &&
               lists(controller::Command::kMute) && lists(controller::Command::kNext) &&
               !lists(controller::Command::kShuffle);
    };
    REQUIRE(eventually([&] { return shows(*kitchen, 100, false) && shows(*lounge, 100, false); }, 10s));

    // A controller's volume and mute reach both players, over each one's playback role.
    lounge->send_controller_command(
        {.command = controller::Command::kVolume, .volume = 40, .mute = false, .position_ms = 0, .offset_ms = 0});
    REQUIRE(eventually([&] { return shows(*kitchen, 40, false) && shows(*lounge, 40, false); }, 10s));
    kitchen->send_controller_command(
        {.command = controller::Command::kMute, .volume = 0, .mute = true, .position_ms = 0, .offset_ms = 0});
    REQUIRE(eventually([&] { return shows(*kitchen, 40, true) && shows(*lounge, 40, true); }, 10s));
    const std::optional<ss::ClientView> kitchen_view = (*host)->client(kitchen_id);
    const std::optional<ss::ClientView> lounge_view = (*host)->client(lounge_id);
    REQUIRE(kitchen_view.has_value());
    REQUIRE(lounge_view.has_value());
    REQUIRE(kitchen_view->ac3forge_state.has_value());
    REQUIRE(lounge_view->player_state.has_value());
    CHECK(kitchen_view->ac3forge_state->volume == 40);
    CHECK(kitchen_view->ac3forge_state->muted == true);
    CHECK(lounge_view->player_state->volume == 40);
    CHECK(lounge_view->player_state->muted == true);

    // The engine's commands go to the host's events; one the state does not list goes nowhere.
    lounge->send_controller_command(
        {.command = controller::Command::kShuffle, .volume = 0, .mute = false, .position_ms = 0, .offset_ms = 0});
    lounge->send_controller_command(
        {.command = controller::Command::kNext, .volume = 0, .mute = false, .position_ms = 0, .offset_ms = 0});
    REQUIRE(eventually([&] { return !events.commands().empty(); }, 10s));
    const std::vector<HostEvents::Command> commands = events.commands();
    REQUIRE(commands.size() == 1);
    CHECK(commands.front().group_id == group->id());
    CHECK(commands.front().client_id == lounge_id);
    CHECK(commands.front().command.command == controller::Command::kNext);

    // Artwork at the channel's source, format and size, in more than one part; then cleared.
    std::vector<std::uint8_t> image(100000);
    for (std::size_t i = 0; i < image.size(); ++i) {
        image[i] = static_cast<std::uint8_t>((i * 7U) & 0xFFU);
    }
    group->set_artwork(0, [image](ss::artwork::Source source, ss::artwork::Format format, std::int32_t width,
                                  std::int32_t height) -> std::optional<std::vector<std::uint8_t>> {
        if (source != ss::artwork::Source::kAlbum || format != ss::artwork::Format::kJpeg || width != 300 || height != 300) {
            return std::nullopt;
        }
        return image;
    });
    const auto image_on = [&](std::size_t bytes) {
        const testsink::Sink::Roles roles = kitchen->roles();
        const auto found = roles.images.find(0);
        return found != roles.images.end() && found->second.size() == bytes && (bytes == 0 || found->second == image);
    };
    REQUIRE(eventually([&] { return image_on(image.size()); }, 10s));
    group->set_artwork(0, {});
    REQUIRE(eventually([&] { return image_on(0); }, 10s));

    // The visualizer streams the types both the sink asked for and the engine analyses, at the lower
    // rate, and the frames of those types.
    group->set_visualizer({ss::visualizer::Type::kLoudness, ss::visualizer::Type::kSpectrum}, 20, false);
    REQUIRE(eventually([&] { return kitchen->roles().visualizer.has_value(); }, 10s));
    CHECK(kitchen->roles().visualizer->types == std::vector<ss::visualizer::Type>{ss::visualizer::Type::kLoudness});
    CHECK(kitchen->roles().visualizer->rate_max == 20);
    for (std::int64_t i = 0; i < 10; ++i) {
        group->push_visualizer({.type = ss::visualizer::Type::kLoudness,
                                .timestamp = i * 50'000,
                                .value = static_cast<std::uint16_t>(i * 1000),
                                .frequency = 0,
                                .downbeat = false,
                                .strength = 0,
                                .bins = {}});
    }
    REQUIRE(eventually([&] { return kitchen->roles().visualizer_frames == 10; }, 10s));

    // Leaving the group clears what it showed; so does the group going.
    const std::uint32_t states = kitchen->roles().states;
    group->remove(kitchen_id);
    REQUIRE(eventually(
        [&] {
            const testsink::Sink::Roles roles = kitchen->roles();
            return roles.states > states && !roles.metadata && !roles.colors && !roles.controller && !roles.visualizer;
        },
        10s));
    CHECK(lounge->roles().controller.has_value());
    group.reset();
    CHECK(eventually([&] { return !lounge->roles().controller.has_value(); }, 10s));
    host->reset();
}

TEST_CASE("group: the host sets a member's volume and mute directly, and the group's own, without a controller",
          "[hearth][group][websocket]") {
    namespace ss = ac3::sendspin;
    const fs::path scratch = fs::path{AC3FORGE_TEST_SCRATCH_DIR} / ("hearth_group_host_volume_" + scratch_pid_suffix());
    fs::remove_all(scratch);
    QuietLog log;
    // The extension role (kitchen, paired) and player@v1 (lounge, approved
    // unpaired) - the same two shapes player_of() branches on, neither with
    // any other role that could complicate what "volume" reads.
    const std::unique_ptr<testsink::Sink> kitchen = start_sink(scratch / "kitchen", "Kitchen", m::Codec::kPcm, log, false);
    const std::unique_ptr<testsink::Sink> lounge = start_sink(scratch / "lounge", "Lounge", m::Codec::kFlac, log);

    std::optional<ss::noise::KeyPair> identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    ss::MemoryServerStore store;
    HostEvents events;
    auto host = ss::ServerHost::start(
        {.identity = *identity, .name = "Test host", .languages = {"en"}, .address = "127.0.0.1", .port = std::nullopt,
         .advertise = false, .browse = false, .mdns_interfaces = {}},
        store, events);
    REQUIRE(host.has_value());
    REQUIRE((*host)->enter_pairing_token(kitchen->pairing_token()));
    (*host)->dial("ws://127.0.0.1:" + std::to_string(kitchen->port()) + "/sendspin");
    (*host)->dial("ws://127.0.0.1:" + std::to_string(lounge->port()) + "/sendspin");
    REQUIRE(events.wait([&](const auto& clients) { return clients.contains(lounge->client_id()); }, 15s));
    REQUIRE((*host)->approve(lounge->client_id(), true));
    REQUIRE(events.wait(
        [](const auto& clients) {
            return clients.size() == 2 && std::all_of(clients.begin(), clients.end(), [](const auto& entry) {
                       return entry.second.playing && entry.second.available;
                   });
        },
        30s));

    const std::string kitchen_id = kitchen->client_id();
    const std::string lounge_id = lounge->client_id();
    std::shared_ptr<ss::Group> group = (*host)->make_group("Downstairs");
    group->add(kitchen_id);
    group->add(lounge_id);

    // Nothing to read before a client is a member, or for one that never was.
    CHECK_FALSE(group->member_player("not-a-member").has_value());

    const auto member_shows = [&](const std::string& client_id, std::int32_t volume, bool muted) {
        const std::optional<ss::controller::Player> player = group->member_player(client_id);
        return player.has_value() && player->volume == volume && player->muted == muted;
    };
    REQUIRE(eventually([&] { return member_shows(kitchen_id, 100, false) && member_shows(lounge_id, 100, false); }, 10s));

    // The group's own volume redistributes across every member that
    // supports it - a DELTA applied to each player's own current volume
    // (100 -> 80 for both, since both started equal), not "set everyone to
    // exactly this value".
    group->set_group_volume(80);
    REQUIRE(eventually([&] { return member_shows(kitchen_id, 80, false) && member_shows(lounge_id, 80, false); }, 10s));

    // Setting one member's volume directly leaves the other alone - no
    // redistribution, unlike the group-wide command above.
    group->set_member_volume(kitchen_id, 30);
    REQUIRE(eventually([&] { return member_shows(kitchen_id, 30, false); }, 10s));
    CHECK(member_shows(lounge_id, 80, false));
    // Reaches the sink itself, not just this read-back.
    const std::optional<ss::ClientView> kitchen_after_member_set = (*host)->client(kitchen_id);
    REQUIRE(kitchen_after_member_set.has_value());
    REQUIRE(kitchen_after_member_set->ac3forge_state.has_value());
    CHECK(kitchen_after_member_set->ac3forge_state->volume == 30);

    // A member's mute is direct too.
    group->set_member_muted(lounge_id, true);
    REQUIRE(eventually([&] { return member_shows(lounge_id, 80, true); }, 10s));
    CHECK(member_shows(kitchen_id, 30, false));

    // The group's mute reaches every member unconditionally - unlike
    // volume, mute is not relative, so both are muted regardless of their
    // now-different volumes.
    group->set_group_muted(true);
    REQUIRE(eventually([&] { return member_shows(kitchen_id, 30, true) && member_shows(lounge_id, 80, true); }, 10s));

    group->remove(kitchen_id);
    CHECK_FALSE(group->member_player(kitchen_id).has_value());

    group.reset();
    host->reset();
}

TEST_CASE("group: a mixed group delivers PCM and bursts to their own members at once",
          "[hearth][group][websocket]") {
    // Every other group test in this file gives a group either all PCM/FLAC
    // members or all extension-role members, never both - this is the one
    // that proves server_host.hpp's own Programme comment for real ("a
    // member playing player@v1 gets the programme's PCM... and a member
    // playing _ac3forge_player@v1 gets the coded stream's bursts, all on
    // one timeline"), ahead of Player growing a network-group output seam
    // that will need to feed both at once (issue #874's own follow-up).
    namespace ss = ac3::sendspin;
    const fs::path scratch = fs::path{AC3FORGE_TEST_SCRATCH_DIR} / ("hearth_group_mixed_" + scratch_pid_suffix());
    fs::remove_all(scratch);
    QuietLog log;
    const std::unique_ptr<testsink::Sink> kitchen = start_sink(scratch / "kitchen", "Kitchen", m::Codec::kPcm, log, false);
    const std::unique_ptr<testsink::Sink> lounge = start_sink(scratch / "lounge", "Lounge", m::Codec::kPcm, log);

    std::optional<ss::noise::KeyPair> identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    ss::MemoryServerStore store;
    HostEvents events;
    auto host = ss::ServerHost::start(
        {.identity = *identity, .name = "Test host", .languages = {"en"}, .address = "127.0.0.1", .port = std::nullopt,
         .advertise = false, .browse = false, .mdns_interfaces = {}},
        store, events);
    REQUIRE(host.has_value());
    REQUIRE((*host)->enter_pairing_token(kitchen->pairing_token()));
    (*host)->dial("ws://127.0.0.1:" + std::to_string(kitchen->port()) + "/sendspin");
    (*host)->dial("ws://127.0.0.1:" + std::to_string(lounge->port()) + "/sendspin");
    REQUIRE(events.wait([&](const auto& clients) { return clients.contains(lounge->client_id()); }, 15s));
    REQUIRE((*host)->approve(lounge->client_id(), true));
    REQUIRE(events.wait(
        [](const auto& clients) {
            return clients.size() == 2 && std::all_of(clients.begin(), clients.end(), [](const auto& entry) {
                       return entry.second.playing && entry.second.available;
                   });
        },
        30s));

    // The burst feed: a real E-AC-3 stream, packed exactly as the JOC test's
    // own helper does.
    const std::vector<std::byte> fixture = read_bytes(AC3FORGE_GOLDEN_OBJECT_DIR "/dee_joc_514.ec3");
    const std::expected<ac3::io::ScannedStream, ac3::io::ScanError> stream = ac3::io::scan(fixture);
    REQUIRE(stream.has_value());
    const std::vector<PackedBurst> bursts = pack_bursts(*stream, 1);
    REQUIRE_FALSE(bursts.empty());

    // The PCM feed: two seconds of a tone, exactly the shape the PCM/FLAC
    // test above uses - deliberately unrelated content to the bursts: this
    // test is about the plumbing carrying both at once, not about the two
    // members hearing "the same" programme (Player, once it has a
    // network-group seam, is what will make that true).
    std::vector<std::int32_t> programme;
    for (int frame = 0; frame < 96000; ++frame) {
        programme.push_back(static_cast<std::int32_t>(std::lround(9000.0 * std::sin(frame * 0.0575))));
        programme.push_back(static_cast<std::int32_t>(std::lround(9000.0 * std::sin(frame * 0.131))));
    }

    std::shared_ptr<ss::Group> group = (*host)->make_group("Mixed");
    group->add(kitchen->client_id());
    group->add(lounge->client_id());
    const m::AudioFormat pcm_format{.codec = m::Codec::kPcm, .channels = 2, .sample_rate = 48000, .bit_depth = 16};
    REQUIRE(group->start({.pcm = pcm_format,
                          .bursts = ss::ac3forge::StreamStart{.data_type = ss::ac3forge::DataType::kEac3, .sample_rate = 48000},
                          .buffered = true}));

    std::size_t next_burst = 0;
    std::size_t pcm_offset = 0;
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while ((next_burst < bursts.size() || pcm_offset < programme.size()) && std::chrono::steady_clock::now() < deadline) {
        bool made_progress = false;
        if (next_burst < bursts.size()) {
            const PackedBurst& burst = bursts[next_burst];
            if (group->push_burst({.pc = burst.pc, .pd = burst.pd, .payload = burst.payload, .frame = burst.frame})) {
                ++next_burst;
                made_progress = true;
            }
        }
        if (pcm_offset < programme.size()) {
            const std::size_t block = std::min<std::size_t>(4800 * 2, programme.size() - pcm_offset);
            const std::size_t taken = group->push(std::span<const std::int32_t>(programme).subspan(pcm_offset, block));
            if (taken > 0) {
                pcm_offset += taken * 2;
                made_progress = true;
            }
        }
        if (!made_progress) {
            std::this_thread::sleep_for(5ms);
        }
    }
    REQUIRE(next_burst == bursts.size());
    REQUIRE(pcm_offset == programme.size());
    CHECK(group->members_playing() == 2);
    group->stop();

    // Both sinks have everything - kitchen its bursts, lounge its frames -
    // neither starved by the other sharing the same group.
    const auto until = std::chrono::steady_clock::now() + 15s;
    while ((kitchen->totals().bursts < bursts.size() || lounge->totals().frames < 96000) &&
           std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(20ms);
    }
    REQUIRE(kitchen->totals().bursts == bursts.size());
    REQUIRE(lounge->totals().frames == 96000);
    group.reset();
    host->reset();

    // Every chunk on both sinks puts its first frame at the same local
    // time, within 1 ms - the group's one shared timeline, proven across
    // both representations at once rather than only within one.
    std::vector<double> kitchen_times = first_frame_times(only_file(scratch / "kitchen" / "out", "bursts-", ".times.csv"));
    const std::vector<double> lounge_times = first_frame_times(scratch / "lounge" / "out" / "stream-1-1.times.csv");
    REQUIRE_FALSE(kitchen_times.empty());
    REQUIRE_FALSE(lounge_times.empty());
    kitchen_times.insert(kitchen_times.end(), lounge_times.begin(), lounge_times.end());
    const auto [earliest, latest] = std::minmax_element(kitchen_times.begin(), kitchen_times.end());
    CHECK(*latest - *earliest < 1000.0);
}

TEST_CASE("group: two paired test sinks play E-AC-3 JOC in step over the extension role",
          "[hearth][group][websocket][ac3forge]") {
    play_joc_programme(fs::path{AC3FORGE_TEST_SCRATCH_DIR} / ("hearth_group_joc_" + scratch_pid_suffix()), "7.1.4", 2);
}

// A4's exit at its full length: ten minutes of the programme, rendered to four speakers to keep the
// WAV files near half a gigabyte each. Run by name.
TEST_CASE("group: ten minutes of E-AC-3 JOC in step on two test sinks", "[.][hearth-soak]") {
    play_joc_programme(fs::path{AC3FORGE_TEST_SCRATCH_DIR} / ("hearth_group_soak_" + scratch_pid_suffix()), "2.0.2", 298);
}

// D11 (planning/ac4.md): the Dolby Encoding Engine's 2.0 AC-4 stream at 48 kHz and frame_rate_index
// 13, the rate the decoder on main decodes, sent to a paired test sink over _ac3forge_player@v1:
// each frame in its own AC-4 data-burst, with the Pc and Pd ac3::iec61937::Ac4BurstPacker writes
// and the frame's 2 048 samples on the group's timeline. The sink's WAV must be the local decode of
// the same frames rendered to its layout as its BurstOutput renders them, sample for sample; what
// its decoder found must reach the host; and every burst's logged play time must put the first
// frame at the same local time, within 1 ms.
TEST_CASE("group: a paired test sink decodes AC-4 sent over the extension role",
          "[hearth][group][websocket][ac3forge][ac4]") {
    const fs::path scratch =
        fs::path{AC3FORGE_TEST_SCRATCH_DIR} / ("hearth_group_ac4_" + scratch_pid_suffix());
    fs::remove_all(scratch);
    const std::string layout_text = "5.1";
    const std::optional<ac3::render::OutputLayout> layout =
        ac3::render::OutputLayout::parse(layout_text);
    REQUIRE(layout.has_value());

    const std::vector<std::byte> file =
        read_bytes(AC3FORGE_GOLDEN_EXTERNAL_BASELINE_DIR "/ac4-stereo-64/dee.ac4");
    const ac4::ScanResult scanned = ac4::scan(file);
    REQUIRE_FALSE(scanned.frames.empty());
    REQUIRE_FALSE(scanned.stopped_at.has_value());
    constexpr std::int64_t kFrameSamples = 2048;
    std::vector<PackedBurst> bursts;
    ac3::iec61937::Ac4BurstPacker packer;
    for (std::size_t i = 0; i < scanned.frames.size(); ++i) {
        const std::size_t begin = scanned.frames[i].offset;
        const std::size_t end =
            i + 1 < scanned.frames.size() ? scanned.frames[i + 1].offset : file.size();
        const std::span<const std::byte> sync_frame =
            std::span<const std::byte>(file).subspan(begin, end - begin);
        REQUIRE(packer.push(sync_frame).has_value());
        const ac3::iec61937::Ac4BurstPacker::Packed& packed = *packer.last();
        // IEC 61937-14 Tables 5 and 7: 2 048 IEC 60958 frames, code 13, at 48 kHz.
        REQUIRE(packed.period == 2048);
        REQUIRE(((packed.pc >> 8U) & 0x0FU) == 13U);
        PackedBurst burst{.pc = packed.pc,
                          .pd = packed.pd,
                          .payload = {},
                          .frame = static_cast<std::int64_t>(i) * kFrameSamples};
        std::transform(sync_frame.begin(), sync_frame.end(), std::back_inserter(burst.payload),
                       [](std::byte b) { return std::to_integer<std::uint8_t>(b); });
        REQUIRE(burst.payload.size() == packed.payload_bytes);
        bursts.push_back(std::move(burst));
    }

    QuietLog log;
    testsink::SinkOptions options;
    options.name = "Study";
    options.address = "127.0.0.1";
    options.port = 0;
    options.state_directory = scratch / "state";
    options.output_directory = scratch / "out";
    options.advertise = false;
    options.codecs = {m::Codec::kPcm};
    options.layout = layout_text;
    auto started = testsink::Sink::start(options, log);
    REQUIRE(started.has_value());
    const std::unique_ptr<testsink::Sink> sink = std::move(*started);

    std::optional<ac3::sendspin::noise::KeyPair> identity =
        ac3::sendspin::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    ac3::sendspin::MemoryServerStore store;
    HostEvents events;
    auto host = ac3::sendspin::ServerHost::start({.identity = *identity,
                                                  .name = "Test host",
                                                  .languages = {"en"},
                                                  .address = "127.0.0.1",
                                                  .port = std::nullopt,
                                                  .advertise = false,
                                                  .browse = false,
                                                  .mdns_interfaces = {}},
                                                 store, events);
    REQUIRE(host.has_value());
    REQUIRE((*host)->enter_pairing_token(sink->pairing_token()));
    (*host)->dial("ws://127.0.0.1:" + std::to_string(sink->port()) + "/sendspin");
    REQUIRE(events.wait(
        [](const auto& clients) {
            return clients.size() == 1 &&
                   std::all_of(clients.begin(), clients.end(), [](const auto& entry) {
                       return entry.second.playing && entry.second.bursts &&
                              entry.second.available &&
                              entry.second.psk == ac3::sendspin::handshake::PskCategory::kLongTerm;
                   });
        },
        30s));

    std::shared_ptr<ac3::sendspin::Group> group = (*host)->make_group("Study");
    group->add(sink->client_id());
    REQUIRE(group->start(
        {.pcm = std::nullopt,
         .bursts =
             ac3::sendspin::ac3forge::StreamStart{
                 .data_type = ac3::sendspin::ac3forge::DataType::kAc4, .sample_rate = 48000},
         .buffered = true}));
    std::size_t next = 0;
    const auto deadline =
        std::chrono::steady_clock::now() + 30s +
        std::chrono::milliseconds(static_cast<std::int64_t>(bursts.size()) * 43 * 3 / 2);
    while (next < bursts.size() && std::chrono::steady_clock::now() < deadline) {
        const PackedBurst& burst = bursts[next];
        if (group->push_burst({.pc = burst.pc,
                               .pd = burst.pd,
                               .payload = burst.payload,
                               .frame = burst.frame,
                               .frames = kFrameSamples})) {
            ++next;
        } else {
            std::this_thread::sleep_for(5ms);
        }
    }
    REQUIRE(next == bursts.size());
    // What the sink's decoder found reaches the host in its state.
    const bool reported = events.wait(
        [](const auto& clients) {
            return clients.size() == 1 &&
                   std::all_of(clients.begin(), clients.end(), [](const auto& entry) {
                       const std::optional<ac3::sendspin::ac3forge::State>& state =
                           entry.second.ac3forge_state;
                       return state && state->decoder &&
                              state->decoder->data_type ==
                                  ac3::sendspin::ac3forge::DataType::kAc4 &&
                              state->decoder->acmod == 2 && !state->decoder->lfe;
                   });
        },
        10s);
    CHECK(reported);
    group->stop();
    const auto until = std::chrono::steady_clock::now() + 30s;
    while (sink->totals().bursts < bursts.size() && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(20ms);
    }
    REQUIRE(sink->totals().bursts == bursts.size());
    // A group must not outlive its host.
    group.reset();
    host->reset();

    // The WAV is the local decode, rendered as BurstOutput renders AC-4, sample for sample.
    ac3::io::WavStreamReader wav;
    REQUIRE(wav.open(only_file(scratch / "out", "bursts-", ".wav").string()).has_value());
    REQUIRE(static_cast<std::size_t>(wav.channels()) == layout->slots());
    std::vector<std::vector<float>> played(layout->slots(),
                                           std::vector<float>(ac3::kSamplesPerBlock));
    std::vector<std::span<float>> played_spans(played.begin(), played.end());
    std::vector<std::array<float, ac3::kSamplesPerBlock>> rendered(layout->slots());
    std::vector<std::span<float>> rendered_spans;
    for (std::array<float, ac3::kSamplesPerBlock>& slot : rendered) {
        rendered_spans.emplace_back(slot);
    }
    ac4::Decoder decoder;
    ac3::render::LayoutRenderer renderer(*layout);
    std::optional<ac3::eac3::chanmap::Layout> bed_set;
    std::uint64_t frames = 0;
    std::uint64_t different = 0;
    for (const ac4::SyncFrame& frame : scanned.frames) {
        const auto decoded = decoder.decode(frame.raw_ac4_frame);
        REQUIRE(decoded.has_value());
        if (!*decoded) {
            continue;
        }
        const ac4::DecodedFrame& pcm = **decoded;
        const ac3::eac3::chanmap::Layout bed = testsink::ac4_bed(pcm.speakers);
        if (!bed_set || bed_set->count != bed.count ||
            !std::equal(bed.begin(), bed.end(), bed_set->begin())) {
            renderer.set_bed(bed);
            bed_set = bed;
        }
        const std::size_t n = pcm.channels.front().size();
        std::vector<std::span<const float>> block_channels(pcm.channels.size());
        for (std::size_t at = 0; at < n; at += ac3::kSamplesPerBlock) {
            const std::size_t m = std::min<std::size_t>(ac3::kSamplesPerBlock, n - at);
            for (std::size_t c = 0; c < pcm.channels.size(); ++c) {
                block_channels[c] = std::span<const float>(pcm.channels[c]).subspan(at, m);
            }
            const ac3::PcmBlock block{
                .index = static_cast<int>(at / ac3::kSamplesPerBlock),
                .blocks = static_cast<int>((n + ac3::kSamplesPerBlock - 1) / ac3::kSamplesPerBlock),
                .channels = block_channels,
                .objects = {},
                .object_indices = {},
                .object_metadata = nullptr};
            renderer.render(block, false, 1.0F, rendered_spans);
            const std::expected<std::size_t, ac3::io::WavError> got =
                wav.read_planar(played_spans, m);
            REQUIRE(got.has_value());
            REQUIRE(*got == m);
            for (std::size_t slot = 0; slot < layout->slots(); ++slot) {
                for (std::size_t t = 0; t < m; ++t) {
                    different += rendered[slot][t] == played[slot][t] ? 0U : 1U;
                }
            }
            frames += m;
        }
    }
    CHECK(frames == scanned.frames.size() * static_cast<std::uint64_t>(kFrameSamples));
    CHECK(different == 0);
    CHECK(wav.frame_count() == frames);

    // Every burst puts the first frame at the same local time, within 1 ms.
    const std::vector<double> times =
        first_frame_times(only_file(scratch / "out", "bursts-", ".times.csv"));
    CHECK(times.size() == bursts.size());
    REQUIRE_FALSE(times.empty());
    const auto [earliest, latest] = std::minmax_element(times.begin(), times.end());
    CHECK(*latest - *earliest < 1000.0);
}