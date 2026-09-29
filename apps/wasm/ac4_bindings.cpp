// Embind wrapper around ac3::forge's AC-4 decode and encode paths (src/ac4,
// src/ac4dec, src/ac4enc), for the roadmap plan phase I4 bindings sweep. One
// combined module, unlike the AC-3 side's separate decode_bindings.cpp/
// encoder_bindings.cpp executables (apps/wasm/CMakeLists.txt's own comment on
// why AC-3 split them): the task this file was written for calls for "an
// AC-4 embind module beside the decode and encode modules", singular, and
// AC-4's decoder and encoder share one table-of-contents/framing library
// (ac4::ac4) regardless, so there is less to gain from a second executable
// here than there was splitting AC-3's decode-only and encode-only builds.
//
// Three JS-visible things:
//   - Ac4Decoder: wraps ac4::Decoder (src/ac4dec/include/ac4dec/decoder.hpp).
//   - Ac4Encoder: wraps ac4::Encoder (src/ac4enc/include/ac4enc/encoder.hpp).
//   - syncFrame: wraps ac4::sync_frame() (src/ac4enc, declared beside Encoder).
//
// Scope cut (the same "reasonable cost" cut used for every other binding in
// this task): every config knob that reaches this file is a flat, cheap-to-
// marshal primitive - what's deliberately left out is the deep, rarely-
// touched-from-a-UI structure either header carries: ObjectUpdate's ramp
// list (DecodedObject::updates - only the properties in force at the frame's
// first sample are returned, not the ramp), and on the encoder side
// EncoderConfig's loudness/drc/downmix/dialogue/substreams/presentations/
// experimental fields (every multi-substream, multi-presentation and DRC/
// loudness-metadata feature) - a caller who needs those still has the full
// C++ API; this binding is the common single-substream, single-presentation
// path, matching the "core config" subset every other binding in this task
// exposes for the encoder side.
//
// Two sentinel conventions cross the embind boundary, used because NEITHER
// existing AC-3 binding (decoder_bindings.cpp's PushDecoder, encoder_
// bindings.cpp's WasmEncoder/WasmAtmosBedEncoder/WasmQcMeter) takes an
// optional numeric constructor argument to copy a convention from - every
// constructor argument over there is a plain, always-present int/bool. These
// are introduced fresh here and documented in js/src/ac4.ts alongside them:
//   - An optional double (only OutputConfig::output_level_dbfs) is NaN for
//     "unset". This is not just a convenience: assigning a JS `undefined`
//     into a wasm heap Float64Array - which is what embind's generated
//     constructor invoker does for a `double` parameter - already coerces to
//     NaN by ordinary JS TypedArray semantics, so "NaN" and "undefined" are
//     the SAME wire value for a double parameter, not two conventions to
//     support separately.
//   - An optional int that is semantically non-negative (a presentation_id
//     or a table-of-contents index) uses -1 for "unset" instead: an int has
//     no NaN of its own, and coercing undefined into an Int32Array slot
//     gives 0 - which would collide with a real presentation_id/index of 0 -
//     so -1 is used, not 0, and not NaN.
//
// Every return shape is a hand-built emscripten::val::object()/val::array(),
// the same technique decoder_bindings.cpp and encoder_bindings.cpp both use
// throughout (neither uses emscripten::value_object<> anywhere).

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include "ac4/ac4.hpp"
#include "ac4dec/decoder.hpp"
#include "ac4enc/encoder.hpp"

namespace {

// --- Small shared helpers ---------------------------------------------------

// Copies `bytes` into a genuinely-owned JS Uint8Array (the `new Uint8Array(view)`
// idiom: the constructor call copies out of the view synchronously, so the
// result stays valid however long JS keeps it - unlike a typed_memory_view
// returned directly, which is only valid until this instance's next call.
// Needed here (unlike PushDecoder's single "valid until next call" views)
// because Ac4Encoder::encode()/flush() can return SEVERAL frames in one JS
// array at once: if every frame's `data` aliased one shared buffer, only the
// last one could ever be safely read.
emscripten::val make_uint8_array(const std::vector<std::byte>& bytes) {
    const emscripten::val view(
        emscripten::typed_memory_view(bytes.size(), reinterpret_cast<const std::uint8_t*>(bytes.data())));
    return emscripten::val::global("Uint8Array").new_(view);
}

emscripten::val make_number_array(const std::array<double, 3>& values) {
    auto arr = emscripten::val::array();
    for (std::size_t i = 0; i < values.size(); ++i) {
        arr.set(static_cast<unsigned>(i), values[i]);
    }
    return arr;
}

std::string_view object_kind_name(ac4::ObjectKind kind) {
    // ac4/ac4.hpp declares ObjectKind with no describe() of its own (unlike
    // DecodeError/DownmixTarget/DrcMode/DecodingMode/Speaker/SubstreamRole,
    // which ac4dec/decoder.hpp all give one) - a small local mapping, the
    // same "the library gives no describe() for this one" situation encoder_
    // bindings.cpp's own WasmLayout/acmod_for_layout helpers are already in.
    switch (kind) {
        case ac4::ObjectKind::kBed: return "bed";
        case ac4::ObjectKind::kDyn: return "dyn";
        case ac4::ObjectKind::kIsf: return "isf";
    }
    return "dyn";
}

// --- ac4::Decoder configuration from primitive embind arguments -------------

ac4::OutputConfig make_output_config(double output_level_dbfs, int drc, bool headphones,
                                      double dialogue_enhancement_db, int downmix, bool mix_lfe,
                                      double dialogue_gain_db, double associated_gain_db) {
    ac4::OutputConfig config;
    if (!std::isnan(output_level_dbfs)) {
        config.output_level_dbfs = output_level_dbfs;
    }
    config.drc = static_cast<ac4::DrcMode>(drc);
    config.headphones = headphones;
    config.dialogue_enhancement_db = dialogue_enhancement_db;
    config.downmix = static_cast<ac4::DownmixTarget>(downmix);
    config.mix_lfe = mix_lfe;
    config.dialogue_gain_db = dialogue_gain_db;
    config.associated_gain_db = associated_gain_db;
    return config;
}

ac4::PresentationChoice make_presentation_choice(int presentation_id, int presentation_index,
                                                  const std::string& language) {
    ac4::PresentationChoice choice;
    if (presentation_id >= 0) {
        choice.presentation_id = presentation_id;
    }
    if (presentation_index >= 0) {
        choice.index = static_cast<std::size_t>(presentation_index);
    }
    choice.language = language;
    return choice;
}

ac4::DecoderConfig make_decoder_config(double output_level_dbfs, int drc, int downmix, int decoding_mode,
                                        int concealment, int presentation_id, int presentation_index,
                                        const std::string& language, int level) {
    ac4::DecoderConfig config;
    // The constructor only takes the two OutputConfig fields the task this
    // was written for names explicitly (output level, DRC mode) plus
    // downmix target; the rest of OutputConfig - headphones, dialogue
    // enhancement, LFE mixing, dialogue/associated gain - keeps its own
    // struct defaults here and is reached post-construction via setOutput(),
    // which exposes the whole struct (OutputConfig is what a system changes
    // "from the next frame", as one unit, per decoder.hpp's own comment).
    config.output = make_output_config(output_level_dbfs, drc, /*headphones=*/false,
                                        /*dialogue_enhancement_db=*/0.0, downmix, /*mix_lfe=*/true,
                                        /*dialogue_gain_db=*/0.0, /*associated_gain_db=*/0.0);
    config.concealment = static_cast<ac4::ConcealmentPolicy>(concealment);
    config.presentation = make_presentation_choice(presentation_id, presentation_index, language);
    config.level = level;
    config.decoding = static_cast<ac4::DecodingMode>(decoding_mode);
    return config;
}

}  // namespace

class Ac4Decoder {
   public:
    Ac4Decoder(double output_level_dbfs, int drc, int downmix, int decoding_mode, int concealment,
               int presentation_id, int presentation_index, const std::string& language, int level)
        : decoder_(make_decoder_config(output_level_dbfs, drc, downmix, decoding_mode, concealment,
                                        presentation_id, presentation_index, language, level)) {}

    // Decodes one raw_ac4_frame (an ac4::SyncFrame's raw_ac4_frame, or an MP4
    // sample - the caller has already stripped any container/sync-frame
    // wrapper, the same input shape ac4::Decoder::decode() itself takes).
    // Null for a frame with no output (decoder.hpp's own decode(): waiting
    // for configuration no I-frame has sent yet) and for a decode error with
    // no concealment configured; refusalReason() says why in either case,
    // the same division of labour decode()'s own doc comment describes.
    emscripten::val decodeFrame(const emscripten::val& js_bytes) {
        const std::vector<std::uint8_t> raw = emscripten::vecFromJSArray<std::uint8_t>(js_bytes);
        const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(raw.data()), raw.size());
        try {
            auto result = decoder_.decode(bytes);
            if (!result || !result->has_value()) {
                return emscripten::val::null();
            }
            // Moved into a member, not read from the local `result`: the
            // channel/object Float32Array views describe_frame() builds
            // point into DecodedFrame's own vectors, which must outlive this
            // call (the usual "valid until next call" contract every other
            // PCM-view-returning method in apps/wasm/ already documents).
            last_frame_ = std::move(**result);
            return describe_frame(*last_frame_);
        } catch (const std::bad_alloc&) {
            return emscripten::val::null();
        }
    }

    void setOutput(double output_level_dbfs, int drc, bool headphones, double dialogue_enhancement_db,
                   int downmix, bool mix_lfe, double dialogue_gain_db, double associated_gain_db) {
        decoder_.set_output(make_output_config(output_level_dbfs, drc, headphones, dialogue_enhancement_db,
                                                downmix, mix_lfe, dialogue_gain_db, associated_gain_db));
    }

    void setPresentation(int presentation_id, int presentation_index, const std::string& language) {
        decoder_.set_presentation(make_presentation_choice(presentation_id, presentation_index, language));
    }

    void reset() {
        decoder_.reset();
        last_frame_.reset();
    }

    [[nodiscard]] std::string refusalReason() const { return std::string(decoder_.refusal_reason()); }

    [[nodiscard]] int latencySamples() const { return decoder_.latency_samples(); }

    // The presentations of the last frame read (decoder.hpp's own
    // presentations()); empty before one.
    [[nodiscard]] emscripten::val presentations() const {
        auto out = emscripten::val::array();
        const auto list = decoder_.presentations();
        for (std::size_t i = 0; i < list.size(); ++i) {
            const auto& info = list[i];
            auto entry = emscripten::val::object();
            entry.set("index", static_cast<unsigned>(info.index));
            entry.set("presentationId",
                      info.presentation_id ? emscripten::val(*info.presentation_id) : emscripten::val::null());
            entry.set("mdCompat", info.md_compat ? emscripten::val(*info.md_compat) : emscripten::val::null());
            entry.set("enabled", info.enabled);
            entry.set("alternative", info.alternative);
            entry.set("name", info.name);
            entry.set("language", info.language);
            entry.set("decodable", info.decodable);
            entry.set("selectable", info.selectable);
            auto speakers = emscripten::val::array();
            for (std::size_t s = 0; s < info.speakers.size(); ++s) {
                speakers.set(static_cast<unsigned>(s), std::string(ac4::describe(info.speakers[s])));
            }
            entry.set("speakers", speakers);
            out.set(static_cast<unsigned>(i), entry);
        }
        return out;
    }

   private:
    static emscripten::val describe_object(const ac4::DecodedObject& object) {
        auto result = emscripten::val::object();
        result.set("kind", std::string(object_kind_name(object.kind)));
        result.set("lfe", object.lfe);
        result.set("speaker",
                   object.speaker ? emscripten::val(std::string(ac4::describe(*object.speaker))) : emscripten::val::null());
        result.set("samples", emscripten::val(emscripten::typed_memory_view(object.samples.size(), object.samples.data())));

        // Annex F.2-F.10's current properties alone (what is in force at the
        // frame's first sample) - ObjectProperties::updates' ramp list is the
        // scope cut this file's header comment names.
        auto properties = emscripten::val::object();
        properties.set("active", object.properties.active);
        properties.set("gainDb", object.properties.gain_db);
        properties.set("position", make_number_array(object.properties.position));
        properties.set("priority", object.properties.priority);
        result.set("properties", properties);

        return result;
    }

    static emscripten::val describe_frame(const ac4::DecodedFrame& frame) {
        auto result = emscripten::val::object();
        result.set("sampleRate", frame.sample_rate_hz);
        result.set("sequenceCounter", frame.sequence_counter);
        result.set("presentation", static_cast<unsigned>(frame.presentation));
        result.set("presentationId",
                   frame.presentation_id ? emscripten::val(*frame.presentation_id) : emscripten::val::null());
        result.set("samples", static_cast<unsigned>(frame.samples));

        auto channels = emscripten::val::array();
        for (std::size_t i = 0; i < frame.channels.size(); ++i) {
            channels.set(static_cast<unsigned>(i), emscripten::val(emscripten::typed_memory_view(
                                                        frame.channels[i].size(), frame.channels[i].data())));
        }
        result.set("channels", channels);

        // Channel layout as strings, the same convention decoder_bindings.cpp's
        // channel_labels()/make_string_array() already uses for AC-3's own
        // channelLabels, rather than a numeric enum.
        auto speakers = emscripten::val::array();
        for (std::size_t i = 0; i < frame.speakers.size(); ++i) {
            speakers.set(static_cast<unsigned>(i), std::string(ac4::describe(frame.speakers[i])));
        }
        result.set("speakers", speakers);

        if (frame.concealed) {
            auto concealed = emscripten::val::object();
            concealed.set("error", std::string(ac4::describe(frame.concealed->error)));
            concealed.set("action", std::string(frame.concealed->action == ac4::ConcealmentAction::kRepeatFade
                                                     ? "repeatFade"
                                                     : "mute"));
            result.set("concealed", concealed);
        } else {
            result.set("concealed", emscripten::val::null());
        }

        auto objects = emscripten::val::array();
        for (std::size_t i = 0; i < frame.objects.size(); ++i) {
            objects.set(static_cast<unsigned>(i), describe_object(frame.objects[i]));
        }
        result.set("objects", objects);

        return result;
    }

    ac4::Decoder decoder_;
    // Kept alive so decodeFrame()'s returned Float32Array views (into
    // last_frame_.channels/.objects[].samples) stay valid until the next
    // decodeFrame()/reset() call, per this file's header comment.
    std::optional<ac4::DecodedFrame> last_frame_;
};

class Ac4Encoder {
   public:
    // The "core config" subset this task's encoder bindings all expose:
    // channels/sample_rate_hz/frame_rate_index/bitrate_kbps/rate_mode/
    // codec_mode/iframe_interval/dialnorm_db. Every other EncoderConfig field
    // (loudness/drc/downmix/dialogue/substreams/presentations/experimental)
    // keeps its struct default (unset/empty), the same scope cut named at
    // the top of this file.
    Ac4Encoder(int channels, int sample_rate_hz, int frame_rate_index, int bitrate_kbps, int rate_mode,
               int codec_mode, int iframe_interval, double dialnorm_db) {
        ac4::EncoderConfig config;
        config.channels = channels;
        config.sample_rate_hz = sample_rate_hz;
        config.frame_rate_index = frame_rate_index;
        config.bitrate_kbps = bitrate_kbps;
        config.rate_mode = static_cast<ac4::RateMode>(rate_mode);
        config.codec_mode = static_cast<ac4::CodecMode>(codec_mode);
        config.iframe_interval = iframe_interval;
        config.dialnorm_db = dialnorm_db;

        auto result = ac4::Encoder::create(config);
        if (!result) {
            // refusal_reason() "does create()'s work to find out" (encoder.hpp)
            // and names the specific rule broken; describe() is only a
            // fallback for the case it somehow comes back empty.
            ctor_error_ = std::string(ac4::Encoder::refusal_reason(config));
            if (ctor_error_.empty()) {
                ctor_error_ = std::string(ac4::describe(result.error()));
            }
            return;
        }
        encoder_.emplace(std::move(*result));
    }

    // Planar samples at full scale 1.0, one Float32Array per input channel -
    // the same shape encoder_bindings.cpp's copy_channels()/spans_of() take,
    // reimplemented locally here since this is a separate translation unit
    // (apps/wasm/decoder_bindings.cpp and encoder_bindings.cpp are likewise
    // each fully self-contained, sharing no helper header between them).
    emscripten::val encode(const emscripten::val& channels_js) {
        error_.clear();
        if (!encoder_) {
            error_ = ctor_error_;
            return emscripten::val::array();
        }
        const auto storage = copy_channels(channels_js);
        const auto spans = spans_of(storage);
        try {
            auto result = encoder_->encode(spans);
            if (!result) {
                error_ = std::string(ac4::describe(result.error()));
                return emscripten::val::array();
            }
            return describe_frames(*result);
        } catch (const std::bad_alloc&) {
            error_ = "out of memory encoding this frame";
            return emscripten::val::array();
        }
    }

    emscripten::val flush() {
        error_.clear();
        if (!encoder_) {
            error_ = ctor_error_;
            return emscripten::val::array();
        }
        try {
            auto result = encoder_->flush();
            if (!result) {
                error_ = std::string(ac4::describe(result.error()));
                return emscripten::val::array();
            }
            return describe_frames(*result);
        } catch (const std::bad_alloc&) {
            error_ = "out of memory flushing the encoder";
            return emscripten::val::array();
        }
    }

    // Not in this file's original method list, but added for the same reason
    // every existing WASM encoder class in this tree (WasmEncoder,
    // WasmAtmosBedEncoder) has one: encode()/flush() have nowhere else to
    // report a failure once the return type is a plain array rather than a
    // null-or-value like PushDecoder's PCM views - without this, a real
    // EncodeError::kInvalidInput would be silently indistinguishable from
    // "the encoder's delay simply had nothing to emit yet".
    [[nodiscard]] std::string error() const { return error_; }

    [[nodiscard]] int codecMode() const {
        return encoder_ ? static_cast<int>(encoder_->codec_mode()) : -1;
    }
    [[nodiscard]] int delaySamples() const { return encoder_ ? encoder_->delay_samples() : 0; }
    [[nodiscard]] int decoderDelaySamples() const { return encoder_ ? encoder_->decoder_delay_samples() : 0; }

    // The 'dac4' box for the stream as encoded so far (ac4::build_dac4());
    // empty where there is nothing to describe (construction failed, or
    // build_dac4() itself refuses - dac4Refusal() says which).
    [[nodiscard]] emscripten::val buildDac4() const {
        if (!encoder_) return make_uint8_array({});
        return make_uint8_array(ac4::build_dac4(encoder_->toc()));
    }

    [[nodiscard]] std::string dac4Refusal() const {
        if (!encoder_) return ctor_error_;
        return std::string(ac4::dac4_refusal(encoder_->toc()));
    }

   private:
    static std::vector<std::vector<float>> copy_channels(const emscripten::val& channels_js) {
        const int count = channels_js["length"].as<int>();
        std::vector<std::vector<float>> storage(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            storage[static_cast<std::size_t>(i)] = emscripten::vecFromJSArray<float>(channels_js[i]);
        }
        return storage;
    }

    static std::vector<std::span<const float>> spans_of(const std::vector<std::vector<float>>& storage) {
        std::vector<std::span<const float>> spans;
        spans.reserve(storage.size());
        for (const auto& channel : storage) {
            spans.emplace_back(channel);
        }
        return spans;
    }

    static emscripten::val describe_frames(const std::vector<ac4::EncodedFrame>& frames) {
        auto out = emscripten::val::array();
        for (std::size_t i = 0; i < frames.size(); ++i) {
            auto entry = emscripten::val::object();
            // A genuine copy per frame (make_uint8_array), not a shared
            // "valid until next call" view: encode() can return several
            // frames in one call, all alive in the same JS array at once.
            entry.set("data", make_uint8_array(frames[i].raw_ac4_frame));
            entry.set("samples", frames[i].samples);
            entry.set("iframe", frames[i].iframe);
            out.set(static_cast<unsigned>(i), entry);
        }
        return out;
    }

    std::optional<ac4::Encoder> encoder_;
    std::string ctor_error_;
    std::string error_;
};

// ac4::sync_frame(): the sync word, optional CRC, frame_size and the raw
// frame - for a raw .ac4 file or MPEG-2 TS, wrapping one of Ac4Encoder's
// `data` frames (or any other raw_ac4_frame the caller already has).
emscripten::val syncFrame(const emscripten::val& js_bytes, bool crc) {
    const std::vector<std::uint8_t> raw = emscripten::vecFromJSArray<std::uint8_t>(js_bytes);
    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(raw.data()), raw.size());
    return make_uint8_array(ac4::sync_frame(bytes, crc));
}

EMSCRIPTEN_BINDINGS(ac3forge_wasm_ac4) {
    emscripten::function("syncFrame", &syncFrame);

    emscripten::class_<Ac4Decoder>("Ac4Decoder")
        .constructor<double, int, int, int, int, int, int, std::string, int>()
        .function("decodeFrame", &Ac4Decoder::decodeFrame)
        .function("setOutput", &Ac4Decoder::setOutput)
        .function("setPresentation", &Ac4Decoder::setPresentation)
        .function("reset", &Ac4Decoder::reset)
        .function("refusalReason", &Ac4Decoder::refusalReason)
        .function("latencySamples", &Ac4Decoder::latencySamples)
        .function("presentations", &Ac4Decoder::presentations);

    emscripten::class_<Ac4Encoder>("Ac4Encoder")
        .constructor<int, int, int, int, int, int, int, double>()
        .function("encode", &Ac4Encoder::encode)
        .function("flush", &Ac4Encoder::flush)
        .function("error", &Ac4Encoder::error)
        .function("codecMode", &Ac4Encoder::codecMode)
        .function("delaySamples", &Ac4Encoder::delaySamples)
        .function("decoderDelaySamples", &Ac4Encoder::decoderDelaySamples)
        .function("buildDac4", &Ac4Encoder::buildDac4)
        .function("dac4Refusal", &Ac4Encoder::dac4Refusal);
}
