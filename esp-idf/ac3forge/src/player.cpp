// The player's two tasks and the ring between them. See
// ../include/ac3forge/player.hpp for what this is and why it is here.

#include "ac3forge/player.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <expected>
#include <optional>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "ac3/io/elementary.hpp"
#include "ac3/io/stream_accumulator.hpp"

namespace ac3forge {
namespace {

// Eight, not §E3.8.2's cap of sixteen and not the six a 5.1 stream needs: the
// decoder asserts a span for every channel the programme renders, so this is
// the widest layout the player accepts - 7.1 - and provisioning for wider would
// put 6 KB per channel behind a stream nothing here plays. The same reasoning,
// and the same number, as apps/baremetal/probe.cpp.
constexpr std::size_t kMaxChannels = 8;

// One event group, shared by the two tasks and the caller. Bits are set and
// cleared by name below; nothing waits with clear-on-exit, so a bit meant for
// one task is never swallowed by another.
constexpr EventBits_t kSourceEnded = BIT0;    // fetch: read() returned 0 and everything is in the ring
constexpr EventBits_t kRewindRequest = BIT1;  // decode -> fetch: the ring is drained, go back
constexpr EventBits_t kRewound = BIT2;        // fetch -> decode: rewind() succeeded
constexpr EventBits_t kRewindFailed = BIT3;   // fetch -> decode: it could not
constexpr EventBits_t kStop = BIT4;           // caller -> both
constexpr EventBits_t kFetchExited = BIT5;
constexpr EventBits_t kDecodeExited = BIT6;
constexpr EventBits_t kFinished = BIT7;       // decode -> caller

}  // namespace

struct Player::Impl {
    Impl(const PlayerConfig& cfg, ByteSource& src, PcmSink& snk)
        : config(cfg), source(src), sink(snk) {}

    PlayerConfig config;
    ByteSource& source;
    PcmSink& sink;

    EventGroupHandle_t events = nullptr;
    StreamBufferHandle_t ring = nullptr;
    bool ring_in_psram = false;
    TaskHandle_t fetch_task = nullptr;
    TaskHandle_t decode_task = nullptr;

    // Caller-owned storage the decoders write through, the shape an embedded
    // integrator has: one block, sized once, reused every frame. The fold
    // happens IN this storage, so it is as wide as the coded programme, not as
    // wide as the output.
    std::array<std::array<float, ac3::kSamplesPerFrame>, kMaxChannels> pcm{};
    std::array<std::span<float>, kMaxChannels> pcm_spans{};
    std::array<std::span<const float>, kMaxChannels> views{};
    // The framer's buffer: 16 KB holds an independent substream plus three
    // dependents, which covers Atmos.
    alignas(4) std::array<std::byte, ac3::io::kRecommendedBuffer> framing{};
    // The fetch task's read block.
    std::vector<std::byte> staging;

    // Two decoders, constructed on the first access unit that needs each -
    // see the header on why a unit that is one AC-3 syncframe cannot go
    // through Eac3Decoder under a fold.
    std::optional<ac3::FrameDecoder> ac3_decoder;
    std::optional<ac3::Eac3Decoder> eac3_decoder;

    // Written by the tasks, read by anyone.
    std::atomic<std::uint64_t> frames_played{0};
    std::atomic<std::uint64_t> frames_held{0};
    std::atomic<std::uint64_t> decode_us{0};
    std::atomic<std::uint64_t> worst_frame_us{0};
    std::atomic<std::uint64_t> fetched_bytes{0};
    std::atomic<std::uint64_t> resync_bytes{0};
    std::atomic<std::uint32_t> passes{0};
    std::atomic<std::size_t> ring_low_water{SIZE_MAX};
    std::atomic<bool> finished{false};
    std::atomic<bool> failed{false};
    std::atomic<bool> have_stream{false};
    std::atomic<const char*> failure{""};
    std::atomic<int> error{0};
    std::atomic<float> volume{1.0F};
    std::atomic<std::size_t> decode_stack_free{0};
    StreamInfo stream{};  // written once, before have_stream is set

    // Sampled from the decode task only: the high-water mark is that task's.
    void sample_decode_stack() {
        decode_stack_free.store(static_cast<std::size_t>(uxTaskGetStackHighWaterMark(nullptr)));
    }

    // The figures as they stood when the last pass completed, copied by the
    // decode task at that moment and read by whoever reports it. A spinlock
    // rather than atomics because the copy has to be of one moment.
    portMUX_TYPE snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
    PlayerStats pass_snapshot{};

    [[nodiscard]] PlayerStats snapshot() const {
        PlayerStats s;
        s.frames_played = frames_played.load();
        s.frames_held = frames_held.load();
        s.decode_us = decode_us.load();
        s.worst_frame_us = worst_frame_us.load();
        s.fetched_bytes = fetched_bytes.load();
        s.resync_bytes = resync_bytes.load();
        s.passes = passes.load();
        const std::size_t low = ring_low_water.load();
        s.ring_low_valid = low != SIZE_MAX;
        s.ring_low_water = s.ring_low_valid ? low : 0;
        s.decode_stack_free = decode_stack_free.load();
        s.finished = finished.load();
        s.failed = failed.load();
        s.failure = failure.load();
        s.error = error.load();
        return s;
    }

    static void fetch_entry(void* self) { static_cast<Impl*>(self)->fetch_loop(); }
    static void decode_entry(void* self) { static_cast<Impl*>(self)->decode_loop(); }

    [[nodiscard]] bool stopping() const { return (xEventGroupGetBits(events) & kStop) != 0; }

    void finish(const char* why, bool is_failure, int code) {
        sample_decode_stack();
        failure.store(why);
        error.store(code);
        if (is_failure) {
            failed.store(true);
        }
        finished.store(true);
        // Stopping the fetch task too: a finished player has no more use for
        // bytes, and a source blocked in read() would otherwise sit there.
        xEventGroupSetBits(events, kFinished | kStop);
    }

    // --- the fetch task ------------------------------------------------------
    void fetch_loop() {
        for (;;) {
            if (stopping()) {
                break;
            }
            const std::size_t got = source.read(staging);
            if (got == 0) {
                // Not "wait" - there will never be more. Tell the decoder, then
                // wait to be asked back to the start, or to stop.
                xEventGroupSetBits(events, kSourceEnded);
                const EventBits_t bits = xEventGroupWaitBits(events, kRewindRequest | kStop, pdFALSE,
                                                             pdFALSE, portMAX_DELAY);
                if ((bits & kStop) != 0) {
                    break;
                }
                xEventGroupClearBits(events, kRewindRequest);
                if (source.rewind()) {
                    xEventGroupClearBits(events, kSourceEnded);
                    xEventGroupSetBits(events, kRewound);
                    continue;
                }
                xEventGroupSetBits(events, kRewindFailed);
                break;
            }
            fetched_bytes.fetch_add(got);
            // Into the ring, in pieces when it is full. A full ring is the
            // source being ahead of the DAC, which is what it is for; the wait
            // is bounded so a stop request is seen.
            std::size_t sent = 0;
            while (sent < got && !stopping()) {
                sent += xStreamBufferSend(ring, staging.data() + sent, got - sent,
                                          pdMS_TO_TICKS(100));
            }
        }
        xEventGroupSetBits(events, kFetchExited);
        vTaskDelete(nullptr);
    }

    // --- the decode task -----------------------------------------------------
    // True when the unit produced audio, false when the decoder held it back
    // (§3.7's transient pre-noise processing releases each frame one call late).
    std::expected<bool, ac3::DecodeError> decode_unit(std::span<const std::byte> unit) {
        const auto header = ac3::io::read_frame_header(unit);
        const bool one_ac3_syncframe = header.has_value() &&
                                       header->kind == ac3::io::StreamKind::kAc3 &&
                                       header->bytes == unit.size();
        if (one_ac3_syncframe) {
            if (!ac3_decoder.has_value()) {
                ac3_decoder.emplace(config.decoder);
            }
            const auto decoded = ac3_decoder->decode_frame_into(unit, pcm_spans);
            if (!decoded) {
                return std::unexpected(decoded.error());
            }
            if (!have_stream.load()) {
                stream = {.eac3 = false,
                          .acmod = static_cast<int>(decoded->acmod),
                          .channels = header->coded_channels(),
                          .substreams = 1,
                          .dialnorm = decoded->dialnorm,
                          .objects = false};
                have_stream.store(true);
            }
            return true;
        }
        if (!eac3_decoder.has_value()) {
            eac3_decoder.emplace(config.decoder);
        }
        const auto decoded = eac3_decoder->decode_access_unit_into(unit, pcm_spans);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        if (!decoded->has_value()) {
            return false;
        }
        if (!have_stream.load()) {
            const auto& au = **decoded;
            stream = {.eac3 = true,
                      .acmod = static_cast<int>(au.acmod),
                      .channels = au.layout.count,
                      .substreams = au.substream_count,
                      .dialnorm = au.dialnorm,
                      .objects = au.object_metadata.has_value()};
            have_stream.store(true);
        }
        return true;
    }

    void decode_loop() {
        using Status = ac3::io::AccessUnitAccumulator::Status;
        ac3::io::AccessUnitAccumulator accumulator{framing};
        // resynchronised_bytes() is per accumulator; the running total
        // survives the re-arm at each pass.
        std::uint64_t resync_before = 0;

        while (!stopping()) {
            const auto unit = accumulator.next();

            if (unit.status == Status::kNeedMoreInput) {
                const auto dst = accumulator.writable();
                // How much the fetch task had banked as the decoder came for
                // more. Not during the first frame, when the ring is still
                // filling, and not after the source has ended, when the ring
                // drains to nothing by design: neither zero says anything about
                // the buffering, and the figure is only worth having if it does.
                if (frames_played.load() > 0 &&
                    (xEventGroupGetBits(events) & kSourceEnded) == 0) {
                    const std::size_t banked = xStreamBufferBytesAvailable(ring);
                    std::size_t low = ring_low_water.load();
                    while (banked < low && !ring_low_water.compare_exchange_weak(low, banked)) {
                    }
                }
                const std::size_t got =
                    xStreamBufferReceive(ring, dst.data(), dst.size(), pdMS_TO_TICKS(100));
                if (got > 0) {
                    accumulator.commit(got);
                    continue;
                }
                if ((xEventGroupGetBits(events) & kSourceEnded) != 0 &&
                    xStreamBufferIsEmpty(ring) == pdTRUE) {
                    // finish() is what closes the last access unit, whose end
                    // is otherwise only found by reading the start of a
                    // successor that is not coming.
                    accumulator.finish();
                }
                continue;
            }

            if (unit.status == Status::kEndOfStream) {
                resync_bytes.store(resync_before + accumulator.resynchronised_bytes());
                sample_decode_stack();
                const std::uint32_t done = passes.fetch_add(1) + 1;
                {
                    PlayerStats snap = snapshot();
                    snap.passes = done;
                    taskENTER_CRITICAL(&snapshot_lock);
                    pass_snapshot = snap;
                    taskEXIT_CRITICAL(&snapshot_lock);
                }
                if (config.max_passes != 0 && done >= config.max_passes) {
                    finish("passes", false, 0);
                    break;
                }
                // Back to the start, if the source can. The ring is empty here
                // by construction: finish() only ran with the ring drained and
                // the source ended.
                xEventGroupClearBits(events, kRewound | kRewindFailed);
                xEventGroupSetBits(events, kRewindRequest);
                const EventBits_t bits = xEventGroupWaitBits(
                    events, kRewound | kRewindFailed | kStop, pdFALSE, pdFALSE, portMAX_DELAY);
                if ((bits & kRewound) == 0) {
                    finish("end of stream", false, 0);
                    break;
                }
                resync_before += accumulator.resynchronised_bytes();
                accumulator = ac3::io::AccessUnitAccumulator{framing};
                continue;
            }

            if (unit.status != Status::kUnit) {
                finish("framing", true, static_cast<int>(accumulator.error()));
                break;
            }

            const std::int64_t started = esp_timer_get_time();
            const auto decoded = decode_unit(unit.bytes);
            const auto elapsed = static_cast<std::uint64_t>(esp_timer_get_time() - started);
            if (!decoded) {
                finish("decode", true, static_cast<int>(decoded.error()));
                break;
            }
            decode_us.fetch_add(elapsed);
            std::uint64_t worst = worst_frame_us.load();
            while (elapsed > worst && !worst_frame_us.compare_exchange_weak(worst, elapsed)) {
            }
            if (!*decoded) {
                frames_held.fetch_add(1);
                continue;
            }
            // The volume, applied in the caller-owned storage before the sink
            // sees it. 3,072 multiplies a stereo frame; nothing at unity.
            const float gain = volume.load();
            if (gain != 1.0F) {
                for (std::size_t ch = 0; ch < config.output_channels; ++ch) {
                    for (float& sample : pcm[ch]) {
                        sample *= gain;
                    }
                }
            }
            for (std::size_t ch = 0; ch < config.output_channels; ++ch) {
                views[ch] = pcm[ch];
            }
            sink.write(std::span<const std::span<const float>>{views.data(), config.output_channels});
            frames_played.fetch_add(1);
            resync_bytes.store(resync_before + accumulator.resynchronised_bytes());
        }
        xEventGroupSetBits(events, kDecodeExited);
        vTaskDelete(nullptr);
    }
};

Player::Player(const PlayerConfig& config, ByteSource& source, PcmSink& sink)
    : impl_(std::make_unique<Impl>(config, source, sink)) {}

Player::~Player() { stop(); }

bool Player::start() {
    auto& im = *impl_;
    if (im.events != nullptr) {
        return true;  // already started
    }
    if (im.config.output_channels == 0 || im.config.output_channels > kMaxChannels) {
        std::printf("player: output_channels must be 1..%u\n", static_cast<unsigned>(kMaxChannels));
        return false;
    }
    for (std::size_t ch = 0; ch < kMaxChannels; ++ch) {
        im.pcm_spans[ch] = std::span<float>(im.pcm[ch]);
    }
    im.staging.resize(im.config.fetch_bytes);
    set_volume(im.config.volume);

    im.events = xEventGroupCreate();
    if (im.events == nullptr) {
        std::printf("player: no memory for the event group\n");
        return false;
    }

    // The ring: PSRAM when asked for and present, internal SRAM otherwise. A
    // trigger level of one byte, so the decoder wakes on whatever arrives.
    if (im.config.ring_in_psram) {
        im.ring = xStreamBufferCreateWithCaps(im.config.ring_bytes, 1,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        im.ring_in_psram = im.ring != nullptr;
    }
    if (im.ring == nullptr) {
        im.ring = xStreamBufferCreateWithCaps(im.config.ring_bytes, 1,
                                              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (im.ring == nullptr) {
        std::printf("player: no memory for a %lu-byte ring\n",
                    static_cast<unsigned long>(im.config.ring_bytes));
        return false;
    }
    std::printf("player: ring %lu bytes in %s, fetch on core %d at priority %u, decode on core %d "
                "at priority %u\n",
                static_cast<unsigned long>(im.config.ring_bytes),
                im.ring_in_psram ? "PSRAM" : "internal SRAM",
                static_cast<int>(im.config.fetch_core), static_cast<unsigned>(im.config.fetch_priority),
                static_cast<int>(im.config.decode_core),
                static_cast<unsigned>(im.config.decode_priority));

    // The decoder first, so the ring never fills before anything can drain it.
    if (xTaskCreatePinnedToCore(&Impl::decode_entry, "ac3-decode", im.config.decode_stack_bytes,
                                &im, im.config.decode_priority, &im.decode_task,
                                im.config.decode_core) != pdPASS) {
        std::printf("player: could not start the decode task\n");
        return false;
    }
    if (xTaskCreatePinnedToCore(&Impl::fetch_entry, "ac3-fetch", im.config.fetch_stack_bytes, &im,
                                im.config.fetch_priority, &im.fetch_task,
                                im.config.fetch_core) != pdPASS) {
        std::printf("player: could not start the fetch task\n");
        xEventGroupSetBits(im.events, kStop);
        return false;
    }
    return true;
}

void Player::stop() {
    auto& im = *impl_;
    if (im.events == nullptr) {
        return;
    }
    xEventGroupSetBits(im.events, kStop);
    // Both tasks check kStop within a bounded wait, except a fetch blocked in
    // the source's own read - a socket with a long timeout - which is the one
    // thing that can make this wait its full length.
    EventBits_t want = 0;
    if (im.fetch_task != nullptr) {
        want |= kFetchExited;
    }
    if (im.decode_task != nullptr) {
        want |= kDecodeExited;
    }
    if (want != 0) {
        const EventBits_t bits =
            xEventGroupWaitBits(im.events, want, pdFALSE, pdTRUE, pdMS_TO_TICKS(15000));
        if ((bits & want) != want) {
            std::printf("player: a task did not exit in 15 s; leaving it\n");
            return;
        }
    }
    im.fetch_task = nullptr;
    im.decode_task = nullptr;
    if (im.ring != nullptr) {
        vStreamBufferDeleteWithCaps(im.ring);
        im.ring = nullptr;
    }
    vEventGroupDelete(im.events);
    im.events = nullptr;
}

PlayerStats Player::stats() const { return impl_->snapshot(); }

PlayerStats Player::last_pass() const {
    auto& im = *impl_;
    taskENTER_CRITICAL(&im.snapshot_lock);
    const PlayerStats s = im.pass_snapshot;
    taskEXIT_CRITICAL(&im.snapshot_lock);
    return s;
}

std::optional<StreamInfo> Player::stream() const {
    if (!impl_->have_stream.load()) {
        return std::nullopt;
    }
    return impl_->stream;
}

bool Player::finished() const { return impl_->finished.load(); }

void Player::set_volume(float volume) {
    impl_->volume.store(volume < 0.0F ? 0.0F : (volume > 1.0F ? 1.0F : volume));
}

float Player::volume() const { return impl_->volume.load(); }

bool Player::wait(TickType_t ticks) {
    if (impl_->events == nullptr) {
        return finished();
    }
    xEventGroupWaitBits(impl_->events, kFinished, pdFALSE, pdFALSE, ticks);
    return finished();
}

}  // namespace ac3forge
