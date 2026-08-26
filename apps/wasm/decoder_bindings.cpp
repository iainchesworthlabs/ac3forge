// Embind wrapper around ac3::forge's decode path, for the reusable push-frame
// package (roadmap UX5, js/) and the docs demo built on top of it
// (apps/wasm/index.html - see js/src/decode-file.ts for the whole-file
// convenience helper the demo actually calls).
//
// Two entry points:
//   - scanStream(bytes): a thin wrapper over ac3::io::scan, so a caller can
//     slice a whole elementary-stream blob into the access units
//     PushDecoder::pushAccessUnit expects without re-walking syncframes
//     itself.
//   - PushDecoder: one ac3::Eac3Decoder per instance, decoding one access
//     unit per call through decode_access_unit_into's caller-buffer form -
//     the PCM buffers are allocated ONCE at construction and reused for
//     every call (apps/baremetal/probe.cpp established the same
//     caller-buffer pattern for the bare-metal PF7 profile), so the hot path
//     never allocates on the C++ side. Eac3Decoder alone is enough for every
//     ac3::io::StreamKind - decode_access_unit's own doc comment: a plain
//     AC-3 syncframe "comes back as substream (kIndependent, 0)" - so
//     scanStream's `kind` is informational only; PushDecoder does not branch
//     on it.
//
// The optional §7.8 fold (DC1's ac3::OutputStage, never a hand-rolled one) is
// applied separately from the main decode, over a small reused COPY of the
// just-decoded channels - see PushDecoder::apply_fold's own comment for why
// it can't be done in place.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/decoder/output.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/oba/oamd.hpp"

namespace {

// §E3.8.2's cap - the same bound apps/baremetal/probe.cpp's g_pcm/g_pcm_spans use.
constexpr std::size_t kMaxChannels = 16;
// x, y, z, gain_db, then §5.6.1.2's width/depth/height.
constexpr std::size_t kPositionStride = 7;

// DecodedAccessUnit's `layout` gives the real Table E2.5 channel identity for
// each position; dual mono (layout.count == 0) is the one case with no
// layout at all (DecodedAccessUnit's own comment), and OutputStage already
// treats it as two unrelated programmes rather than a soundfield - so the
// only label fallback needed here is Ch1/Ch2.
std::vector<std::string> channel_labels(const ac3::eac3::chanmap::Layout& layout,
                                        ac3::Acmod acmod) {
    if (layout.count > 0) {
        std::vector<std::string> labels;
        labels.reserve(static_cast<std::size_t>(layout.count));
        for (const auto location : layout) {
            labels.emplace_back(ac3::eac3::chanmap::name(location));
        }
        return labels;
    }
    if (acmod == ac3::Acmod::kDualMono) {
        return {"Ch1", "Ch2"};
    }
    return {};
}

emscripten::val make_string_array(const std::vector<std::string>& values) {
    auto arr = emscripten::val::array();
    for (std::size_t i = 0; i < values.size(); ++i) {
        arr.set(static_cast<unsigned>(i), values[i]);
    }
    return arr;
}

emscripten::val make_error(const std::string& message) {
    auto result = emscripten::val::object();
    result.set("ok", false);
    result.set("error", message);
    return result;
}

}  // namespace

// Splits a whole elementary-stream byte blob into the access units
// PushDecoder::pushAccessUnit expects, one call per entry - used by the
// whole-file convenience helper (js/src/decode-file.ts) and available to any
// consumer that already has a complete file rather than a live push feed
// (an hls.js/MSE bridge instead slices its own container samples and never
// needs this). Reuses ac3::io::scan rather than re-walking syncframes here.
emscripten::val scanStream(const emscripten::val& js_bytes) {
    const std::vector<std::uint8_t> raw = emscripten::vecFromJSArray<std::uint8_t>(js_bytes);
    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(raw.data()),
                                            raw.size());

    const auto scanned = ac3::io::scan(bytes);
    if (!scanned) {
        return make_error(std::string(ac3::io::describe(scanned.error())));
    }

    auto result = emscripten::val::object();
    result.set("ok", true);
    switch (scanned->kind) {
        case ac3::io::StreamKind::kAc3:
            result.set("kind", std::string("AC-3"));
            break;
        case ac3::io::StreamKind::kEac3:
            result.set("kind", std::string("E-AC-3"));
            break;
        case ac3::io::StreamKind::kAc3CoreEac3Extension:
            result.set("kind", std::string("AC-3 core + E-AC-3 extension"));
            break;
    }
    result.set("sampleRate", static_cast<int>(ac3::sample_rate_hz(scanned->sample_rate)));

    auto units = emscripten::val::array();
    for (std::size_t i = 0; i < scanned->access_units.size(); ++i) {
        const auto& unit = scanned->access_units[i];
        auto entry = emscripten::val::object();
        entry.set("offset", static_cast<unsigned>(reinterpret_cast<const std::uint8_t*>(unit.data()) -
                                                    raw.data()));
        entry.set("length", static_cast<unsigned>(unit.size()));
        units.set(static_cast<unsigned>(i), entry);
    }
    result.set("accessUnits", units);
    return result;
}

class PushDecoder {
   public:
    // foldTarget: ac3::DownmixTarget's own numeric order (0=as-coded,
    // 1=Lo/Ro, 2=Lt/Rt, 3=mono) - kept as a plain int rather than an enum
    // binding for one constructor argument. 0/false/false (the default a
    // caller gets by passing target=asCoded) means "no fold": foldPcm/
    // foldChannelCount then always report nothing, and apply_fold's copy is
    // skipped entirely.
    PushDecoder(int fold_target, bool fold_apply_dialnorm, bool fold_mix_lfe)
        : fold_(ac3::OutputConfig{.target = static_cast<ac3::DownmixTarget>(fold_target),
                                  .apply_dialnorm = fold_apply_dialnorm,
                                  .mix_lfe = fold_mix_lfe}) {
        for (auto& channel : pcm_) {
            channel.resize(ac3::kSamplesPerFrame);
        }
        for (auto& channel : fold_scratch_) {
            channel.resize(ac3::kSamplesPerFrame);
        }
        spans_.reserve(pcm_.size());
        for (auto& channel : pcm_) {
            spans_.emplace_back(channel);
        }
    }

    emscripten::val pushAccessUnit(const emscripten::val& js_bytes) {
        object_positions_.clear();
        object_audio_.clear();
        object_labels_.clear();
        fold_channel_count_ = 0;

        const std::vector<std::uint8_t> raw = emscripten::vecFromJSArray<std::uint8_t>(js_bytes);
        const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(raw.data()),
                                                raw.size());
        try {
            auto decoded = decoder_.decode_access_unit_into(bytes, spans_);
            if (!decoded) {
                return make_error(std::string(ac3::describe(decoded.error())));
            }
            if (!decoded->has_value()) {
                auto result = emscripten::val::object();
                result.set("ok", true);
                result.set("holdBack", true);
                return result;
            }
            return describe_unit(**decoded);
        } catch (const std::bad_alloc&) {
            // ALLOW_MEMORY_GROWTH (CMakeLists.txt) turns heap exhaustion into
            // an exception rather than a dead tab - same reasoning as the old
            // whole-file Decoder's decode().
            return make_error(
                "out of memory: this frame's decoded audio does not fit the module's memory "
                "budget");
        }
    }

    // §3.7's tail: whichever substream identities were still holding a frame
    // back at end-of-stream. Rare and end-of-stream-only (unlike
    // pushAccessUnit above, decoder_.flush()'s own return value already
    // allocates, so doing the same here costs nothing extra) - no fold or
    // object metadata is attached to these entries, since a flushed result is
    // a raw per-substream DecodedSubstream rather than an assembled
    // DecodedAccessUnit (flush()'s own doc comment) and neither the layout
    // union nor the OAMD/JOC line-up this class does elsewhere apply to one
    // substream in isolation.
    emscripten::val flush() {
        flushed_ = decoder_.flush();
        auto out = emscripten::val::array();
        for (std::size_t i = 0; i < flushed_.size(); ++i) {
            const auto& sub = flushed_[i];
            const auto layout = ac3::eac3::chanmap::expand(sub.location_map());
            const auto labels = channel_labels(layout, sub.acmod);

            auto entry = emscripten::val::object();
            entry.set("ok", true);
            entry.set("holdBack", false);
            entry.set("sampleRate", static_cast<int>(ac3::sample_rate_hz(sub.sample_rate)));
            entry.set("frameSamples",
                      ac3::eac3::blocks_per_syncframe(sub.numblkscod) * ac3::kSamplesPerBlock);
            entry.set("dialnorm", sub.dialnorm);
            entry.set("channelCount", static_cast<int>(labels.size()));
            entry.set("channelLabels", make_string_array(labels));
            entry.set("objectCount", 0);
            entry.set("flushIndex", static_cast<unsigned>(i));
            out.set(static_cast<unsigned>(i), entry);
        }
        return out;
    }

    [[nodiscard]] emscripten::val flushedChannelPcm(int index, int channel) const {
        if (index < 0 || static_cast<std::size_t>(index) >= flushed_.size()) {
            return emscripten::val::null();
        }
        const auto& channels = flushed_[static_cast<std::size_t>(index)].channels;
        if (channel < 0 || static_cast<std::size_t>(channel) >= channels.size()) {
            return emscripten::val::null();
        }
        const auto& pcm = channels[static_cast<std::size_t>(channel)];
        return emscripten::val(emscripten::typed_memory_view(pcm.size(), pcm.data()));
    }

    // Zero-copy view into the last pushAccessUnit() call's coded/rendered PCM
    // for `channel` - valid only until the next pushAccessUnit()/flush()
    // call, the same contract the original whole-file Decoder's channelPcm()
    // documented and apps/wasm/demo.js already respects (copy out
    // immediately, never hold the view).
    [[nodiscard]] emscripten::val channelPcm(int channel) const {
        if (channel < 0 || static_cast<std::size_t>(channel) >= last_channel_count_) {
            return emscripten::val::null();
        }
        return emscripten::val(emscripten::typed_memory_view(
            static_cast<std::size_t>(last_frame_samples_),
            pcm_[static_cast<std::size_t>(channel)].data()));
    }

    [[nodiscard]] int foldChannelCount() const { return fold_channel_count_; }

    // The optional §7.8 fold this instance was constructed for (ac3::
    // OutputStage/DC1 - never a hand-rolled one), same zero-copy-until-next-
    // call contract as channelPcm above. Null when fold_target was
    // kAsCoded/0 (no fold requested) or channel is out of range.
    [[nodiscard]] emscripten::val foldPcm(int channel) const {
        if (channel < 0 || channel >= fold_channel_count_) {
            return emscripten::val::null();
        }
        return emscripten::val(emscripten::typed_memory_view(
            static_cast<std::size_t>(last_frame_samples_),
            fold_scratch_[static_cast<std::size_t>(channel)].data()));
    }

    [[nodiscard]] int objectCount() const { return static_cast<int>(object_labels_.size()); }

    // This frame's [x, y, z, gain_db, width, depth, height] for `object` -
    // one snapshot per pushAccessUnit() call, not accumulated across calls
    // (that bookkeeping is js/src/decode-file.ts's job now, for whichever
    // consumer wants a whole-file timeline rather than a live one).
    [[nodiscard]] emscripten::val objectPosition(int object) const {
        if (object < 0 || static_cast<std::size_t>(object) >= object_positions_.size()) {
            return emscripten::val::null();
        }
        return emscripten::val(emscripten::typed_memory_view(
            kPositionStride, object_positions_[static_cast<std::size_t>(object)].data()));
    }

    [[nodiscard]] emscripten::val objectAudioPcm(int object) const {
        if (object < 0 || static_cast<std::size_t>(object) >= object_audio_.size()) {
            return emscripten::val::null();
        }
        const auto& pcm = object_audio_[static_cast<std::size_t>(object)];
        return emscripten::val(emscripten::typed_memory_view(pcm.size(), pcm.data()));
    }

    [[nodiscard]] std::string objectLabel(int object) const {
        if (object < 0 || static_cast<std::size_t>(object) >= object_labels_.size()) {
            return {};
        }
        return object_labels_[static_cast<std::size_t>(object)];
    }

   private:
    emscripten::val describe_unit(ac3::DecodedAccessUnit& unit) {
        last_channel_count_ = static_cast<std::size_t>(
            unit.layout.count > 0 ? unit.layout.count
                                   : (unit.acmod == ac3::Acmod::kDualMono ? 2 : 0));
        last_frame_samples_ = ac3::eac3::blocks_per_syncframe(unit.numblkscod) * ac3::kSamplesPerBlock;

        const auto labels = channel_labels(unit.layout, unit.acmod);

        apply_fold(unit);

        // object_metadata (OAMD, PR #168) and object_audio (JOC, PR #169) are
        // both real decode-side capabilities, now available per pushed frame
        // instead of only after a whole file finished decoding - see
        // oba::describe_objects()'s own header for what counts as "an
        // object" (every JOC output: dynamic objects for the program
        // AtmosEncoder writes, or the bed's own channels for channel-based-
        // immersive third-party content).
        const auto objects = unit.object_metadata
                                  ? ac3::oba::describe_objects(*unit.object_metadata)
                                  : std::vector<ac3::oba::DisplayObject>{};
        object_positions_.reserve(objects.size());
        object_labels_.reserve(objects.size());
        for (const auto& object : objects) {
            object_positions_.push_back({static_cast<float>(object.position.x),
                                          static_cast<float>(object.position.y),
                                          static_cast<float>(object.position.z),
                                          static_cast<float>(object.gain_db),
                                          static_cast<float>(object.size.width),
                                          static_cast<float>(object.size.depth),
                                          static_cast<float>(object.size.height)});
            object_labels_.emplace_back(object.label);
        }
        if (unit.object_audio.size() == objects.size()) {
            object_audio_ = std::move(unit.object_audio);
        } else if (!objects.empty()) {
            // An uploaded/pushed stream can carry OAMD positions with no
            // matching JOC audio - pad with silence rather than desync this
            // frame's object count from its own audio, same rule the old
            // whole-file Decoder's pad_object_audio_with_silence() used.
            object_audio_.assign(objects.size(),
                                 std::vector<float>(static_cast<std::size_t>(last_frame_samples_), 0.0F));
        }

        auto result = emscripten::val::object();
        result.set("ok", true);
        result.set("holdBack", false);
        result.set("sampleRate", static_cast<int>(ac3::sample_rate_hz(unit.sample_rate)));
        result.set("frameSamples", last_frame_samples_);
        result.set("dialnorm", unit.dialnorm);
        result.set("channelCount", static_cast<int>(labels.size()));
        result.set("channelLabels", make_string_array(labels));
        result.set("foldChannelCount", fold_channel_count_);
        result.set("objectCount", static_cast<int>(objects.size()));
        result.set("objectLabels", make_string_array(object_labels_));
        return result;
    }

    // The fold can't be done in place on pcm_: ac3::OutputStage's span form
    // writes its result into the first output_channel_count() slots of
    // whatever it's given, using the REST of that same array as its input -
    // so folding pcm_ directly would overwrite the coded channel 0/1 data a
    // caller also wants (the speaker-ring/per-channel visualization, in the
    // demo's case). Copying into fold_scratch_ first - a bounded, allocation-
    // free copy, both buffers sized once at construction - keeps both
    // outputs available from one decode.
    void apply_fold(const ac3::DecodedAccessUnit& unit) {
        if (fold_.config().target == ac3::DownmixTarget::kAsCoded) {
            return;
        }
        fold_views_.clear();
        fold_views_.reserve(last_channel_count_);
        for (std::size_t ch = 0; ch < last_channel_count_; ++ch) {
            std::copy_n(pcm_[ch].data(), static_cast<std::size_t>(last_frame_samples_),
                       fold_scratch_[ch].data());
            fold_views_.emplace_back(fold_scratch_[ch].data(),
                                     static_cast<std::size_t>(last_frame_samples_));
        }
        const auto levels = ac3::mix_levels(unit.mixing);
        const bool has_lfe = unit.layout.count > 0 &&
                             unit.layout.index_of(ac3::eac3::chanmap::Location::kLfe) >= 0;
        if (unit.layout.count > 0) {
            fold_.apply(fold_views_, unit.layout, unit.acmod, has_lfe, levels, unit.dialnorm);
        } else {
            fold_.apply(fold_views_, unit.acmod, has_lfe, levels, unit.dialnorm);
        }
        fold_channel_count_ =
            static_cast<int>(ac3::output_channel_count(fold_.config(), unit.acmod, has_lfe));
    }

    ac3::Eac3Decoder decoder_;  // Default DecoderConfig: always raw/coded output.
    ac3::OutputStage fold_;     // The optional side fold - see apply_fold's own comment.

    std::array<std::vector<float>, kMaxChannels> pcm_;
    std::vector<std::span<float>> spans_;
    std::size_t last_channel_count_ = 0;
    int last_frame_samples_ = 0;

    std::array<std::vector<float>, kMaxChannels> fold_scratch_;
    std::vector<std::span<float>> fold_views_;
    int fold_channel_count_ = 0;

    // This frame only - see objectPosition's own comment on why these are
    // not accumulated across calls the way the old whole-file Decoder did.
    std::vector<std::array<float, kPositionStride>> object_positions_;
    std::vector<std::vector<float>> object_audio_;
    std::vector<std::string> object_labels_;

    std::vector<ac3::DecodedSubstream> flushed_;
};

EMSCRIPTEN_BINDINGS(ac3forge_wasm_push_decode) {
    emscripten::function("scanStream", &scanStream);
    emscripten::class_<PushDecoder>("PushDecoder")
        .constructor<int, bool, bool>()
        .function("pushAccessUnit", &PushDecoder::pushAccessUnit)
        .function("flush", &PushDecoder::flush)
        .function("flushedChannelPcm", &PushDecoder::flushedChannelPcm)
        .function("channelPcm", &PushDecoder::channelPcm)
        .function("foldChannelCount", &PushDecoder::foldChannelCount)
        .function("foldPcm", &PushDecoder::foldPcm)
        .function("objectCount", &PushDecoder::objectCount)
        .function("objectPosition", &PushDecoder::objectPosition)
        .function("objectAudioPcm", &PushDecoder::objectAudioPcm)
        .function("objectLabel", &PushDecoder::objectLabel);
}
