// The Sendspin player's audio. See ../include/ac3forge/burst_player.hpp.

#include "ac3forge/burst_player.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/message_buffer.h"
#include "freertos/task.h"

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/oba/oamd.hpp"
#include "ac3/render/identify.hpp"
#include "ac3/render/render.hpp"
#include "ac3/render/routing.hpp"
#include "ac3/render/trim_delay.hpp"
#include "ac3/sendspin/ac3forge_player.hpp"
#include "ac3/sendspin/chunks.hpp"

#include "ac3forge/access_units.hpp"

namespace ac3forge {
namespace {

namespace ss = ac3::sendspin;
namespace ac = ss::ac3forge;
namespace m = ss::messages;
using ac3::render::IdentifyTone;
using ac3::render::LayoutRenderer;
using ac3::render::OutputLayout;
using ac3::render::Routing;
using ac3::render::TrimDelay;
using Location = ac3::eac3::chanmap::Location;

constexpr std::size_t kBlock = Playout::kBlockFrames;
static_assert(kBlock == ac3::kSamplesPerBlock, "the player writes the decoder's block");
constexpr std::size_t kMaxSlots = OutputLayout::kMaxSlots;
// A burst is always 1,536 samples (planning/hearth-sendspin-extension.md).
constexpr std::uint64_t kBurstFrames = 1536;
// Ring bytes a chunk may not take, so a stream start, clear or end always
// fits behind it.
constexpr std::size_t kControlReserve = 512;
// Levels are measured over 100 ms at 48 kHz.
constexpr std::uint32_t kLevelWindow = 4800;
// Units a decoder can hold before their blocks arrive.
constexpr std::size_t kMaxPendingBeds = 8;
// Bursts whose times are remembered for the frames their decode releases
// late.
constexpr std::size_t kMaxMarks = 16;

constexpr EventBits_t kExited = BIT0;
constexpr EventBits_t kHeld = BIT1;

enum class Kind : std::uint8_t {
    kStartBursts,
    kStartPcm,
    kClear,
    kEnd,
    kBurst,
    kPcm,
};

// A chunk that arrived before the clock map said anything.
constexpr std::int64_t kNoMap = std::numeric_limits<std::int64_t>::min();

// Every ring entry starts with this, and a chunk's bytes follow it.
struct Header {
    Kind kind = Kind::kBurst;
    std::uint8_t data_type = 0;
    std::uint16_t pc = 0;
    std::uint16_t pd = 0;
    std::uint16_t reserved = 0;
    std::uint32_t generation = 0;
    std::int32_t sample_rate = 0;
    std::int32_t channels = 0;
    std::int32_t bit_depth = 0;
    std::int64_t local_us = 0;
    std::int64_t server_us = 0;
    // BurstPlayerConfig::local_time for server_us when the chunk arrived.
    std::int64_t map_local_us = kNoMap;
};
static_assert(std::is_trivially_copyable_v<Header>);

enum class Stream : std::uint8_t { kIdle, kBursts, kPcm };

[[nodiscard]] bool same_layout(const ac3::eac3::chanmap::Layout& a, const ac3::eac3::chanmap::Layout& b) {
    if (a.count != b.count) {
        return false;
    }
    for (int i = 0; i < a.count; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] float db_of(float linear) {
    if (!(linear > 1e-6F)) {
        return static_cast<float>(ac::kSilenceDb);
    }
    return std::max(20.0F * std::log10(linear), static_cast<float>(ac::kSilenceDb));
}

// PSRAM when the part has it, for the player's big buffers; internal RAM
// otherwise. "Has it" is asked, not learned from a failed allocation, which a
// failed-allocation hook would read as running out.
[[nodiscard]] std::uint32_t bulk_caps() {
    return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0 ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                                                            : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

}  // namespace

// The sink, with the time spent in its writes kept apart from the decode's.
class TimedSink final : public PlayoutSink {
   public:
    explicit TimedSink(ScheduledSink& sink) : sink_(&sink) {}
    [[nodiscard]] std::optional<PlayoutWrite> write(std::span<const std::span<const float>> outputs) override {
        const std::int64_t entered = esp_timer_get_time();
        std::optional<PlayoutWrite> written = sink_->write(outputs);
        spent_us_ += static_cast<std::uint64_t>(esp_timer_get_time() - entered);
        return written;
    }
    [[nodiscard]] std::uint64_t take_spent() { return std::exchange(spent_us_, 0); }

   private:
    ScheduledSink* sink_;
    std::uint64_t spent_us_ = 0;
};

struct BurstPlayer::Impl {
    Impl(const BurstPlayerConfig& cfg, ScheduledSink& snk)
        : config(cfg), sink(snk), timed(snk), layout(cfg.layout), renderer(cfg.layout, cfg.sample_rate),
          tone(cfg.sample_rate) {}

    BurstPlayerConfig config;
    ScheduledSink& sink;
    TimedSink timed;

    // The ring, allocated by hand and created with the Static API, as the
    // Player's is (player.cpp says why).
    MessageBufferHandle_t ring = nullptr;
    std::uint8_t* ring_storage = nullptr;
    StaticMessageBuffer_t* ring_struct = nullptr;
    // The session task's scratch for one entry, and the decode task's.
    std::uint8_t* send_buffer = nullptr;
    std::uint8_t* receive_buffer = nullptr;
    std::size_t entry_bytes = 0;
    std::atomic<std::uint32_t> generation{1};

    TaskHandle_t task = nullptr;
    EventGroupHandle_t events = nullptr;
    std::atomic<bool> stopping{false};
    std::atomic<bool> held{false};

    // Every float buffer, one allocation: the rendered slots, the outputs,
    // the playout's staging, the PCM path's channels and the delay lines.
    float* floats = nullptr;
    std::array<std::span<float>, kMaxSlots> rendered{};
    std::array<std::span<float>, Playout::kMaxOutputs> outputs{};
    std::array<std::span<const float>, kMaxSlots> rendered_views{};
    std::array<std::span<const float>, Playout::kMaxOutputs> output_views{};
    std::array<std::span<float>, 2> pcm_channels{};
    std::array<std::span<const float>, 2> pcm_views{};
    std::span<float> delay_storage;
    std::size_t max_delay_samples = 0;
    std::optional<Playout> playout;

    // --- the decode task's own, and nobody else's ---------------------------
    Stream stream = Stream::kIdle;
    ac::DataType data_type = ac::DataType::kEac3;
    m::AudioFormat pcm_format;
    std::optional<ac3::FrameDecoder> ac3_decoder;
    std::optional<ac3::Eac3Decoder> eac3_decoder;
    std::optional<int> programme;
    std::array<ac3::eac3::chanmap::Layout, kMaxPendingBeds> beds{};
    std::size_t bed_head = 0;
    std::size_t bed_count = 0;
    std::optional<ac3::eac3::chanmap::Layout> renderer_bed;

    OutputLayout layout;
    ac3::render::Serving serving;
    ac3::DecoderConfig decoder_config;
    LayoutRenderer renderer;
    Routing routing;
    TrimDelay trim_delay;
    // The trims and delays last set, kept so that reopening the sink for
    // another output count keeps them.
    std::array<double, Playout::kMaxOutputs> trims_db{};
    std::array<double, Playout::kMaxOutputs> delays_ms{};
    std::size_t outputs_open = 0;
    IdentifyTone tone;
    std::optional<ac::Identify> identifying;
    std::optional<ac::Identify> identify_applied;

    // Where decoded frames go in time: the stream's first burst's server
    // time, the frame the decoder hands over next, and the recent bursts'
    // local times by frame.
    bool have_origin = false;
    std::int64_t origin_server_us = 0;
    std::uint64_t decoded_frame = 0;
    bool decoded_frame_set = false;
    struct Mark {
        std::uint64_t frame = 0;
        std::int64_t local_us = 0;
    };
    std::array<Mark, kMaxMarks> marks{};
    std::size_t mark_head = 0;
    std::size_t mark_count = 0;

    // The meter.
    std::array<float, Playout::kMaxOutputs> window_peak{};
    std::array<double, Playout::kMaxOutputs> window_sum{};
    std::uint32_t window_frames = 0;
    std::array<double, Playout::kMaxOutputs> stream_sum{};
    std::uint64_t stream_frames = 0;

    // Volume, in by any task, ramped by the decode task.
    std::atomic<float> target_gain{1.0F};
    float gain = 1.0F;

    // Burst timing.
    std::uint64_t decode_us_total = 0;
    std::uint64_t sink_us_total = 0;
    std::uint64_t timed_bursts = 0;
    std::uint32_t worst_burst_us = 0;
    // The most the ring has held since the stream began, in bytes: how far
    // behind the network the decode fell at worst. Measured against what an
    // empty ring has free, which the buffer's own bookkeeping takes from.
    std::size_t ring_high = 0;
    std::size_t ring_free_empty = 0;

    std::uint64_t late_chunks = 0;
    std::uint64_t bursts_played = 0;
    bool have_decoder_report = false;
    ac::DecoderReport decoder_report;

    // --- from other tasks -------------------------------------------------------
    std::atomic<std::uint64_t> dropped_chunks{0};
    std::atomic<std::uint64_t> invalid_chunks{0};
    std::atomic<bool> active{false};
    std::mutex settings_mutex;
    std::optional<ac::Settings> pending_settings;
    std::optional<OutputLayout> pending_layout;
    bool have_server_settings = false;
    std::optional<ac::Identify> pending_identify;
    bool identify_changed = false;
    std::int64_t settings_revision = 0;

    mutable std::mutex status_mutex;
    BurstPlayerStatus status;

    // ---------------------------------------------------------------------------

    void send(const Header& header, std::span<const std::uint8_t> payload, bool control) {
        if (ring == nullptr) {
            return;
        }
        const std::size_t bytes = sizeof(Header) + payload.size();
        if (bytes > entry_bytes) {
            dropped_chunks.fetch_add(1);
            return;
        }
        // A chunk leaves room behind it for a start, clear or end.
        if (!control && xMessageBufferSpacesAvailable(ring) < bytes + kControlReserve) {
            dropped_chunks.fetch_add(1);
            return;
        }
        std::memcpy(send_buffer, &header, sizeof(Header));
        if (!payload.empty()) {
            std::memcpy(send_buffer + sizeof(Header), payload.data(), payload.size());
        }
        const std::size_t sent = xMessageBufferSend(ring, send_buffer, bytes, control ? pdMS_TO_TICKS(100) : 0);
        if (sent != bytes && !control) {
            dropped_chunks.fetch_add(1);
        } else if (sent != bytes) {
            std::printf("burst player: a stream control message did not fit the ring\n");
        }
    }

    // --- settings -----------------------------------------------------------

    // The outputs the sink is opened for: the layout's slots, or as many as
    // the routing reaches, and the identified output.
    [[nodiscard]] std::size_t outputs_needed() const {
        std::size_t needed = routing.outputs() > 0 ? 0 : layout.slots();
        for (std::size_t c = 0; c < routing.channels(); ++c) {
            const int output = routing.output_of(c);
            if (output != Routing::kUnassigned) {
                needed = std::max(needed, static_cast<std::size_t>(output) + 1);
            }
        }
        if (identifying) {
            needed = std::max(needed, static_cast<std::size_t>(identifying->output) + 1);
        }
        return std::clamp<std::size_t>(needed, 1, std::min(config.max_outputs, Playout::kMaxOutputs));
    }

    // Everything that follows from the layout and the decoder's settings.
    void configure(const OutputLayout& new_layout, const ac3::DecoderConfig& decoder, ac3::DownmixTarget fold,
                   ac3::render::ObjectsPolicy objects, double crossover_hz, const std::optional<Routing>& new_routing,
                   std::span<const double> trims, std::span<const double> delays) {
        layout = new_layout;
        serving = ac3::render::serve(layout, fold, objects);
        decoder_config = decoder;
        ac3::render::configure_decoder(serving, decoder_config);
        renderer = LayoutRenderer{layout, config.sample_rate, crossover_hz};
        renderer_bed.reset();
        if (new_routing) {
            routing = *new_routing;
        } else {
            routing = Routing::identity(layout.slots(), layout.slots()).value_or(Routing{});
        }
        trims_db.fill(0.0);
        delays_ms.fill(0.0);
        std::copy_n(trims.begin(), std::min(trims.size(), trims_db.size()), trims_db.begin());
        std::copy_n(delays.begin(), std::min(delays.size(), delays_ms.size()), delays_ms.begin());
        open_outputs(false);
        reset_decoding();
    }

    // The sink opened for as many outputs as the routing and the identify
    // tone reach, and the trims and delays set on them. `always` opens it
    // even when the count has not changed, for a stream that begins after
    // something else may have opened the sink its own way.
    void open_outputs(bool always) {
        const std::size_t needed = outputs_needed();
        if (always || needed != outputs_open) {
            if (playout && needed != outputs_open) {
                playout->flush(esp_timer_get_time(), timed);
            }
            if (sink.open(config.sample_rate, needed)) {
                outputs_open = needed;
            } else {
                std::printf("burst player: the sink cannot open %u outputs\n", static_cast<unsigned>(needed));
            }
            if (playout) {
                playout->set_outputs(outputs_open);
                playout->realign();
            }
        }
        (void)trim_delay.configure(delay_storage, outputs_open, max_delay_samples);
        for (std::size_t o = 0; o < outputs_open; ++o) {
            (void)trim_delay.set_trim_db(o, trims_db[o]);
            (void)trim_delay.set_delay(o, TrimDelay::samples_for_ms(delays_ms[o], config.sample_rate));
        }
    }

    void configure_defaults(const OutputLayout& new_layout) {
        configure(new_layout, config.decoder, config.stereo_fold, config.objects, LayoutRenderer::kDefaultCrossoverHz,
                  std::nullopt, {}, {});
    }

    // The server's settings, which replace every earlier one whole: a key
    // left out takes the board's own value. Checked when they arrived.
    void apply(const ac::Settings& s) {
        OutputLayout new_layout = config.layout;
        if (s.layout) {
            if (const auto parsed = OutputLayout::parse(*s.layout)) {
                new_layout = *parsed;
            }
        }
        ac3::DecoderConfig decoder = config.decoder;
        const ac::DecoderSettings& d = s.decoder;
        if (d.mode) {
            decoder.output.mode = *d.mode == ac::DecoderMode::kRf     ? ac3::OperatingMode::kRf
                                  : *d.mode == ac::DecoderMode::kLine ? ac3::OperatingMode::kLine
                                                                       : ac3::OperatingMode::kCustom;
        }
        if (d.heavy_compression) {
            decoder.heavy_compression = *d.heavy_compression;
        }
        if (d.dialnorm) {
            decoder.output.apply_dialnorm = *d.dialnorm;
        }
        if (d.ltrt_phase_shift) {
            decoder.output.ltrt_phase_shift = *d.ltrt_phase_shift;
        }
        if (d.mix_lfe) {
            decoder.output.mix_lfe = *d.mix_lfe;
        }
        if (d.has_programme) {
            decoder.programme = d.programme;
        }
        if (d.concealment) {
            decoder.concealment = *d.concealment == ac::Concealment::kRepeatFade ? ac3::ConcealmentPolicy::kRepeatFade
                                  : *d.concealment == ac::Concealment::kMute     ? ac3::ConcealmentPolicy::kMute
                                                                                  : ac3::ConcealmentPolicy::kNone;
        }
        ac3::DownmixTarget fold = config.stereo_fold;
        if (d.downmix) {
            fold = *d.downmix == ac::Downmix::kLtRt ? ac3::DownmixTarget::kLtRt : ac3::DownmixTarget::kLoRo;
        }
        ac3::render::ObjectsPolicy objects = config.objects;
        if (d.objects) {
            objects = *d.objects == ac::ObjectsPolicy::kAlways  ? ac3::render::ObjectsPolicy::kAlways
                      : *d.objects == ac::ObjectsPolicy::kNever ? ac3::render::ObjectsPolicy::kNever
                                                                : ac3::render::ObjectsPolicy::kAuto;
        }
        std::optional<Routing> new_routing;
        if (s.routing) {
            new_routing = Routing::parse(*s.routing, std::min(config.max_outputs, Playout::kMaxOutputs));
        }
        const std::vector<double> none;
        configure(new_layout, decoder, fold, objects, s.crossover_hz.value_or(LayoutRenderer::kDefaultCrossoverHz),
                  new_routing, s.trim_db ? std::span<const double>(*s.trim_db) : std::span<const double>(none),
                  s.delay_ms ? std::span<const double>(*s.delay_ms) : std::span<const double>(none));
        std::printf("burst player: settings %lld applied: layout %s, %u outputs\n",
                    static_cast<long long>(s.revision), layout.text().data(), static_cast<unsigned>(outputs_open));
    }

    // At a burst boundary, or while nothing plays.
    void apply_pending() {
        std::optional<ac::Settings> settings;
        std::optional<OutputLayout> own_layout;
        std::optional<ac::Identify> identify;
        bool identify_update = false;
        {
            const std::lock_guard lock(settings_mutex);
            settings.swap(pending_settings);
            own_layout.swap(pending_layout);
            identify = pending_identify;
            identify_update = std::exchange(identify_changed, false);
        }
        if (settings) {
            apply(*settings);
            const std::lock_guard lock(settings_mutex);
            settings_revision = settings->revision;
        } else if (own_layout) {
            configure_defaults(*own_layout);
        }
        if (identify_update) {
            identifying = identify;
            if (identifying) {
                (void)tone.set_level_db(identifying->level_db);
                tone.reset();
            }
            if (outputs_needed() != outputs_open) {
                open_outputs(false);
            }
        }
    }

    // --- decoding ---------------------------------------------------------------

    void reset_decoding() {
        ac3_decoder.reset();
        eac3_decoder.reset();
        programme.reset();
        bed_head = 0;
        bed_count = 0;
        renderer_bed.reset();
        renderer.reset();
        trim_delay.reset();
        decoded_frame_set = false;
        mark_count = 0;
        mark_head = 0;
    }

    void new_stream(Stream kind) {
        reset_decoding();
        stream = kind;
        have_origin = false;
        have_decoder_report = false;
        bursts_played = 0;
        late_chunks = 0;
        dropped_chunks.store(0);
        invalid_chunks.store(0);
        decode_us_total = 0;
        sink_us_total = 0;
        timed_bursts = 0;
        worst_burst_us = 0;
        ring_high = 0;
        window_peak.fill(0.0F);
        window_sum.fill(0.0);
        window_frames = 0;
        stream_sum.fill(0.0);
        stream_frames = 0;
        playout->restart();
        // The least free internal heap from here to the stream's end, which
        // its closing line reports. Already running when the last stream was
        // never closed: its start, then, is the earlier one.
        (void)heap_caps_monitor_local_minimum_free_size_start();
        // Opened again whatever it was: a play from the control surface may
        // have opened the sink its own way since the last stream.
        open_outputs(true);
        sink.begin_stream();
        active.store(kind != Stream::kIdle);
    }

    // The console's verdict on a stream, as the Player's plays end with one:
    // its counters, the playout's figures, the least free internal heap while
    // it played (heap_free, which CI holds to a floor), and each output's RMS
    // over the whole stream.
    void report_stream_end() {
        if (stream == Stream::kIdle) {
            return;
        }
        const std::size_t least_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        (void)heap_caps_monitor_local_minimum_free_size_stop();
        const Playout::Stats& p = playout->stats();
        std::printf("sendspin.stream=%s bursts=%llu late=%llu dropped=%llu invalid=%llu underruns=%llu "
                    "resyncs=%u silence_frames=%llu skipped_frames=%llu dropped_frames=%llu repeated_frames=%llu "
                    "worst_error_us=%lld burst_us=%llu sink_us=%llu ring_high=%lu heap_free=%lu\n",
                    stream == Stream::kBursts ? "bursts" : "pcm", static_cast<unsigned long long>(bursts_played),
                    static_cast<unsigned long long>(late_chunks),
                    static_cast<unsigned long long>(dropped_chunks.load()),
                    static_cast<unsigned long long>(invalid_chunks.load()),
                    static_cast<unsigned long long>(p.underruns), static_cast<unsigned>(p.resyncs),
                    static_cast<unsigned long long>(p.silence_frames), static_cast<unsigned long long>(p.skipped_frames),
                    static_cast<unsigned long long>(p.dropped_frames),
                    static_cast<unsigned long long>(p.repeated_frames), static_cast<long long>(p.worst_error_us),
                    static_cast<unsigned long long>(timed_bursts > 0 ? decode_us_total / timed_bursts : 0),
                    static_cast<unsigned long long>(timed_bursts > 0 ? sink_us_total / timed_bursts : 0),
                    static_cast<unsigned long>(ring_high), static_cast<unsigned long>(least_heap));
        for (std::size_t o = 0; o < outputs_open; ++o) {
            std::printf("sendspin.rms[%u]=%lu\n", static_cast<unsigned>(o),
                        static_cast<unsigned long>(stream_rms(o)));
        }
    }

    [[nodiscard]] std::uint32_t stream_rms(std::size_t output) const {
        if (stream_frames == 0) {
            return 0;
        }
        const double rms = std::sqrt(stream_sum[output] / static_cast<double>(stream_frames));
        return static_cast<std::uint32_t>((rms * 1e6) + 0.5);
    }

    void end_stream() {
        playout->flush(esp_timer_get_time(), timed);
        report_stream_end();
        reset_decoding();
        stream = Stream::kIdle;
        have_decoder_report = false;
        active.store(false);
    }

    // config.local_time for `server_us`, or kNoMap.
    [[nodiscard]] std::int64_t map_now(std::int64_t server_us) const {
        if (!config.local_time) {
            return kNoMap;
        }
        return config.local_time(server_us).value_or(kNoMap);
    }

    // A chunk's local time, moved by as much as the clock map has moved since
    // the chunk arrived (BurstPlayerConfig::local_time).
    [[nodiscard]] std::int64_t local_of(const Header& header) const {
        if (header.map_local_us == kNoMap) {
            return header.local_us;
        }
        const std::int64_t now = map_now(header.server_us);
        return now == kNoMap ? header.local_us : header.local_us + (now - header.map_local_us);
    }

    [[nodiscard]] std::uint64_t frame_of(std::int64_t server_us) const {
        const std::int64_t since = server_us - origin_server_us;
        if (since <= 0) {
            return 0;
        }
        return static_cast<std::uint64_t>(((since * config.sample_rate) + 500'000) / 1'000'000);
    }

    void mark(std::uint64_t frame, std::int64_t local_us) {
        const std::size_t at = (mark_head + mark_count) % kMaxMarks;
        marks[at] = Mark{.frame = frame, .local_us = local_us};
        if (mark_count < kMaxMarks) {
            ++mark_count;
        } else {
            mark_head = (mark_head + 1) % kMaxMarks;
        }
    }

    // When decoded frame `frame` should play: its burst's time, and the
    // frames since.
    [[nodiscard]] std::optional<std::int64_t> target_of(std::uint64_t frame) {
        std::optional<Mark> best;
        for (std::size_t i = 0; i < mark_count; ++i) {
            const Mark& candidate = marks[(mark_head + i) % kMaxMarks];
            if (candidate.frame <= frame && (!best || candidate.frame >= best->frame)) {
                best = candidate;
            }
        }
        if (!best) {
            return std::nullopt;
        }
        const std::uint64_t since = frame - best->frame;
        return best->local_us + static_cast<std::int64_t>(((since * 1'000'000) + (config.sample_rate / 2)) / config.sample_rate);
    }

    void push_bed(const ac3::eac3::chanmap::Layout& bed) {
        if (bed_count == kMaxPendingBeds) {
            bed_head = (bed_head + 1) % kMaxPendingBeds;
            --bed_count;
        }
        beds[(bed_head + bed_count) % kMaxPendingBeds] = bed;
        ++bed_count;
    }

    // One decoded block: placed, routed, trimmed, delayed, metered and
    // scheduled.
    void deliver(const ac3::PcmBlock& block) {
        std::size_t n = block.channels.empty() ? 0 : block.channels.front().size();
        if (n == 0 && !block.objects.empty()) {
            n = block.objects.front().size();
        }
        n = std::min(n, kBlock);
        if (n == 0) {
            return;
        }
        const std::size_t slots = layout.slots();
        const std::span<const std::span<float>> out(rendered.data(), slots);
        if (block.index == 0 && bed_count > 0) {
            const ac3::eac3::chanmap::Layout bed = beds[bed_head];
            bed_head = (bed_head + 1) % kMaxPendingBeds;
            --bed_count;
            if (!serving.fold && (!renderer_bed || !same_layout(*renderer_bed, bed))) {
                renderer.set_bed(bed);
                renderer_bed = bed;
            }
        }
        if (serving.fold) {
            renderer.render_folded(block, 1.0F, out);
        } else {
            if (serving.reconstruct && block.index == 0) {
                renderer.set_objects(block.object_metadata, block.objects.size());
            }
            renderer.render(block, serving.reconstruct, 1.0F, out);
        }
        emit(n);
    }

    // The rendered slots from here on: `n` frames of them.
    void emit(std::size_t n) {
        const std::size_t slots = layout.slots();
        for (std::size_t s = 0; s < slots; ++s) {
            rendered_views[s] = std::span<const float>(rendered[s].data(), n);
        }
        std::array<std::span<float>, Playout::kMaxOutputs> view{};
        for (std::size_t o = 0; o < outputs_open; ++o) {
            view[o] = outputs[o].first(n);
            output_views[o] = view[o];
        }
        const std::span<const std::span<float>> out(view.data(), outputs_open);
        routing.apply(std::span<const std::span<const float>>(rendered_views.data(), slots), out);
        // Outputs past the patch's own, which only the identify tone opens.
        for (std::size_t o = routing.outputs(); o < outputs_open; ++o) {
            std::fill(view[o].begin(), view[o].end(), 0.0F);
        }
        trim_delay.process(out);
        if (identifying) {
            // Every output carries the tone or silence; the stream goes on
            // underneath, decoded and discarded, and keeps its timeline.
            const std::size_t output = identifying->output >= 0 ? static_cast<std::size_t>(identifying->output)
                                                                : outputs_open;
            tone.fill(out, output, carries_lfe(output) ? IdentifyTone::Band::kLow : IdentifyTone::Band::kFull);
        }
        apply_gain(out, n);
        meter(out, n);

        const std::uint64_t first = decoded_frame;
        decoded_frame += n;
        std::optional<std::int64_t> target = target_of(first);
        if (!target) {
            return;  // frames from before any burst this decode knows of
        }
        playout->push(std::span<const std::span<const float>>(output_views.data(), outputs_open), n, *target, first,
                      esp_timer_get_time(), timed);
    }

    [[nodiscard]] bool carries_lfe(std::size_t output) const {
        const int channel = routing.channel_of(output);
        if (channel == Routing::kUnassigned || static_cast<std::size_t>(channel) >= layout.slots()) {
            return false;
        }
        return layout.slot(static_cast<std::size_t>(channel)).kind == ac3::render::Speaker::Kind::kLfe;
    }

    void apply_gain(std::span<const std::span<float>> out, std::size_t n) {
        const float target = target_gain.load();
        if (target == 1.0F && gain == 1.0F) {
            return;
        }
        const float start = gain;
        const float step = (target - start) / static_cast<float>(n);
        for (const std::span<float>& samples : out) {
            float g = start;
            for (std::size_t k = 0; k < n; ++k) {
                g += step;
                samples[k] *= g;
            }
        }
        gain = target;
    }

    void meter(std::span<const std::span<float>> out, std::size_t n) {
        for (std::size_t o = 0; o < out.size(); ++o) {
            const std::span<float> samples = out[o];
            float peak = window_peak[o];
            double sum = 0.0;
            std::size_t k = 0;
            // Squared in float sixteen at a time and summed in double, as
            // the example's MeteredSink does: every double operation on
            // this part is a software call.
            for (; k + 16 <= n; k += 16) {
                float partial = 0.0F;
                for (std::size_t j = 0; j < 16; ++j) {
                    const float x = samples[k + j];
                    partial += x * x;
                    peak = std::max(peak, std::fabs(x));
                }
                sum += static_cast<double>(partial);
            }
            for (; k < n; ++k) {
                const float x = samples[k];
                sum += static_cast<double>(x * x);
                peak = std::max(peak, std::fabs(x));
            }
            window_peak[o] = peak;
            window_sum[o] += sum;
            stream_sum[o] += sum;
        }
        window_frames += static_cast<std::uint32_t>(n);
        stream_frames += n;
        if (window_frames >= kLevelWindow) {
            publish_levels();
        }
    }

    void publish_levels() {
        const std::lock_guard lock(status_mutex);
        BurstPlayerStatus& s = status;
        s.outputs = outputs_open;
        for (std::size_t o = 0; o < outputs_open; ++o) {
            s.peak_db[o] = db_of(window_peak[o]);
            s.rms_db[o] = db_of(static_cast<float>(std::sqrt(window_sum[o] / std::max<double>(window_frames, 1.0))));
            s.stream_rms[o] = stream_rms(o);
        }
        s.have_levels = true;
        ++s.levels_serial;
        window_peak.fill(0.0F);
        window_sum.fill(0.0);
        window_frames = 0;
    }

    void decode_unit(std::span<const std::byte> unit) {
        const auto header = ac3::io::read_frame_header(unit);
        if (!header) {
            restart_after_error();
            return;
        }
        if (header->kind == ac3::io::StreamKind::kEac3) {
            if (!programme) {
                programme = header->substreamid;
            } else if (header->substreamid != *programme) {
                return;
            }
        }
        if (!serving.fold) {
            const std::optional<ac3::eac3::chanmap::Layout> bed = peek_unit_layout(unit);
            if (!bed) {
                restart_after_error();
                return;
            }
            push_bed(*bed);
        }
        const auto deliver_block = [this](const ac3::PcmBlock& block) { deliver(block); };
        if (header->kind == ac3::io::StreamKind::kAc3 && header->bytes == unit.size()) {
            if (!ac3_decoder) {
                ac3_decoder.emplace(decoder_config);
            }
            const auto decoded = ac3_decoder->decode_frame_by_block(unit, deliver_block);
            if (!decoded) {
                restart_after_error();
                return;
            }
            if (!have_decoder_report) {
                have_decoder_report = true;
                decoder_report = ac::DecoderReport{.data_type = data_type,
                                                   .acmod = static_cast<std::int32_t>(decoded->acmod),
                                                   .lfe = decoded->lfe,
                                                   .substreams = 1,
                                                   .objects = 0,
                                                   .objects_placed = false,
                                                   .dialnorm = -static_cast<double>(decoded->dialnorm)};
            }
            return;
        }
        if (!eac3_decoder) {
            eac3_decoder.emplace(decoder_config);
        }
        const auto decoded = eac3_decoder->decode_access_unit_by_block(unit, deliver_block);
        if (!decoded) {
            restart_after_error();
            return;
        }
        // The report comes from the first unit that decodes, and again from
        // the first to carry objects if that one did not, as the test sink's
        // does.
        if (!*decoded || (have_decoder_report && (decoder_report.objects > 0 || !(*decoded)->object_metadata))) {
            return;
        }
        const ac3::DecodedAccessUnit& au = **decoded;
        const std::int32_t objects =
            au.object_metadata ? static_cast<std::int32_t>(ac3::oba::describe_objects(*au.object_metadata).size()) : 0;
        have_decoder_report = true;
        decoder_report = ac::DecoderReport{.data_type = data_type,
                                           .acmod = static_cast<std::int32_t>(au.acmod),
                                           .lfe = au.layout.index_of(Location::kLfe) >= 0,
                                           .substreams = au.substream_count,
                                           .objects = objects,
                                           .objects_placed = objects > 0 && serving.reconstruct,
                                           .dialnorm = -static_cast<double>(au.dialnorm)};
    }

    // A unit that does not decode: the decoders start again, and the next
    // burst is the start of a stream.
    void restart_after_error() {
        invalid_chunks.fetch_add(1);
        reset_decoding();
    }

    void play_burst(const Header& header, std::span<const std::uint8_t> payload) {
        if (stream != Stream::kBursts) {
            return;
        }
        if (static_cast<ss::BurstDataType>(header.pc & 0x1FU) != ac::burst_data_type(data_type)) {
            invalid_chunks.fetch_add(1);
            return;
        }
        if (!have_origin) {
            have_origin = true;
            origin_server_us = header.server_us;
        }
        const std::uint64_t frame = frame_of(header.server_us);
        const std::int64_t local_us = local_of(header);
        // Too late to play any of it: dropped before it is decoded, and the
        // decoder starts again with the next.
        if (const std::optional<std::int64_t> next = playout->next_play_us(esp_timer_get_time())) {
            const std::int64_t end = local_us + static_cast<std::int64_t>((kBurstFrames * 1'000'000) / config.sample_rate);
            if (end < *next) {
                ++late_chunks;
                reset_decoding();
                return;
            }
        }
        if (!decoded_frame_set) {
            decoded_frame_set = true;
            decoded_frame = frame;
        }
        mark(frame, local_us);
        ++bursts_played;
        const std::int64_t started = esp_timer_get_time();
        const bool whole = for_each_access_unit(std::as_bytes(payload),
                                                [this](std::span<const std::byte> unit) { decode_unit(unit); });
        if (!whole) {
            restart_after_error();
        }
        count_time(started);
    }

    // A chunk's time, from `started` to now: the sink's part and the rest.
    // A progress line follows every config.report_every_chunks chunks.
    void count_time(std::int64_t started) {
        const std::uint64_t spent = static_cast<std::uint64_t>(esp_timer_get_time() - started);
        const std::uint64_t in_sink = timed.take_spent();
        const std::uint64_t own = spent > in_sink ? spent - in_sink : 0;
        decode_us_total += own;
        sink_us_total += in_sink;
        ++timed_bursts;
        worst_burst_us = std::max(worst_burst_us, static_cast<std::uint32_t>(own));
        if (config.report_every_chunks > 0 && timed_bursts % config.report_every_chunks == 0) {
            report_progress();
        }
    }

    // What the closing line will say, so far, with the internal heap free
    // now, its largest block and the least since the stream began: the
    // network's receive buffers come from there, and a stream that runs it
    // out loses packets.
    void report_progress() const {
        const Playout::Stats& p = playout->stats();
        std::printf("sendspin.progress chunks=%llu late=%llu underruns=%llu resyncs=%u burst_us=%llu "
                    "worst_burst_us=%lu sink_us=%llu ring_high=%lu heap_free=%lu heap_largest=%lu heap_least=%lu\n",
                    static_cast<unsigned long long>(bursts_played), static_cast<unsigned long long>(late_chunks),
                    static_cast<unsigned long long>(p.underruns), static_cast<unsigned>(p.resyncs),
                    static_cast<unsigned long long>(decode_us_total / timed_bursts),
                    static_cast<unsigned long>(worst_burst_us),
                    static_cast<unsigned long long>(sink_us_total / timed_bursts), static_cast<unsigned long>(ring_high),
                    static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                    static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                    static_cast<unsigned long>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    }

    void play_pcm(const Header& header, std::span<const std::uint8_t> payload) {
        if (stream != Stream::kPcm) {
            return;
        }
        const std::size_t channels = static_cast<std::size_t>(std::clamp<std::int32_t>(pcm_format.channels, 1, 2));
        const std::size_t width = pcm_format.bit_depth == 24 ? 3 : 2;
        const std::size_t frame_bytes = channels * width;
        if (payload.size() % frame_bytes != 0) {
            invalid_chunks.fetch_add(1);
            return;
        }
        if (!have_origin) {
            have_origin = true;
            origin_server_us = header.server_us;
        }
        const std::uint64_t first = frame_of(header.server_us);
        mark(first, local_of(header));
        decoded_frame = first;
        ++bursts_played;
        const std::size_t frames = payload.size() / frame_bytes;
        const ac3::eac3::chanmap::Layout bed =
            ac3::eac3::chanmap::expand(ac3::eac3::chanmap::acmod_map(channels == 1 ? ac3::Acmod::k1_0 : ac3::Acmod::k2_0, false));
        if (!serving.fold && (!renderer_bed || !same_layout(*renderer_bed, bed))) {
            renderer.set_bed(bed);
            renderer_bed = bed;
        }
        const std::int64_t started = esp_timer_get_time();
        std::size_t done = 0;
        while (done < frames) {
            const std::size_t n = std::min(kBlock, frames - done);
            const std::uint8_t* bytes = payload.data() + (done * frame_bytes);
            for (std::size_t k = 0; k < n; ++k) {
                for (std::size_t c = 0; c < channels; ++c) {
                    const std::uint8_t* at = bytes + (k * frame_bytes) + (c * width);
                    float value = 0.0F;
                    if (width == 2) {
                        const auto sample = static_cast<std::int16_t>(static_cast<std::uint16_t>(at[0] | (at[1] << 8U)));
                        value = static_cast<float>(sample) / 32768.0F;
                    } else {
                        std::int32_t sample = static_cast<std::int32_t>(at[0] | (at[1] << 8U) | (at[2] << 16U));
                        if ((sample & 0x800000) != 0) {
                            sample -= 0x1000000;
                        }
                        value = static_cast<float>(sample) / 8388608.0F;
                    }
                    pcm_channels[c][k] = value;
                }
            }
            std::size_t used = channels;
            if (serving.fold && channels == 2 && layout.speaker_count() == 1) {
                // A one-speaker room hears both channels.
                for (std::size_t k = 0; k < n; ++k) {
                    pcm_channels[0][k] = 0.5F * (pcm_channels[0][k] + pcm_channels[1][k]);
                }
                used = 1;
            } else if (serving.fold && channels == 1 && layout.speaker_count() == 2) {
                // And a two-speaker room hears one on both.
                std::copy_n(pcm_channels[0].begin(), n, pcm_channels[1].begin());
                used = 2;
            }
            for (std::size_t c = 0; c < used; ++c) {
                pcm_views[c] = std::span<const float>(pcm_channels[c].data(), n);
            }
            const ac3::PcmBlock block{.index = 0,
                                      .blocks = 1,
                                      .channels = std::span<const std::span<const float>>(pcm_views.data(), used),
                                      .objects = {},
                                      .object_indices = {},
                                      .object_metadata = nullptr};
            const std::span<const std::span<float>> out(rendered.data(), layout.slots());
            if (serving.fold) {
                renderer.render_folded(block, 1.0F, out);
            } else {
                renderer.render(block, false, 1.0F, out);
            }
            emit(n);
            done += n;
        }
        count_time(started);
    }

    // The identify tone with nothing playing: blocks of it straight to the
    // sink, paced by the sink.
    void identify_idle() {
        const std::span<const std::span<float>> out(outputs.data(), outputs_open);
        const std::size_t output =
            identifying->output >= 0 ? static_cast<std::size_t>(identifying->output) : outputs_open;
        tone.fill(out, output, carries_lfe(output) ? IdentifyTone::Band::kLow : IdentifyTone::Band::kFull);
        apply_gain(out, kBlock);
        meter(out, kBlock);
        for (std::size_t o = 0; o < outputs_open; ++o) {
            output_views[o] = outputs[o];
        }
        (void)sink.write(std::span<const std::span<const float>>(output_views.data(), outputs_open));
    }

    void publish_status() {
        const std::lock_guard lock(status_mutex);
        BurstPlayerStatus& s = status;
        s.stream = stream == Stream::kBursts ? "bursts" : stream == Stream::kPcm ? "pcm" : "idle";
        s.have_decoder = have_decoder_report;
        s.decoder = decoder_report;
        s.outputs = outputs_open;
        s.counters = ac::Counters{.bursts_played = bursts_played,
                                  .underruns = playout->stats().underruns,
                                  .late_chunks = late_chunks,
                                  .dropped_chunks = dropped_chunks.load(),
                                  .invalid_chunks = invalid_chunks.load()};
        s.playout = playout->stats();
        if (s.playout.have_last) {
            s.have_play = true;
            s.play_frame = s.playout.last_frame;
            s.play_local_us = s.playout.last_play_us;
        } else {
            s.have_play = false;
        }
        s.burst_us = static_cast<std::uint32_t>(timed_bursts > 0 ? decode_us_total / timed_bursts : 0);
        s.worst_burst_us = worst_burst_us;
        s.stack_free = static_cast<std::size_t>(uxTaskGetStackHighWaterMark(nullptr));
        {
            const std::lock_guard settings_lock(settings_mutex);
            s.settings_revision = settings_revision;
        }
        const std::string_view text = layout.text();
        const std::size_t length = std::min(text.size(), s.layout.size() - 1);
        std::memcpy(s.layout.data(), text.data(), length);
        s.layout[length] = '\0';
        s.identifying = identifying.has_value();
        if (stream == Stream::kIdle) {
            s.have_levels = false;
        }
    }

    // Something else has the sink: nothing is written until it is given back,
    // and then the sink is opened again the player's own way.
    void wait_while_held() {
        playout->restart();
        reset_decoding();
        have_origin = false;
        xEventGroupSetBits(events, kHeld);
        while (held.load() && !stopping.load()) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        xEventGroupClearBits(events, kHeld);
        open_outputs(true);
    }

    void run() {
        std::int64_t last_status = 0;
        while (!stopping.load()) {
            if (held.load()) {
                wait_while_held();
                continue;
            }
            if (stream == Stream::kIdle) {
                apply_pending();
            }
            const TickType_t wait = identifying && stream == Stream::kIdle ? 0 : pdMS_TO_TICKS(50);
            const std::size_t free_now = xMessageBufferSpacesAvailable(ring);
            const std::size_t held = ring_free_empty > free_now ? ring_free_empty - free_now : 0;
            const std::size_t got = xMessageBufferReceive(ring, receive_buffer, entry_bytes, wait);
            if (stream != Stream::kIdle) {
                ring_high = std::max(ring_high, held);
            }
            const std::int64_t now = esp_timer_get_time();
            if (now - last_status >= 100'000) {
                last_status = now;
                publish_status();
            }
            if (got < sizeof(Header)) {
                if (identifying && stream == Stream::kIdle) {
                    identify_idle();
                }
                continue;
            }
            Header header;
            std::memcpy(&header, receive_buffer, sizeof(Header));
            const std::span<const std::uint8_t> payload(receive_buffer + sizeof(Header), got - sizeof(Header));
            const bool stale = header.generation != generation.load();
            switch (header.kind) {
                case Kind::kStartBursts:
                case Kind::kStartPcm: {
                    const Stream kind = header.kind == Kind::kStartBursts ? Stream::kBursts : Stream::kPcm;
                    const ac::DataType type = static_cast<ac::DataType>(header.data_type);
                    const m::AudioFormat format{.codec = m::Codec::kPcm,
                                                .channels = header.channels,
                                                .sample_rate = header.sample_rate,
                                                .bit_depth = header.bit_depth};
                    // A start for the stream already running changes it in
                    // place: what is buffered still plays.
                    const bool in_place = stream == kind &&
                                          (kind == Stream::kBursts ? type == data_type : format == pcm_format);
                    apply_pending();
                    if (!in_place) {
                        if (stream != Stream::kIdle) {
                            report_stream_end();
                        }
                        data_type = type;
                        pcm_format = format;
                        new_stream(kind);
                    } else if (kind == Stream::kBursts && type != data_type) {
                        data_type = type;
                        reset_decoding();
                    }
                    break;
                }
                case Kind::kClear:
                    // The sink stays as it is: the stream this clears is the
                    // one it was opened for.
                    reset_decoding();
                    playout->restart();
                    have_origin = false;
                    break;
                case Kind::kEnd:
                    end_stream();
                    apply_pending();
                    break;
                case Kind::kBurst:
                    if (!stale) {
                        apply_pending();
                        play_burst(header, payload);
                    }
                    break;
                case Kind::kPcm:
                    if (!stale) {
                        apply_pending();
                        play_pcm(header, payload);
                    }
                    break;
            }
        }
        xEventGroupSetBits(events, kExited);
        vTaskDelete(nullptr);
    }

    static void entry(void* self) { static_cast<Impl*>(self)->run(); }

    void release() {
        if (ring != nullptr) {
            vMessageBufferDelete(ring);
            ring = nullptr;
        }
        heap_caps_free(ring_struct);
        heap_caps_free(ring_storage);
        heap_caps_free(send_buffer);
        heap_caps_free(receive_buffer);
        heap_caps_free(floats);
        ring_struct = nullptr;
        ring_storage = nullptr;
        send_buffer = nullptr;
        receive_buffer = nullptr;
        floats = nullptr;
        playout.reset();
        if (events != nullptr) {
            vEventGroupDelete(events);
            events = nullptr;
        }
    }
};

BurstPlayer::BurstPlayer(const BurstPlayerConfig& config, ScheduledSink& sink)
    : impl_(std::make_unique<Impl>(config, sink)) {}

BurstPlayer::~BurstPlayer() { stop(); }

bool BurstPlayer::start() {
    Impl& im = *impl_;
    if (im.task != nullptr) {
        return true;
    }
    im.config.max_outputs = std::clamp<std::size_t>(im.config.max_outputs, 1, Playout::kMaxOutputs);
    const std::uint32_t caps = bulk_caps();
    im.entry_bytes = sizeof(Header) + im.config.max_chunk_bytes;
    im.ring_struct = static_cast<StaticMessageBuffer_t*>(heap_caps_malloc(sizeof(StaticMessageBuffer_t), caps));
    im.ring_storage = static_cast<std::uint8_t*>(heap_caps_malloc(im.config.ring_bytes, caps));
    im.send_buffer = static_cast<std::uint8_t*>(heap_caps_malloc(im.entry_bytes, caps));
    im.receive_buffer = static_cast<std::uint8_t*>(heap_caps_malloc(im.entry_bytes, caps));
    im.max_delay_samples = TrimDelay::samples_for_ms(im.config.max_delay_ms, im.config.sample_rate);
    const std::size_t outputs = im.config.max_outputs;
    const std::size_t float_count = (kMaxSlots * kBlock) + (outputs * kBlock) + (outputs * kBlock) + (2 * kBlock) +
                                    TrimDelay::storage_floats(outputs, im.max_delay_samples);
    im.floats = static_cast<float*>(heap_caps_calloc(float_count, sizeof(float), caps));
    im.events = xEventGroupCreate();
    if (im.ring_struct == nullptr || im.ring_storage == nullptr || im.send_buffer == nullptr ||
        im.receive_buffer == nullptr || im.floats == nullptr || im.events == nullptr) {
        std::printf("burst player: no memory for its buffers (%u floats, a %u-byte ring)\n",
                    static_cast<unsigned>(float_count), static_cast<unsigned>(im.config.ring_bytes));
        im.release();
        return false;
    }
    im.ring = xMessageBufferCreateStatic(im.config.ring_bytes, im.ring_storage, im.ring_struct);
    if (im.ring == nullptr) {
        std::printf("burst player: could not create the ring\n");
        im.release();
        return false;
    }
    im.ring_free_empty = xMessageBufferSpacesAvailable(im.ring);
    std::span<float> all(im.floats, float_count);
    std::size_t at = 0;
    for (std::size_t s = 0; s < kMaxSlots; ++s, at += kBlock) {
        im.rendered[s] = all.subspan(at, kBlock);
    }
    for (std::size_t o = 0; o < outputs; ++o, at += kBlock) {
        im.outputs[o] = all.subspan(at, kBlock);
    }
    const std::span<float> staging = all.subspan(at, outputs * kBlock);
    at += outputs * kBlock;
    for (std::size_t c = 0; c < 2; ++c, at += kBlock) {
        im.pcm_channels[c] = all.subspan(at, kBlock);
    }
    im.delay_storage = all.subspan(at);
    im.playout.emplace(staging, outputs, im.config.tuning);

    im.configure_defaults(im.config.layout);
    std::printf("burst player: ring %u bytes and buffers in %s, layout %s, %u outputs, delay up to %u samples\n",
                static_cast<unsigned>(im.config.ring_bytes),
                (caps & MALLOC_CAP_SPIRAM) != 0 ? "PSRAM" : "internal SRAM", im.layout.text().data(),
                static_cast<unsigned>(im.outputs_open), static_cast<unsigned>(im.max_delay_samples));

    im.stopping.store(false);
    if (xTaskCreatePinnedToCore(&Impl::entry, "sendspin-play", im.config.stack_bytes, &im, im.config.priority,
                                &im.task, im.config.core) != pdPASS) {
        std::printf("burst player: could not start its task\n");
        im.task = nullptr;
        im.release();
        return false;
    }
    return true;
}

void BurstPlayer::stop() {
    Impl& im = *impl_;
    if (im.task == nullptr) {
        return;
    }
    im.stopping.store(true);
    const EventBits_t bits = xEventGroupWaitBits(im.events, kExited, pdFALSE, pdTRUE, pdMS_TO_TICKS(5000));
    if ((bits & kExited) == 0) {
        std::printf("burst player: its task did not stop; leaving it\n");
        return;
    }
    im.task = nullptr;
    im.release();
}

std::size_t BurstPlayer::buffer_capacity() const {
    const std::size_t ring = impl_->config.ring_bytes;
    return ring > kControlReserve * 2 ? ring - (kControlReserve * 2) : 0;
}

void BurstPlayer::start_bursts(const ac::StreamStart& stream) {
    Header header;
    header.kind = Kind::kStartBursts;
    header.data_type = static_cast<std::uint8_t>(stream.data_type);
    header.sample_rate = stream.sample_rate;
    header.generation = impl_->generation.load();
    impl_->send(header, {}, true);
}

void BurstPlayer::start_pcm(const m::AudioFormat& format) {
    Header header;
    header.kind = Kind::kStartPcm;
    header.sample_rate = format.sample_rate;
    header.channels = format.channels;
    header.bit_depth = format.bit_depth;
    header.generation = impl_->generation.load();
    impl_->send(header, {}, true);
}

void BurstPlayer::clear() {
    Header header;
    header.kind = Kind::kClear;
    // Whatever the ring holds from before this is dropped as it comes out.
    header.generation = impl_->generation.fetch_add(1) + 1;
    impl_->send(header, {}, true);
}

void BurstPlayer::end() {
    Header header;
    header.kind = Kind::kEnd;
    header.generation = impl_->generation.fetch_add(1) + 1;
    impl_->send(header, {}, true);
}

void BurstPlayer::burst(const ss::BurstChunk& chunk, std::int64_t local_us) {
    Header header;
    header.kind = Kind::kBurst;
    header.pc = chunk.pc;
    header.pd = chunk.pd;
    header.generation = impl_->generation.load();
    header.local_us = local_us;
    header.server_us = chunk.chunk.timestamp_us;
    header.map_local_us = impl_->map_now(header.server_us);
    impl_->send(header, chunk.chunk.data, false);
}

void BurstPlayer::pcm(std::span<const std::uint8_t> frame, std::int64_t server_us, std::int64_t local_us) {
    Header header;
    header.kind = Kind::kPcm;
    header.generation = impl_->generation.load();
    header.local_us = local_us;
    header.server_us = server_us;
    header.map_local_us = impl_->map_now(server_us);
    impl_->send(header, frame, false);
}

void BurstPlayer::invalid_chunk() { impl_->invalid_chunks.fetch_add(1); }

std::optional<std::string> BurstPlayer::settings(const ac::Settings& settings, const ac::Support& support) {
    if (std::optional<std::string> why = ac::check_settings(settings, support)) {
        return why;
    }
    if (settings.layout) {
        const std::optional<OutputLayout> parsed = OutputLayout::parse(*settings.layout);
        if (!parsed) {
            return std::string("the layout is not one this board reads: ") + *settings.layout;
        }
        if (!settings.routing && parsed->slots() > impl_->sink.max_outputs()) {
            return std::string("the layout has more speakers than this board has outputs");
        }
    }
    if (settings.routing && !Routing::parse(*settings.routing, std::min(impl_->config.max_outputs,
                                                                        static_cast<std::size_t>(support.outputs.count)))) {
        return std::string("the routing is not one this board can patch: ") + *settings.routing;
    }
    if (settings.delay_ms) {
        for (const double ms : *settings.delay_ms) {
            if (TrimDelay::samples_for_ms(ms, impl_->config.sample_rate) > impl_->max_delay_samples) {
                return std::string("a delay is longer than this board holds");
            }
        }
    }
    const std::lock_guard lock(impl_->settings_mutex);
    impl_->pending_settings = settings;
    impl_->pending_layout.reset();
    impl_->have_server_settings = true;
    return std::nullopt;
}

bool BurstPlayer::set_layout(const OutputLayout& layout) {
    if (layout.slots() > impl_->sink.max_outputs()) {
        return false;
    }
    const std::lock_guard lock(impl_->settings_mutex);
    impl_->config.layout = layout;
    impl_->pending_layout = layout;
    return true;
}

void BurstPlayer::identify(std::optional<ac::Identify> tone) {
    const std::lock_guard lock(impl_->settings_mutex);
    impl_->pending_identify = tone;
    impl_->identify_changed = true;
}

void BurstPlayer::set_volume(int volume, bool muted) {
    const float linear = static_cast<float>(std::clamp(volume, 0, 100)) / 100.0F;
    impl_->target_gain.store(muted ? 0.0F : linear * std::sqrt(linear));
}

bool BurstPlayer::hold(bool held) {
    Impl& im = *impl_;
    // Chunks from before a hold, and from during it, are not played after it.
    im.generation.fetch_add(1);
    im.held.store(held);
    if (im.task == nullptr || im.events == nullptr) {
        return true;
    }
    if (!held) {
        return true;
    }
    const EventBits_t bits = xEventGroupWaitBits(im.events, kHeld, pdFALSE, pdTRUE, pdMS_TO_TICKS(2000));
    return (bits & kHeld) != 0;
}

BurstPlayerStatus BurstPlayer::status() const {
    const std::lock_guard lock(impl_->status_mutex);
    return impl_->status;
}

bool BurstPlayer::active() const { return impl_->active.load(); }

}  // namespace ac3forge
