// The board as a Sendspin player. See ../../sendspin.hpp.

#include "sendspin.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ac3/render/layout.hpp"
#include "ac3/render/render.hpp"
#include "ac3/render/trim_delay.hpp"
#include "ac3/sendspin/ac3forge_player.hpp"
#include "ac3/sendspin/base64url.hpp"
#include "ac3/sendspin/messages.hpp"
#include "ac3/sendspin/noise.hpp"
#include "ac3/sendspin/pairing.hpp"
#include "ac3/sendspin/player_session.hpp"
#include "ac3/sendspin/websocket.hpp"
#include "ac3forge/burst_player.hpp"
#include "ac3forge/log.hpp"
#include "ac3forge/playout.hpp"
#include "ac3forge/sendspin_host.hpp"

#include "audio_sink.hpp"
#include "network.hpp"
#include "settings.hpp"

namespace player {
namespace {

namespace ss = ac3::sendspin;
namespace ac = ss::ac3forge;
namespace m = ss::messages;

constexpr std::uint32_t kSampleRate = 48000;
// The one port, which a build without the player cannot name through
// src/sendspin.
static_assert(kSendspinPort == ss::transport::websocket::kClientPort, "a player listens on Sendspin's client port");
// From Kconfig (main/Kconfig.projbuild), as plain constants.
constexpr std::size_t kRingBytes = CONFIG_AC3FORGE_EXAMPLE_SENDSPIN_RING_BYTES;
constexpr std::size_t kMaxChunkBytes = CONFIG_AC3FORGE_EXAMPLE_SENDSPIN_MAX_CHUNK_BYTES;
constexpr std::uint32_t kDecodeStackBytes = CONFIG_AC3FORGE_EXAMPLE_SENDSPIN_DECODE_STACK_BYTES;
constexpr std::size_t kServerStackBytes = CONFIG_AC3FORGE_EXAMPLE_SENDSPIN_SERVER_STACK_BYTES;
constexpr int kMaxDelayMs = CONFIG_AC3FORGE_EXAMPLE_SENDSPIN_MAX_DELAY_MS;
constexpr std::size_t kMaxCodedChannels = CONFIG_AC3FORGE_EXAMPLE_SENDSPIN_MAX_CODED_CHANNELS;
constexpr std::uint32_t kReportEveryChunks = CONFIG_AC3FORGE_EXAMPLE_REPORT_EVERY_FRAMES;
constexpr std::int32_t kLeadMs = CONFIG_AC3FORGE_EXAMPLE_SENDSPIN_LEAD_MS;
constexpr std::int32_t kBufferMs = CONFIG_AC3FORGE_EXAMPLE_SENDSPIN_BUFFER_MS;
constexpr bool kUnpairedAccess = CONFIG_AC3FORGE_EXAMPLE_SENDSPIN_UNPAIRED_ACCESS != 0;
constexpr bool kOfferPcm = CONFIG_AC3FORGE_EXAMPLE_SENDSPIN_PCM != 0;
constexpr int kSuite = CONFIG_AC3FORGE_SENDSPIN_SUITE;
constexpr BaseType_t kDecodeCore = CONFIG_AC3FORGE_EXAMPLE_DECODE_CORE < 0 ? tskNO_AFFINITY
                                                                           : CONFIG_AC3FORGE_EXAMPLE_DECODE_CORE;
constexpr ac3::OperatingMode kMode = CONFIG_AC3FORGE_EXAMPLE_DRC_MODE == 1   ? ac3::OperatingMode::kRf
                                     : CONFIG_AC3FORGE_EXAMPLE_DRC_MODE == 2 ? ac3::OperatingMode::kCustom
                                                                             : ac3::OperatingMode::kLine;
constexpr ac3::DownmixTarget kStereoFold =
    CONFIG_AC3FORGE_EXAMPLE_STEREO_FOLD != 0 ? ac3::DownmixTarget::kLtRt : ac3::DownmixTarget::kLoRo;
constexpr ac3::render::ObjectsPolicy kObjects = CONFIG_AC3FORGE_EXAMPLE_OBJECTS == 1   ? ac3::render::ObjectsPolicy::kNever
                                                : CONFIG_AC3FORGE_EXAMPLE_OBJECTS == 2 ? ac3::render::ObjectsPolicy::kAlways
                                                                                       : ac3::render::ObjectsPolicy::kAuto;
constexpr ac3::oba::joc::Domain kJocDomain =
    CONFIG_AC3FORGE_EXAMPLE_JOC_DOMAIN != 0 ? ac3::oba::joc::Domain::kMdctBand : ac3::oba::joc::Domain::kQmf;

// The decoder settings this board takes from a server: every one the
// extension page names that the library has a setting for. drc_cut and
// drc_boost wait for the library's separate cut and boost scales.
const std::vector<std::string> kDecoderSettings{"mode",    "heavy_compression", "dialnorm",   "downmix",   "ltrt_phase_shift",
                                                "mix_lfe", "programme",         "objects",    "concealment"};

// The sink seam as the burst player's sink.
class SeamSink final : public ac3forge::ScheduledSink {
   public:
    [[nodiscard]] std::optional<ac3forge::PlayoutWrite> write(std::span<const std::span<const float>> outputs) override {
        return sink_write_timed(outputs);
    }
    [[nodiscard]] bool open(std::uint32_t sample_rate, std::size_t outputs) override {
        return sink_open(sample_rate, static_cast<int>(outputs));
    }
    [[nodiscard]] std::size_t max_outputs() override { return static_cast<std::size_t>(std::max(sink_slots(), 1)); }
    void begin_stream() override { sink_begin_play(); }
};

// What the board reports, beside what the burst player measures.
struct Reported {
    std::int32_t volume = 100;
    bool muted = false;
    std::int32_t output_delay_ms = 0;
    std::optional<ac::SettingsError> settings_error;
};

SeamSink g_sink;

// The player and its host. start_player makes and starts both on the key
// task, and only then hands them to every other task through g_running: one
// load gives both or neither, and nothing takes them down afterwards, so a
// pointer a task has loaded stays good. A start that fails frees what it made
// before anything else could reach it. The server's events and the player's
// clock have pointers of their own (Events, BurstPlayerConfig::local_time),
// since both can run before the hand-over.
struct Running {
    std::unique_ptr<ac3forge::BurstPlayer> player;
    std::unique_ptr<ac3forge::SendspinHost> host;
};
Running g_started;  // start_player's until g_running points at it
std::atomic<const Running*> g_running{nullptr};
// The host whose clock the player reads, from when start_player() has made it.
std::atomic<const ac3forge::SendspinHost*> g_clock{nullptr};

std::atomic<bool> g_external{false};
std::mutex g_mutex;  // guards what follows
Reported g_player_reported;
Reported g_role_reported;
std::optional<ac::State> g_last_role_state;
std::int64_t g_last_role_report_us = 0;
// What the control surface asked for while no player was running, for
// start_player to take up: a layout, on the heap only while one waits (a
// layout is some 600 bytes, which internal RAM would otherwise hold for
// good); and a change to the board, which a hello made before it would not
// show.
std::unique_ptr<ac3::render::OutputLayout> g_pending_layout;
bool g_board_changed = false;

// The player and its host, or nothing until both have started.
[[nodiscard]] const Running* running() { return g_running.load(); }

[[nodiscard]] m::PlayerState player_state(const Reported& r) {
    return m::PlayerState{.volume = r.volume,
                          .muted = r.muted,
                          .output_delay_ms = r.output_delay_ms,
                          .required_lead_time_ms = kLeadMs,
                          .min_buffer_ms = kBufferMs,
                          .supported_commands = std::vector<m::PlayerCommand>{m::PlayerCommand::kVolume,
                                                                              m::PlayerCommand::kMute,
                                                                              m::PlayerCommand::kSetOutputDelay},
                          .format = std::nullopt};
}

[[nodiscard]] ac::Support support(const ac3forge::BurstPlayer& player) {
    ac::Support s;
    s.data_types = {ac::DataType::kAc3, ac::DataType::kEac3};
    s.sample_rates = {static_cast<std::int32_t>(kSampleRate)};
    s.outputs.count = sink_slots();
    s.outputs.bit_depth = sink_slot_bits();
    // The widths the board's own page can set: an I2S bus takes either, a
    // sink with no hardware behind it keeps the one it was built with.
    if (std::string_view(sink_name()) == "i2s") {
        s.outputs.bit_depths = {16, 32};
    } else {
        s.outputs.bit_depths = {sink_slot_bits()};
    }
    s.layout_grammar = 1;
    s.management.routing = true;
    s.management.trim_db = {ac3::render::TrimDelay::kMinTrimDb, ac3::render::TrimDelay::kMaxTrimDb};
    s.management.max_delay_ms = static_cast<double>(kMaxDelayMs);
    s.management.crossover_hz = {ac3::render::LayoutRenderer::kMinCrossoverHz,
                                 ac3::render::LayoutRenderer::kMaxCrossoverHz};
    s.management.identify = true;
    s.decoder_settings = kDecoderSettings;
    s.buffer_capacity = player.buffer_capacity();
    return s;
}

[[nodiscard]] std::string mac_text() {
    std::array<std::uint8_t, 6> mac{};
    if (!board_mac(mac)) {
        return {};
    }
    std::array<char, 18> text{};
    (void)std::snprintf(text.data(), text.size(), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3],
                        mac[4], mac[5]);
    return text.data();
}

[[nodiscard]] ss::PlayerConfig player_config(const ac3forge::BurstPlayer& player) {
    ss::PlayerConfig config;
    config.suite = kSuite == 1 ? ss::noise::Suite::kAesGcmSha256 : ss::noise::Suite::kChaChaPolySha256;
    config.name = settings().name.data();
    const esp_app_desc_t* app = esp_app_get_description();
    config.device_info = m::DeviceInfo{.product_name = "Hearth sink",
                                       .manufacturer = "AC3Forge",
                                       .software_version = app != nullptr ? app->version : "",
                                       .mac_address = mac_text()};
    config.supported_roles = {std::string(ac::kRole)};
    if (kOfferPcm) {
        config.supported_roles.emplace_back("player@v1");
    }
    const std::uint64_t capacity = player.buffer_capacity();
    // PCM only: FLAC and Opus would need their decoders on the board, which
    // the player has not measured room for (planning/hearth-reference-player.md,
    // B3). Stereo at 48 kHz, which is the only rate the board plays.
    config.player_support = m::PlayerSupport{
        .supported_formats = {{.codec = m::Codec::kPcm, .channels = 2, .sample_rate = 48000, .bit_depth = 24},
                              {.codec = m::Codec::kPcm, .channels = 2, .sample_rate = 48000, .bit_depth = 16}},
        .buffer_capacity = capacity,
        .commands = {m::PlayerCommand::kVolume, m::PlayerCommand::kMute}};
    config.ac3forge_support = support(player);
    config.pair_methods = {
        {.method = m::PairMethod::kPairingPsk,
         .locations = {m::SecretLocation::kOperator},
         .out_channels = {},
         .formats = {},
         .min_pin_length = 0},
        {.method = m::PairMethod::kDynamicCode,
         .locations = {},
         .out_channels = {m::OutChannel::kDisplay},
         .formats = {m::CodeFormat::kDigits},
         .min_pin_length = 6},
    };
    config.unpaired_access = kUnpairedAccess;
    {
        const std::lock_guard lock(g_mutex);
        config.player_state = player_state(g_player_reported);
    }
    config.ac3forge_state.volume = 100;
    config.ac3forge_state.muted = false;
    config.ac3forge_state.required_lead_time_ms = kLeadMs;
    config.ac3forge_state.min_buffer_ms = kBufferMs;
    config.ac3forge_state.supported_commands = {ac::Command::kVolume, ac::Command::kMute,
                                                ac::Command::kSetOutputDelay, ac::Command::kSettings,
                                                ac::Command::kIdentify};
    config.max_message_bytes = kMaxChunkBytes + 64;
    return config;
}

// The role's client/state, from what the burst player measured and what the
// board was told.
[[nodiscard]] ac::State role_state(const ac3forge::BurstPlayerStatus& status, bool with_levels) {
    ac::State s;
    {
        const std::lock_guard lock(g_mutex);
        s.volume = g_role_reported.volume;
        s.muted = g_role_reported.muted;
        s.output_delay_ms = g_role_reported.output_delay_ms;
        s.settings_error = g_role_reported.settings_error;
    }
    s.required_lead_time_ms = kLeadMs;
    s.min_buffer_ms = kBufferMs;
    s.supported_commands = {ac::Command::kVolume, ac::Command::kMute, ac::Command::kSetOutputDelay,
                            ac::Command::kSettings, ac::Command::kIdentify};
    s.settings_revision = status.settings_revision;
    if (status.have_decoder) {
        s.decoder = status.decoder;
    }
    if (with_levels && status.have_levels) {
        std::vector<ac::Level> levels;
        for (std::size_t o = 0; o < status.outputs; ++o) {
            levels.push_back(ac::Level{.output = static_cast<std::int32_t>(o),
                                       .peak_db = static_cast<double>(status.peak_db[o]),
                                       .rms_db = static_cast<double>(status.rms_db[o])});
        }
        s.levels = std::move(levels);
    }
    s.counters = status.counters;
    return s;
}

[[nodiscard]] bool same_report(const ac::State& a, const ac::State& b) {
    return a.levels.has_value() == b.levels.has_value() && a.volume == b.volume && a.muted == b.muted &&
           a.output_delay_ms == b.output_delay_ms &&
           a.settings_revision == b.settings_revision && a.settings_error.has_value() == b.settings_error.has_value() &&
           a.decoder == b.decoder && a.counters.bursts_played == b.counters.bursts_played &&
           a.counters.underruns == b.counters.underruns && a.counters.late_chunks == b.counters.late_chunks &&
           a.counters.dropped_chunks == b.counters.dropped_chunks &&
           a.counters.invalid_chunks == b.counters.invalid_chunks;
}

class Events final : public ac3forge::SendspinEvents {
   public:
    // Set by start_player before the host starts, and so before its first
    // event, which can come before the two are handed to the other tasks.
    ac3forge::BurstPlayer* player = nullptr;
    ac3forge::SendspinHost* host = nullptr;

    void on_stream_start(const m::PlayerStream& stream) override { player->start_pcm(stream.format); }
    void on_stream_clear() override { player->clear(); }
    void on_stream_end() override { player->end(); }
    void on_audio(std::span<const std::uint8_t> frame, std::int64_t server_time, std::int64_t local_time) override {
        player->pcm(frame, server_time, local_time);
    }
    void on_player_command(const m::PlayerCommandMessage& command) override {
        m::PlayerState state;
        {
            const std::lock_guard lock(g_mutex);
            apply(g_player_reported, command.command == m::PlayerCommand::kVolume   ? Change::kVolume
                                     : command.command == m::PlayerCommand::kMute ? Change::kMute
                                                                                  : Change::kDelay,
                  command.volume, command.mute, command.output_delay_ms);
            state = player_state(g_player_reported);
            player->set_volume(g_player_reported.volume, g_player_reported.muted);
        }
        host->set_player_state(state);
    }

    void on_burst_stream_start(const ac::StreamStart& stream) override { player->start_bursts(stream); }
    void on_burst_stream_clear() override { player->clear(); }
    void on_burst_stream_end() override { player->end(); }
    void on_burst(const ss::BurstChunk& chunk, std::int64_t local_time) override { player->burst(chunk, local_time); }
    void on_invalid_burst() override { player->invalid_chunk(); }

    void on_ac3forge_command(const ac::CommandMessage& command) override {
        switch (command.command) {
            case ac::Command::kVolume:
            case ac::Command::kMute:
            case ac::Command::kSetOutputDelay: {
                const std::lock_guard lock(g_mutex);
                apply(g_role_reported, command.command == ac::Command::kVolume ? Change::kVolume
                                       : command.command == ac::Command::kMute ? Change::kMute
                                                                               : Change::kDelay,
                      command.volume, command.mute, command.output_delay_ms);
                player->set_volume(g_role_reported.volume, g_role_reported.muted);
                break;
            }
            case ac::Command::kSettings: {
                const std::optional<std::string> why = player->settings(command.settings, support(*player));
                const std::lock_guard lock(g_mutex);
                if (why) {
                    g_role_reported.settings_error = ac::SettingsError{.revision = command.settings.revision, .why = *why};
                    std::printf("sendspin: settings %lld refused: %s\n",
                                static_cast<long long>(command.settings.revision), why->c_str());
                } else {
                    g_role_reported.settings_error.reset();
                }
                break;
            }
            case ac::Command::kIdentify:
                player->identify(command.identify);
                break;
        }
        report_now();
    }

    void on_settings_refused(const ac::SettingsError& error) override {
        {
            const std::lock_guard lock(g_mutex);
            g_role_reported.settings_error = error;
        }
        report_now();
    }

    void on_pairing_code(std::string_view /*digits*/) override {}
    void on_pairing_held_back() override {}
    void on_pairing_ended(std::string_view /*outcome*/) override {}

   private:
    enum class Change : std::uint8_t { kVolume, kMute, kDelay };

    static void apply(Reported& reported, Change change, std::int32_t volume, bool mute, std::int32_t delay_ms) {
        switch (change) {
            case Change::kVolume:
                reported.volume = std::clamp<std::int32_t>(volume, 0, 100);
                break;
            case Change::kMute:
                reported.muted = mute;
                break;
            case Change::kDelay:
                reported.output_delay_ms = std::clamp<std::int32_t>(delay_ms, 0, 5000);
                break;
        }
    }

    // The next poll sends the state whatever it measured.
    static void report_now() {
        const std::lock_guard lock(g_mutex);
        g_last_role_state.reset();
    }
};

Events g_events;

void print_token(const ac3forge::SendspinHost& host) {
    const ac3forge::SendspinStore& store = host.store();
    std::array<std::uint8_t, 64> payload{};
    std::copy(store.identity().public_key().begin(), store.identity().public_key().end(), payload.begin());
    std::copy(store.pairing_psk().begin(), store.pairing_psk().end(), payload.begin() + 32);
    // The console is the board's own display: whoever reads it is holding the
    // board, which is what the token asks of them. So it stays out of GET
    // /log, which anyone on the network can read.
    {
        const ac3forge::ConsoleOnly console_only;
        std::printf("sendspin: pairing token %s\n",
                    ss::pairing::encode_token(ss::pairing::TokenVersion::kPairingPsk, payload).c_str());
    }
    ss::crypto::wipe(payload);
}

// A stack for the work that makes or reads the player's keys: starting it
// (a Noise identity to make on the first boot, the store to read, and the
// configuration its sessions copy) and forgetting every pairing (a new
// identity). That is more than app_main's 8 KB, or the control server's or the
// console's task, leaves. The task is created in internal RAM, which NVS
// writes need, and goes when the work is done.
constexpr std::uint32_t kKeyWorkStackBytes = 16384;

struct KeyWork {
    std::function<void()> work;
    SemaphoreHandle_t done = nullptr;
    UBaseType_t unused = 0;
};

void key_work_task(void* argument) {
    auto* job = static_cast<KeyWork*>(argument);
    job->work();
    job->unused = uxTaskGetStackHighWaterMark(nullptr);
    xSemaphoreGive(job->done);
    vTaskDelete(nullptr);
}

// Runs `work` on its own stack and waits for it; false when there was no
// memory for the task.
bool on_key_stack(const char* what, std::function<void()> work) {
    KeyWork job{.work = std::move(work), .done = xSemaphoreCreateBinary()};
    if (job.done == nullptr) {
        return false;
    }
    if (xTaskCreatePinnedToCore(&key_work_task, "sendspin-keys", kKeyWorkStackBytes, &job, tskIDLE_PRIORITY + 5,
                                nullptr, tskNO_AFFINITY) != pdPASS) {
        vSemaphoreDelete(job.done);
        std::printf("sendspin: no memory for a %u-byte stack to %s on\n", static_cast<unsigned>(kKeyWorkStackBytes),
                    what);
        return false;
    }
    xSemaphoreTake(job.done, portMAX_DELAY);
    vSemaphoreDelete(job.done);
    std::printf("sendspin: %s left %u of a %u-byte stack unused\n", what, static_cast<unsigned>(job.unused),
                static_cast<unsigned>(kKeyWorkStackBytes));
    return true;
}

void start_player(const ac3::render::OutputLayout& layout);

}  // namespace

bool sendspin_built() { return true; }

void sendspin_start(const ac3::render::OutputLayout& layout) {
    if (running() != nullptr || !network_ready()) {
        if (!network_ready()) {
            std::printf("sendspin: no network, so no player\n");
        }
        return;
    }
    (void)on_key_stack("starting the player", [&layout] { start_player(layout); });
}

namespace {

void start_player(const ac3::render::OutputLayout& layout) {
    // The player starts first, and the host after it. The player's task
    // stack (kDecodeStackBytes, 32 KB on an ESP32-S3) is the largest block
    // anything here asks internal RAM for. By now the network, mDNS, the
    // control surface and the sink's DMA buffers have left that RAM in
    // pieces: on 2026-09-25 an ESP32-S3 had 104,319 bytes free but no block
    // above 31,744 once the host's state had been made first, and the player
    // did not start. The player reads the host's clock only once a server
    // plays to it, which is after the host has started; until then g_clock
    // is null and there is no server time to convert.
    ac3forge::BurstPlayerConfig config;
    config.sample_rate = kSampleRate;
    config.ring_bytes = kRingBytes;
    config.max_chunk_bytes = kMaxChunkBytes;
    // An I2S bus's ceiling moves with its slot width and wiring, so the
    // buffers are sized once for the most the sink can reach at any setting:
    // sixteen on an ESP32-S3's two lines, eight on an ESP32-C6's one, where
    // every per-output buffer is internal RAM. A sink with no hardware behind
    // it keeps the slots it was built with.
    config.max_outputs = std::min<std::size_t>(ac3forge::Playout::kMaxOutputs,
                                               static_cast<std::size_t>(std::max(sink_max_slots(), 1)));
    config.max_delay_ms = static_cast<double>(kMaxDelayMs);
    config.max_coded_channels = kMaxCodedChannels;
    config.core = kDecodeCore;
    config.stack_bytes = kDecodeStackBytes;
    config.report_every_chunks = kReportEveryChunks;
    config.local_time = [](std::int64_t server_us) -> std::optional<std::int64_t> {
        const ac3forge::SendspinHost* const clock = g_clock.load();
        return clock != nullptr ? clock->local_time(server_us) : std::nullopt;
    };
    config.layout = layout;
    {
        // A layout the control surface set after the caller read its own:
        // app_main applies the command carrying it only after this returns.
        const std::lock_guard lock(g_mutex);
        if (g_pending_layout) {
            config.layout = *g_pending_layout;
            g_pending_layout.reset();
        }
    }
    config.decoder.output.mode = kMode;
    config.decoder.joc_domain = kJocDomain;
    config.stereo_fold = kStereoFold;
    config.objects = kObjects;
    auto player = std::make_unique<ac3forge::BurstPlayer>(config, g_sink);
    if (!player->start()) {
        std::printf("sendspin: no player: it could not start (the lines above say why)\n");
        return;
    }
    auto host = std::make_unique<ac3forge::SendspinHost>();
    g_clock.store(host.get());
    g_events.player = player.get();
    g_events.host = host.get();
    ac3forge::SendspinHostConfig host_config;
    host_config.port = kSendspinPort;
    host_config.stack_bytes = kServerStackBytes;
    host_config.core = 0;
    {
        // The hello is made from the board as it is now; a change from here
        // on is taken up below.
        const std::lock_guard lock(g_mutex);
        g_board_changed = false;
    }
    host_config.player = player_config(*player);
    if (!host->start(std::move(host_config), g_events)) {
        // The player's task reads the host's clock, so it goes first.
        g_clock.store(nullptr);
        player.reset();
        return;
    }

    // Handed to the other tasks, with what the control surface asked for
    // meanwhile: a layout for the player, and a changed board for the host's
    // hello. Both requests take the lock this publishes under, so each one
    // lands either here or on the running player.
    g_started.player = std::move(player);
    g_started.host = std::move(host);
    while (true) {
        {
            const std::lock_guard lock(g_mutex);
            if (g_pending_layout) {
                if (!g_started.player->set_layout(*g_pending_layout)) {
                    std::printf("sendspin: the player cannot take layout %s\n", g_pending_layout->text().data());
                }
                g_pending_layout.reset();
            }
            if (!std::exchange(g_board_changed, false)) {
                g_running.store(&g_started);
                break;
            }
        }
        // Outside the lock, which player_config() takes.
        g_started.host->set_player_config(player_config(*g_started.player));
    }
    print_token(*g_started.host);
    // What is left once the player is up and nothing plays: a first unit's
    // decoder and a server's connection buffers come out of this. On a part
    // with no PSRAM every buffer above is internal RAM too.
    std::printf("sendspin: player up with internal heap %u free, largest block %u\n",
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
}

}  // namespace

bool sendspin_running() { return running() != nullptr; }

void sendspin_leave() {
    const Running* const r = running();
    if (r == nullptr) {
        return;
    }
    r->host->leave();
    if (!r->player->hold(true)) {
        std::printf("sendspin: the player did not stop writing in time\n");
    }
}

bool sendspin_playing() {
    const Running* const r = running();
    return r != nullptr && r->player->active();
}

void sendspin_set_external(bool external) {
    const Running* const r = running();
    if (r == nullptr || g_external.exchange(external) == external) {
        return;
    }
    // The servers are told the board is not available, and their sessions
    // drop what they are still sent. The player stops writing before this
    // returns, so whatever takes the sink next has it to itself.
    r->host->set_external_source(external);
    if (!r->player->hold(external)) {
        std::printf("sendspin: the player did not stop writing in time\n");
    }
}

bool sendspin_set_layout(const ac3::render::OutputLayout& layout) {
    const std::lock_guard lock(g_mutex);
    if (const Running* const r = running()) {
        return r->player->set_layout(layout);
    }
    // For a player that has yet to start, or is starting.
    if (g_pending_layout) {
        *g_pending_layout = layout;
    } else {
        g_pending_layout = std::make_unique<ac3::render::OutputLayout>(layout);
    }
    return true;
}

void sendspin_board_changed() {
    const Running* r = nullptr;
    {
        const std::lock_guard lock(g_mutex);
        r = running();
        if (r == nullptr) {
            // For a player that has yet to start, or is starting.
            g_board_changed = true;
            return;
        }
    }
    r->host->set_player_config(player_config(*r->player));
}

std::optional<ac3forge::ControlSendspin> sendspin_status() {
    const Running* const r = running();
    if (r == nullptr) {
        return std::nullopt;
    }
    const ac3forge::SendspinStatus host = r->host->status();
    const ac3forge::BurstPlayerStatus play = r->player->status();
    ac3forge::ControlSendspin s;
    s.server = host.server_name.data();
    s.server_id = host.server_id.data();
    s.dialect = host.dialect;
    s.psk = host.psk;
    s.activity = host.activity;
    s.role = host.role;
    s.clock_converged = host.clock_converged;
    s.clock_error_us = host.clock_error_us;
    s.clock_updates = host.clock_updates;
    s.clock_rejected = host.clock_rejected;
    s.connections = host.connections;
    s.client_id = host.client_id.data();
    s.paired = host.paired_servers;
    s.pairing_code = host.pairing_code.data();
    s.pairing_held = host.pairing_held_back;
    s.pairing_rounds = host.pairing_rounds;
    s.pairing_outcome = host.pairing_outcome.data();
    s.lost_pairing = host.server_has_lost_pairing;
    s.stream = play.stream;
    s.bursts = play.counters.bursts_played;
    s.underruns = play.counters.underruns;
    s.late = play.counters.late_chunks;
    s.dropped = play.counters.dropped_chunks;
    s.invalid = play.counters.invalid_chunks;
    s.resyncs = play.playout.resyncs;
    s.error_us = play.playout.smoothed_error_us;
    s.worst_error_us = play.playout.worst_error_us;
    if (play.have_play) {
        s.play_frame = play.play_frame;
        if (const std::optional<std::int64_t> server = r->host->server_time(play.play_local_us)) {
            s.play_server_us = *server;
            // When frame 0 played, by the same measure: what two boards in a
            // group compare, whichever frames each last reported.
            s.origin_server_us =
                *server - static_cast<std::int64_t>((play.play_frame * 1'000'000 + (kSampleRate / 2)) / kSampleRate);
        }
    }
    if (play.have_levels) {
        for (std::size_t o = 0; o < play.outputs; ++o) {
            s.peak_db.push_back(play.peak_db[o]);
            s.rms_db.push_back(play.rms_db[o]);
            s.stream_rms.push_back(play.stream_rms[o]);
        }
    }
    s.burst_us = play.burst_us;
    s.worst_burst_us = play.worst_burst_us;
    s.decode_stack_free = static_cast<unsigned long>(play.stack_free);
    s.server_stack_free = static_cast<unsigned long>(host.stack_free);
    s.settings_revision = play.settings_revision;
    s.identifying = play.identifying;
    return s;
}

bool sendspin_pairing(std::string_view action) {
    const Running* const r = running();
    if (r == nullptr) {
        return false;
    }
    if (action == "reset") {
        r->host->reset_pairing_rounds();
        return true;
    }
    if (action == "cancel") {
        r->host->cancel_pairing();
        return true;
    }
    if (action == "forget") {
        bool forgotten = false;
        if (!on_key_stack("forgetting every pairing",
                          [r, &forgotten] { forgotten = r->host->forget_pairings(); })) {
            return false;
        }
        std::printf("sendspin: pairings %s\n", forgotten ? "forgotten; a new identity" : "could not all be forgotten");
        if (forgotten) {
            print_token(*r->host);
        }
        return forgotten;
    }
    return false;
}

std::optional<ac3forge::ControlPairings> sendspin_pairings() {
    const Running* const r = running();
    if (r == nullptr) {
        return std::nullopt;
    }
    const ac3forge::SendspinPairings pairings = r->host->pairings();
    ac3forge::ControlPairings out;
    out.capacity = static_cast<unsigned>(ac3forge::SendspinStore::kRecordCapacity);
    out.servers.reserve(pairings.count);
    for (std::size_t i = 0; i < pairings.count; ++i) {
        const ac3forge::SendspinPairing& p = pairings.servers[i];
        out.servers.push_back(ac3forge::ControlPairing{.server_id = ss::base64url::encode(p.server_key),
                                                       .name = p.name.data(),
                                                       .connected = p.connected,
                                                       .last_playback = p.last_playback,
                                                       .seen = p.seen});
    }
    return out;
}

std::optional<bool> sendspin_forget_server(std::string_view server_id) {
    const Running* const r = running();
    if (r == nullptr) {
        return std::nullopt;
    }
    ss::crypto::Key32 key{};
    if (!ss::base64url::decode_exact(server_id, key)) {
        return false;
    }
    return r->host->forget_server(key);
}

namespace {

// `pair list`: each record's server_id as the console shows it, its name, and
// what it is doing.
void print_pairings(const ac3forge::SendspinHost& host) {
    const ac3forge::SendspinPairings pairings = host.pairings();
    std::printf("sendspin: %u of %u pairing records, the most recently used first\n",
                static_cast<unsigned>(pairings.count), static_cast<unsigned>(ac3forge::SendspinStore::kRecordCapacity));
    for (std::size_t i = 0; i < pairings.count; ++i) {
        const ac3forge::SendspinPairing& p = pairings.servers[i];
        std::printf("sendspin:   %s  %s%s%s%s\n", ss::base64url::encode(p.server_key).substr(0, 8).c_str(),
                    p.name[0] != '\0' ? p.name.data() : "(no name yet)", p.connected ? ", connected" : "",
                    p.last_playback ? ", the last to play" : "", p.seen ? "" : ", not seen since the board started");
    }
}

// `pair forget ID`: the one record whose server_id starts with `id`, which is
// the whole of it or the first eight or more of its characters, as
// `pair list` prints them. Nothing when none does, or more than one.
[[nodiscard]] std::optional<ss::crypto::Key32> record_for(const ac3forge::SendspinPairings& pairings,
                                                          std::string_view id) {
    if (id.size() < 8) {
        return std::nullopt;
    }
    std::optional<ss::crypto::Key32> found;
    for (std::size_t i = 0; i < pairings.count; ++i) {
        if (ss::base64url::encode(pairings.servers[i].server_key).starts_with(id)) {
            if (found) {
                return std::nullopt;
            }
            found = pairings.servers[i].server_key;
        }
    }
    return found;
}

}  // namespace

bool sendspin_console(std::string_view line) {
    const Running* const r = running();
    if (r == nullptr) {
        return false;
    }
    if (line == "pair reset") {
        return sendspin_pairing("reset");
    }
    if (line == "pair cancel") {
        return sendspin_pairing("cancel");
    }
    if (line == "pair forget") {
        return sendspin_pairing("forget");
    }
    // Both read the servers' names from NVS, which the console's own 4 KB
    // task has too little stack for.
    if (line == "pair list") {
        (void)on_key_stack("listing the pairings", [r] { print_pairings(*r->host); });
        return true;
    }
    constexpr std::string_view kForgetOne = "pair forget ";
    if (line.starts_with(kForgetOne)) {
        const std::string_view id = line.substr(kForgetOne.size());
        (void)on_key_stack("forgetting a pairing", [r, id] {
            const std::optional<ss::crypto::Key32> key = record_for(r->host->pairings(), id);
            if (!key || !r->host->forget_server(*key)) {
                std::printf("sendspin: no one pairing has a server_id starting '%.*s'; 'pair list' shows them\n",
                            static_cast<int>(id.size()), id.data());
            }
        });
        return true;
    }
    if (line == "pair token") {
        print_token(*r->host);
        return true;
    }
    if (line == "sendspin") {
        const std::optional<ac3forge::ControlSendspin> s = sendspin_status();
        if (!s) {
            return true;
        }
        std::printf("sendspin: server '%s' (%s, %s, %s), role %s, clock %s (%lld us), %u connection(s), %u paired, "
                    "playing %s: %llu bursts, %llu underruns, %llu late, error %lld us\n",
                    s->server.c_str(), s->dialect.c_str(), s->psk.c_str(), s->activity.c_str(), s->role.c_str(),
                    s->clock_converged ? "converged" : "converging", s->clock_error_us, s->connections, s->paired,
                    s->stream.c_str(), s->bursts, s->underruns, s->late, s->error_us);
        return true;
    }
    return false;
}

void sendspin_poll() {
    const Running* const r = running();
    if (r == nullptr) {
        return;
    }
    const ac3forge::BurstPlayerStatus status = r->player->status();
    const bool playing = status.stream != std::string_view("idle");
    const std::int64_t now = esp_timer_get_time();
    ac::State state = role_state(status, playing);
    bool send = false;
    {
        const std::lock_guard lock(g_mutex);
        // While a stream plays, fresh levels at most ten times a second; any
        // other change at once (the extension page, State object).
        if (!g_last_role_state || !same_report(*g_last_role_state, state)) {
            send = true;
        } else if (playing && state.levels && now - g_last_role_report_us >= 100'000) {
            send = true;
        }
        if (send) {
            g_last_role_state = state;
            g_last_role_report_us = now;
        }
    }
    if (send) {
        r->host->set_ac3forge_state(state);
    }
}

}  // namespace player
