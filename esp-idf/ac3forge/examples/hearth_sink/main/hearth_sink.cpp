// Decode AC-3 or E-AC-3 from wherever the bytes are and play it onto whatever
// speakers the room has - the wiring around ac3forge::Player, which is where
// the work happens.
//
// The difference between this and the i2s_player example beside it is where the
// audio comes from, and that difference is the entire point. That one decodes a
// bitstream linked into its own image, which is fine for showing the codec
// works and is not how anything real gets its audio. This one never has the
// whole stream in memory: a fetch task reads it in blocks from wherever it is,
// a ring buffer holds what has arrived, and a decode task on the other core
// frames it with ac3::io::AccessUnitAccumulator, decodes whatever complete
// access units come out a block at a time, renders each block onto the
// configured layout and writes it to the sink.
//
// Both tasks, the ring and the decoders are the component's
// (esp-idf/ac3forge/include/ac3forge/player.hpp), and so is the control surface
// (control.hpp) that lets something on the network say what to play and onto
// what; the layout and the renderer are the library's
// (src/forge/include/ac3/render/layout.hpp, render.hpp). What is left here is
// what an integrator's own firmware would have to write too: the seams,
// adapted; a level meter; a command queue between the HTTP server's task and
// this one, which owns the player; and the reporting.
//
// BOTH ENDS ARE SEAMS, and CMake resolves both - see byte_source.hpp and
// audio_sink.hpp. This file mentions neither a partition nor I2S.
//
//   source/partition/  flash. The default, and the first one CI runs.
//   source/fatfs/      a FAT volume in flash; the SD source's file layer, runnable.
//   source/sd/         an SD card over SDMMC.
//   source/http/       an HTTP body, over WiFi or QEMU's Ethernet.
//
//   sink/i2s/          an I2S DAC, standard or TDM, reconfigured to whatever
//                      the layout needs.
//   sink/capture/      converts and checks; what CI runs.
//   sink/null/         counts blocks.

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ac3/core/tables.hpp"
#include "ac3/render/layout.hpp"
#include "ac3forge/control.hpp"
#include "ac3forge/firmware.hpp"
#include "ac3forge/log.hpp"
#include "ac3forge/player.hpp"

#include "audio_sink.hpp"
#include "byte_source.hpp"
#include "discovery.hpp"
#include "network.hpp"
#include "provision.hpp"
#include "sendspin.hpp"
#include "settings.hpp"

// The example's half of the stage timers. main/CMakeLists.txt links the
// bare-metal probe's backend (apps/baremetal/stage_timers.cpp) when the
// repository is there to provide it; it reads this clock, and its report
// replaces the stand-ins below, which are what links in a component archive
// that carries no apps/. Built with AC3FORGE_STAGE_TIMERS, each play ends with a
// play.stage[<zone>] line per decoder stage; built without, the library enters
// no zones and the report prints nothing.
namespace ac3probe {
std::uint64_t now_us() { return static_cast<std::uint64_t>(esp_timer_get_time()); }
[[gnu::weak]] void reset_stages() {}
[[gnu::weak]] void report_stages(const char* /*codec*/, int /*frames*/) {}
}  // namespace ac3probe

namespace {

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint64_t kFrameDurationUs = 32000;  // §5.3.2: 1,536 samples at 48 kHz
constexpr std::size_t kMaxSlots = ac3::render::OutputLayout::kMaxSlots;

// From Kconfig, ints so they arrive as plain constants rather than through
// preprocessor conditionals - see main/Kconfig.projbuild. kMaxLaps of 0 plays
// until the source cannot rewind; CI sets a small number so the run ends with
// a verdict. kReportEveryFrames of 0 reports only at the end of a pass.
// kControlPort of 0 means no REST surface, and the application returns from
// app_main when the stream ends, as a CI run needs it to.
constexpr std::uint32_t kMaxLaps = CONFIG_AC3FORGE_EXAMPLE_MAX_LAPS;
constexpr std::uint64_t kReportEveryFrames = CONFIG_AC3FORGE_EXAMPLE_REPORT_EVERY_FRAMES;
constexpr std::uint16_t kControlPort = CONFIG_AC3FORGE_EXAMPLE_CONTROL_PORT;
constexpr const char* kLayoutText = CONFIG_AC3FORGE_EXAMPLE_LAYOUT;
constexpr ac3::DownmixTarget kStereoFold = CONFIG_AC3FORGE_EXAMPLE_STEREO_FOLD != 0
                                               ? ac3::DownmixTarget::kLtRt
                                               : ac3::DownmixTarget::kLoRo;
constexpr ac3forge::PlayerConfig::Objects kObjects =
    CONFIG_AC3FORGE_EXAMPLE_OBJECTS == 1   ? ac3forge::PlayerConfig::Objects::kNever
    : CONFIG_AC3FORGE_EXAMPLE_OBJECTS == 2 ? ac3forge::PlayerConfig::Objects::kAlways
                                           : ac3forge::PlayerConfig::Objects::kAuto;
constexpr ac3::oba::joc::Domain kJocDomain = CONFIG_AC3FORGE_EXAMPLE_JOC_DOMAIN != 0
                                                 ? ac3::oba::joc::Domain::kMdctBand
                                                 : ac3::oba::joc::Domain::kQmf;
constexpr ac3::OperatingMode kMode = CONFIG_AC3FORGE_EXAMPLE_DRC_MODE == 1   ? ac3::OperatingMode::kRf
                                     : CONFIG_AC3FORGE_EXAMPLE_DRC_MODE == 2 ? ac3::OperatingMode::kCustom
                                                                             : ac3::OperatingMode::kLine;

BaseType_t core_from_kconfig(int value) { return value < 0 ? tskNO_AFFINITY : value; }

// --- the seams, as the player's source and sink --------------------------------

// The source seam as the player's ByteSource. The seam's functions are what
// CMake resolved to a directory; this is the adapter, and it is all of it.
class SeamSource final : public ac3forge::ByteSource {
   public:
    std::size_t read(std::span<std::byte> dst) override { return player::source_read(dst); }
    bool rewind() override { return player::source_rewind(); }
};

// The sink seam as the player's PcmSink, with a level meter in front of it.
//
// The meter is what turns result=pass from "some units decoded without
// returning an error" - which a stream decoding to silence satisfies - into an
// end-to-end check: the RMS of what was actually sent, per slot, scaled by 1e6
// the way apps/baremetal/probe.cpp reports levels. The player reports it and
// does not judge it; what the levels should be is a property of the stream and
// the layout, so CI holds the expectation. Written from the decode task, read
// from app_main after the run has ended.
class MeteredSink final : public ac3forge::PcmSink {
   public:
    void write(std::span<const std::span<const float>> slots) override {
        // Squared and summed in float, sixteen samples at a time, and only the
        // partial sums added in double. Every double operation on this part is
        // a call into the soft-float library: done per sample, the meter cost
        // 60 ms a frame on twelve slots - more than the decode it was
        // measuring. A float partial of sixteen squares is good to a few parts
        // in 10^7, far inside CI's tolerance of one digit of RMS x 1e6.
        const std::size_t n = slots.size() < kMaxSlots ? slots.size() : kMaxSlots;
        for (std::size_t slot = 0; slot < n; ++slot) {
            const std::span<const float> samples = slots[slot];
            double sum = 0.0;
            std::size_t i = 0;
            for (; i + kStretch <= samples.size(); i += kStretch) {
                float partial = 0.0F;
                for (std::size_t k = 0; k < kStretch; ++k) {
                    partial += samples[i + k] * samples[i + k];
                }
                sum += static_cast<double>(partial);
            }
            for (; i < samples.size(); ++i) {
                sum += static_cast<double>(samples[i] * samples[i]);
            }
            sum_squares_[slot] += sum;
        }
        if (n > slots_) {
            slots_ = n;
        }
        samples_ += slots.empty() ? 0 : slots[0].size();
        player::sink_write(slots);
    }

    // A play is beginning: the meter starts again, and so do the sink's own
    // figures, so the levels and the sink's line both describe the play they
    // end (audio_sink.hpp).
    void reset() {
        sum_squares_ = {};
        samples_ = 0;
        slots_ = 0;
        player::sink_begin_play();
    }

    void report() const {
        for (std::size_t slot = 0; slot < slots_; ++slot) {
            const double rms = samples_ == 0 ? 0.0
                                             : std::sqrt(sum_squares_[slot] /
                                                         static_cast<double>(samples_));
            std::printf("stream.rms[%u]=%ld\n", static_cast<unsigned>(slot),
                        static_cast<long>((rms * 1e6) + 0.5));
        }
    }

   private:
    static constexpr std::size_t kStretch = 16;
    std::array<double, kMaxSlots> sum_squares_{};
    std::size_t samples_ = 0;
    std::size_t slots_ = 0;
};

// --- what the control surface sees ----------------------------------------------
// The HTTP server runs on its own task and app_main owns the player, so the two
// meet in a queue for commands and a mutex for the player's snapshot. Nothing
// the server's task does touches the player directly.

enum class CommandKind : std::uint8_t { kPlay, kStop, kVolume, kLayout, kFlashMode };

struct Command {
    CommandKind kind = CommandKind::kStop;
    float volume = 1.0F;
    char text[512] = {};  // a location for kPlay, a layout for kLayout
};

QueueHandle_t g_commands = nullptr;
SemaphoreHandle_t g_player_mutex = nullptr;
std::unique_ptr<ac3forge::Player> g_player;  // app_main's; read under the mutex by /status
ac3forge::PlayerStats g_last_stats{};        // of the last run, once it has ended
std::optional<ac3forge::StreamInfo> g_last_stream;
std::atomic<const char*> g_state{"stopped"};
std::atomic<float> g_volume{1.0F};
// The layout the next play uses, and its text for /layout and /status. Written
// by app_main, read under the mutex by the control surface.
ac3::render::OutputLayout g_layout;
// How many channels the sink is presently open for - 0 before the first
// begin_play, which is always a reconfigure since a real layout needs at
// least one. Written only from begin_play, on the task that owns the player.
int g_sink_channels_open = 0;

// Updates over the network (ac3forge/firmware.hpp, planning/esp32-ota.md).
// Its routes are the control surface's; flash mode's teardown runs here, on
// app_main's task, which owns the player, and the firmware's task waits for
// it on g_flash_mode_done. g_control_started is one of the trial's conditions.
ac3forge::Firmware g_firmware;
SemaphoreHandle_t g_flash_mode_done = nullptr;
std::atomic<bool> g_control_started{false};

// --- reporting -------------------------------------------------------------------

// ring_low prints as "-" until the player has measured it: a stream shorter
// than the ring, or one that has just begun, has nothing to say about buffering,
// and a zero there would read as a stall.
void print_ring_low(const ac3forge::PlayerStats& s) {
    if (s.ring_low_valid) {
        std::printf("%lu", static_cast<unsigned long>(s.ring_low_water));
    } else {
        std::printf("-");
    }
}

// realtime_permille is decode-and-render time against the audio time it
// produced: 1000 is exactly real time and anything at or above it cannot play
// without gaps. On a paced sink the figure includes the wait for the DAC, so it
// reads close to 1000 there by construction and the sink's own counters say
// whether the wait was ever too long; on a sink with no pacing it is the cost.
// The worst SINGLE frame matters as much as the average, because the sink's
// queue only absorbs a spike that small - it says how deep. ring_low is the
// least the ring ever held when the decoder came for more: zero means the
// decoder waited on the source at least once, and how far above zero it stays
// is the margin the ring's depth is buying. render_us_per_frame and
// sink_us_per_frame are the parts of us_per_frame spent placing blocks onto the
// layout and inside the sink's write (meter included); the rest is the
// decoder's own.
void report_timing(const char* label, unsigned long value, const ac3forge::PlayerStats& s) {
    const auto per_frame = [&s](std::uint64_t us) {
        return static_cast<unsigned long>(s.frames_played > 0 ? us / s.frames_played : 0);
    };
    const std::uint64_t permille =
        s.frames_played > 0 ? (s.decode_us * 1000) / (kFrameDurationUs * s.frames_played) : 0;
    std::printf("%s=%lu frames=%lu us_per_frame=%lu worst_frame_us=%lu realtime_permille=%lu "
                "render_us_per_frame=%lu sink_us_per_frame=%lu resync=%lu ring_low=",
                label, value, static_cast<unsigned long>(s.frames_played), per_frame(s.decode_us),
                static_cast<unsigned long>(s.worst_frame_us), static_cast<unsigned long>(permille),
                per_frame(s.render_us), per_frame(s.sink_us),
                static_cast<unsigned long>(s.resync_bytes));
    print_ring_low(s);
    std::printf(" heap_free=%lu\n",
                static_cast<unsigned long>(
                    heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
}

void describe(const ac3forge::StreamInfo& info) {
    std::printf("stream: %s acmod=%d channels=%d substreams=%d dialnorm=-%d objects=%s, onto %s "
                "(%d slots%s)\n",
                info.eac3 ? "E-AC-3" : "AC-3", info.acmod, info.channels, info.substreams,
                info.dialnorm, info.objects ? "yes" : "no", g_layout.text().data(), info.slots,
                info.objects_rendered ? ", objects placed" : "");
}

// One run of the player: from source_open to the verdict.
struct Session {
    bool described = false;
    std::int64_t started_us = 0;
    std::uint32_t passes_seen = 0;
    std::uint64_t next_report = kReportEveryFrames;
    unsigned long reports = 0;
};

MeteredSink g_sink;
SeamSource g_source;

void end_play() {
    std::unique_ptr<ac3forge::Player> finished;
    xSemaphoreTake(g_player_mutex, portMAX_DELAY);
    finished = std::move(g_player);
    xSemaphoreGive(g_player_mutex);
    if (finished) {
        finished->stop();
        // The sink is the Sendspin player's again (sendspin.hpp).
        player::sendspin_set_external(false);
    }
}

// `on_source_open` runs once the source is open - and with it the network,
// where there is one - and before the player's tasks take their memory. The
// first play at boot starts the control surface there; see app_main.
bool begin_play(Session& session, const std::function<void()>& on_source_open = {}) {
    end_play();
    // From here until the player runs, /status describes the play being
    // started, not the one before it: the location is already the new one,
    // and the previous run's state and figures beside it would read as this
    // play having finished before it began. "opening" covers the source
    // open, which over a network is the slow part.
    xSemaphoreTake(g_player_mutex, portMAX_DELAY);
    g_last_stats = {};
    g_last_stream.reset();
    xSemaphoreGive(g_player_mutex);
    g_state.store("opening");
    if (!player::source_open()) {
        g_state.store("failed");
        return false;
    }
    if (on_source_open) {
        on_source_open();
    }
    // What the decoder is about to allocate into: the source is open, so a
    // network stack, where there is one, is already up. Internal RAM is the
    // constraint on the network shapes, and this is the figure to hold the
    // decoder's footprint against.
    std::printf("heap: internal free %u (largest block %u), psram free %u\n",
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

    // Read directly into config.layout rather than a separate local: a
    // second stack-resident OutputLayout here is exactly what once
    // boot-looped this example on a main-task stack overflow before
    // kTextBytes was held back (see its own comment in layout.hpp) - the
    // struct is copied by value onto a tight FreeRTOS stack either way, so
    // one copy is what the budget allows.
    ac3forge::PlayerConfig config;
    xSemaphoreTake(g_player_mutex, portMAX_DELAY);
    config.layout = g_layout;
    xSemaphoreGive(g_player_mutex);

    // This play takes the sink from the Sendspin player, which stops writing
    // before this returns and may have opened the sink its own way, so the
    // sink is opened again below whatever this task last opened it for.
    if (player::sendspin_running()) {
        player::sendspin_set_external(true);
        g_sink_channels_open = 0;
    }

    // A layout that needs a different slot count or mode than the sink is
    // presently open for reconfigures it here, between plays and before the
    // new Player exists - never mid-play, per audio_sink.hpp - so a layout
    // sent to the control surface never needs a rebuild or a reboot to take
    // effect. accept_layout already refused anything past the sink's
    // ceiling, so a failure here is the sink itself refusing, not that.
    const int needed_slots = static_cast<int>(config.layout.slots());
    if (needed_slots != g_sink_channels_open) {
        if (!player::sink_open(kSampleRate, needed_slots)) {
            g_state.store("failed");
            player::sendspin_set_external(false);
            return false;
        }
        g_sink_channels_open = needed_slots;
    }

    g_sink.reset();
    session = Session{};
    ac3probe::reset_stages();

    config.stereo_fold = kStereoFold;
    config.objects = kObjects;
    config.decoder.joc_domain = kJocDomain;
    config.decoder.output.mode = kMode;
    config.ring_bytes = CONFIG_AC3FORGE_EXAMPLE_RING_BYTES;
    config.ring_in_psram = CONFIG_AC3FORGE_EXAMPLE_RING_IN_PSRAM != 0;
    config.fetch_core = core_from_kconfig(CONFIG_AC3FORGE_EXAMPLE_FETCH_CORE);
    config.decode_core = core_from_kconfig(CONFIG_AC3FORGE_EXAMPLE_DECODE_CORE);
    config.decode_stack_bytes = CONFIG_AC3FORGE_EXAMPLE_DECODE_STACK_BYTES;
    config.hold_first_unit = CONFIG_AC3FORGE_EXAMPLE_HOLD_FIRST_UNIT != 0;
    config.max_passes = kMaxLaps;
    config.volume = g_volume.load();
    config.sample_rate_hz = kSampleRate;

    auto player = std::make_unique<ac3forge::Player>(config, g_source, g_sink);
    if (!player->start()) {
        g_state.store("failed");
        player.reset();
        player::sendspin_set_external(false);
        return false;
    }
    xSemaphoreTake(g_player_mutex, portMAX_DELAY);
    g_player = std::move(player);
    xSemaphoreGive(g_player_mutex);
    g_state.store("playing");
    return true;
}

// A layout for the next play, from the control surface: parsed here, on the
// server's task, so a refusal is answered at once; applied by app_main.
bool accept_layout(std::string_view text) {
    const auto layout = ac3::render::OutputLayout::parse(text);
    if (!layout.has_value() || layout->slots() > static_cast<std::size_t>(player::sink_slots())) {
        return false;
    }
    // The Sendspin player's too, for streams whose server sets no layout.
    if (!player::sendspin_set_layout(*layout)) {
        return false;
    }
    Command c;
    c.kind = CommandKind::kLayout;
    if (text.size() >= sizeof(c.text)) {
        return false;
    }
    std::memcpy(c.text, text.data(), text.size());
    return xQueueSend(g_commands, &c, 0) == pdTRUE;
}

// The verdict. stream.audio_ms against stream.wall_ms is the whole-pipeline
// real-time check: a player that kept up spent as long playing as the audio
// lasted, one that stalled spent longer by exactly the silence it inserted,
// and a sink with no peripheral runs ahead of the clock. The sink's own line
// says where.
void report_end(const Session& session, const ac3forge::PlayerStats& stats) {
    if (stats.failed) {
        std::printf("error: %s failed (%d)\n", stats.failure, stats.error);
    } else {
        std::printf("stream: %s ended (%s)\n", player::source_name(), stats.failure);
    }
    g_sink.report();
    player::sink_report();
    const std::int64_t wall_us =
        session.started_us == 0 ? 0 : esp_timer_get_time() - session.started_us;
    std::printf("stream.units=%lu stream.held=%lu stream.resync_bytes=%lu stream.sink=%s "
                "stream.sink_frames=%lu stream.source=%s stream.fetched=%lu stream.layout=%s "
                "stream.layout_mismatches=%lu stream.ring_low=",
                static_cast<unsigned long>(stats.frames_played),
                static_cast<unsigned long>(stats.frames_held),
                static_cast<unsigned long>(stats.resync_bytes), player::sink_name(),
                static_cast<unsigned long>(player::sink_frames_written()), player::source_name(),
                static_cast<unsigned long>(stats.fetched_bytes), g_layout.text().data(),
                static_cast<unsigned long>(stats.layout_mismatches));
    print_ring_low(stats);
    std::printf(" stream.decode_stack_free=%lu stream.audio_ms=%lu stream.wall_ms=%lu\n",
                static_cast<unsigned long>(stats.decode_stack_free),
                static_cast<unsigned long>((stats.frames_played * kFrameDurationUs) / 1000),
                static_cast<unsigned long>(wall_us / 1000));
    // Where the play's frames went, stage by stage, when the library was built
    // with AC3FORGE_STAGE_TIMERS; nothing otherwise.
    ac3probe::report_stages("play", static_cast<int>(stats.frames_played));
    std::printf("result=%s\n", (stats.frames_played > 0 && !stats.failed) ? "pass" : "fail");
}

// The control surface's view, all of it through the mutex, the queue or an
// atomic.
ac3forge::ControlHandlers control_handlers() {
    ac3forge::ControlHandlers h;
    h.play = [](std::string_view location) {
        Command c;
        c.kind = CommandKind::kPlay;
        if (location.size() >= sizeof(c.text)) {
            return false;
        }
        std::memcpy(c.text, location.data(), location.size());
        return xQueueSend(g_commands, &c, 0) == pdTRUE;
    };
    h.stop = []() {
        Command c;
        c.kind = CommandKind::kStop;
        xQueueSend(g_commands, &c, 0);
    };
    h.set_volume = [](float volume) {
        Command c;
        c.kind = CommandKind::kVolume;
        c.volume = volume;
        return xQueueSend(g_commands, &c, 0) == pdTRUE;
    };
    h.volume = []() { return g_volume.load(); };
    h.layout = []() {
        xSemaphoreTake(g_player_mutex, portMAX_DELAY);
        std::string text{g_layout.text()};
        xSemaphoreGive(g_player_mutex);
        return text;
    };
    h.set_layout = accept_layout;
    h.name = []() { return std::string(player::settings().name.data()); };
    h.set_name = [](std::string_view text) {
        if (!player::settings_set_name(text)) {
            return false;
        }
        // What a server lists the board as comes from its hello.
        player::sendspin_board_changed();
        return true;
    };
    // On a part with one I2S controller (an ESP32-C6), and on a sink that
    // drives one line only, there is no second line to wire: /status leaves
    // the wiring out, so the page offers no choice about it, and PUT /wiring
    // says it is fixed rather than storing a line nothing can open.
    if (player::sink_second_line_possible()) {
        h.second_line = []() { return player::settings().second_line; };
        h.set_second_line = [](bool wired) {
            xSemaphoreTake(g_player_mutex, portMAX_DELAY);
            const bool playing = g_player != nullptr || player::sendspin_playing();
            xSemaphoreGive(g_player_mutex);
            if (playing || !player::settings_set_second_line(wired)) {
                return false;
            }
            // The sink's ceiling moves with the wiring, so the next play plans
            // against the new one rather than what is open now, and a server is
            // told how many outputs the board has now.
            g_sink_channels_open = 0;
            player::sendspin_board_changed();
            return true;
        };
    }
    h.set_network = [](std::string_view ssid, std::string_view password) {
        return player::settings_set_network(ssid, password);
    };
    h.network = []() -> std::optional<ac3forge::ControlNetwork> {
        player::NetworkLink link = player::network_link();
        if (link.kind == nullptr) {
            return std::nullopt;
        }
        return ac3forge::ControlNetwork{.kind = link.kind,
                                        .ssid = std::move(link.ssid),
                                        .rssi_dbm = link.rssi_dbm,
                                        .address = player::network_address()};
    };
    h.slot_bits = []() { return player::sink_slot_bits(); };
    h.set_slot_bits = [](int bits) {
        xSemaphoreTake(g_player_mutex, portMAX_DELAY);
        const bool playing = g_player != nullptr || player::sendspin_playing();
        xSemaphoreGive(g_player_mutex);
        if (playing) {
            // The lines are carrying a play; changing their width would take
            // the bus out from under it. The caller stops first.
            return false;
        }
        // The Sendspin player lets go of the sink while its lines are closed,
        // and opens them again at its own width afterwards.
        player::sendspin_set_external(true);
        const bool changed = player::sink_set_slot_bits(bits);
        player::sendspin_set_external(false);
        if (!changed) {
            return false;
        }
        // Reopen at the next play whatever its layout asks for: the width
        // change closed the lines, and a layout needing the same slot count
        // as the last one would otherwise find them already open.
        g_sink_channels_open = 0;
        player::sendspin_board_changed();
        return true;
    };
    h.stats = []() {
        xSemaphoreTake(g_player_mutex, portMAX_DELAY);
        const ac3forge::PlayerStats s = g_player ? g_player->stats() : g_last_stats;
        xSemaphoreGive(g_player_mutex);
        return s;
    };
    h.stream = []() {
        xSemaphoreTake(g_player_mutex, portMAX_DELAY);
        const auto s = g_player ? g_player->stream() : g_last_stream;
        xSemaphoreGive(g_player_mutex);
        return s;
    };
    h.location = []() { return std::string{player::source_location()}; };
    h.source_name = []() { return player::source_name(); };
    h.sink_name = []() { return player::sink_name(); };
    h.sink_slots = []() { return player::sink_slots(); };
    h.sink_max_slots = []() { return player::sink_max_slots(); };
    h.sink_max_slots_bits = []() { return player::sink_max_slots_bit_width(); };
    h.state = []() { return g_state.load(); };
    // A build with the player reports on it, as null until it runs; a build
    // without leaves the object out of /status.
    if (player::sendspin_built()) {
        h.sendspin = []() { return player::sendspin_status(); };
        h.pairing = [](std::string_view action) { return player::sendspin_pairing(action); };
        h.pairings = []() { return player::sendspin_pairings(); };
        h.forget_server = [](std::string_view server_id) { return player::sendspin_forget_server(server_id); };
    }
    h.firmware = &g_firmware;
    return h;
}

// What only this board knows about an update (ac3forge::FirmwareHooks).
ac3forge::FirmwareHooks firmware_hooks() {
    ac3forge::FirmwareHooks hooks;
    // On the firmware's task: the teardown itself is app_main's, which owns
    // the player (CommandKind::kFlashMode below).
    // Bounded both ways: this runs on the HTTP server's task for an upload and
    // for PUT /firmware/mode, and an app_main stuck with a full queue would
    // otherwise hold every route with it.
    hooks.enter_flash_mode = [] {
        Command c;
        c.kind = CommandKind::kFlashMode;
        if (xQueueSend(g_commands, &c, pdMS_TO_TICKS(15000)) != pdTRUE) {
            std::printf("firmware: the player took no command for 15 s; going on without stopping it\n");
            return;
        }
        if (xSemaphoreTake(g_flash_mode_done, pdMS_TO_TICKS(15000)) != pdTRUE) {
            std::printf("firmware: the player had not stopped after 15 s; going on without it\n");
        }
    };
    // What a Hearth sink has to hold before an updated image is accepted: the
    // network it was on, the page and the REST routes, and the Sendspin player
    // its servers reach it through.
    hooks.trial_conditions = [] {
        std::vector<std::pair<std::string, bool>> conditions;
        conditions.emplace_back("a network address", player::network_ready());
        conditions.emplace_back("the HTTP server", g_control_started.load());
        if (player::sendspin_built()) {
            conditions.emplace_back("the Sendspin player", player::sendspin_running());
        }
        return conditions;
    };
    hooks.host_names = [] { return std::vector<std::string>{player::discovery_host_name()}; };
    hooks.network_source = [] { return player::network_source(); };
    // Servers hear the board is restarting, rather than finding it gone.
    hooks.before_restart = [] { player::sendspin_leave(); };
    return hooks;
}

ac3forge::FirmwareConfig firmware_config() {
    ac3forge::FirmwareConfig config;
    config.trial.hold_ms = static_cast<std::uint32_t>(CONFIG_AC3FORGE_FIRMWARE_TRIAL_HOLD_S) * 1000U;
    config.trial.deadline_ms = static_cast<std::uint32_t>(CONFIG_AC3FORGE_FIRMWARE_TRIAL_DEADLINE_S) * 1000U;
    config.flash_mode_idle_ms = static_cast<std::uint32_t>(CONFIG_AC3FORGE_FIRMWARE_FLASH_MODE_IDLE_S) * 1000U;
    // CI's rollback tests only: main/CMakeLists.txt's AC3FORGE_FIRMWARE_TEST.
    config.test_unhealthy = AC3FORGE_FIRMWARE_TEST_UNHEALTHY != 0;
    config.test_panic_at_trial = AC3FORGE_FIRMWARE_TEST_PANIC_ON_TRIAL != 0;
    return config;
}

}  // namespace

namespace {

// A failed allocation otherwise shows only as abort() from operator new, which
// says neither how much was asked for nor how much was left. This says both,
// once per failure, before the abort that follows it.
void on_alloc_failed(std::size_t size, std::uint32_t caps, const char* function) {
    std::printf("heap: %s could not allocate %u bytes (caps 0x%lx); internal free %u, largest %u\n",
                function, static_cast<unsigned>(size), static_cast<unsigned long>(caps),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
}

// The Sendspin player, on a board that is on a network. A play that has the
// sink when the player starts keeps it: the player starts held, as
// begin_play leaves one that was already running, and end_play hands it the
// sink.
void start_sendspin() {
    player::sendspin_start(g_layout);
    xSemaphoreTake(g_player_mutex, portMAX_DELAY);
    const bool playing = g_player != nullptr;
    xSemaphoreGive(g_player_mutex);
    if (playing) {
        player::sendspin_set_external(true);
    }
}

}  // namespace

extern "C" void app_main() {
    // The console's recent output for GET /log, from the first line on: a
    // board updated over its network usually has no cable on it
    // (ac3forge/log.hpp).
    (void)ac3forge::log_start(CONFIG_AC3FORGE_LOG_BYTES);
    (void)heap_caps_register_failed_alloc_callback(on_alloc_failed);

    // What this BOARD is, before anything asks: the name it answers to, the
    // network it joins and how its DAC is wired (settings.hpp). NVS comes up
    // here rather than inside the network, so a board that never associates
    // still knows what it is. An empty partition - every QEMU run, and every
    // board before someone provisions it - leaves the image's own Kconfig
    // answers in place.
    player::settings_load();
    if (player::settings().slot_bits != player::sink_slot_bits() &&
        !player::sink_set_slot_bits(player::settings().slot_bits)) {
        std::printf("warning: this sink keeps its %d-bit slots; the stored %d-bit setting is not "
                    "one it can take\n",
                    player::sink_slot_bits(), player::settings().slot_bits);
    }
    // A network built into this image is stored, so the board keeps it through
    // an update to an image without one (network.hpp).
    player::network_adopt_built_in();

    // Updates over the network, before anything else starts: an image on trial
    // starts its clock here (ac3forge/firmware.hpp).
    g_flash_mode_done = xSemaphoreCreateBinary();
    (void)g_firmware.start(firmware_hooks(), firmware_config());

    // The network, if this build has one, before anything plays: a sink is
    // found before it is played to, so the control surface has to answer and
    // mDNS has to be advertising while the board sits idle (network.hpp). A
    // build with no network says so and carries on; so does a board with
    // nothing stored to join, which is what Improv is then there to fix.
    (void)player::network_up();

    // Found by name once it is on one (discovery.hpp), and told what to join
    // when it is not: a browser over the same USB port this console is on
    // (provision.hpp), which starts below with the Sendspin player's console
    // commands when there is a player.
    player::discovery_start();

    const auto layout = ac3::render::OutputLayout::parse(kLayoutText);
    if (!layout.has_value()) {
        std::printf("error: CONFIG_AC3FORGE_EXAMPLE_LAYOUT \"%s\" is not a layout - a name like "
                    "5.1.4, or a speaker list like L,R,C,LFE,Ls,Rs\n",
                    kLayoutText);
        std::printf("result=fail\n");
        return;
    }
    g_layout = *layout;
    std::printf("ac3forge hearth_sink: AC-3 or E-AC-3 onto %s\n", g_layout.text().data());

    g_commands = xQueueCreate(4, sizeof(Command));
    g_player_mutex = xSemaphoreCreateMutex();

    // The configured location plays at once, as it always has; the control
    // surface, where there is one, can stop it and play something else.
    //
    // The control surface starts inside that first play: after its source
    // opens, which is what brings the network up, and before the player's
    // tasks do. The server's task stack has to come from internal RAM, and
    // once the decoder has allocated its first unit there may not be 4 KB of
    // it left in one piece - a board on the network shape, starting the server
    // 41 ms after the player, found the largest free block at 3,328 bytes and
    // came up with no control surface. A play that fails before its source
    // opens still gets one afterwards, so that a location can be sent to it.
    ac3forge::Control control;
    bool control_started = false;
    const auto start_control = [&control, &control_started] {
        if (kControlPort != 0 && !control_started) {
            control_started = true;
            g_control_started = control.start(control_handlers(), kControlPort);
        }
    };
    //
    // A Sendspin sink is played to by its servers, so a build whose location
    // is empty has no play at boot, and sits waiting for one.
    Session session;
    const bool boot_play = player::source_location()[0] != '\0';
    const bool playing = boot_play && begin_play(session, start_control);
    if (!playing && kControlPort == 0 && !player::sendspin_built()) {
        std::printf("result=fail\n");
        return;
    }
    start_control();
    if (!boot_play) {
        g_state.store("stopped");
    }

    // The boot play's source asks for the network too, and can be what brings
    // it up when the call above could not; mDNS then starts here.
    if (player::network_ready()) {
        player::discovery_start();
    }

    // The Sendspin player after the control surface, whose server's stack
    // has to come from internal RAM in one piece, and after the boot play has
    // started, which holds the sink until it ends.
    start_sendspin();
    // Whether the calls above had a network to start on. One that comes up
    // later - over Improv, on a board that had none stored or could not join
    // the one it had, or by itself once a network that was down at boot is
    // back - gets the same calls from the loop below, in the same order; the
    // control surface is already listening. A network that drops after this
    // and comes back needs none of them again: the servers keep listening,
    // and mDNS announces the board again when it has an address.
    bool networked = player::network_ready();
    player::provisioning_start(player::sendspin_running() ? &player::sendspin_console : nullptr);

    // Everything from here is reporting and command handling. The player runs
    // on its own two tasks; this task wakes ten times a second.
    for (;;) {
        if (!networked && player::network_ready()) {
            networked = true;
            player::discovery_start();
            start_sendspin();
            if (player::sendspin_running()) {
                player::provisioning_start(&player::sendspin_console);
            }
        }
        player::sendspin_poll();
        Command cmd;
        while (xQueueReceive(g_commands, &cmd, 0) == pdTRUE) {
            switch (cmd.kind) {
                case CommandKind::kPlay:
                    end_play();
                    if (!player::source_set_location(cmd.text)) {
                        std::printf("control: %s refused location %s\n", player::source_name(),
                                    cmd.text);
                        g_state.store("stopped");
                        break;
                    }
                    (void)begin_play(session);
                    break;
                case CommandKind::kStop:
                    end_play();
                    g_state.store("stopped");
                    break;
                case CommandKind::kVolume:
                    g_volume.store(cmd.volume);
                    xSemaphoreTake(g_player_mutex, portMAX_DELAY);
                    if (g_player) {
                        g_player->set_volume(cmd.volume);
                    }
                    xSemaphoreGive(g_player_mutex);
                    break;
                case CommandKind::kLayout:
                    // Already validated by accept_layout; parsed again here
                    // because the queue carries text, not a layout.
                    if (const auto next = ac3::render::OutputLayout::parse(cmd.text)) {
                        xSemaphoreTake(g_player_mutex, portMAX_DELAY);
                        g_layout = *next;
                        xSemaphoreGive(g_player_mutex);
                        std::printf("control: layout %s for the next play\n",
                                    g_layout.text().data());
                    }
                    break;
                case CommandKind::kFlashMode:
                    // Flash mode (planning/esp32-ota.md): every play stops,
                    // servers hear the board is going, the sink closes and the
                    // Sendspin service is withdrawn. Nothing starts any of it
                    // again: the board restarts to leave flash mode.
                    end_play();
                    player::sendspin_leave();
                    player::sink_close();
                    g_sink_channels_open = 0;
                    player::discovery_withdraw();
                    g_state.store("flash");
                    xSemaphoreGive(g_flash_mode_done);
                    break;
            }
        }

        xSemaphoreTake(g_player_mutex, portMAX_DELAY);
        ac3forge::Player* const current = g_player.get();
        xSemaphoreGive(g_player_mutex);
        if (current == nullptr) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        const bool done = current->wait(pdMS_TO_TICKS(100));
        const auto stats = current->stats();
        if (!session.described) {
            if (const auto info = current->stream()) {
                session.started_us = esp_timer_get_time();
                describe(*info);
                session.described = true;
            }
        }
        // The pass's own figures, taken by the decode task as the pass ended,
        // not this task's later view of them. If more than one pass completed
        // since the last wake - which only a sink with no pacing manages - the
        // earlier ones carry the latest snapshot.
        while (session.passes_seen < stats.passes) {
            ++session.passes_seen;
            report_timing("lap", session.passes_seen, current->last_pass());
        }
        if (kReportEveryFrames != 0 && stats.frames_played >= session.next_report) {
            ++session.reports;
            report_timing("progress", session.reports, stats);
            player::sink_report();
            session.next_report += kReportEveryFrames;
        }
        if (!done) {
            continue;
        }

        // The verdict goes out last. A client that waits for result= and
        // then asks /status - CI's HTTP step does exactly that - must find the
        // run recorded as finished or failed, not a player half torn down.
        // Nothing report_end prints needs the player: the stats are a
        // snapshot, and the meter and the sink are global.
        xSemaphoreTake(g_player_mutex, portMAX_DELAY);
        g_last_stats = stats;
        g_last_stream = current->stream();
        xSemaphoreGive(g_player_mutex);
        end_play();
        g_state.store(stats.failed ? "failed" : "finished");
        report_end(session, stats);
        if (kControlPort == 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
            return;
        }
    }
}
