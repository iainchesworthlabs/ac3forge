#include "optional_modules.hpp"

#include "ac4/ac4.hpp"
#include "ac4dec/decoder.hpp"
#include "ac4enc/encoder.hpp"

#include "binding_support.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// The variant of the `ac3.ac4` submodule compiled when ac4::ac4/ac4::decoder/
// ac4::encoder are in this build.
//
// pybind11-direct on ac4::Decoder/ac4::Encoder, the same policy as the rest of
// this extension (see bindings.cpp's own header comment) - no intermediate C
// API. The surface bound here is a deliberate subset of what the two headers
// declare, matching the cut this project's AC-4 C API took for the same
// reason (see src/capi's own AC-4 header): decoder output config, presentation
// selection, concealment, decoded PCM/speakers/objects and loudness metadata;
// encoder core config, encode/flush, and a minimal Toc wrapper for container
// muxing. Left out, on both sides: the syntax trace, DRC/dialogue enhancement/
// downmix detail beyond LoudnessInfo, substream/presentation configuration
// lists, ObjectUpdate ramps, and every `experimental` field - each is real
// surface the C++ headers document, not yet bound here.
//
// See optional_modules.hpp for why this is a translation unit rather than an
// #ifdef, and python/CMakeLists.txt for the selection that picks this file
// over the absent/ one beside it.

namespace {

namespace py = pybind11;

// --- zero-copy planar float32 channel views (encode direction) -------------
//
// This TU's own copy of bindings.cpp's ChannelViews/extract_channel_views -
// anonymous-namespace helpers are per-translation-unit in this extension by
// design (binding_support.hpp's own header comment), and containers/signing
// never needed a copy because neither moves PCM. ac4::Encoder::encode() takes
// "any length" input (unlike ac3::FrameEncoder's fixed SAMPLES_PER_FRAME), so
// there is no expected_len to check here - a channel-count/length mismatch is
// ac4::EncodeError::kInvalidInput, which the caller below turns into a
// ValueError.
struct ChannelViews {
    std::vector<py::array_t<float, py::array::c_style | py::array::forcecast>> owners;
    std::vector<std::span<const float>> spans;
};

ChannelViews extract_channel_views(const py::object& channels) {
    ChannelViews out;
    if (py::isinstance<py::array>(channels) && py::cast<py::array>(channels).ndim() == 2) {
        py::array_t<float, py::array::c_style | py::array::forcecast> arr(channels);
        const auto n_channels = static_cast<std::size_t>(arr.shape(0));
        const auto n_samples = static_cast<std::size_t>(arr.shape(1));
        out.spans.reserve(n_channels);
        for (std::size_t ch = 0; ch < n_channels; ++ch) {
            out.spans.emplace_back(arr.data(static_cast<py::ssize_t>(ch), 0), n_samples);
        }
        out.owners.push_back(std::move(arr));
        return out;
    }

    const auto seq = py::reinterpret_borrow<py::sequence>(channels);
    const auto n = static_cast<std::size_t>(py::len(seq));
    out.owners.reserve(n);
    out.spans.reserve(n);
    for (const auto& item : seq) {
        py::array_t<float, py::array::c_style | py::array::forcecast> arr(item);
        if (arr.ndim() != 1) {
            throw py::value_error("each channel must be a 1-D array");
        }
        out.spans.emplace_back(arr.data(), static_cast<std::size_t>(arr.shape(0)));
        out.owners.push_back(std::move(arr));
    }
    return out;
}

// --- decode-direction channel views (read-only, zero-copy) ------------------
//
// Mirrors bindings.cpp's float_view/channel_views: a view over memory owned by
// `base` (the DecodedFrame/DecodedObject Python instance itself), kept alive
// by its refcount rather than a copy. Marked non-writeable for the same reason
// as bindings.cpp's copy - see there.
py::array_t<float> float_view(const std::vector<float>& v, py::handle base) {
    py::array_t<float> arr(static_cast<py::ssize_t>(v.size()), v.data(), base);
    arr.attr("flags").attr("writeable") = false;
    return arr;
}

py::list channel_views(const std::vector<std::vector<float>>& channels, py::handle base) {
    py::list out;
    for (const auto& channel : channels) {
        out.append(float_view(channel, base));
    }
    return out;
}

}  // namespace

namespace ac3::python {

namespace py = pybind11;
using detail::KwargBinder;
using detail::to_bytes;

void register_ac4(py::module_& m) {
    auto ac4_module = m.def_submodule(
        "ac4",
        "AC-4 decode/encode (ETSI TS 103 190-1 V1.4.1, TS 103 190-2 V1.3.1) - ac4::Decoder/"
        "ac4::Encoder bound directly. See src/ac4dec/include/ac4dec/decoder.hpp and "
        "src/ac4enc/include/ac4enc/encoder.hpp for the full scope statement and what each "
        "refuses; this binding covers a subset of both - see this file's own header comment.");

    // --- enums ---------------------------------------------------------------
    // Every value keeps its C++ name verbatim (kXxx), as every other enum in
    // this extension does (ac3.Acmod.kDualMono, not ac3.Acmod.DualMono) - no
    // .export_values(), matching that same convention.

    py::enum_<ac4::Speaker>(ac4_module, "Speaker",
                            "Where a decoded channel is meant to be heard (Part 1 clause D.1, "
                            "Part 2 clause A.3).")
        .value("kLeft", ac4::Speaker::kLeft)
        .value("kRight", ac4::Speaker::kRight)
        .value("kCentre", ac4::Speaker::kCentre)
        .value("kLfe", ac4::Speaker::kLfe)
        .value("kLeftSurround", ac4::Speaker::kLeftSurround, "Ls: a side speaker in the 7.X modes")
        .value("kRightSurround", ac4::Speaker::kRightSurround)
        .value("kLeftBack", ac4::Speaker::kLeftBack, "Lb, in 7.X 3/4/0 and 7.X.4")
        .value("kRightBack", ac4::Speaker::kRightBack)
        .value("kLeftWide", ac4::Speaker::kLeftWide, "Lw, in 7.X 5/2/0")
        .value("kRightWide", ac4::Speaker::kRightWide)
        .value("kTopFrontLeft", ac4::Speaker::kTopFrontLeft, "Tfl, in 7.X 3/2/2 and the X.4 layouts")
        .value("kTopFrontRight", ac4::Speaker::kTopFrontRight)
        .value("kTopBackLeft", ac4::Speaker::kTopBackLeft, "Tbl, in the X.4 layouts")
        .value("kTopBackRight", ac4::Speaker::kTopBackRight)
        .value("kTopSideLeft", ac4::Speaker::kTopSideLeft, "Tsl, the top pair of the X.2 layouts")
        .value("kTopSideRight", ac4::Speaker::kTopSideRight)
        .value("kLfe2", ac4::Speaker::kLfe2, "the second LFE a bed can assign");

    py::enum_<ac4::ObjectKind>(ac4_module, "ObjectKind",
                               "bed_dyn_obj_assignment() (Part 2 clause 6.2.1.10).")
        .value("kBed", ac4::ObjectKind::kBed)
        .value("kDyn", ac4::ObjectKind::kDyn)
        .value("kIsf", ac4::ObjectKind::kIsf, "intermediate spatial format");

    py::enum_<ac4::DownmixTarget>(
        ac4_module, "DownmixTarget",
        "The layout Decoder.decode() renders decoded channels to (Part 1 clause 6.2.17; "
        "Part 2 clause 5.10.2 for the immersive element).")
        .value("kAsCoded", ac4::DownmixTarget::kAsCoded, "the channels as coded")
        .value("k5X", ac4::DownmixTarget::k5X, "a 7.X element's channels folded to 5.X (Table 219)")
        .value("kStereo", ac4::DownmixTarget::kStereo, "Lo/Ro or Lt/Rt per the stream's preference")
        .value("kLoRo", ac4::DownmixTarget::kLoRo)
        .value("kLtRt", ac4::DownmixTarget::kLtRt)
        .value("kMono", ac4::DownmixTarget::kMono, "L + R of the stereo downmix")
        .value("k7X4", ac4::DownmixTarget::k7X4, "the immersive element's other layouts (Part 2 Tables 38-42)")
        .value("k7X2", ac4::DownmixTarget::k7X2)
        .value("k7X0", ac4::DownmixTarget::k7X0)
        .value("k5X4", ac4::DownmixTarget::k5X4)
        .value("k5X2", ac4::DownmixTarget::k5X2);

    py::enum_<ac4::DrcMode>(ac4_module, "DrcMode", "Part 1 Table 161's DRC decoder modes.")
        .value("kOff", ac4::DrcMode::kOff, "no compression: the output level gain alone")
        .value("kDefault", ac4::DrcMode::kDefault, "the mode clause 5.7.9.2 selects for the output level")
        .value("kHomeTheatre", ac4::DrcMode::kHomeTheatre)
        .value("kFlatPanelTv", ac4::DrcMode::kFlatPanelTv)
        .value("kPortableSpeakers", ac4::DrcMode::kPortableSpeakers)
        .value("kPortableHeadphones", ac4::DrcMode::kPortableHeadphones);

    py::enum_<ac4::AssociatedType>(
        ac4_module, "AssociatedType",
        "Part 1 Table 92's refinements of associated audio (PresentationChoice.associated_type).")
        .value("kAny", ac4::AssociatedType::kAny, "whatever content_classifier says")
        .value("kAudioDescription", ac4::AssociatedType::kAudioDescription)
        .value("kAudioDescriptionSubtitles", ac4::AssociatedType::kAudioDescriptionSubtitles)
        .value("kSpokenSubtitles", ac4::AssociatedType::kSpokenSubtitles)
        .value("kEmergencyInformation", ac4::AssociatedType::kEmergencyInformation);

    py::enum_<ac4::ConcealmentPolicy>(
        ac4_module, "ConcealmentPolicy",
        "What Decoder.decode() does with a frame that will not decode (DecoderConfig.concealment). "
        "kNone, the default, raises instead.")
        .value("kNone", ac4::ConcealmentPolicy::kNone)
        .value("kRepeatFade", ac4::ConcealmentPolicy::kRepeatFade,
               "the last good frame again, fading 20 dB per 32 ms lost in a row")
        .value("kMute", ac4::ConcealmentPolicy::kMute,
               "silence, the last good frame's overlap playing out through it");

    py::enum_<ac4::ConcealmentAction>(ac4_module, "ConcealmentAction",
                                      "What a concealed frame's decode() did (Concealment.action).")
        .value("kRepeatFade", ac4::ConcealmentAction::kRepeatFade)
        .value("kMute", ac4::ConcealmentAction::kMute);

    py::enum_<ac4::DecodeError>(
        ac4_module, "DecodeError",
        "Why a frame did not decode (Concealment.error; Decoder.decode() raises ValueError with "
        "this text - see describe() in the C++ header - when no concealment policy applies).")
        .value("kTruncated", ac4::DecodeError::kTruncated)
        .value("kInvalidToc", ac4::DecodeError::kInvalidToc)
        .value("kInvalidStream", ac4::DecodeError::kInvalidStream)
        .value("kUnsupported", ac4::DecodeError::kUnsupported, "legal AC-4 this decoder does not read yet")
        .value("kMissingIFrame", ac4::DecodeError::kMissingIFrame);

    py::enum_<ac4::DecodingMode>(
        ac4_module, "DecodingMode",
        "Part 2 clause 4.7: full decoding, or core decoding (5.X.2) for low-complexity platforms.")
        .value("kFull", ac4::DecodingMode::kFull)
        .value("kCore", ac4::DecodingMode::kCore);

    py::enum_<ac4::CodecMode>(
        ac4_module, "CodecMode",
        "The channel element's codec mode (Part 1 clause 4.3.6.1) or the immersive element's "
        "(Part 2 clause 6.3.5.1, Table 73).")
        .value("kAuto", ac4::CodecMode::kAuto, "chosen from the rate - see the C++ header for the table")
        .value("kSimple", ac4::CodecMode::kSimple, "the audio spectral frontend over the whole band")
        .value("kAspx", ac4::CodecMode::kAspx)
        .value("kAspxAcpl1", ac4::CodecMode::kAspxAcpl1, "experimental.acpl only")
        .value("kAspxAcpl2", ac4::CodecMode::kAspxAcpl2)
        .value("kAspxAcpl3", ac4::CodecMode::kAspxAcpl3)
        .value("kScpl", ac4::CodecMode::kScpl, "the immersive layouts (Part 2 Table 73)")
        .value("kAspxScpl", ac4::CodecMode::kAspxScpl)
        .value("kAspxAjcc", ac4::CodecMode::kAspxAjcc, "experimental.ajcc only");

    py::enum_<ac4::RateMode>(ac4_module, "RateMode", "How frames share the rate (Part 1 Table 81's wait_frames).")
        .value("kConstant", ac4::RateMode::kConstant, "every frame's exact share, to the byte")
        .value("kAverage", ac4::RateMode::kAverage, "frames lend each other bytes within the decoder's buffer")
        .value("kVariable", ac4::RateMode::kVariable, "as kAverage without the buffer limit");

    // --- decoder-side plain structs (kwargs-constructible, every field defaulted) --------------

    py::class_<ac4::OutputConfig>(
        ac4_module, "OutputConfig",
        "The controls Decoder.set_output() changes from the next frame (Part 1 clauses 5.7.8, "
        "5.7.9, 6.2.17; Part 2 clause 5.10.2 for the immersive element).")
        .def(py::init([](py::kwargs kwargs) {
            return KwargBinder<ac4::OutputConfig>(std::move(kwargs))
                .field("output_level_dbfs", &ac4::OutputConfig::output_level_dbfs)
                .field("drc", &ac4::OutputConfig::drc)
                .field("headphones", &ac4::OutputConfig::headphones)
                .field("dialogue_enhancement_db", &ac4::OutputConfig::dialogue_enhancement_db)
                .field("downmix", &ac4::OutputConfig::downmix)
                .field("mix_lfe", &ac4::OutputConfig::mix_lfe)
                .field("dialogue_gain_db", &ac4::OutputConfig::dialogue_gain_db)
                .field("associated_gain_db", &ac4::OutputConfig::associated_gain_db)
                .finish();
        }))
        .def_readwrite("output_level_dbfs", &ac4::OutputConfig::output_level_dbfs)
        .def_readwrite("drc", &ac4::OutputConfig::drc)
        .def_readwrite("headphones", &ac4::OutputConfig::headphones)
        .def_readwrite("dialogue_enhancement_db", &ac4::OutputConfig::dialogue_enhancement_db)
        .def_readwrite("downmix", &ac4::OutputConfig::downmix)
        .def_readwrite("mix_lfe", &ac4::OutputConfig::mix_lfe)
        .def_readwrite("dialogue_gain_db", &ac4::OutputConfig::dialogue_gain_db)
        .def_readwrite("associated_gain_db", &ac4::OutputConfig::associated_gain_db);

    py::class_<ac4::PresentationChoice>(
        ac4_module, "PresentationChoice",
        "Which presentation Decoder.decode() decodes (Part 2 clause 4.8.2) - by presentation_id, "
        "by position, or by preference; see select_presentation() in the C++ header for the order.")
        .def(py::init([](py::kwargs kwargs) {
            return KwargBinder<ac4::PresentationChoice>(std::move(kwargs))
                .field("presentation_id", &ac4::PresentationChoice::presentation_id)
                .field("index", &ac4::PresentationChoice::index)
                .field("language", &ac4::PresentationChoice::language)
                .field("associated", &ac4::PresentationChoice::associated)
                .field("associated_type", &ac4::PresentationChoice::associated_type)
                .field("headphones", &ac4::PresentationChoice::headphones)
                .finish();
        }))
        .def_readwrite("presentation_id", &ac4::PresentationChoice::presentation_id)
        .def_readwrite("index", &ac4::PresentationChoice::index)
        .def_readwrite("language", &ac4::PresentationChoice::language)
        .def_readwrite("associated", &ac4::PresentationChoice::associated)
        .def_readwrite("associated_type", &ac4::PresentationChoice::associated_type)
        .def_readwrite("headphones", &ac4::PresentationChoice::headphones);

    py::class_<ac4::DecoderConfig>(
        ac4_module, "DecoderConfig",
        "A decoder's configuration. Decoder.set_output()/set_presentation() change output and "
        "presentation later; the rest is fixed for the decoder. The syntax trace "
        "(ac4::DecoderConfig::syntax) is not bound here.")
        .def(py::init([](py::kwargs kwargs) {
            return KwargBinder<ac4::DecoderConfig>(std::move(kwargs))
                .field("output", &ac4::DecoderConfig::output)
                .field("concealment", &ac4::DecoderConfig::concealment)
                .field("presentation", &ac4::DecoderConfig::presentation)
                .field("level", &ac4::DecoderConfig::level)
                .field("decoding", &ac4::DecoderConfig::decoding)
                .finish();
        }))
        .def_readwrite("output", &ac4::DecoderConfig::output)
        .def_readwrite("concealment", &ac4::DecoderConfig::concealment)
        .def_readwrite("presentation", &ac4::DecoderConfig::presentation)
        .def_readwrite("level", &ac4::DecoderConfig::level)
        .def_readwrite("decoding", &ac4::DecoderConfig::decoding);

    // --- decoded-frame pieces (read-only: these only ever come from a decode) -----------------

    py::class_<ac4::Concealment>(
        ac4_module, "Concealment",
        "What decode() did on a concealed frame (DecodedFrame.concealed) - why the real frame "
        "did not decode, and how the concealed one was made.")
        .def_readonly("error", &ac4::Concealment::error)
        .def_readonly("action", &ac4::Concealment::action);

    py::class_<ac4::ObjectProperties>(
        ac4_module, "ObjectProperties",
        "One block update of an object's metadata (Part 2 Annex F.2-F.10). x/y/z and "
        "width_x/width_y/width_z are Annex F.2's position and F.6's width, each 0 to 1 (z -1 to "
        "1); the rest F.4, F.5, F.7 to F.10 and Table 121.")
        .def_readonly("active", &ac4::ObjectProperties::active)
        .def_readonly("gain_db", &ac4::ObjectProperties::gain_db)
        .def_readonly("priority", &ac4::ObjectProperties::priority)
        .def_property_readonly("x", [](const ac4::ObjectProperties& p) { return p.position[0]; })
        .def_property_readonly("y", [](const ac4::ObjectProperties& p) { return p.position[1]; })
        .def_property_readonly("z", [](const ac4::ObjectProperties& p) { return p.position[2]; })
        .def_readonly("zone_mask", &ac4::ObjectProperties::zone_mask)
        .def_readonly("enable_elevation", &ac4::ObjectProperties::enable_elevation)
        .def_readonly("snap", &ac4::ObjectProperties::snap)
        .def_property_readonly("width_x", [](const ac4::ObjectProperties& p) { return p.width[0]; })
        .def_property_readonly("width_y", [](const ac4::ObjectProperties& p) { return p.width[1]; })
        .def_property_readonly("width_z", [](const ac4::ObjectProperties& p) { return p.width[2]; })
        .def_readonly("screen_factor", &ac4::ObjectProperties::screen_factor)
        .def_readonly("depth_exponent", &ac4::ObjectProperties::depth_exponent)
        .def_readonly("distance", &ac4::ObjectProperties::distance)
        .def_readonly("divergence", &ac4::ObjectProperties::divergence)
        .def_readonly("trim_disabled", &ac4::ObjectProperties::trim_disabled)
        .def_readonly("headphone_render_mode", &ac4::ObjectProperties::headphone_render_mode)
        .def_readonly("head_track_disabled", &ac4::ObjectProperties::head_track_disabled);

    py::class_<ac4::DecodedObject>(
        ac4_module, "DecodedObject",
        "One object of a presentation with object audio (Part 2 clause 4.8.3.4) - a bed or "
        "dynamic object; an intermediate spatial format's objects are rendered into "
        "DecodedFrame.channels instead, not listed here. ObjectUpdate ramps within the frame are "
        "not bound - `properties` is what is in force at the frame's first sample.")
        .def_readonly("kind", &ac4::DecodedObject::kind)
        .def_readonly("lfe", &ac4::DecodedObject::lfe)
        .def_readonly("speaker", &ac4::DecodedObject::speaker)
        .def_property_readonly("samples", [](py::object self) {
            return float_view(self.cast<const ac4::DecodedObject&>().samples, self);
        })
        .def_readonly("properties", &ac4::DecodedObject::properties);

    py::class_<ac4::DecodedFrame>(
        ac4_module, "DecodedFrame",
        "One frame of Decoder.decode() output. `channels` is planar PCM at full scale 1.0, the "
        "decoder's delay already applied (Decoder.latency_samples); `samples` is its length.")
        .def_readonly("sample_rate_hz", &ac4::DecodedFrame::sample_rate_hz)
        .def_readonly("sequence_counter", &ac4::DecodedFrame::sequence_counter)
        .def_readonly("presentation_index", &ac4::DecodedFrame::presentation)
        .def_readonly("presentation_id", &ac4::DecodedFrame::presentation_id)
        .def_readonly("speakers", &ac4::DecodedFrame::speakers)
        .def_property_readonly("channels", [](py::object self) {
            return channel_views(self.cast<const ac4::DecodedFrame&>().channels, self);
        })
        .def_readonly("samples", &ac4::DecodedFrame::samples)
        .def_readonly("concealed", &ac4::DecodedFrame::concealed)
        .def_readonly("objects", &ac4::DecodedFrame::objects);

    py::class_<ac4::PresentationInfo>(
        ac4_module, "PresentationInfo",
        "One presentation of the last frame's table of contents (Decoder.presentations), as the "
        "frames read so far have sent it.")
        .def_readonly("index", &ac4::PresentationInfo::index)
        .def_readonly("presentation_id", &ac4::PresentationInfo::presentation_id)
        .def_readonly("md_compat", &ac4::PresentationInfo::md_compat)
        .def_readonly("enabled", &ac4::PresentationInfo::enabled)
        .def_readonly("alternative", &ac4::PresentationInfo::alternative)
        .def_readonly("pre_virtualized", &ac4::PresentationInfo::pre_virtualized)
        .def_readonly("name", &ac4::PresentationInfo::name)
        .def_readonly("language", &ac4::PresentationInfo::language)
        .def_readonly("decodable", &ac4::PresentationInfo::decodable,
                      "whether this decoder turns every substream of it into PCM")
        .def_readonly("selectable", &ac4::PresentationInfo::selectable,
                      "whether select_presentation() may choose it at the decoder's level")
        .def_readonly("speakers", &ac4::PresentationInfo::speakers);

    py::class_<ac4::LoudnessInfo>(
        ac4_module, "LoudnessInfo",
        "Part 1 clause 4.3.12's loudness values as the stream sends them (Decoder.metadata_loudness). "
        "DrcInfo/DialogueEnhancementInfo/DownmixInfo detail is not bound here.")
        .def_readonly("dialnorm_dbfs", &ac4::LoudnessInfo::dialnorm_dbfs)
        .def_readonly("integrated_lkfs", &ac4::LoudnessInfo::integrated_lkfs)
        .def_readonly("true_peak_dbtp", &ac4::LoudnessInfo::true_peak_dbtp)
        .def_readonly("loudness_range_lu", &ac4::LoudnessInfo::loudness_range_lu);

    // --- Decoder ---------------------------------------------------------------

    py::class_<ac4::Decoder>(
        ac4_module, "Decoder",
        "One decoder per stream - configuration sent only in I-frames persists until a change of "
        "source (Part 1 clause 4.3.3.2.2). See the C++ header for what it refuses.")
        .def(py::init<>())
        .def(py::init<const ac4::DecoderConfig&>(), py::arg("config"))
        .def(
            "decode",
            [](ac4::Decoder& self, const py::buffer& frame) -> py::object {
                const auto bytes = to_bytes(frame);
                std::optional<ac4::DecodedFrame> result;
                {
                    py::gil_scoped_release release;
                    auto decoded = self.decode(bytes);
                    if (!decoded) {
                        throw py::value_error(std::string(ac4::describe(decoded.error())));
                    }
                    result = std::move(*decoded);
                }
                return result ? py::cast(*result) : py::none();
            },
            py::arg("frame"),
            "Decode one raw_ac4_frame (an ac4.sync_frame payload with the sync word and "
            "frame_size stripped, or an MP4 sample). None when the frame has no output yet (its "
            "substreams need configuration no I-frame has sent). Raises ValueError - the table of "
            "contents' or a substream's DecodeError, described - when it does not read, unless "
            "DecoderConfig.concealment supplies a frame in its place.")
        .def("set_output", &ac4::Decoder::set_output, py::arg("output"),
             "The output processing, from the next frame.")
        .def_property_readonly("output", &ac4::Decoder::output)
        .def("set_presentation", &ac4::Decoder::set_presentation, py::arg("choice"),
             "The presentation choice, from the next frame.")
        .def_property_readonly(
            "presentations",
            [](const ac4::Decoder& self) {
                const auto span = self.presentations();
                return std::vector<ac4::PresentationInfo>(span.begin(), span.end());
            },
            "The presentations of the last frame read, table-of-contents order; empty before one.")
        .def_property_readonly(
            "metadata_loudness", [](const ac4::Decoder& self) { return self.metadata().loudness; },
            "The loudness metadata of the presentation the last frame selected.")
        .def_property_readonly(
            "refusal_reason", [](const ac4::Decoder& self) { return std::string(self.refusal_reason()); },
            "Why the last decode() failed, returned nothing or returned a concealed frame; empty "
            "after a decode() that decoded its frame outright.")
        .def_property_readonly("latency_samples", &ac4::Decoder::latency_samples)
        .def("reset", &ac4::Decoder::reset,
             "Forgets everything carried between frames.");

    // --- encoder-side plain structs ---------------------------------------------

    py::class_<ac4::EncoderConfig>(
        ac4_module, "EncoderConfig",
        "An encoder's configuration - core fields only (see the C++ header for the channel "
        "counts `channels` takes at each value). Loudness/DRC/downmix/dialogue, several "
        "substreams and presentations, the syntax trace and every `experimental` field are not "
        "bound here.")
        .def(py::init([](py::kwargs kwargs) {
            return KwargBinder<ac4::EncoderConfig>(std::move(kwargs))
                .field("channels", &ac4::EncoderConfig::channels)
                .field("sample_rate_hz", &ac4::EncoderConfig::sample_rate_hz)
                .field("frame_rate_index", &ac4::EncoderConfig::frame_rate_index)
                .field("bitrate_kbps", &ac4::EncoderConfig::bitrate_kbps)
                .field("rate_mode", &ac4::EncoderConfig::rate_mode)
                .field("codec_mode", &ac4::EncoderConfig::codec_mode)
                .field("iframe_interval", &ac4::EncoderConfig::iframe_interval)
                .field("dialnorm_db", &ac4::EncoderConfig::dialnorm_db)
                .finish();
        }))
        .def_readwrite("channels", &ac4::EncoderConfig::channels)
        .def_readwrite("sample_rate_hz", &ac4::EncoderConfig::sample_rate_hz)
        .def_readwrite("frame_rate_index", &ac4::EncoderConfig::frame_rate_index)
        .def_readwrite("bitrate_kbps", &ac4::EncoderConfig::bitrate_kbps)
        .def_readwrite("rate_mode", &ac4::EncoderConfig::rate_mode)
        .def_readwrite("codec_mode", &ac4::EncoderConfig::codec_mode)
        .def_readwrite("iframe_interval", &ac4::EncoderConfig::iframe_interval)
        .def_readwrite("dialnorm_db", &ac4::EncoderConfig::dialnorm_db);

    py::class_<ac4::EncodedFrame>(
        ac4_module, "EncodedFrame",
        "One coded frame - what an MP4 sample holds as it is, and what ac4.sync_frame() wraps "
        "for a raw .ac4 file or MPEG-2 TS.")
        .def_property_readonly("data",
                               [](const ac4::EncodedFrame& f) {
                                   return py::bytes(reinterpret_cast<const char*>(f.raw_ac4_frame.data()),
                                                    f.raw_ac4_frame.size());
                               })
        .def_readonly("samples", &ac4::EncodedFrame::samples,
                     "PCM samples per channel this frame decodes to, at the input's rate.")
        .def_readonly("iframe", &ac4::EncodedFrame::iframe);

    py::class_<ac4::Toc>(
        ac4_module, "Toc",
        "A minimal wrapper over the table of contents every frame carries (Encoder.toc), for "
        "what a container muxer needs - the rest of ac4::Toc (presentations, substream groups) "
        "is not bound here.")
        .def(
            "build_dac4",
            [](const ac4::Toc& self) {
                const auto box = ac4::build_dac4(self);
                return py::bytes(reinterpret_cast<const char*>(box.data()), box.size());
            },
            "The 'dac4' box payload (ac4_dsi_v1, Annex E.6), box header excluded. Empty when "
            "dac4_refusal() is not empty.")
        .def(
            "dac4_refusal", [](const ac4::Toc& self) { return std::string(ac4::dac4_refusal(self)); },
            "Why build_dac4() wrote nothing for this Toc; empty where it described every "
            "presentation whole.")
        .def(
            "media_timing",
            [](const ac4::Toc& self) -> std::optional<std::pair<std::uint32_t, std::uint32_t>> {
                const auto timing = ac4::media_timing(self);
                if (!timing) {
                    return std::nullopt;
                }
                return std::make_pair(timing->timescale, timing->sample_delta);
            },
            "(timescale, sample_delta) for an ISOBMFF track (TS 103 190-2 Table E.1); unset for a "
            "frame rate Table 83/84 does not define.")
        .def(
            "samples_per_frame",
            [](const ac4::Toc& self) -> std::optional<std::uint32_t> { return ac4::samples_per_frame(self); },
            "Samples per AC-4 frame at the stream's own sample rate; unset at the 1000/1001 frame "
            "rates, whose frame length alternates between two values (see media_timing()).");

    // --- Encoder ---------------------------------------------------------------

    py::class_<ac4::Encoder>(
        ac4_module, "Encoder",
        "Writes mono, stereo, 5.0, 5.1, 5.0.4 or 5.1.4 PCM as one or more channel-coded "
        "substreams (see the C++ header for the rules a configuration must keep). Constructed "
        "only through create().")
        .def_static(
            "create",
            [](const ac4::EncoderConfig& config) {
                auto result = ac4::Encoder::create(config);
                if (!result) {
                    throw py::value_error(std::string(ac4::Encoder::refusal_reason(config)));
                }
                return std::move(*result);
            },
            py::arg("config"),
            "An Encoder for `config`, or raises ValueError with the reason (the first rule the "
            "configuration breaks) when it writes a configuration outside what this encoder "
            "writes, or whose rate cannot hold its least frame.")
        .def(
            "encode",
            [](ac4::Encoder& self, const py::object& channels) {
                auto views = extract_channel_views(channels);
                std::vector<ac4::EncodedFrame> frames;
                {
                    py::gil_scoped_release release;
                    auto result = self.encode(views.spans);
                    if (!result) {
                        throw py::value_error(std::string(ac4::describe(result.error())));
                    }
                    frames = std::move(*result);
                }
                return frames;
            },
            py::arg("channels"),
            "Planar float32 samples at full scale 1.0, one channel per EncoderConfig.channels, "
            "any equal length - a 2-D array or a sequence of 1-D arrays (zero-copy when already "
            "contiguous float32; don't mutate them from another thread while this call is in "
            "flight). Returns the frames this input completes, in order; the encoder's delay "
            "holds back the frames the last input still needs.")
        .def(
            "flush",
            [](ac4::Encoder& self) {
                std::vector<ac4::EncodedFrame> frames;
                {
                    py::gil_scoped_release release;
                    auto result = self.flush();
                    if (!result) {
                        throw py::value_error(std::string(ac4::describe(result.error())));
                    }
                    frames = std::move(*result);
                }
                return frames;
            },
            "Pads the input with silence to the end of its last frame and returns the frames the "
            "delay still held. Takes no input after this.")
        .def_property_readonly("toc", [](const ac4::Encoder& self) -> ac4::Toc { return self.toc(); },
                               "A snapshot of the table of contents every frame carries.")
        .def_property_readonly("codec_mode", &ac4::Encoder::codec_mode,
                               "What kAuto chose from the rate; never kAuto.")
        .def_property_readonly("delay_samples", &ac4::Encoder::delay_samples,
                               "Samples of silence the encoder puts before the input, at the input's rate.")
        .def_property_readonly("decoder_delay_samples", &ac4::Encoder::decoder_delay_samples,
                               "The delay ac4::Decoder adds, at the input's rate - see delay_samples "
                               "for the combined relationship to the encoder's own input samples.");

    // --- free functions ----------------------------------------------------

    ac4_module.def(
        "sync_frame",
        [](const py::buffer& raw_ac4_frame, bool crc) {
            const auto bytes = to_bytes(raw_ac4_frame);
            const auto framed = ac4::sync_frame(bytes, crc);
            return py::bytes(reinterpret_cast<const char*>(framed.data()), framed.size());
        },
        py::arg("raw_ac4_frame"), py::arg("crc"),
        "Annex G.3.1's ac4_syncframe(): the sync word (0xAC40, or 0xAC41 with a trailing "
        "crc_word per Annex G.4.2 when `crc` is set), then frame_size and `raw_ac4_frame`.");
}

}  // namespace ac3::python
