// Embind wrapper around ac3::forge's encode path, for the roadmap-UX6
// browser encode page (apps/wasm/encode/index.html). Three JS-visible
// classes:
//   - WasmEncoder: real AC-3 (ac3::FrameEncoder) or E-AC-3
//     (ac3::eac3::FrameEncoder) bed encoding, frame by frame.
//   - WasmAtmosBedEncoder: real Atmos/JOC bed encoding
//     (ac3::oba::AtmosEncoder). Bound now so the module's binding surface is
//     complete even though encode/app.js does not build an object-authoring
//     UI on top of it yet - see docs/platforms/wasm.md's own note on why.
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

#include "ac3/core/tables.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/encoder/eac3_frame.hpp"
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

// The three coding modes encode/app.js exposes - the layouts a dropped WAV's
// WAVEFORMATEXTENSIBLE channel order can be reordered into with confidence
// (see app.js's own comment on that mapping). Wider layouts (7.1, height
// channels) are a fast-follow, not this page.
enum class WasmLayout : int { kMono = 0, kStereo = 1, k5_1 = 2 };

ac3::Acmod acmod_for_layout(int layout) {
    switch (static_cast<WasmLayout>(layout)) {
        case WasmLayout::kMono: return ac3::Acmod::k1_0;
        case WasmLayout::kStereo: return ac3::Acmod::k2_0;
        case WasmLayout::k5_1: return ac3::Acmod::k3_2;
    }
    return ac3::Acmod::k2_0;
}

bool lfe_for_layout(int layout) { return static_cast<WasmLayout>(layout) == WasmLayout::k5_1; }

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
    // format: 0 = AC-3, 1 = E-AC-3. layout: WasmLayout above.
    WasmEncoder(int format, int layout, int sample_rate_hz, int bitrate_kbps)
        : acmod_(acmod_for_layout(layout)), lfe_(lfe_for_layout(layout)), is_eac3_(format != 0) {
        if (is_eac3_) {
            ac3::eac3::FrameConfig cfg;
            cfg.sample_rate = sample_rate_for_hz(sample_rate_hz);
            cfg.bitrate_kbps = static_cast<std::uint32_t>(bitrate_kbps);
            cfg.acmod = acmod_;
            cfg.lfe = lfe_;
            eac3_.emplace(cfg);
        } else {
            ac3::EncoderConfig cfg;
            cfg.sample_rate = sample_rate_for_hz(sample_rate_hz);
            cfg.bitrate_kbps = static_cast<std::uint32_t>(bitrate_kbps);
            cfg.acmod = acmod_;
            cfg.lfe = lfe_;
            ac3_.emplace(cfg);
        }
    }

    [[nodiscard]] int samplesPerFrame() const { return ac3::kSamplesPerFrame; }
    [[nodiscard]] int channelCount() const {
        return ac3::fullbw_channel_count(acmod_) + (lfe_ ? 1 : 0);
    }
    [[nodiscard]] bool hasLfe() const { return lfe_; }

    // Returns one syncframe's bytes as a Uint8Array, or null on failure -
    // call error() for why. The returned view points into last_frame_ and is
    // only valid until the next encodeFrame() call, same contract
    // decoder_bindings.cpp's own PCM/energy views document - copy it out in
    // JS immediately.
    emscripten::val encodeFrame(const emscripten::val& channels_js) {
        error_.clear();
        const auto storage = copy_channels(channels_js);
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
    ac3::Acmod acmod_;
    bool lfe_;
    bool is_eac3_;
    std::optional<ac3::FrameEncoder> ac3_;
    std::optional<ac3::eac3::FrameEncoder> eac3_;
    std::vector<std::byte> last_frame_;
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

class WasmQcMeter {
   public:
    WasmQcMeter(int layout, int sample_rate_hz)
        : meter_(sample_rate_for_hz(sample_rate_hz), acmod_for_layout(layout), lfe_for_layout(layout)) {}

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
        .constructor<int, int, int, int>()
        .function("samplesPerFrame", &WasmEncoder::samplesPerFrame)
        .function("channelCount", &WasmEncoder::channelCount)
        .function("hasLfe", &WasmEncoder::hasLfe)
        .function("encodeFrame", &WasmEncoder::encodeFrame)
        .function("error", &WasmEncoder::error);

    emscripten::class_<WasmAtmosBedEncoder>("AtmosBedEncoder")
        .constructor<int, int, int>()
        .function("samplesPerFrame", &WasmAtmosBedEncoder::samplesPerFrame)
        .function("objectCount", &WasmAtmosBedEncoder::objectCount)
        .function("encodeFrame", &WasmAtmosBedEncoder::encodeFrame)
        .function("error", &WasmAtmosBedEncoder::error);

    emscripten::class_<WasmQcMeter>("QcMeter")
        .constructor<int, int>()
        .function("push", &WasmQcMeter::push)
        .function("integratedLkfs", &WasmQcMeter::integratedLkfs)
        .function("momentaryLkfs", &WasmQcMeter::momentaryLkfs)
        .function("shortTermLkfs", &WasmQcMeter::shortTermLkfs)
        .function("truePeakDbtp", &WasmQcMeter::truePeakDbtp)
        .function("verdicts", &WasmQcMeter::verdicts);
}
