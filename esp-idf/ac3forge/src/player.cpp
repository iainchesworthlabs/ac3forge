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

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/io/stream_accumulator.hpp"
#include "ac3/oba/oamd.hpp"

#include "ac3forge/render.hpp"
#include "ac3forge/unit_hold.hpp"

namespace ac3forge {
namespace {

// One block per slot is what the player holds of the audio: sixteen slots of
// 256 samples, 16 KB, against the 96 KB a frame of them would be. Sixteen is
// §E3.8.2's cap on a rendered programme and OutputLayout's on a layout.
constexpr std::size_t kMaxSlots = OutputLayout::kMaxSlots;

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

// One decoded block as the renderer needs it, wherever it came from: straight
// from the decoder's PcmBlock, or out of the hold on a play's first unit
// (UnitHold). `bed` is the coded layout the channels are in; `objects` and
// `places` are the object signals and, on a unit's first block, what the
// renderer places them by.
constexpr std::size_t kMaxObjects = LayoutRenderer::kMaxObjects;

struct BlockView {
    int index = 0;
    std::span<const std::span<const float>> channels;
    std::span<const std::span<const float>> objects;
    const ac3::eac3::chanmap::Layout* bed = nullptr;
    std::span<const ac3::oba::DisplayObject> places;
};

bool same_layout(const ac3::eac3::chanmap::Layout& a, const ac3::eac3::chanmap::Layout& b) {
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

// The layout an access unit will decode to, from its headers alone: every
// syncframe's acmod and lfeon, a dependent's chanmap where it carries one,
// unioned in Table E2.5 order - which is how the decoder assembles it
// (§E3.8.2), and what its DecodedAccessUnit::layout reports afterwards. Needed
// BEFORE the decode because the block form hands the samples over during the
// call and the layout only after it; the decoded layout is checked against
// this once the call returns, and wins if they differ.
std::optional<ac3::eac3::chanmap::Layout> peek_layout(std::span<const std::byte> unit) {
    std::uint16_t map = 0;
    std::size_t offset = 0;
    while (offset < unit.size()) {
        const auto header = ac3::io::read_frame_header(unit.subspan(offset));
        if (!header || header->bytes == 0) {
            return std::nullopt;
        }
        const std::uint16_t own = ac3::eac3::chanmap::acmod_map(header->acmod, header->lfe);
        if (header->kind == ac3::io::StreamKind::kEac3 &&
            header->strmtyp == ac3::eac3::StreamType::kDependent) {
            map |= header->chanmap.value_or(own);
        } else {
            map |= own;
        }
        offset += header->bytes;
    }
    if (map == 0) {
        return std::nullopt;
    }
    return ac3::eac3::chanmap::expand(map);
}

}  // namespace

struct Player::Impl {
    Impl(const PlayerConfig& cfg, ByteSource& src, PcmSink& snk)
        : config(cfg), source(src), sink(snk), renderer(cfg.layout) {}

    PlayerConfig config;
    ByteSource& source;
    PcmSink& sink;
    LayoutRenderer renderer;

    // Decided at start() from the layout: the decoder's own §7.8 fold for a
    // stereo or mono layout, the renderer for everything else.
    std::optional<ac3::DownmixTarget> fold;
    bool reconstruct = false;
    // The coded layout the renderer is set up for, from the headers of the
    // unit about to decode (see peek_layout) or the decoded layout of the
    // last one when the two disagreed.
    ac3::eac3::chanmap::Layout bed{};
    bool have_bed = false;
    // The slots that bed reaches, kept with it; see StreamInfo::silent.
    std::uint16_t bed_fed = 0;
    // The word StreamInfo::render reports for a fold, set at start(); null when
    // the layout is rendered rather than folded.
    const char* fold_word = nullptr;
    // Which programme plays: the first E-AC-3 access unit's. A stream may
    // carry up to eight, as independent substreams (§E2.3.1.2), and they are
    // alternatives rather than layers - a second language, a commentary - so
    // the other programmes' units are skipped before they are decoded.
    std::optional<int> programme;

    EventGroupHandle_t events = nullptr;
    StreamBufferHandle_t ring = nullptr;
    // The ring's two allocations, owned here rather than by
    // xStreamBufferCreateWithCaps - see make_ring() for why.
    std::uint8_t* ring_storage = nullptr;
    StaticStreamBuffer_t* ring_struct = nullptr;
    bool ring_in_psram = false;
    TaskHandle_t fetch_task = nullptr;
    TaskHandle_t decode_task = nullptr;

    // One block per slot, the sink's view of it, and the spans the renderer
    // writes through. Sized once, reused for every block.
    std::array<std::array<float, ac3::kSamplesPerBlock>, kMaxSlots> block{};
    std::array<std::span<float>, kMaxSlots> block_spans{};
    std::array<std::span<const float>, kMaxSlots> block_views{};
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

    // The renderer's bed as last set. A block carries the bed it was decoded
    // against - a held block its own unit's, which the next unit's headers may
    // since have changed - and the renderer is set up again when that changes.
    ac3::eac3::chanmap::Layout renderer_bed{};
    bool renderer_has_bed = false;
    // A unit's object descriptions, gathered on its first block.
    std::array<ac3::oba::DisplayObject, kMaxObjects> places{};

    // The hold on a play's first unit (PlayerConfig::hold_first_unit), set at
    // start(). The hold is armed by the unit's first block and released when
    // the second unit's first block arrives or the play ends, and `holding` is
    // then cleared for the rest of the play. The held unit's bed and object
    // descriptions are kept with it, since by the time it plays the next unit
    // has been set up.
    bool holding = false;
    UnitHold hold;
    float* hold_storage = nullptr;
    ac3::eac3::chanmap::Layout held_bed{};
    bool held_has_bed = false;
    std::array<ac3::oba::DisplayObject, kMaxObjects> held_places{};
    std::size_t held_place_count = 0;

    // Written by the tasks, read by anyone.
    std::atomic<std::uint64_t> frames_played{0};
    std::atomic<std::uint64_t> frames_held{0};
    std::atomic<std::uint64_t> decode_us{0};
    std::atomic<std::uint64_t> render_us{0};
    std::atomic<std::uint64_t> sink_us{0};
    std::atomic<std::uint64_t> worst_frame_us{0};
    std::atomic<std::uint64_t> fetched_bytes{0};
    std::atomic<std::uint64_t> resync_bytes{0};
    std::atomic<std::uint32_t> passes{0};
    std::atomic<std::uint32_t> layout_mismatches{0};
    std::atomic<std::size_t> ring_low_water{SIZE_MAX};
    std::atomic<bool> finished{false};
    std::atomic<bool> failed{false};
    std::atomic<bool> have_stream{false};
    std::atomic<const char*> failure{""};
    std::atomic<int> error{0};
    std::atomic<float> volume{1.0F};
    std::atomic<std::size_t> decode_stack_free{0};
    // The slots this play has sent something to, bit n for slot n - what
    // StreamInfo::silent names the rest of. Or-ed in by the decode task once
    // per unit.
    std::atomic<std::uint16_t> fed_slots{0};
    StreamInfo stream{};  // written once, before have_stream is set

    // The ring, allocated by hand rather than through
    // xStreamBufferCreateWithCaps.
    //
    // ESP-IDF v6.1's matching vStreamBufferDeleteWithCaps is broken: it
    // deletes the buffer with vSemaphoreDelete(), i.e. vQueueDelete(), which
    // reads the stream buffer's struct as a queue's and checks the queue's
    // "statically allocated" byte - at an offset that in the smaller
    // StaticStreamBuffer_t is another field, or memory past its end. When
    // that byte reads 0 it frees the struct itself, and the helper's own
    // heap_caps_free then frees it again. Whether it does depends on what the
    // heap put beside the ring, so it panicked ("block already marked as
    // free", from Player::stop()) on every CI QEMU run of the streaming
    // example's http shape and on no local run of the same image.
    //
    // What follows is what the helper does, less the wrong delete: our own
    // heap_caps_malloc for both parts, xStreamBufferCreateStatic, and in
    // free_ring() vStreamBufferDelete - which frees nothing for a static
    // buffer - then heap_caps_free for both. It keeps "PSRAM when present",
    // which is all the helper was here for.
    bool make_ring(std::uint32_t caps) {
        ring_struct = static_cast<StaticStreamBuffer_t*>(
            heap_caps_malloc(sizeof(StaticStreamBuffer_t), caps));
        ring_storage = static_cast<std::uint8_t*>(heap_caps_malloc(config.ring_bytes, caps));
        if (ring_struct != nullptr && ring_storage != nullptr) {
            ring = xStreamBufferCreateStatic(config.ring_bytes, 1, ring_storage, ring_struct);
        }
        if (ring == nullptr) {
            free_ring();
            return false;
        }
        return true;
    }

    void free_ring() {
        if (ring != nullptr) {
            vStreamBufferDelete(ring);
            ring = nullptr;
        }
        heap_caps_free(ring_struct);
        heap_caps_free(ring_storage);
        ring_struct = nullptr;
        ring_storage = nullptr;
    }

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
        s.render_us = render_us.load();
        s.sink_us = sink_us.load();
        s.worst_frame_us = worst_frame_us.load();
        s.fetched_bytes = fetched_bytes.load();
        s.resync_bytes = resync_bytes.load();
        s.passes = passes.load();
        s.layout_mismatches = layout_mismatches.load();
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

    // The decode is over: the last pass played, the source could not rewind,
    // or something failed. A unit still held plays first, so a stream of one
    // unit is heard. Called from the decode task only.
    void finish(const char* why, bool is_failure, int code) {
        release_hold();
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

    // The coded layout a unit's blocks are placed by, from its headers. Only
    // when it changes, which for a stream is once. The renderer itself is set
    // up when a block arrives with a bed other than the one it has
    // (output_block), so a held block is placed by its own unit's bed.
    void prepare_bed(std::span<const std::byte> unit) {
        if (fold.has_value()) {
            return;  // the decoder's output stage does the placing
        }
        const auto peeked = peek_layout(unit);
        if (!peeked.has_value()) {
            return;  // a unit the framer accepted and the header reader did not; the decoder will say
        }
        if (!have_bed || !same_layout(bed, *peeked)) {
            bed = *peeked;
            have_bed = true;
        }
    }

    // What the decoder returned afterwards, against what the headers said. A
    // disagreement places the next unit by the decoded layout.
    void confirm_bed(const ac3::eac3::chanmap::Layout& decoded) {
        if (fold.has_value() || same_layout(bed, decoded)) {
            return;
        }
        layout_mismatches.fetch_add(1);
        bed = decoded;
        have_bed = true;
    }

    // The objects a unit's first block is placed by, as describe_objects
    // gives them, into `into` (kMaxObjects room): the first of the object
    // signals the block carries, at most kMaxObjects. Only what the renderer
    // reads is kept - the label views the decoder's storage, which is gone
    // once the decode call returns, so it is cleared.
    static std::size_t gather_places(const ac3::PcmBlock& pcm, ac3::oba::DisplayObject* into) {
        if (pcm.object_metadata == nullptr || pcm.objects.empty()) {
            return 0;
        }
        const std::vector<ac3::oba::DisplayObject> described =
            ac3::oba::describe_objects(*pcm.object_metadata);
        const std::size_t count = std::min({described.size(), pcm.objects.size(), kMaxObjects});
        for (std::size_t i = 0; i < count; ++i) {
            into[i] = described[i];
            into[i].label = {};
        }
        return count;
    }

    // One block onto the layout and into the sink, timed in two parts:
    // placing it, and the sink's write - which on a paced sink is mostly the
    // wait for the DAC. Runs in the decode task: inside the decode call, or
    // from finish() for a unit still held.
    void output_block(const BlockView& view) {
        const std::int64_t entered = esp_timer_get_time();
        const std::size_t slots = config.layout.slots();
        const std::size_t n = view.channels.empty() ? 0 : view.channels.front().size();
        const float gain = volume.load();
        const std::span<const std::span<float>> out(block_spans.data(), slots);
        // The renderer reads the block's samples; the object description has
        // already reached it through `places`.
        const ac3::PcmBlock pcm{.index = view.index,
                                .blocks = 0,
                                .channels = view.channels,
                                .objects = view.objects,
                                .object_indices = {},
                                .object_metadata = nullptr};
        if (fold.has_value()) {
            renderer.render_folded(pcm, gain, out);
            if (view.index == 0) {
                fed_slots.fetch_or(config.layout.connected_slots());
            }
        } else {
            if (view.bed != nullptr && (!renderer_has_bed || !same_layout(renderer_bed, *view.bed))) {
                renderer_bed = *view.bed;
                renderer_has_bed = true;
                renderer.set_bed(renderer_bed);
                bed_fed = renderer.bed_slots();
            }
            if (reconstruct && view.index == 0) {
                renderer.set_objects(view.places);
            }
            renderer.render(pcm, reconstruct, gain, out);
            if (view.index == 0) {
                // What render() placed: the objects and the bed's LFE when it
                // placed objects, the bed when it did not.
                const bool placed = reconstruct && renderer.object_count() > 0 && !view.objects.empty();
                fed_slots.fetch_or(placed ? static_cast<std::uint16_t>(renderer.object_slots() |
                                                                       renderer.bed_slots(true))
                                          : bed_fed);
            }
        }
        for (std::size_t slot = 0; slot < slots; ++slot) {
            block_views[slot] = std::span<const float>(block[slot].data(), std::min(n, block[slot].size()));
        }
        const std::int64_t rendered = esp_timer_get_time();
        sink.write(std::span<const std::span<const float>>(block_views.data(), slots));
        render_us.fetch_add(static_cast<std::uint64_t>(rendered - entered));
        sink_us.fetch_add(static_cast<std::uint64_t>(esp_timer_get_time() - rendered));
    }

    // One block from the decoder: into the hold while a play's first unit is
    // held, and onto the layout and into the sink otherwise.
    void deliver(const ac3::PcmBlock& pcm) {
        if (holding) {
            if (hold_block(pcm)) {
                return;
            }
            // The second unit's first block, or one the hold cannot take: what
            // is held plays first, and nothing is held after it.
            release_hold();
        }
        const std::size_t count =
            reconstruct && pcm.index == 0 ? gather_places(pcm, places.data()) : 0;
        output_block(BlockView{.index = pcm.index,
                               .channels = pcm.channels,
                               .objects = pcm.objects,
                               .bed = have_bed ? &bed : nullptr,
                               .places = std::span<const ac3::oba::DisplayObject>(places.data(),
                                                                                  count)});
    }

    // Into the hold, which the play's first block arms with room for blocks
    // like it. The unit's bed and object descriptions go beside it.
    bool hold_block(const ac3::PcmBlock& pcm) {
        if (!hold.armed() && !arm_hold(pcm)) {
            return false;
        }
        const bool first = hold.held() == 0;
        if (!hold.offer(pcm.index, pcm.channels, pcm.objects)) {
            return false;
        }
        if (first) {
            held_bed = bed;
            held_has_bed = have_bed;
        }
        if (reconstruct && pcm.index == 0) {
            held_place_count = gather_places(pcm, held_places.data());
        }
        return true;
    }

    // Room for a unit of blocks like `pcm`: in PSRAM when the part has it, as
    // the bitstream ring is, with "has it" asked rather than learned from a
    // failed allocation (see start()). Without the room the play goes on
    // unheld.
    bool arm_hold(const ac3::PcmBlock& pcm) {
        const std::size_t floats = UnitHold::storage_floats(pcm.channels.size() + pcm.objects.size(),
                                                            ac3::kSamplesPerBlock);
        const std::uint32_t caps = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0
                                       ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                                       : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        hold_storage = static_cast<float*>(heap_caps_malloc(floats * sizeof(float), caps));
        if (hold_storage != nullptr &&
            hold.arm(std::span<float>(hold_storage, floats), pcm.channels.size(),
                     pcm.objects.size(), ac3::kSamplesPerBlock)) {
            return true;
        }
        std::printf("player: no room to hold the first unit (%u bytes); it plays as it comes\n",
                    static_cast<unsigned>(floats * sizeof(float)));
        free_hold();
        return false;
    }

    // What the hold has, played in the order it came; then the play goes on
    // unheld.
    void release_hold() {
        if (!holding) {
            return;
        }
        holding = false;
        hold.release([&](std::size_t /*position*/, int index,
                         std::span<const std::span<const float>> channels,
                         std::span<const std::span<const float>> objects) {
            output_block(BlockView{
                .index = index,
                .channels = channels,
                .objects = objects,
                .bed = held_has_bed ? &held_bed : nullptr,
                .places = index == 0 ? std::span<const ac3::oba::DisplayObject>(held_places.data(),
                                                                                held_place_count)
                                     : std::span<const ac3::oba::DisplayObject>{}});
        });
        free_hold();
    }

    // The hold's storage back, with nothing left held in it.
    void free_hold() {
        hold.clear();
        heap_caps_free(hold_storage);
        hold_storage = nullptr;
    }

    // StreamInfo's account of how the layout is served (see player.hpp), once
    // the first unit has said what the stream is: `silent` is filled in by
    // Player::stream(), since it grows as the play goes on.
    void describe_stream(const std::optional<ac3::eac3::chanmap::Layout>& coded, bool dual_mono) {
        const std::string_view text = config.layout.text();
        const std::size_t n = std::min(text.size(), stream.layout.size() - 1);
        std::copy_n(text.data(), n, stream.layout.data());
        stream.layout[n] = '\0';
        stream.render = fold_word != nullptr ? fold_word : (stream.objects_rendered ? "objects" : "channels");
        std::size_t used = 0;
        const auto add = [&](std::string_view name) {
            if (used + name.size() + 2 > stream.coded.size()) {
                return;
            }
            if (used > 0) {
                stream.coded[used++] = ',';
            }
            std::copy_n(name.data(), name.size(), stream.coded.data() + used);
            used += name.size();
            stream.coded[used] = '\0';
        };
        if (dual_mono) {
            add("Ch1");
            add("Ch2");
        } else if (coded.has_value()) {
            for (const auto location : *coded) {
                add(ac3::eac3::chanmap::name(location));
            }
        }
    }

    // True when the unit produced audio, false when the decoder held it back
    // (§3.7's transient pre-noise processing releases each frame one call late).
    std::expected<bool, ac3::DecodeError> decode_unit(std::span<const std::byte> unit) {
        const auto header = ac3::io::read_frame_header(unit);
        const bool one_ac3_syncframe = header.has_value() &&
                                       header->kind == ac3::io::StreamKind::kAc3 &&
                                       header->bytes == unit.size();
        prepare_bed(unit);
        bool delivered = false;
        const auto deliver_block = [&](const ac3::PcmBlock& pcm) {
            deliver(pcm);
            delivered = true;
        };
        if (one_ac3_syncframe) {
            if (!ac3_decoder.has_value()) {
                ac3_decoder.emplace(config.decoder);
            }
            const auto decoded = ac3_decoder->decode_frame_by_block(unit, deliver_block);
            if (!decoded) {
                return std::unexpected(decoded.error());
            }
            if (!have_stream.load()) {
                stream = {.eac3 = false,
                          .acmod = static_cast<int>(decoded->acmod),
                          .channels = header->coded_channels(),
                          .substreams = 1,
                          .dialnorm = decoded->dialnorm,
                          .objects = false,
                          .objects_rendered = false,
                          .slots = static_cast<int>(config.layout.slots())};
                describe_stream(peek_layout(unit), decoded->acmod == ac3::Acmod::kDualMono);
                have_stream.store(true);
            }
            return delivered;
        }
        if (!eac3_decoder.has_value()) {
            eac3_decoder.emplace(config.decoder);
        }
        const auto decoded = eac3_decoder->decode_access_unit_by_block(unit, deliver_block);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        if (!decoded->has_value()) {
            return false;
        }
        const auto& au = **decoded;
        confirm_bed(au.layout);
        if (!have_stream.load()) {
            // Dual mono has no Table E2.5 layout, so its count is 0; it is two
            // channels all the same.
            const bool dual_mono = au.acmod == ac3::Acmod::kDualMono;
            stream = {.eac3 = true,
                      .acmod = static_cast<int>(au.acmod),
                      .channels = dual_mono ? 2 : au.layout.count,
                      .substreams = au.substream_count,
                      .dialnorm = au.dialnorm,
                      .objects = au.object_metadata.has_value(),
                      .objects_rendered = reconstruct && au.object_metadata.has_value(),
                      .slots = static_cast<int>(config.layout.slots())};
            describe_stream(au.layout, dual_mono);
            have_stream.store(true);
        }
        return delivered;
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
                // Once the source has ended, everything it will ever send is
                // already in the ring: take what is there without waiting, so
                // the end of a pass is seen at once. Blocking here - for the
                // whole timeout, on a ring nothing will refill - added 100 ms
                // to every pass, which made the six-frame sample take half as
                // long again as its audio.
                const bool source_ended = (xEventGroupGetBits(events) & kSourceEnded) != 0;
                const std::size_t got = xStreamBufferReceive(
                    ring, dst.data(), dst.size(), source_ended ? 0 : pdMS_TO_TICKS(100));
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

            // Before anything is decoded: a stream at a rate the sink does not
            // run at is refused (PlayerConfig::sample_rate_hz), and a unit of a
            // programme other than the one playing is skipped, uncounted.
            if (const auto header = ac3::io::read_frame_header(unit.bytes)) {
                const std::uint32_t hz = ac3::sample_rate_hz(header->sample_rate);
                if (hz != config.sample_rate_hz) {
                    finish("sample rate", true, static_cast<int>(hz));
                    break;
                }
                if (header->kind == ac3::io::StreamKind::kEac3) {
                    if (!programme.has_value()) {
                        programme = header->substreamid;
                    } else if (header->substreamid != *programme) {
                        continue;
                    }
                }
            }

            // The time is decode AND render AND the sink's wait, because the
            // blocks reach the sink from inside the decode call. On a paced
            // sink that wait is the DAC's clock, not work; deliver() times the
            // render and the sink's part separately, and the sink's own
            // counters say whether the wait was ever too long.
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
    const std::size_t slots = im.config.layout.slots();
    if (slots == 0 || slots > kMaxSlots) {
        std::printf("player: the layout must have 1..%u slots\n", static_cast<unsigned>(kMaxSlots));
        return false;
    }
    for (std::size_t slot = 0; slot < kMaxSlots; ++slot) {
        im.block_spans[slot] = std::span<float>(im.block[slot]);
    }
    im.staging.resize(im.config.fetch_bytes);
    set_volume(im.config.volume);

    // How the layout is served: the decoder's own §7.8 stage for a stereo or
    // mono room, the renderer for everything else, with the objects
    // reconstructed when the layout asks for what the bed cannot give.
    im.fold = im.config.layout.fold(im.config.stereo_fold);
    im.fold_word = im.fold == ac3::DownmixTarget::kMono   ? "mono"
                   : im.fold == ac3::DownmixTarget::kLtRt ? "ltrt"
                   : im.fold.has_value()                  ? "loro"
                                                          : nullptr;
    im.config.decoder.output.target = im.fold.value_or(ac3::DownmixTarget::kAsCoded);
    switch (im.config.objects) {
        case PlayerConfig::Objects::kNever: im.reconstruct = false; break;
        case PlayerConfig::Objects::kAlways: im.reconstruct = !im.fold.has_value(); break;
        case PlayerConfig::Objects::kAuto:
            im.reconstruct = !im.fold.has_value() && im.config.layout.has_height();
            break;
    }
    im.config.decoder.skip_object_reconstruction = !im.reconstruct;
    im.renderer = LayoutRenderer{im.config.layout};

    im.events = xEventGroupCreate();
    if (im.events == nullptr) {
        std::printf("player: no memory for the event group\n");
        return false;
    }

    // The ring: PSRAM when asked for and present, internal SRAM otherwise. A
    // trigger level of one byte, so the decoder wakes on whatever arrives.
    // "Present" is asked, not learned from a failed allocation: that failure
    // reaches any failed-allocation hook the application has registered, and
    // reads there as running out of memory.
    if (im.config.ring_in_psram && heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
        im.ring_in_psram = im.make_ring(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (im.ring == nullptr) {
        (void)im.make_ring(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
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
    const char* how = "as coded, the bed placed";
    if (im.fold == ac3::DownmixTarget::kMono) {
        how = "the decoder's mono fold";
    } else if (im.fold == ac3::DownmixTarget::kLtRt) {
        how = "the decoder's Lt/Rt fold";
    } else if (im.fold.has_value()) {
        how = "the decoder's Lo/Ro fold";
    } else if (im.reconstruct) {
        how = "as coded, objects placed when the stream has them";
    }
    std::printf("player: layout %s, %u slots, %s\n", im.config.layout.text().data(),
                static_cast<unsigned>(slots), how);

    im.holding = im.config.hold_first_unit;
    if (im.holding) {
        std::printf("player: a play's first unit is held until its second has decoded\n");
    }

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
    im.holding = false;
    im.free_hold();
    im.free_ring();
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
    const auto& im = *impl_;
    if (!im.have_stream.load()) {
        return std::nullopt;
    }
    StreamInfo info = im.stream;
    // The connected slots nothing has reached yet, named as the layout names
    // them; the decode task keeps adding to what has been reached.
    im.config.layout.names_of(
        static_cast<std::uint16_t>(im.config.layout.connected_slots() & ~im.fed_slots.load()),
        info.silent);
    return info;
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
