// Embind wrapper around ac3::forge's encode path, for the roadmap-UX6
// browser encode page (apps/wasm/encode/index.html). Three JS-visible
// classes:
//   - WasmEncoder: real AC-3 (ac3::FrameEncoder) or E-AC-3
//     (ac3::eac3::FrameEncoder) bed encoding, frame by frame.
//   - WasmAtmosBedEncoder: real Atmos/JOC bed encoding
//     (ac3::oba::AtmosEncoder) - the class the object-authoring page
//     (apps/wasm/atmos/) drives, one placement set per frame so a drag on
//     its room canvas IS the pan.
//   - WasmQcMeter: ac3::meta::LoudnessMeter plus a verdict against every
//     ac3::meta::QcPresetId - the "browser-side qc... in the page" roadmap
//     UX6 asks for, not a separate page.
//
// Every class does the real thing: encode_frame() calls the real codec, the
// same functions ac3cli/ac3::forge_c/the Python bindings call, not a
// reimplementation for the browser.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/plan.hpp"
#include "ac3/encoder/silent_frame.hpp"
#include "ac3/meta/loudness.hpp"
#include "ac3/meta/qc.hpp"
#include "ac3/oba/atmos.hpp"

namespace {

// FrameError has no library-wide describe() (unlike DecodeError/ScanError/...
// - see ac3/decoder/decoder.hpp, ac3/io/elementary.hpp): every case below is
// a configuration-validation failure a well-behaved caller does not hit in
// practice, and every case's own doc comment (ac3/encoder/silent_frame.hpp)
// already says what it means. A page-error-message translation stays local
// to this binding rather than becoming AC3FORGE_EXPORT library API for a
// message nothing else in the tree needs yet.
std::string_view describe_frame_error(ac3::FrameError error) {
    switch (error) {
        case ac3::FrameError::kInvalidBitrate: return "bitrate is not valid for this configuration";
        case ac3::FrameError::kInvalidDialnorm: return "dialnorm is out of range (1..31)";
        case ac3::FrameError::kInvalidSubstream: return "invalid substream configuration";
        case ac3::FrameError::kInvalidChannelMap: return "channel map does not match the coding mode";
        case ac3::FrameError::kTooManyChannels: return "too many rendered channel locations";
        case ac3::FrameError::kInvalidMixLevel: return "invalid mixing-metadata level";
        case ac3::FrameError::kInvalidBsi: return "invalid bit stream information field";
        case ac3::FrameError::kInvalidObjectAudio: return "invalid object-audio configuration";
    }
    return "unknown encode error";
}

// The coding modes encode/app.js exposes. 0-2 are the single-substream
// layouts a plain FrameEncoder codes; 3-5 are the wide layouts that take an
// AccessUnitEncoder (a 5.1 bed plus dependent substreams - E-AC-3 only, the
// same constraint ac3cli's own eac3-encode layouts carry). The WAV-side
// channel identification mirrors ac3::plan's generic_wav_layout: 8 channels
// reads as 7.1, 10 as 5.1.4, 12 as 7.1.4 (the commoner delivery layout at
// each ambiguous count), and codedOrderForWav() below hands JS the exact
// reorder so the mapping logic lives here once, beside the plan code that
// defines it, instead of being restated in JavaScript.
enum class WasmLayout : int {
    kMono = 0,
    kStereo = 1,
    k5_1 = 2,
    k7_1 = 3,
    k5_1_4 = 4,
    k7_1_4 = 5,
};

bool is_wide_layout(int layout) { return layout >= static_cast<int>(WasmLayout::k7_1); }

ac3::plan::LayoutId layout_id_for(int layout) {
    switch (static_cast<WasmLayout>(layout)) {
        case WasmLayout::k7_1: return ac3::plan::LayoutId::k71;
        case WasmLayout::k5_1_4: return ac3::plan::LayoutId::k514;
        case WasmLayout::k7_1_4: return ac3::plan::LayoutId::k714;
        default: return ac3::plan::LayoutId::k51;
    }
}

// Narrow path only - the wide layouts go through ac3::plan below and never
// consult these.
ac3::Acmod acmod_for_layout(int layout) {
    switch (static_cast<WasmLayout>(layout)) {
        case WasmLayout::kMono: return ac3::Acmod::k1_0;
        case WasmLayout::kStereo: return ac3::Acmod::k2_0;
        case WasmLayout::k5_1:
        case WasmLayout::k7_1:
        case WasmLayout::k5_1_4:
        case WasmLayout::k7_1_4: return ac3::Acmod::k3_2;
    }
    return ac3::Acmod::k2_0;
}

bool lfe_for_layout(int layout) { return layout >= static_cast<int>(WasmLayout::k5_1); }

ac3::SampleRate sample_rate_for_hz(int hz) {
    switch (hz) {
        case 44100: return ac3::SampleRate::k44100;
        case 32000: return ac3::SampleRate::k32000;
        default: return ac3::SampleRate::k48000;
    }
}

// channels_js: a JS Array of per-channel Float32Array, AC-3 Table 5.8 order
// (LFE last when present). Copies out into owned storage - vecFromJSArray
// takes ownership of nothing on the JS side, so the spans below stay valid
// only as long as `storage` does, which the caller keeps alive for exactly
// the one encode_frame() call that reads them.
std::vector<std::vector<float>> copy_channels(const emscripten::val& channels_js) {
    const int count = channels_js["length"].as<int>();
    std::vector<std::vector<float>> storage(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        storage[static_cast<std::size_t>(i)] = emscripten::vecFromJSArray<float>(channels_js[i]);
    }
    return storage;
}

std::vector<std::span<const float>> spans_of(const std::vector<std::vector<float>>& storage) {
    std::vector<std::span<const float>> spans;
    spans.reserve(storage.size());
    for (const auto& channel : storage) {
        spans.emplace_back(channel);
    }
    return spans;
}

emscripten::val optional_to_val(std::optional<double> value) {
    return value ? emscripten::val(*value) : emscripten::val::null();
}

}  // namespace

class WasmEncoder {
   public:
    // format: 0 = AC-3, 1 = E-AC-3. layout: WasmLayout above. dialnorm:
    // §5.4.2.8's 1..31 - the page derives it from the QC pass's own measured
    // integrated loudness (dialnorm = round(-LKFS), clamped) instead of
    // shipping the unmeasured default 31, which would leave a real decoder's
    // normalisation under-attenuating loud content this page produced.
    //
    // The wide layouts (7.1/5.1.4/7.1.4) build an ac3::eac3::
    // AccessUnitEncoder from ac3::plan's own config and ROUTE the source
    // onto it (ac3::plan::route/render - the same direction-based placement
    // ac3cli itself uses), so encodeFrame() takes the source's channels in
    // plain WAV order at ANY width for those layouts: a stereo source aimed
    // at 7.1.4 is panned onto it, a 12-channel source is carried. The
    // channel-order knowledge stays in the plan code that defines it,
    // instead of being restated in JavaScript.
    WasmEncoder(int format, int layout, int sample_rate_hz, int bitrate_kbps, int dialnorm)
        : acmod_(acmod_for_layout(layout)), lfe_(lfe_for_layout(layout)), is_eac3_(format != 0),
          wide_(is_wide_layout(layout)) {
        if (wide_) {
            plan_.codec = ac3::plan::Codec::kEac3;
            plan_.layout = layout_id_for(layout);
            plan_.sample_rate = sample_rate_for_hz(sample_rate_hz);
            plan_.bitrate_kbps = static_cast<std::uint32_t>(bitrate_kbps);
            plan_.meta.dialnorm = dialnorm;
            if (const auto invalid = ac3::plan::validate(plan_)) {
                ctor_error_ = std::string(ac3::plan::describe(*invalid));
                return;
            }
            access_unit_.emplace(ac3::plan::eac3_config(plan_));
        } else if (is_eac3_) {
            ac3::eac3::FrameConfig cfg;
            cfg.sample_rate = sample_rate_for_hz(sample_rate_hz);
            cfg.bitrate_kbps = static_cast<std::uint32_t>(bitrate_kbps);
            cfg.acmod = acmod_;
            cfg.lfe = lfe_;
            cfg.dialnorm = dialnorm;
            eac3_.emplace(cfg);
        } else {
            ac3::EncoderConfig cfg;
            cfg.sample_rate = sample_rate_for_hz(sample_rate_hz);
            cfg.bitrate_kbps = static_cast<std::uint32_t>(bitrate_kbps);
            cfg.acmod = acmod_;
            cfg.lfe = lfe_;
            cfg.dialnorm = dialnorm;
            ac3_.emplace(cfg);
        }
    }

    [[nodiscard]] int samplesPerFrame() const { return ac3::kSamplesPerFrame; }
    // The channel count encodeFrame() expects. Narrow: the coded layout's
    // own count, exactly as before. Wide: 0 until the first frame fixes the
    // SOURCE width (any WAV width routes), then that width.
    [[nodiscard]] int channelCount() const {
        if (wide_) {
            return routing_ ? routing_->source_channels : 0;
        }
        return ac3::fullbw_channel_count(acmod_) + (lfe_ ? 1 : 0);
    }
    [[nodiscard]] bool hasLfe() const { return lfe_; }
    // Wide path only: whether the source took route()'s exact-match path -
    // its width equals the layout's rendered channel count, so every source
    // channel reaches its own speaker untouched - or was rendered onto the
    // layout by direction (panned). The page reports which, so "encoded
    // 7.1.4" never silently means "panned my stereo file around a 12-speaker
    // room" without saying so. NOT Routing::is_permutation(): for 7.1/7.1.4
    // the bed's surrounds additionally fold side+rear together (the 5.1
    // fallback a dependent-substream stream structurally carries), which is
    // part of carrying the layout, not a pan of the source.
    [[nodiscard]] bool sourceWasCarried() const {
        return routing_ && routing_->source_channels == rendered_channels_;
    }

    // Returns one syncframe's bytes as a Uint8Array, or null on failure -
    // call error() for why. The returned view points into last_frame_ and is
    // only valid until the next encodeFrame() call, same contract
    // decoder_bindings.cpp's own PCM/energy views document - copy it out in
    // JS immediately.
    emscripten::val encodeFrame(const emscripten::val& channels_js) {
        error_.clear();
        if (!ctor_error_.empty()) {
            error_ = ctor_error_;
            return emscripten::val::null();
        }
        const auto storage = copy_channels(channels_js);
        if (wide_) {
            return encodeWideFrame(storage);
        }
        if (static_cast<int>(storage.size()) != channelCount()) {
            error_ = "expected " + std::to_string(channelCount()) + " channel(s), got " +
                      std::to_string(storage.size());
            return emscripten::val::null();
        }
        for (const auto& channel : storage) {
            if (static_cast<int>(channel.size()) != samplesPerFrame()) {
                error_ = "each channel must be exactly " + std::to_string(samplesPerFrame()) +
                          " samples";
                return emscripten::val::null();
            }
        }
        const auto spans = spans_of(storage);
        try {
            if (is_eac3_) {
                auto result = eac3_->encode_frame(spans);
                if (!result) {
                    error_ = std::string(describe_frame_error(result.error()));
                    return emscripten::val::null();
                }
                last_frame_ = std::move(*result);
            } else {
                auto result = ac3_->encode_frame(spans);
                if (!result) {
                    error_ = std::string(describe_frame_error(result.error()));
                    return emscripten::val::null();
                }
                last_frame_ = std::move(*result);
            }
        } catch (const std::bad_alloc&) {
            error_ = "out of memory encoding this frame";
            return emscripten::val::null();
        }
        return emscripten::val(emscripten::typed_memory_view(
            last_frame_.size(), reinterpret_cast<const std::uint8_t*>(last_frame_.data())));
    }

    [[nodiscard]] std::string error() const { return error_; }

   private:
    emscripten::val encodeWideFrame(const std::vector<std::vector<float>>& storage) {
        const auto source_channels = storage.size();
        for (const auto& channel : storage) {
            if (static_cast<int>(channel.size()) != samplesPerFrame()) {
                error_ = "each channel must be exactly " + std::to_string(samplesPerFrame()) +
                          " samples";
                return emscripten::val::null();
            }
        }
        // The routing is fixed by the FIRST frame's source width - a stream
        // whose channel count changes mid-file is not a thing WAV or a
        // capture endpoint produces.
        if (!routing_ || routing_->source_channels != static_cast<int>(source_channels)) {
            if (routing_) {
                error_ = "the source's channel count changed mid-stream";
                return emscripten::val::null();
            }
            const auto resolved = ac3::plan::resolve(plan_);
            auto routing = ac3::plan::route(resolved, source_channels, plan_.meta.cmixlev,
                                            plan_.meta.surmixlev);
            if (!routing) {
                error_ = "no standard speaker layout has " + std::to_string(source_channels) +
                          " channels";
                return emscripten::val::null();
            }
            routing_ = std::move(*routing);
            rendered_channels_ = static_cast<int>(ac3::plan::rendered_channel_count(resolved));
            coded_storage_.assign(static_cast<std::size_t>(routing_->coded_channels),
                                  std::vector<float>(static_cast<std::size_t>(samplesPerFrame())));
        }
        const auto source_spans = spans_of(storage);
        std::vector<std::span<float>> coded_spans;
        coded_spans.reserve(coded_storage_.size());
        for (auto& channel : coded_storage_) {
            coded_spans.emplace_back(channel);
        }
        ac3::plan::render(*routing_, source_spans, coded_spans,
                          static_cast<std::size_t>(samplesPerFrame()));

        const auto coded_views = spans_of(coded_storage_);
        try {
            auto result = access_unit_->encode_access_unit(coded_views, {});
            if (!result) {
                error_ = std::string(describe_frame_error(result.error()));
                return emscripten::val::null();
            }
            last_frame_ = std::move(result->bytes);
        } catch (const std::bad_alloc&) {
            error_ = "out of memory encoding this frame";
            return emscripten::val::null();
        }
        return emscripten::val(emscripten::typed_memory_view(
            last_frame_.size(), reinterpret_cast<const std::uint8_t*>(last_frame_.data())));
    }

    ac3::Acmod acmod_;
    bool lfe_;
    bool is_eac3_;
    bool wide_ = false;
    ac3::plan::Plan plan_{};
    std::optional<ac3::FrameEncoder> ac3_;
    std::optional<ac3::eac3::FrameEncoder> eac3_;
    std::optional<ac3::eac3::AccessUnitEncoder> access_unit_;
    std::optional<ac3::plan::Routing> routing_;
    int rendered_channels_ = -1;
    std::vector<std::vector<float>> coded_storage_;
    std::vector<std::byte> last_frame_;
    std::string ctor_error_;
    std::string error_;
};

class WasmAtmosBedEncoder {
   public:
    WasmAtmosBedEncoder(int sample_rate_hz, int bitrate_kbps, int object_count)
        : encoder_(make_config(sample_rate_hz, bitrate_kbps), object_count),
          object_count_(object_count) {}

    [[nodiscard]] int samplesPerFrame() const { return ac3::kSamplesPerFrame; }
    [[nodiscard]] int objectCount() const { return object_count_; }

    // objects_js: JS Array of objectCount() mono Float32Array (one per
    // object, samplesPerFrame() long). placements_js: JS Array of
    // objectCount() {x, y, z, gain} plain objects - room-anchored position
    // (TS 103 420 §4.2.1) and linear gain; extent/lfeSend stay at
    // ObjectPlacement's own point-source defaults for this first pass (see
    // ac3/oba/atmos.hpp - a later object-authoring UI is what would expose
    // them). Returns the encoded access unit's bytes (Uint8Array) or null -
    // see error(). Same "valid until next call" view contract as
    // WasmEncoder::encodeFrame.
    emscripten::val encodeFrame(const emscripten::val& objects_js, const emscripten::val& placements_js) {
        error_.clear();
        const auto storage = copy_channels(objects_js);
        if (static_cast<int>(storage.size()) != object_count_) {
            error_ = "expected " + std::to_string(object_count_) + " object(s), got " +
                      std::to_string(storage.size());
            return emscripten::val::null();
        }
        for (const auto& object : storage) {
            if (static_cast<int>(object.size()) != samplesPerFrame()) {
                error_ = "each object must be exactly " + std::to_string(samplesPerFrame()) +
                          " samples";
                return emscripten::val::null();
            }
        }
        const auto spans = spans_of(storage);

        std::vector<ac3::oba::ObjectPlacement> placement(static_cast<std::size_t>(object_count_));
        for (int i = 0; i < object_count_; ++i) {
            const emscripten::val entry = placements_js[i];
            placement[static_cast<std::size_t>(i)].position = {
                .x = entry["x"].as<double>(), .y = entry["y"].as<double>(), .z = entry["z"].as<double>()};
            placement[static_cast<std::size_t>(i)].gain = entry["gain"].as<double>();
        }

        try {
            auto result = encoder_.encode_frame(spans, placement);
            if (!result) {
                error_ = std::string(describe_frame_error(result.error()));
                return emscripten::val::null();
            }
            last_frame_ = std::move(result->bytes);
        } catch (const std::bad_alloc&) {
            error_ = "out of memory encoding this frame";
            return emscripten::val::null();
        }
        return emscripten::val(emscripten::typed_memory_view(
            last_frame_.size(), reinterpret_cast<const std::uint8_t*>(last_frame_.data())));
    }

    [[nodiscard]] std::string error() const { return error_; }

   private:
    static ac3::oba::AtmosConfig make_config(int sample_rate_hz, int bitrate_kbps) {
        ac3::oba::AtmosConfig cfg;
        cfg.sample_rate = sample_rate_for_hz(sample_rate_hz);
        cfg.bitrate_kbps = static_cast<std::uint32_t>(bitrate_kbps);
        return cfg;
    }

    ac3::oba::AtmosEncoder encoder_;
    int object_count_;
    std::vector<std::byte> last_frame_;
    std::string error_;
};

// The rendered Table E2.5 location mask for a wide layout - the bed plus
// every dependent's additions, the set BS.1770-5's extended algorithm meters.
std::uint16_t rendered_mask_for(int layout) {
    const auto plan = ac3::plan::channel_plan_for(layout_id_for(layout));
    std::uint16_t mask = ac3::eac3::chanmap::acmod_map(plan.bed_acmod, plan.bed_lfe);
    for (const auto chanmap : plan.dependents) {
        mask |= chanmap;
    }
    return mask;
}

class WasmQcMeter {
   public:
    // Narrow layouts meter by acmod (BS.1770-4 Annex 1), exactly as before.
    // The wide layouts meter the RENDERED location set (BS.1770-5 Annex 3's
    // extended algorithm over the bed plus every dependent's additions) -
    // push() then expects that set's own Table E2.5 bit order, which
    // meterOrderForWav() below maps a WAV's channel order onto so the
    // ordering knowledge stays here rather than restated in JavaScript.
    WasmQcMeter(int layout, int sample_rate_hz)
        : meter_(is_wide_layout(layout)
                     ? ac3::meta::LoudnessMeter(sample_rate_for_hz(sample_rate_hz),
                                                ac3::eac3::chanmap::expand(
                                                    rendered_mask_for(layout)))
                     : ac3::meta::LoudnessMeter(sample_rate_for_hz(sample_rate_hz),
                                                acmod_for_layout(layout),
                                                lfe_for_layout(layout))) {}

    // For a WAV whose channel count matches `layout`'s rendered width:
    // meterIndex -> wavIndex, so JS builds push()'s spans as
    // wav[order[i]]. Empty for the narrow layouts (their WAV order is
    // handled by app.js's existing 5.1 map).
    static emscripten::val meterOrderForWav(int layout) {
        emscripten::val out = emscripten::val::array();
        if (!is_wide_layout(layout)) {
            return out;
        }
        const auto locations = ac3::eac3::chanmap::expand(rendered_mask_for(layout));
        const std::span<const ac3::eac3::chanmap::Location> span{
            locations.items.data(), static_cast<std::size_t>(locations.count)};
        // wav_order: for WAV slot w, which index into `locations`. Invert it
        // so meter slot m knows which WAV channel to read.
        const auto wav_slots = ac3::plan::wav_order(span);
        std::vector<int> meter_to_wav(wav_slots.size(), 0);
        for (std::size_t wav_slot = 0; wav_slot < wav_slots.size(); ++wav_slot) {
            meter_to_wav[wav_slots[wav_slot]] = static_cast<int>(wav_slot);
        }
        for (std::size_t i = 0; i < meter_to_wav.size(); ++i) {
            out.call<void>("push", emscripten::val(meter_to_wav[i]));
        }
        return out;
    }

    // channels_js: the same AC-3-order channel layout WasmEncoder::encodeFrame
    // takes - any number of samples per call, not just one frame's worth
    // (LoudnessMeter::push's own contract).
    void push(const emscripten::val& channels_js) {
        const auto storage = copy_channels(channels_js);
        const auto spans = spans_of(storage);
        meter_.push(spans);
    }

    [[nodiscard]] emscripten::val integratedLkfs() const { return optional_to_val(meter_.integrated_lkfs()); }
    [[nodiscard]] emscripten::val momentaryLkfs() const { return optional_to_val(meter_.momentary_lkfs()); }
    [[nodiscard]] emscripten::val shortTermLkfs() const { return optional_to_val(meter_.short_term_lkfs()); }
    [[nodiscard]] emscripten::val truePeakDbtp() const { return optional_to_val(meter_.true_peak_dbtp()); }

    // One verdict object per ac3::meta::kQcPresetIds entry - the delivery-
    // preset pass/fail table encode/app.js renders. integrated_lkfs() is a
    // gated, whole-programme measure (std::nullopt until enough of the
    // programme has passed BS.1770's absolute gate), so every verdict here
    // is necessarily an END-OF-CAPTURE readout, not a live one - see
    // docs/platforms/wasm.md's QC section for why momentary/short-term above
    // are the live-updating numbers instead.
    [[nodiscard]] emscripten::val verdicts() const {
        emscripten::val out = emscripten::val::array();
        for (const ac3::meta::QcPresetId id : ac3::meta::kQcPresetIds) {
            const ac3::meta::QcPreset preset = ac3::meta::qc_preset(id);
            const ac3::meta::QcVerdict verdict =
                ac3::meta::evaluate_qc_gate(preset, meter_.integrated_lkfs(), meter_.true_peak_dbtp());
            emscripten::val row = emscripten::val::object();
            row.set("preset", std::string(ac3::meta::qc_preset_name(id)));
            row.set("source", std::string(preset.source));
            row.set("targetLkfs", preset.target_lkfs);
            row.set("toleranceLu", preset.tolerance_lu);
            row.set("isCeiling", preset.loudness_limit == ac3::meta::QcLoudnessLimit::kCeiling);
            row.set("maxTruePeakDbtp", preset.max_true_peak_dbtp);
            row.set("loudnessDeltaLu", optional_to_val(verdict.loudness_delta_lu));
            row.set("loudnessPass", verdict.loudness_pass);
            row.set("truePeakMarginDbtp", optional_to_val(verdict.true_peak_margin_dbtp));
            row.set("truePeakPass", verdict.true_peak_pass);
            row.set("pass", verdict.pass());
            out.call<void>("push", row);
        }
        return out;
    }

   private:
    ac3::meta::LoudnessMeter meter_;
};

EMSCRIPTEN_BINDINGS(ac3forge_wasm_encode) {
    emscripten::class_<WasmEncoder>("Encoder")
        .constructor<int, int, int, int, int>()
        .function("samplesPerFrame", &WasmEncoder::samplesPerFrame)
        .function("channelCount", &WasmEncoder::channelCount)
        .function("hasLfe", &WasmEncoder::hasLfe)
        .function("sourceWasCarried", &WasmEncoder::sourceWasCarried)
        .function("encodeFrame", &WasmEncoder::encodeFrame)
        .function("error", &WasmEncoder::error);

    emscripten::class_<WasmAtmosBedEncoder>("AtmosBedEncoder")
        .constructor<int, int, int>()
        .function("samplesPerFrame", &WasmAtmosBedEncoder::samplesPerFrame)
        .function("objectCount", &WasmAtmosBedEncoder::objectCount)
        .function("encodeFrame", &WasmAtmosBedEncoder::encodeFrame)
        .function("error", &WasmAtmosBedEncoder::error);

    emscripten::class_<WasmQcMeter>("QcMeter")
        .class_function("meterOrderForWav", &WasmQcMeter::meterOrderForWav)
        .constructor<int, int>()
        .function("push", &WasmQcMeter::push)
        .function("integratedLkfs", &WasmQcMeter::integratedLkfs)
        .function("momentaryLkfs", &WasmQcMeter::momentaryLkfs)
        .function("shortTermLkfs", &WasmQcMeter::shortTermLkfs)
        .function("truePeakDbtp", &WasmQcMeter::truePeakDbtp)
        .function("verdicts", &WasmQcMeter::verdicts);
}
