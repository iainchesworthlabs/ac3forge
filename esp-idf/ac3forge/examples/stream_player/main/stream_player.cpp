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
// Both tasks, the ring, the decoders and the renderer are the component's
// (esp-idf/ac3forge/include/ac3forge/player.hpp, layout.hpp, render.hpp), and
// so is the control surface (control.hpp) that lets something on the network
// say what to play and onto what. What is left here is what an integrator's
// own firmware would have to write too: the seams, adapted; a level meter; a
// command queue between the HTTP server's task and this one, which owns the
// player; and the reporting.
//
// BOTH ENDS ARE SEAMS, and CMake resolves both - see byte_source.hpp and
// audio_sink.hpp. This file mentions neither a partition nor I2S.
//
//   source/partition/  flash. The default, and the first one CI runs.
//   source/fatfs/      a FAT volume in flash; the SD source's file layer, runnable.
//   source/sd/         an SD card over SDMMC.
//   source/http/       an HTTP body, over WiFi or QEMU's Ethernet.
//
//   sink/i2s/          a stereo DAC.
//   sink/tdm/          multi-channel on one data line.
//   sink/capture/      converts and checks; what CI runs.
//   sink/null/         counts blocks.

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ac3/core/tables.hpp"
#include "ac3forge/control.hpp"
#include "ac3forge/layout.hpp"
#include "ac3forge/player.hpp"

#include "audio_sink.hpp"
#include "byte_source.hpp"

namespace {

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint64_t kFrameDurationUs = 32000;  // §5.3.2: 1,536 samples at 48 kHz
constexpr std::size_t kMaxSlots = ac3forge::OutputLayout::kMaxSlots;

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
        const std::size_t n = slots.size() < kMaxSlots ? slots.size() : kMaxSlots;
        for (std::size_t slot = 0; slot < n; ++slot) {
            for (const float sample : slots[slot]) {
                sum_squares_[slot] += static_cast<double>(sample) * static_cast<double>(sample);
            }
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
    std::array<double, kMaxSlots> sum_squares_{};
    std::size_t samples_ = 0;
    std::size_t slots_ = 0;
};

// --- what the control surface sees ----------------------------------------------
// The HTTP server runs on its own task and app_main owns the player, so the two
// meet in a queue for commands and a mutex for the player's snapshot. Nothing
// the server's task does touches the player directly.

enum class CommandKind : std::uint8_t { kPlay, kStop, kVolume, kLayout };

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
ac3forge::OutputLayout g_layout;

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
// is the margin the ring's depth is buying.
void report_timing(const char* label, unsigned long value, const ac3forge::PlayerStats& s) {
    const std::uint64_t permille =
        s.frames_played > 0 ? (s.decode_us * 1000) / (kFrameDurationUs * s.frames_played) : 0;
    std::printf("%s=%lu frames=%lu us_per_frame=%lu worst_frame_us=%lu realtime_permille=%lu "
                "resync=%lu ring_low=",
                label, value, static_cast<unsigned long>(s.frames_played),
                static_cast<unsigned long>(s.frames_played > 0 ? s.decode_us / s.frames_played : 0),
                static_cast<unsigned long>(s.worst_frame_us), static_cast<unsigned long>(permille),
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
    }
}

bool begin_play(Session& session) {
    end_play();
    if (!player::source_open()) {
        g_state.store("failed");
        return false;
    }
    g_sink.reset();
    session = Session{};

    ac3forge::PlayerConfig config;
    xSemaphoreTake(g_player_mutex, portMAX_DELAY);
    config.layout = g_layout;
    xSemaphoreGive(g_player_mutex);
    config.stereo_fold = kStereoFold;
    config.objects = kObjects;
    config.decoder.joc_domain = kJocDomain;
    config.decoder.output.mode = kMode;
    config.ring_bytes = CONFIG_AC3FORGE_EXAMPLE_RING_BYTES;
    config.ring_in_psram = CONFIG_AC3FORGE_EXAMPLE_RING_IN_PSRAM != 0;
    config.fetch_core = core_from_kconfig(CONFIG_AC3FORGE_EXAMPLE_FETCH_CORE);
    config.decode_core = core_from_kconfig(CONFIG_AC3FORGE_EXAMPLE_DECODE_CORE);
    config.decode_stack_bytes = CONFIG_AC3FORGE_EXAMPLE_DECODE_STACK_BYTES;
    config.max_passes = kMaxLaps;
    config.volume = g_volume.load();

    auto player = std::make_unique<ac3forge::Player>(config, g_source, g_sink);
    if (!player->start()) {
        g_state.store("failed");
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
    const auto layout = ac3forge::OutputLayout::parse(text);
    if (!layout.has_value() || layout->slots() > static_cast<std::size_t>(player::sink_slots())) {
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
    h.state = []() { return g_state.load(); };
    return h;
}

}  // namespace

extern "C" void app_main() {
    const auto layout = ac3forge::OutputLayout::parse(kLayoutText);
    if (!layout.has_value()) {
        std::printf("error: CONFIG_AC3FORGE_EXAMPLE_LAYOUT \"%s\" is not a layout - a name like "
                    "5.1.4, or a speaker list like L,R,C,LFE,Ls,Rs\n",
                    kLayoutText);
        std::printf("result=fail\n");
        return;
    }
    g_layout = *layout;
    std::printf("ac3forge stream_player: AC-3 or E-AC-3 onto %s\n", g_layout.text().data());

    // The layout's slots, not the coded count: the player renders onto the
    // layout whatever arrives. A layout too wide for the sink stops here and
    // says so; the sink's own message names its limit.
    if (!player::sink_open(kSampleRate, static_cast<int>(g_layout.slots()))) {
        std::printf("result=fail\n");
        return;
    }
    g_commands = xQueueCreate(4, sizeof(Command));
    g_player_mutex = xSemaphoreCreateMutex();

    // The configured location plays at once, as it always has; the control
    // surface, where there is one, can stop it and play something else.
    Session session;
    const bool playing = begin_play(session);
    if (!playing && kControlPort == 0) {
        std::printf("result=fail\n");
        return;
    }

    ac3forge::Control control;
    if (kControlPort != 0) {
        // After the source, which is what brings the network up.
        (void)control.start(control_handlers(), kControlPort);
    }

    // Everything from here is reporting and command handling. The player runs
    // on its own two tasks; this task wakes ten times a second.
    for (;;) {
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
                    if (const auto next = ac3forge::OutputLayout::parse(cmd.text)) {
                        xSemaphoreTake(g_player_mutex, portMAX_DELAY);
                        g_layout = *next;
                        xSemaphoreGive(g_player_mutex);
                        std::printf("control: layout %s for the next play\n",
                                    g_layout.text().data());
                    }
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

        report_end(session, stats);
        xSemaphoreTake(g_player_mutex, portMAX_DELAY);
        g_last_stats = stats;
        g_last_stream = current->stream();
        xSemaphoreGive(g_player_mutex);
        end_play();
        g_state.store(stats.failed ? "failed" : "finished");
        if (kControlPort == 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
            return;
        }
    }
}
