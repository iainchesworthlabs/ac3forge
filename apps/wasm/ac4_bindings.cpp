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
//   - Ac4Decoder: wraps ac4::Decoder (src/ac4dec/include/iclforge/ac4dec/decoder.hpp).
//   - Ac4Encoder: wraps ac4::Encoder (src/ac4enc/include/iclforge/ac4enc/encoder.hpp).
//   - syncFrame: wraps ac4::sync_frame() (src/ac4enc, declared beside Encoder).
//
// Scope cut (the same "reasonable cost" cut used for every other binding in
// this task): what is left out is the deep, rarely-touched-from-a-UI
// structure either header carries - on the encoder side EncoderConfig's
// loudness/drc/downmix/dialogue/substreams/presentations fields and the
// drc_gains and three_zero experimental flags (every multi-substream,
// multi-presentation and DRC/loudness-metadata feature); a caller who needs
// those still has the full C++ API. The encoder takes the core fields, the
// I-frame lists, the experimental flags that need no nested group and one
// object substream (A-JOC or direct-coded, ObjectsConfig) with its metadata
// updates; the decoder returns each object's properties and the block
// updates within the frame (DecodedObject::updates).
//
// Two conventions cross the embind boundary:
//   - The decoder's constructor and setters take flat primitives, with two
//     sentinels because neither existing AC-3 binding (decoder_bindings.cpp's
//     PushDecoder, encoder_bindings.cpp's WasmEncoder/WasmAtmosBedEncoder/
//     WasmQcMeter) takes an optional numeric argument to copy a convention
//     from. An optional double (only OutputConfig::output_level_dbfs) is NaN
//     for "unset": assigning a JS `undefined` into a wasm heap Float64Array -
//     which is what embind's generated invoker does for a `double` parameter -
//     already coerces to NaN, so "NaN" and "undefined" are the SAME wire value
//     for a double parameter. An optional non-negative int (a presentation_id
//     or a table-of-contents index) uses -1: an int has no NaN of its own, and
//     undefined coerces to 0, which would collide with a real id of 0.
//   - The encoder's constructor and the metadata updates are plain JS
//     objects (an emscripten::val), because the object substream is a list of
//     objects each with its own metadata and no flat parameter list holds
//     that. A field the object lacks, or holds undefined or null, keeps the
//     C++ struct's default, so the defaults live here in one place: js/src/
//     ac4.ts passes the caller's options as given. An enumerator the C++
//     header does not define is refused at construction (constructionError()
//     says so), as ac3forge_ac4_encoder_create() refuses one.
//
// Every return shape is a hand-built emscripten::val::object()/val::array(),
// the same technique decoder_bindings.cpp and encoder_bindings.cpp both use
// throughout (neither uses emscripten::value_object<> anywhere).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include "iclforge/ac4/ac4.hpp"
#include "iclforge/ac4dec/decoder.hpp"
#include "iclforge/ac4enc/encoder.hpp"

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

// --- ac4::ObjectProperties, both ways ---------------------------------------
//
// The decoder returns every field of an object's metadata (Part 2 Annex F.2 to
// F.10) and the encoder takes the same fields, under the same names: gainDb
// (-Infinity for silence), priority, position [x, y, z], zoneMask,
// enableElevation, snap, width [x, y, z], screenFactor, depthExponent,
// distance (a number or null; Infinity for an object at infinity),
// divergence, trimDisabled, headphoneRenderMode (a number or null) and
// headTrackDisabled, with active.
emscripten::val describe_properties(const ac4::ObjectProperties& p) {
    auto properties = emscripten::val::object();
    properties.set("active", p.active);
    properties.set("gainDb", p.gain_db);
    properties.set("priority", p.priority);
    properties.set("position", make_number_array(p.position));
    properties.set("zoneMask", p.zone_mask);
    properties.set("enableElevation", p.enable_elevation);
    properties.set("snap", p.snap);
    properties.set("width", make_number_array(p.width));
    properties.set("screenFactor", p.screen_factor);
    properties.set("depthExponent", p.depth_exponent);
    properties.set("distance", p.distance ? emscripten::val(*p.distance) : emscripten::val::null());
    properties.set("divergence", p.divergence);
    properties.set("trimDisabled", p.trim_disabled);
    properties.set("headphoneRenderMode", p.headphone_render_mode
                                              ? emscripten::val(*p.headphone_render_mode)
                                              : emscripten::val::null());
    properties.set("headTrackDisabled", p.head_track_disabled);
    return properties;
}

// --- ac4::EncoderConfig and the object metadata updates from JS objects -------
//
// A field the JS object lacks, or holds undefined or null, keeps the C++
// struct's default; a value that is not one the C++ header defines is an
// OptionError, which the encoder's constructor reports as constructionError()
// and encode() as the update it refuses.

struct OptionError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Whether `object` has `key` set to something other than undefined or null.
bool has(const emscripten::val& object, const char* key) {
    if (!object.hasOwnProperty(key)) {
        return false;
    }
    const emscripten::val value = object[key];
    return !value.isUndefined() && !value.isNull();
}

template <typename T>
void read(const emscripten::val& object, const char* key, T& out) {
    if (has(object, key)) {
        out = object[key].as<T>();
    }
}

template <typename T>
void read_optional(const emscripten::val& object, const char* key, std::optional<T>& out) {
    if (has(object, key)) {
        out = object[key].as<T>();
    }
}

// An enumerator by its number, refused outside `least` to `most`.
template <typename E>
void read_enum(const emscripten::val& object, const char* key, E& out, int least, int most) {
    if (!has(object, key)) {
        return;
    }
    const int code = object[key].as<int>();
    if (code < least || code > most) {
        throw OptionError(std::string(key) + " is not one of the values the encoder defines");
    }
    out = static_cast<E>(code);
}

std::array<double, 3> read_triple(const emscripten::val& object, const char* key,
                                  const std::array<double, 3>& fallback) {
    if (!has(object, key)) {
        return fallback;
    }
    const std::vector<double> values = emscripten::vecFromJSArray<double>(object[key]);
    if (values.size() != 3) {
        throw OptionError(std::string(key) + " is not three numbers");
    }
    return {values[0], values[1], values[2]};
}

std::vector<std::int64_t> read_int64s(const emscripten::val& object, const char* key) {
    std::vector<std::int64_t> out;
    if (has(object, key)) {
        // JS numbers, exact to 2^53 samples.
        for (const double value : emscripten::vecFromJSArray<double>(object[key])) {
            out.push_back(static_cast<std::int64_t>(value));
        }
    }
    return out;
}

ac4::ObjectProperties properties_from_js(const emscripten::val& js) {
    ac4::ObjectProperties p;
    if (js.isUndefined() || js.isNull()) {
        return p;
    }
    read(js, "active", p.active);
    read(js, "gainDb", p.gain_db);
    read(js, "priority", p.priority);
    p.position = read_triple(js, "position", p.position);
    read(js, "zoneMask", p.zone_mask);
    read(js, "enableElevation", p.enable_elevation);
    read(js, "snap", p.snap);
    p.width = read_triple(js, "width", p.width);
    read(js, "screenFactor", p.screen_factor);
    read(js, "depthExponent", p.depth_exponent);
    read_optional(js, "distance", p.distance);
    read(js, "divergence", p.divergence);
    read(js, "trimDisabled", p.trim_disabled);
    read_optional(js, "headphoneRenderMode", p.headphone_render_mode);
    read(js, "headTrackDisabled", p.head_track_disabled);
    return p;
}

// Part 2 Table 66's codes: 3 is not a loudspeaker a bed object can name.
bool valid_bed_channel(int code) {
    return code >= 0 && code <= 15 && code != 3;
}

ac4::ObjectsConfig objects_config_from_js(const emscripten::val& js) {
    ac4::ObjectsConfig out;
    if (has(js, "objects")) {
        const emscripten::val list = js["objects"];
        // Only as many as the encoder accepts, and one more, which it refuses by
        // count first: a caller's absurd length costs no more than 65.
        const int count = std::min(list["length"].as<int>(), 65);
        for (int i = 0; i < count; ++i) {
            const emscripten::val entry = list[i];
            ac4::ObjectConfig object;
            if (has(entry, "bed")) {
                const int code = entry["bed"].as<int>();
                if (!valid_bed_channel(code)) {
                    throw OptionError("a bed channel Part 2 Table 66 has no code for");
                }
                object.bed = static_cast<ac4::BedChannel>(code);
            }
            read(entry, "lfe", object.lfe);
            if (has(entry, "properties")) {
                object.properties = properties_from_js(entry["properties"]);
            }
            out.objects.push_back(object);
        }
    }
    read_enum(js, "coding", out.coding, 0, 1);
    read_enum(js, "downmix", out.downmix, 0, 2);
    read_optional(js, "downmixSignals", out.downmix_signals);
    read(js, "decorrelation", out.decorrelation);
    read_optional(js, "parameterBands", out.parameter_bands);
    read_optional(js, "coarse", out.coarse);
    read_optional(js, "screenSizeRatioCode", out.screen_size_ratio_code);
    read(js, "bedObjectChanDistribute", out.bed_object_chan_distribute);
    return out;
}

ac4::EncoderConfig encoder_config_from_js(const emscripten::val& js) {
    ac4::EncoderConfig config;
    read(js, "channels", config.channels);
    read(js, "sampleRateHz", config.sample_rate_hz);
    read(js, "frameRateIndex", config.frame_rate_index);
    read(js, "bitrateKbps", config.bitrate_kbps);
    read_enum(js, "rateMode", config.rate_mode, 0, 2);
    read_enum(js, "codecMode", config.codec_mode, 0, 8);
    read(js, "iframeInterval", config.iframe_interval);
    read(js, "dialnormDb", config.dialnorm_db);
    config.iframes = read_int64s(js, "iframes");
    config.fragment_starts = read_int64s(js, "fragmentStarts");
    if (has(js, "experimental")) {
        const emscripten::val experimental = js["experimental"];
        ac4::EncoderConfig::Experimental& flags = config.experimental;
        read(experimental, "aspxBalance", flags.aspx_balance);
        read(experimental, "aspxVarvar", flags.aspx_varvar);
        read(experimental, "aspxInterleave", flags.aspx_interleave);
        read(experimental, "codingConfigs", flags.coding_configs);
        read_enum(experimental, "sevenX", flags.seven_x, 0, 3);
        read(experimental, "acpl", flags.acpl);
        read(experimental, "backPair", flags.back_pair);
        read(experimental, "ajcc", flags.ajcc);
        read(experimental, "objects", flags.objects);
    }
    if (has(js, "objects")) {
        // With substreams set, the substreams' own codec_mode is the one in
        // force: the mode given is the object substream's, and the stream's own
        // stays kAuto (ac4::EncoderConfig::substreams; the C API does the same).
        ac4::SubstreamConfig substream;
        substream.codec_mode = config.codec_mode;
        config.codec_mode = ac4::CodecMode::kAuto;
        substream.objects = objects_config_from_js(js["objects"]);
        config.substreams.push_back(std::move(substream));
    }
    return config;
}

// One update per entry: {object, sample, rampSamples, properties}.
std::vector<ac4::ObjectMetadataUpdate> updates_from_js(const emscripten::val& js) {
    std::vector<ac4::ObjectMetadataUpdate> out;
    if (js.isUndefined() || js.isNull()) {
        return out;
    }
    const int count = js["length"].as<int>();
    for (int i = 0; i < count; ++i) {
        const emscripten::val entry = js[i];
        ac4::ObjectMetadataUpdate update;
        read(entry, "object", update.object);
        if (has(entry, "sample")) {
            update.sample = static_cast<std::int64_t>(entry["sample"].as<double>());
        }
        read(entry, "rampSamples", update.ramp_samples);
        if (has(entry, "properties")) {
            update.properties = properties_from_js(entry["properties"]);
        }
        out.push_back(update);
    }
    return out;
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

        // Annex F.2-F.10's properties in force at the frame's first sample, and
        // the block updates within the frame (Annex F.11), in the order they
        // take effect: the output sample of the frame each takes effect at,
        // counted with the decoder's delay as the frame's channels are, and the
        // samples a renderer takes to move to its properties.
        result.set("properties", describe_properties(object.properties));
        auto updates = emscripten::val::array();
        for (std::size_t u = 0; u < object.updates.size(); ++u) {
            auto entry = emscripten::val::object();
            entry.set("sample", static_cast<unsigned>(object.updates[u].sample));
            entry.set("rampSamples", object.updates[u].ramp_samples);
            entry.set("properties", describe_properties(object.updates[u].properties));
            updates.set(static_cast<unsigned>(u), entry);
        }
        result.set("updates", updates);

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
    // `options` is a plain JS object (see this file's header comment): the
    // core fields channels/sampleRateHz/frameRateIndex/bitrateKbps/rateMode/
    // codecMode/iframeInterval/dialnormDb, iframes and fragmentStarts,
    // `experimental` (aspxBalance, aspxVarvar, aspxInterleave, codingConfigs,
    // sevenX, acpl, backPair, ajcc, objects) and `objects`, an object
    // substream - {objects: [{bed, lfe, properties}], coding, downmix,
    // downmixSignals, decorrelation, parameterBands, coarse,
    // screenSizeRatioCode, bedObjectChanDistribute} - whose codecMode is then
    // the object substream's. Every other EncoderConfig field
    // (loudness/drc/downmix/dialogue/substreams/presentations) keeps its
    // struct default, the same scope cut named at the top of this file. A
    // configuration the encoder refuses leaves no encoder, and
    // constructionError() says why.
    explicit Ac4Encoder(emscripten::val options) {
        ac4::EncoderConfig config;
        try {
            config = encoder_config_from_js(options);
        } catch (const OptionError& e) {
            ctor_error_ = e.what();
            return;
        }
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

    // Planar samples at full scale 1.0, one Float32Array per input channel (or
    // per object of the object substream) - the same shape encoder_
    // bindings.cpp's copy_channels()/spans_of() take, reimplemented locally
    // here since this is a separate translation unit (apps/wasm/decoder_
    // bindings.cpp and encoder_bindings.cpp are likewise each fully
    // self-contained, sharing no helper header between them) - and the changes
    // to the objects' metadata within this input or after it, as an array of
    // {object, sample, rampSamples, properties} (empty for none). One the
    // encoder refuses - an object it lacks, a sample before this input's
    // first, a property off its range - fails the whole call: no frames, and
    // error() says why.
    emscripten::val encode(const emscripten::val& channels_js, const emscripten::val& updates_js) {
        error_.clear();
        if (!encoder_) {
            error_ = ctor_error_;
            return emscripten::val::array();
        }
        const auto storage = copy_channels(channels_js);
        const auto spans = spans_of(storage);
        std::vector<ac4::ObjectMetadataUpdate> updates;
        try {
            updates = updates_from_js(updates_js);
        } catch (const OptionError& e) {
            error_ = e.what();
            return emscripten::val::array();
        }
        try {
            auto result = encoder_->encode(spans, updates);
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

    // Why the constructor made no encoder - the first rule the configuration
    // breaks (ac4::Encoder::refusal_reason()), or the option it could not read;
    // empty once construction succeeded.
    [[nodiscard]] std::string constructionError() const { return ctor_error_; }

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
        .constructor<emscripten::val>()
        .function("encode", &Ac4Encoder::encode)
        .function("flush", &Ac4Encoder::flush)
        .function("error", &Ac4Encoder::error)
        .function("constructionError", &Ac4Encoder::constructionError)
        .function("codecMode", &Ac4Encoder::codecMode)
        .function("delaySamples", &Ac4Encoder::delaySamples)
        .function("decoderDelaySamples", &Ac4Encoder::decoderDelaySamples)
        .function("buildDac4", &Ac4Encoder::buildDac4)
        .function("dac4Refusal", &Ac4Encoder::dac4Refusal);
}
