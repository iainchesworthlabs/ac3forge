// pybind11 bindings for ac3forge (roadmap F2) - wraps ac3::FrameEncoder, ac3::FrameDecoder,
// ac3::Eac3Decoder and ac3::oba::AtmosEncoder directly (pybind11-direct, per the roadmap's own
// dependency note - no intermediate C API). Every C++ class kept here is exactly the one
// declared in src/forge/include/ac3/{encoder/encoder,decoder/decoder,oba/atmos}.hpp; this file adds
// no codec behaviour of its own; error handling exists only because Python has no
// std::expected-shaped calling convention.
//
// GIL handling: argument conversion (numpy -> owned std::vector, bytes -> owned
// std::vector<std::byte>) happens with the GIL held, since it touches Python objects; the actual
// encode/decode call runs inside a py::gil_scoped_release block operating only on C++ locals, and
// results are converted back to Python objects after that block ends (GIL reacquired by the
// guard's destructor, including on the exception path - unwinding through the block still runs
// it). This is deliberately explicit per call site rather than py::call_guard, which only wraps
// the C++ invocation and not this file's chosen "own the buffer, don't hold a live view into a
// Python object across the release" pattern.

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstring>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/encoder/plan.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/latency.hpp"
#include "ac3/meta/drc.hpp"
#include "ac3/meta/mixing.hpp"
#include "ac3/oba/atmos.hpp"
#include "ac3/oba/oamd.hpp"

namespace py = pybind11;

namespace {

// --- kwargs-constructible plain config structs ------------------------------
// Every EncoderConfig/AtmosConfig/DecoderConfig/Profile/HeavyConfig/Position/ObjectPlacement
// field already has a real C++ default, so a Python caller only needs to name what they want to
// change - this is the one piece of binding-layer machinery that gets reused across all of them,
// in place of writing the same "collect kwargs, apply known ones, reject the rest" logic six
// times over. Rejecting unknown keys (rather than silently ignoring them) catches the ordinary
// typo ("EncoderConfig(dialnrm=10)") that a silently-accepted **kwargs would hide as a
// wrong-but-legal default.
template <typename T>
class KwargBinder {
   public:
    explicit KwargBinder(py::kwargs kwargs) : kwargs_(std::move(kwargs)) {}

    template <typename V>
    KwargBinder& field(const char* name, V T::*member) {
        if (kwargs_.contains(name)) {
            // kwargs_[name] is an access through the implicit this-> of a class template, so
            // two-phase lookup treats it as dependent regardless of kwargs_'s own (non-dependent)
            // declared type - Clang enforces the standard's `template` disambiguator here and
            // rejects the call without it; MSVC/GCC merely tolerate the omission as a
            // permissive extension. Confirmed the hard way: this built clean on both of those
            // and only failed in CI's real AppleClang leg.
            value_.*member = kwargs_[name].template cast<V>();
            seen_.insert(name);
        }
        return *this;
    }

    T finish() {
        for (auto item : kwargs_) {
            const auto key = py::cast<std::string>(item.first);
            if (!seen_.contains(key)) {
                throw py::type_error("unexpected keyword argument '" + key + "'");
            }
        }
        return value_;
    }

   private:
    py::kwargs kwargs_;
    T value_{};
    std::set<std::string> seen_;
};

// --- error translation -------------------------------------------------------

struct EncodeFailure : std::runtime_error {
    ac3::FrameError code;
    explicit EncodeFailure(ac3::FrameError c)
        : std::runtime_error("ac3forge encode failed: " + std::string(ac3::describe(c))), code(c) {}
};

struct DecodeFailure : std::runtime_error {
    ac3::DecodeError code;
    explicit DecodeFailure(ac3::DecodeError c)
        : std::runtime_error("ac3forge decode failed: " + std::string(ac3::describe(c))), code(c) {}
};

struct ScanFailure : std::runtime_error {
    ac3::io::ScanError code;
    explicit ScanFailure(ac3::io::ScanError c)
        : std::runtime_error("ac3forge scan failed: " + std::string(ac3::io::describe(c))), code(c) {}
};

// --- buffer / array plumbing -------------------------------------------------

std::vector<std::byte> to_bytes(const py::buffer& buf) {
    const py::buffer_info info = buf.request();
    if (info.ndim != 1) {
        throw py::value_error("expected a 1-D bytes-like object");
    }
    const auto total = static_cast<std::size_t>(info.size) * static_cast<std::size_t>(info.itemsize);
    std::vector<std::byte> out(total);
    if (total > 0) {
        std::memcpy(out.data(), info.ptr, total);
    }
    return out;
}

// --- scan() plumbing ----------------------------------------------------------
//
// ac3::io::ScannedStream/ScannedProgramme's own `access_units` are std::span<const std::byte>
// pointing into the CALLER's buffer (documented on ScannedStream itself) - here, that buffer is
// `to_bytes(stream)`'s local copy inside the scan() lambda below, which does not outlive that
// call. Rather than tie a returned Python object's lifetime to a buffer it does not own (the
// pattern this file's decode-output views use, where the buffer genuinely IS the returned
// object's own memory), every access unit is copied into an ordinary self-contained py::bytes
// right here, once, while the source buffer is still alive - same reasoning split_frames/
// split_access_units already copy their own spans into py::bytes for, just applied to every span
// scan() itself produces instead of only the top-level list.
struct ScanProgrammeInfo {
    int substreamid = 0;
    ac3::Acmod acmod = ac3::Acmod::k2_0;
    bool lfe = false;
    int channels = 0;
    int bsid = 0;
    int bsmod = 0;
    std::size_t substreams_per_unit = 0;
    std::optional<int> oba_complexity_index;
    std::vector<py::bytes> access_units;
};

// Everything ac3::io::ScannedStream reports, with access_units (both its own and every
// programme's) already converted to owned py::bytes - see ScanProgrammeInfo's comment above for
// why. access_unit_timing()/stream_duration_samples() and friends (bound as free functions
// below) only ever read `access_unit_samples`/`sample_rate` off a ScannedStream (confirmed
// against src/forge/src/io/elementary.cpp), so they reconstruct a throwaway
// ac3::io::ScannedStream from those two fields alone rather than needing this struct to keep the
// real one, spans and all, alive.
struct ScanResult {
    ac3::io::StreamKind kind = ac3::io::StreamKind::kAc3;
    ac3::SampleRate sample_rate = ac3::SampleRate::k48000;
    ac3::Acmod acmod = ac3::Acmod::k2_0;
    bool lfe = false;
    int channels = 0;
    std::vector<py::bytes> access_units;
    std::vector<std::uint32_t> access_unit_samples;
    std::size_t substreams_per_unit = 0;
    std::vector<ScanProgrammeInfo> programmes;
    int bsid = 0;
    int bsmod = 0;
    int bit_rate_code = 0;
    std::optional<int> oba_complexity_index;
    bool bsmod_present = false;
    int dsurmod = 0;
    bool mix_metadata = false;
    std::uint8_t independent_substreams = 0;
    std::vector<ac3::io::SubstreamService> associated_substreams;
    std::uint16_t channel_map = 0;
};

std::vector<py::bytes> to_bytes_list(const std::vector<std::span<const std::byte>>& spans) {
    std::vector<py::bytes> out;
    out.reserve(spans.size());
    for (const auto span : spans) {
        out.emplace_back(reinterpret_cast<const char*>(span.data()), span.size());
    }
    return out;
}

ScanProgrammeInfo to_programme_info(const ac3::io::ScannedProgramme& p) {
    ScanProgrammeInfo info;
    info.substreamid = p.substreamid;
    info.acmod = p.acmod;
    info.lfe = p.lfe;
    info.channels = p.channels;
    info.bsid = p.bsid;
    info.bsmod = p.bsmod;
    info.substreams_per_unit = p.substreams_per_unit;
    info.oba_complexity_index = p.oba_complexity_index;
    info.access_units = to_bytes_list(p.access_units);
    return info;
}

ScanResult to_scan_result(const ac3::io::ScannedStream& s) {
    ScanResult out;
    out.kind = s.kind;
    out.sample_rate = s.sample_rate;
    out.acmod = s.acmod;
    out.lfe = s.lfe;
    out.channels = s.channels;
    out.access_units = to_bytes_list(s.access_units);
    out.access_unit_samples = s.access_unit_samples;
    out.substreams_per_unit = s.substreams_per_unit;
    out.programmes.reserve(s.programmes.size());
    for (const auto& p : s.programmes) {
        out.programmes.push_back(to_programme_info(p));
    }
    out.bsid = s.bsid;
    out.bsmod = s.bsmod;
    out.bit_rate_code = s.bit_rate_code;
    out.oba_complexity_index = s.oba_complexity_index;
    out.bsmod_present = s.bsmod_present;
    out.dsurmod = s.dsurmod;
    out.mix_metadata = s.mix_metadata;
    out.independent_substreams = s.independent_substreams;
    out.associated_substreams.assign(s.associated_substreams.begin(), s.associated_substreams.end());
    out.channel_map = s.channel_map;
    return out;
}

// The minimal reconstruction access_unit_timing()/stream_duration_samples()/etc. below need - see
// ScanResult's own comment on why this is safe (those free functions only ever touch these two
// fields of the ScannedStream they're given).
ac3::io::ScannedStream timing_view(const ScanResult& s) {
    ac3::io::ScannedStream stream;
    stream.sample_rate = s.sample_rate;
    stream.access_unit_samples = s.access_unit_samples;
    return stream;
}

// channels: a Python sequence of 1-D array-likes, one per audio channel, AC-3 order (Table 5.8,
// LFE last - see docs/library/index.md's own "Channel order" convention). expected_len == 0
// skips the length check (used nowhere today, kept for the one call site that legitimately has
// no fixed length).
std::vector<std::vector<float>> extract_channels(const py::sequence& channels,
                                                  std::size_t expected_len) {
    std::vector<std::vector<float>> out;
    out.reserve(static_cast<std::size_t>(py::len(channels)));
    for (const auto& item : channels) {
        py::array_t<float, py::array::c_style | py::array::forcecast> arr(item);
        if (arr.ndim() != 1) {
            throw py::value_error("each channel must be a 1-D array");
        }
        const auto n = static_cast<std::size_t>(arr.shape(0));
        if (expected_len != 0 && n != expected_len) {
            throw py::value_error("each channel must have exactly " + std::to_string(expected_len) +
                                   " samples (ac3.SAMPLES_PER_FRAME)");
        }
        const float* data = arr.data();
        out.emplace_back(data, data + n);
    }
    return out;
}

std::vector<std::span<const float>> as_spans(const std::vector<std::vector<float>>& owned) {
    std::vector<std::span<const float>> spans;
    spans.reserve(owned.size());
    for (const auto& channel : owned) {
        spans.emplace_back(channel);
    }
    return spans;
}

py::array_t<float> to_ndarray(const std::vector<float>& v) {
    py::array_t<float> arr(static_cast<py::ssize_t>(v.size()));
    if (!v.empty()) {
        std::memcpy(arr.mutable_data(), v.data(), v.size() * sizeof(float));
    }
    return arr;
}

py::list channels_to_list(const std::vector<std::vector<float>>& channels) {
    py::list out;
    for (const auto& channel : channels) {
        out.append(to_ndarray(channel));
    }
    return out;
}

std::vector<std::uint8_t> to_vec(const std::array<std::uint8_t, ac3::kBlocksPerFrame>& a) {
    return {a.begin(), a.end()};
}

py::list blksw_to_list(const std::vector<std::array<bool, ac3::kBlocksPerFrame>>& blksw) {
    py::list out;
    for (const auto& per_channel : blksw) {
        py::list inner;
        for (bool b : per_channel) {
            inner.append(b);
        }
        out.append(inner);
    }
    return out;
}

// AC-3's own channel labels (Table 5.8) - independent of the E-AC-3 Table E2.5 chanmap machinery
// used for DecodedSubstream/DecodedAccessUnit below, same split apps/wasm/decoder_bindings.cpp
// draws between its own ac3_channel_labels() and apply_layout()/apply_layout_substream().
std::vector<std::string> ac3_channel_labels(ac3::Acmod acmod, bool lfe) {
    static const std::array<std::vector<std::string>, 8> kByAcmod{{
        {"Ch1", "Ch2"},              // kDualMono
        {"C"},                       // k1_0
        {"L", "R"},                  // k2_0
        {"L", "C", "R"},             // k3_0
        {"L", "R", "S"},             // k2_1
        {"L", "C", "R", "S"},        // k3_1
        {"L", "R", "Ls", "Rs"},      // k2_2
        {"L", "C", "R", "Ls", "Rs"}  // k3_2
    }};
    auto labels = kByAcmod[static_cast<std::uint8_t>(acmod)];
    if (lfe) {
        labels.emplace_back("LFE");
    }
    return labels;
}

std::vector<std::string> chanmap_labels(std::uint16_t map) {
    std::vector<std::string> labels;
    const auto layout = ac3::eac3::chanmap::expand(map);
    for (int i = 0; i < layout.count; ++i) {
        labels.emplace_back(ac3::eac3::chanmap::name(layout[i]));
    }
    return labels;
}

// The named-layout convenience roadmap AP6 asks for: a caller names a
// LayoutId (e.g. k71) instead of hand-building the dependents' chanmaps -
// ac3::plan::channel_plan_for() already carries that table
// (ac3/encoder/plan.hpp), this just turns its ChannelPlan into a real
// eac3.AccessUnitConfig. Each dependent's acmod/lfe is derived from its own
// chanmap via chanmap::acmod_for_chanmap() the same way
// ac3::plan::eac3_config() does internally. dependent_bitrate_kbps defaults
// to half the independent's rate - the same ratio
// docs/library/encoding-eac3.md's own 7.1 example uses (384/192) - since
// substreams share a frame period rather than dividing it.
ac3::eac3::AccessUnitConfig access_unit_config_for_layout(
    ac3::plan::LayoutId layout, std::uint32_t bitrate_kbps,
    std::optional<std::uint32_t> dependent_bitrate_kbps, ac3::SampleRate sample_rate) {
    const auto plan = ac3::plan::channel_plan_for(layout);
    ac3::eac3::AccessUnitConfig config;
    config.independent.sample_rate = sample_rate;
    config.independent.bitrate_kbps = bitrate_kbps;
    config.independent.acmod = plan.bed_acmod;
    config.independent.lfe = plan.bed_lfe;
    const std::uint32_t dep_kbps = dependent_bitrate_kbps.value_or(bitrate_kbps / 2);
    for (const auto chanmap : plan.dependents) {
        const auto acmod_lfe = ac3::eac3::chanmap::acmod_for_chanmap(chanmap);
        if (!acmod_lfe) {
            // Unreachable for any LayoutId's own table (every kLayouts entry is
            // built from a chanmap this always resolves) - guarded rather than
            // asserted so a future LayoutId addition fails loudly in Python
            // instead of silently building an incomplete access unit.
            throw py::value_error("no acmod/lfe combination codes this layout's dependent channels");
        }
        ac3::eac3::FrameConfig dependent;
        dependent.sample_rate = sample_rate;
        dependent.bitrate_kbps = dep_kbps;
        dependent.acmod = acmod_lfe->first;
        dependent.lfe = acmod_lfe->second;
        dependent.chanmap = chanmap;
        config.dependents.push_back(dependent);
    }
    return config;
}

}  // namespace

PYBIND11_MODULE(_ac3forge, m) {
    m.doc() = "pybind11 bindings for ac3forge - AC-3/E-AC-3 encode/decode plus Atmos objects";

    m.attr("SAMPLES_PER_FRAME") = ac3::kSamplesPerFrame;
    m.attr("BLOCKS_PER_FRAME") = ac3::kBlocksPerFrame;
    m.attr("TRANSFORM_DELAY_SAMPLES") = ac3::kTransformDelaySamples;

    // --- exceptions ----------------------------------------------------------
    // Defined here (rather than pure-Python subclasses of RuntimeError) so the extension owns
    // its own exception identity - ac3forge/__init__.py just re-exports these three names.
    static py::exception<std::runtime_error> ac3_error(m, "Ac3Error", PyExc_RuntimeError);
    static py::exception<std::runtime_error> encode_error(m, "Ac3EncodeError", ac3_error.ptr());
    static py::exception<std::runtime_error> decode_error(m, "Ac3DecodeError", ac3_error.ptr());
    static py::exception<std::runtime_error> scan_error(m, "Ac3ScanError", ac3_error.ptr());

    // Constructing through PyObject_CallFunction rather than py::exception<T>'s own operator()
    // (deprecated, and only ever sets a plain string message) - this needs a real INSTANCE so
    // ".error" can be attached to it before it becomes the active exception.
    py::register_exception_translator([](std::exception_ptr p) {
        if (!p) {
            return;
        }
        try {
            std::rethrow_exception(p);
        } catch (const EncodeFailure& e) {
            py::object exc =
                py::reinterpret_steal<py::object>(PyObject_CallFunction(encode_error.ptr(), "s", e.what()));
            exc.attr("error") = py::cast(e.code);
            PyErr_SetObject(encode_error.ptr(), exc.ptr());
        } catch (const DecodeFailure& e) {
            py::object exc =
                py::reinterpret_steal<py::object>(PyObject_CallFunction(decode_error.ptr(), "s", e.what()));
            exc.attr("error") = py::cast(e.code);
            PyErr_SetObject(decode_error.ptr(), exc.ptr());
        } catch (const ScanFailure& e) {
            py::object exc =
                py::reinterpret_steal<py::object>(PyObject_CallFunction(scan_error.ptr(), "s", e.what()));
            exc.attr("error") = py::cast(e.code);
            PyErr_SetObject(scan_error.ptr(), exc.ptr());
        }
    });

    // --- enums -----------------------------------------------------------------
    py::enum_<ac3::Acmod>(m, "Acmod", "A/52 Table 5.8 audio coding mode")
        .value("kDualMono", ac3::Acmod::kDualMono, "1+1: two independent programmes (Ch1, Ch2)")
        .value("k1_0", ac3::Acmod::k1_0, "C")
        .value("k2_0", ac3::Acmod::k2_0, "L, R")
        .value("k3_0", ac3::Acmod::k3_0, "L, C, R")
        .value("k2_1", ac3::Acmod::k2_1, "L, R, S")
        .value("k3_1", ac3::Acmod::k3_1, "L, C, R, S")
        .value("k2_2", ac3::Acmod::k2_2, "L, R, Ls, Rs")
        .value("k3_2", ac3::Acmod::k3_2, "L, C, R, Ls, Rs");

    py::enum_<ac3::SampleRate>(m, "SampleRate")
        .value("k48000", ac3::SampleRate::k48000)
        .value("k44100", ac3::SampleRate::k44100)
        .value("k32000", ac3::SampleRate::k32000)
        .value("k24000", ac3::SampleRate::k24000, "E-AC-3 fscod2 only")
        .value("k22050", ac3::SampleRate::k22050, "E-AC-3 fscod2 only")
        .value("k16000", ac3::SampleRate::k16000, "E-AC-3 fscod2 only");

    py::enum_<ac3::DecodeError>(m, "DecodeError")
        .value("kTruncated", ac3::DecodeError::kTruncated)
        .value("kBadSyncWord", ac3::DecodeError::kBadSyncWord)
        .value("kBadCrc", ac3::DecodeError::kBadCrc)
        .value("kReservedValue", ac3::DecodeError::kReservedValue)
        .value("kUnsupported", ac3::DecodeError::kUnsupported)
        .value("kInvalidStream", ac3::DecodeError::kInvalidStream);

    py::enum_<ac3::FrameError>(m, "FrameError")
        .value("kInvalidBitrate", ac3::FrameError::kInvalidBitrate)
        .value("kInvalidDialnorm", ac3::FrameError::kInvalidDialnorm)
        .value("kInvalidSubstream", ac3::FrameError::kInvalidSubstream)
        .value("kInvalidChannelMap", ac3::FrameError::kInvalidChannelMap)
        .value("kTooManyChannels", ac3::FrameError::kTooManyChannels)
        .value("kInvalidMixLevel", ac3::FrameError::kInvalidMixLevel)
        .value("kInvalidBsi", ac3::FrameError::kInvalidBsi)
        .value("kInvalidObjectAudio", ac3::FrameError::kInvalidObjectAudio);

    py::enum_<ac3::eac3::StreamType>(m, "StreamType")
        .value("kIndependent", ac3::eac3::StreamType::kIndependent)
        .value("kDependent", ac3::eac3::StreamType::kDependent)
        .value("kConvertible", ac3::eac3::StreamType::kConvertible);

    // ac3::io::StreamKind - roadmap AP6's scan(). A different question from StreamType above
    // (which frames belong to which E-AC-3 substream role); this is "is the stream AC-3, E-AC-3,
    // or an AC-3 core carrying E-AC-3 dependent extensions" - see ac3::io::StreamKind's own
    // header comment for kAc3CoreEac3Extension.
    py::enum_<ac3::io::StreamKind>(m, "StreamKind")
        .value("kAc3", ac3::io::StreamKind::kAc3)
        .value("kEac3", ac3::io::StreamKind::kEac3)
        .value("kAc3CoreEac3Extension", ac3::io::StreamKind::kAc3CoreEac3Extension);

    py::enum_<ac3::io::ScanError>(m, "ScanError")
        .value("kEmpty", ac3::io::ScanError::kEmpty)
        .value("kLostSync", ac3::io::ScanError::kLostSync)
        .value("kUnsupportedBsid", ac3::io::ScanError::kUnsupportedBsid)
        .value("kReservedValue", ac3::io::ScanError::kReservedValue)
        .value("kTruncated", ac3::io::ScanError::kTruncated)
        .value("kUnsupportedStructure", ac3::io::ScanError::kUnsupportedStructure);

    py::enum_<ac3::meta::ProfileId>(m, "ProfileId", "Conventional Dolby DRC profiles (ac3.profile_for)")
        .value("kFilmStandard", ac3::meta::ProfileId::kFilmStandard)
        .value("kFilmLight", ac3::meta::ProfileId::kFilmLight)
        .value("kMusicStandard", ac3::meta::ProfileId::kMusicStandard)
        .value("kMusicLight", ac3::meta::ProfileId::kMusicLight)
        .value("kSpeech", ac3::meta::ProfileId::kSpeech);

    py::enum_<ac3::meta::CentreMixLevel>(m, "CentreMixLevel")
        .value("kMinus3dB", ac3::meta::CentreMixLevel::kMinus3dB)
        .value("kMinus4_5dB", ac3::meta::CentreMixLevel::kMinus4_5dB)
        .value("kMinus6dB", ac3::meta::CentreMixLevel::kMinus6dB);

    py::enum_<ac3::meta::SurroundMixLevel>(m, "SurroundMixLevel")
        .value("kMinus3dB", ac3::meta::SurroundMixLevel::kMinus3dB)
        .value("kMinus6dB", ac3::meta::SurroundMixLevel::kMinus6dB)
        .value("kSilent", ac3::meta::SurroundMixLevel::kSilent);

    // --- free functions ----------------------------------------------------
    m.def("fullbw_channel_count", &ac3::fullbw_channel_count, py::arg("acmod"));
    m.def("sample_rate_hz", &ac3::sample_rate_hz, py::arg("sample_rate"));
    // Three overloads of the same C++ name now (ac3::describe(DecodeError),
    // ac3::describe(FrameError) since AP2's FrameError::describe(), and
    // ac3::describe(DiagnosticEvent), roadmap AP11's ac3/decoder/diagnostics.hpp) -
    // &ac3::describe alone is ambiguous, so each bound overload is cast to its own
    // function-pointer type. DiagnosticEvent's overload is not surfaced to Python at
    // all (see docs/library/python-api.md's "What isn't exposed").
    m.def("describe", static_cast<std::string_view (*)(ac3::DecodeError)>(&ac3::describe),
          py::arg("error"), "Text for a DecodeError value");
    m.def("describe", static_cast<std::string_view (*)(ac3::FrameError)>(&ac3::describe),
          py::arg("error"), "Text for a FrameError value");
    m.def(
        "describe", [](ac3::io::ScanError e) { return std::string(ac3::io::describe(e)); },
        py::arg("error"), "Text for a ScanError value");
    m.def(
        "profile_for", [](ac3::meta::ProfileId id) { return ac3::meta::profile(id); }, py::arg("id"),
        "The ac3.Profile for one of the conventional ac3.ProfileId presets");
    m.def(
        "profile_name", [](ac3::meta::ProfileId id) { return std::string(ac3::meta::profile_name(id)); },
        py::arg("id"));

    m.def(
        "split_frames",
        [](const py::buffer& stream) {
            const auto bytes = to_bytes(stream);
            const auto result = ac3::split_frames(bytes);
            if (!result) {
                throw DecodeFailure(result.error());
            }
            py::list out;
            for (const auto frame : *result) {
                out.append(py::bytes(reinterpret_cast<const char*>(frame.data()), frame.size()));
            }
            return out;
        },
        py::arg("stream"), "Split a raw AC-3/E-AC-3 elementary stream into individual syncframes");

    m.def(
        "split_access_units",
        [](const py::buffer& stream) {
            const auto bytes = to_bytes(stream);
            const auto result = ac3::split_access_units(bytes);
            if (!result) {
                throw DecodeFailure(result.error());
            }
            py::list out;
            for (const auto unit : *result) {
                out.append(py::bytes(reinterpret_cast<const char*>(unit.data()), unit.size()));
            }
            return out;
        },
        py::arg("stream"),
        "Group an E-AC-3 elementary stream's syncframes into access units (Eac3Decoder.decode_access_unit's own input shape)");

    m.def(
        "stream_bsid",
        [](const py::buffer& frame) {
            const auto bytes = to_bytes(frame);
            const auto result = ac3::stream_bsid(bytes);
            if (!result) {
                throw DecodeFailure(result.error());
            }
            return *result;
        },
        py::arg("frame"));

    // --- scan() and friends (roadmap AP6) - ac3::io/elementary.hpp -------------------------
    py::class_<ac3::io::SubstreamService>(
        m, "SubstreamService",
        "One associated independent substream's own bed, as ScannedStream.associated_substreams "
        "reports it for the MPEG-TS/ATSC registry descriptors.")
        .def_readonly("present", &ac3::io::SubstreamService::present)
        .def_readonly("bsmod", &ac3::io::SubstreamService::bsmod)
        .def_readonly("bsmod_present", &ac3::io::SubstreamService::bsmod_present)
        .def_readonly("acmod", &ac3::io::SubstreamService::acmod)
        .def_readonly("lfe", &ac3::io::SubstreamService::lfe)
        .def_readonly("mix_metadata", &ac3::io::SubstreamService::mix_metadata);

    py::class_<ac3::io::FrameHeader>(
        m, "FrameHeader",
        "One syncframe's bit stream information, read without decoding any audio - "
        "read_frame_header()'s own return type.")
        .def_readonly("kind", &ac3::io::FrameHeader::kind)
        .def_readonly("bytes", &ac3::io::FrameHeader::bytes)
        .def_readonly("bsid", &ac3::io::FrameHeader::bsid)
        .def_readonly("bsmod", &ac3::io::FrameHeader::bsmod)
        .def_readonly("bsmod_present", &ac3::io::FrameHeader::bsmod_present)
        .def_readonly("dsurmod", &ac3::io::FrameHeader::dsurmod)
        .def_readonly("sample_rate", &ac3::io::FrameHeader::sample_rate)
        .def_readonly("acmod", &ac3::io::FrameHeader::acmod)
        .def_readonly("lfe", &ac3::io::FrameHeader::lfe)
        .def_readonly("dialnorm", &ac3::io::FrameHeader::dialnorm)
        .def_readonly("compr", &ac3::io::FrameHeader::compr)
        .def_readonly("dialnorm2", &ac3::io::FrameHeader::dialnorm2)
        .def_readonly("compr2", &ac3::io::FrameHeader::compr2)
        .def_readonly("strmtyp", &ac3::io::FrameHeader::strmtyp)
        .def_readonly("substreamid", &ac3::io::FrameHeader::substreamid)
        .def_readonly("numblkscod", &ac3::io::FrameHeader::numblkscod)
        .def_readonly("reduced_rate", &ac3::io::FrameHeader::reduced_rate)
        .def_readonly("chanmap", &ac3::io::FrameHeader::chanmap)
        .def_readonly("oba_complexity_index", &ac3::io::FrameHeader::oba_complexity_index)
        .def_readonly("mix_metadata", &ac3::io::FrameHeader::mix_metadata)
        .def_readonly("bit_rate_code", &ac3::io::FrameHeader::bit_rate_code)
        .def_readonly("bitrate_kbps", &ac3::io::FrameHeader::bitrate_kbps)
        .def_property_readonly("coded_channels", &ac3::io::FrameHeader::coded_channels);

    py::class_<ac3::io::AccessUnitTiming>(
        m, "AccessUnitTiming",
        "Where one access unit starts and how long it lasts - access_unit_timing()'s own return "
        "type. The integer accessors avoid the drift a running sum of per-frame durations would "
        "have, by always computing from the absolute sample position.")
        .def_readonly("start_sample", &ac3::io::AccessUnitTiming::start_sample)
        .def_readonly("duration_samples", &ac3::io::AccessUnitTiming::duration_samples)
        .def_readonly("sample_rate", &ac3::io::AccessUnitTiming::sample_rate)
        .def_property_readonly("start_seconds", &ac3::io::AccessUnitTiming::start_seconds)
        .def_property_readonly("duration_seconds", &ac3::io::AccessUnitTiming::duration_seconds)
        .def("start_in_timescale", &ac3::io::AccessUnitTiming::start_in_timescale, py::arg("timescale"))
        .def("duration_in_timescale", &ac3::io::AccessUnitTiming::duration_in_timescale,
             py::arg("timescale"));

    py::class_<ScanProgrammeInfo>(
        m, "ScannedProgramme",
        "One programme scan() found: an independent substream (or, for StreamKind."
        "kAc3CoreEac3Extension, the AC-3 core standing in for one) plus the dependents that "
        "extend it, across every frame period the stream covers.")
        .def_readonly("substreamid", &ScanProgrammeInfo::substreamid)
        .def_readonly("acmod", &ScanProgrammeInfo::acmod)
        .def_readonly("lfe", &ScanProgrammeInfo::lfe)
        .def_readonly("channels", &ScanProgrammeInfo::channels)
        .def_readonly("bsid", &ScanProgrammeInfo::bsid)
        .def_readonly("bsmod", &ScanProgrammeInfo::bsmod)
        .def_readonly("substreams_per_unit", &ScanProgrammeInfo::substreams_per_unit)
        .def_readonly("oba_complexity_index", &ScanProgrammeInfo::oba_complexity_index)
        .def_readonly("access_units", &ScanProgrammeInfo::access_units,
                       "This programme's access units, in stream order, each the independent "
                       "substream's bytes followed immediately by its dependents' - the input "
                       "shape Eac3Decoder.decode_access_unit expects.");

    py::class_<ScanResult>(
        m, "ScannedStream",
        "What scan() learns about an elementary stream without decoding any audio - shape, "
        "channel layout, every programme, and the raw syntax values a container muxer needs "
        "(see ac3::io::ScannedStream's own header comment). Every scalar field below describes "
        "the FIRST (or only) programme; see `programmes` for the rest.")
        .def_readonly("kind", &ScanResult::kind)
        .def_readonly("sample_rate", &ScanResult::sample_rate)
        .def_readonly("acmod", &ScanResult::acmod)
        .def_readonly("lfe", &ScanResult::lfe)
        .def_readonly("channels", &ScanResult::channels)
        .def_readonly("access_units", &ScanResult::access_units,
                       "The first programme's access units - split_access_units's own input, "
                       "already split for you.")
        .def_readonly("access_unit_samples", &ScanResult::access_unit_samples)
        .def_readonly("substreams_per_unit", &ScanResult::substreams_per_unit)
        .def_readonly("programmes", &ScanResult::programmes)
        .def_readonly("bsid", &ScanResult::bsid)
        .def_readonly("bsmod", &ScanResult::bsmod)
        .def_readonly("bit_rate_code", &ScanResult::bit_rate_code)
        .def_readonly("oba_complexity_index", &ScanResult::oba_complexity_index)
        .def_readonly("bsmod_present", &ScanResult::bsmod_present)
        .def_readonly("dsurmod", &ScanResult::dsurmod)
        .def_readonly("mix_metadata", &ScanResult::mix_metadata)
        .def_readonly("independent_substreams", &ScanResult::independent_substreams)
        .def_readonly("associated_substreams", &ScanResult::associated_substreams)
        .def_readonly("channel_map", &ScanResult::channel_map);

    m.def(
        "read_frame_header",
        [](const py::buffer& at) {
            const auto bytes = to_bytes(at);
            const auto result = ac3::io::read_frame_header(bytes);
            if (!result) {
                throw ScanFailure(result.error());
            }
            return *result;
        },
        py::arg("at"),
        "The header of the syncframe starting at `at` - `at` must begin with a sync word and "
        "hold at least the whole of bsi; it may be longer, same contract as ac3::io::"
        "read_frame_header.");

    m.def(
        "scan",
        [](const py::buffer& stream) {
            const auto bytes = to_bytes(stream);
            const auto result = ac3::io::scan(bytes);
            if (!result) {
                throw ScanFailure(result.error());
            }
            return to_scan_result(*result);
        },
        py::arg("stream"),
        "Read an AC-3/E-AC-3 elementary stream's shape - access units, channel layout, every "
        "programme - without decoding any audio.");

    m.def(
        "access_unit_timing",
        [](const ScanResult& s, std::size_t index) {
            return ac3::io::access_unit_timing(timing_view(s), index);
        },
        py::arg("stream"), py::arg("index"), "Access unit `index`'s own timing, or None past the end.");
    m.def(
        "stream_duration_samples",
        [](const ScanResult& s) { return ac3::io::stream_duration_samples(timing_view(s)); },
        py::arg("stream"));
    m.def(
        "stream_duration_seconds",
        [](const ScanResult& s) { return ac3::io::stream_duration_seconds(timing_view(s)); },
        py::arg("stream"));
    m.def(
        "access_unit_at_sample",
        [](const ScanResult& s, std::uint64_t sample) {
            return ac3::io::access_unit_at_sample(timing_view(s), sample);
        },
        py::arg("stream"), py::arg("sample"),
        "The access unit covering `sample`, or None past the end of the stream.");
    m.def(
        "access_unit_at_seconds",
        [](const ScanResult& s, double seconds) {
            return ac3::io::access_unit_at_seconds(timing_view(s), seconds);
        },
        py::arg("stream"), py::arg("seconds"), "Same question as access_unit_at_sample, in seconds.");
    m.def(
        "uniform_access_unit_samples",
        [](const ScanResult& s) { return ac3::io::uniform_access_unit_samples(timing_view(s)); },
        py::arg("stream"),
        "The one access-unit length every unit in the stream shares, or None when they differ.");

    // --- plain data types (kwargs-constructible where a caller builds one) -------------------
    // --- latency (roadmap PF6) ---------------------------------------------
    py::class_<ac3::LatencyBudget>(
        m, "LatencyBudget",
        "Algorithmic delay of an encode->decode chain, in samples at the coded rate. "
        "frame_samples is the encoder's input granularity; transform_samples is the "
        "MDCT/IMDCT overlap and the one term that is a sample-domain shift (decoded sample "
        "k is input sample k - transform_samples); lookahead_samples is input needed beyond "
        "the frame being coded (zero throughout this library); holdback_samples is the "
        "E-AC-3 §3.7 decoder hold-back.")
        .def_readonly("frame_samples", &ac3::LatencyBudget::frame_samples)
        .def_readonly("transform_samples", &ac3::LatencyBudget::transform_samples)
        .def_readonly("lookahead_samples", &ac3::LatencyBudget::lookahead_samples)
        .def_readonly("holdback_samples", &ac3::LatencyBudget::holdback_samples)
        .def_property_readonly("total_samples", &ac3::LatencyBudget::total_samples)
        .def(
            "milliseconds",
            [](const ac3::LatencyBudget& self, ac3::SampleRate sample_rate) {
                return ac3::latency_ms(self, sample_rate);
            },
            py::arg("sample_rate"), "total_samples at the given coded rate, in milliseconds")
        .def("__repr__", [](const ac3::LatencyBudget& b) {
            return "LatencyBudget(frame=" + std::to_string(b.frame_samples) +
                   ", transform=" + std::to_string(b.transform_samples) +
                   ", lookahead=" + std::to_string(b.lookahead_samples) +
                   ", holdback=" + std::to_string(b.holdback_samples) +
                   ", total=" + std::to_string(b.total_samples()) + ")";
        });

    py::class_<ac3::meta::Profile>(m, "Profile", "A/52 §7.7 DRC compression characteristic")
        .def(py::init([](py::kwargs kwargs) {
            return KwargBinder<ac3::meta::Profile>(std::move(kwargs))
                .field("null_low_db", &ac3::meta::Profile::null_low_db)
                .field("null_high_db", &ac3::meta::Profile::null_high_db)
                .field("boost_ratio", &ac3::meta::Profile::boost_ratio)
                .field("max_boost_db", &ac3::meta::Profile::max_boost_db)
                .field("early_cut_ratio", &ac3::meta::Profile::early_cut_ratio)
                .field("early_cut_end_db", &ac3::meta::Profile::early_cut_end_db)
                .field("cut_ratio", &ac3::meta::Profile::cut_ratio)
                .field("attack_ms", &ac3::meta::Profile::attack_ms)
                .field("release_ms", &ac3::meta::Profile::release_ms)
                .finish();
        }))
        .def_readwrite("null_low_db", &ac3::meta::Profile::null_low_db)
        .def_readwrite("null_high_db", &ac3::meta::Profile::null_high_db)
        .def_readwrite("boost_ratio", &ac3::meta::Profile::boost_ratio)
        .def_readwrite("max_boost_db", &ac3::meta::Profile::max_boost_db)
        .def_readwrite("early_cut_ratio", &ac3::meta::Profile::early_cut_ratio)
        .def_readwrite("early_cut_end_db", &ac3::meta::Profile::early_cut_end_db)
        .def_readwrite("cut_ratio", &ac3::meta::Profile::cut_ratio)
        .def_readwrite("attack_ms", &ac3::meta::Profile::attack_ms)
        .def_readwrite("release_ms", &ac3::meta::Profile::release_ms);

    py::class_<ac3::meta::HeavyConfig>(m, "HeavyConfig", "A/52 §7.7.2 heavy compression (RF mode)")
        .def(py::init([](py::kwargs kwargs) {
            return KwargBinder<ac3::meta::HeavyConfig>(std::move(kwargs))
                .field("dialogue_target_dbfs", &ac3::meta::HeavyConfig::dialogue_target_dbfs)
                .field("peak_ceiling_dbfs", &ac3::meta::HeavyConfig::peak_ceiling_dbfs)
                .field("release_db_per_second", &ac3::meta::HeavyConfig::release_db_per_second)
                .finish();
        }))
        .def_readwrite("dialogue_target_dbfs", &ac3::meta::HeavyConfig::dialogue_target_dbfs)
        .def_readwrite("peak_ceiling_dbfs", &ac3::meta::HeavyConfig::peak_ceiling_dbfs)
        .def_readwrite("release_db_per_second", &ac3::meta::HeavyConfig::release_db_per_second);

    py::class_<ac3::oba::Position>(m, "Position", "Room-anchored object position (§4.2.1)")
        .def(py::init([](py::kwargs kwargs) {
            return KwargBinder<ac3::oba::Position>(std::move(kwargs))
                .field("x", &ac3::oba::Position::x)
                .field("y", &ac3::oba::Position::y)
                .field("z", &ac3::oba::Position::z)
                .finish();
        }))
        .def_readwrite("x", &ac3::oba::Position::x)
        .def_readwrite("y", &ac3::oba::Position::y)
        .def_readwrite("z", &ac3::oba::Position::z)
        .def("__repr__", [](const ac3::oba::Position& p) {
            return "Position(x=" + std::to_string(p.x) + ", y=" + std::to_string(p.y) +
                   ", z=" + std::to_string(p.z) + ")";
        });

    py::class_<ac3::oba::ObjectPlacement>(m, "ObjectPlacement", "One object's placement for one frame")
        .def(py::init([](py::kwargs kwargs) {
            return KwargBinder<ac3::oba::ObjectPlacement>(std::move(kwargs))
                .field("position", &ac3::oba::ObjectPlacement::position)
                .field("gain", &ac3::oba::ObjectPlacement::gain)
                .field("lfe_send", &ac3::oba::ObjectPlacement::lfe_send)
                .finish();
        }))
        .def_readwrite("position", &ac3::oba::ObjectPlacement::position)
        .def_readwrite("gain", &ac3::oba::ObjectPlacement::gain)
        .def_readwrite("lfe_send", &ac3::oba::ObjectPlacement::lfe_send);

    py::class_<ac3::oba::DynamicObject>(m, "DynamicObject", "A decoded object's position and gain")
        .def_readonly("position", &ac3::oba::DynamicObject::position)
        .def_readonly("gain_db", &ac3::oba::DynamicObject::gain_db);

    py::class_<ac3::oba::Program>(m, "Program", "OAMD programme shape (§5.6)")
        .def_readonly("dynamic_only", &ac3::oba::Program::dynamic_only)
        .def_readonly("lfe", &ac3::oba::Program::lfe)
        .def_readonly("bed", &ac3::oba::Program::bed)
        .def_readonly("dynamic_objects", &ac3::oba::Program::dynamic_objects);

    py::class_<ac3::oba::DecodedProgram>(m, "DecodedProgram", "Decoded OAMD: programme shape plus every object")
        .def_readonly("program", &ac3::oba::DecodedProgram::program)
        .def_readonly("objects", &ac3::oba::DecodedProgram::objects);

    // --- encoder -------------------------------------------------------------
    py::class_<ac3::EncoderConfig>(m, "EncoderConfig")
        .def(py::init([](py::kwargs kwargs) {
            return KwargBinder<ac3::EncoderConfig>(std::move(kwargs))
                .field("sample_rate", &ac3::EncoderConfig::sample_rate)
                .field("bitrate_kbps", &ac3::EncoderConfig::bitrate_kbps)
                .field("dialnorm", &ac3::EncoderConfig::dialnorm)
                .field("dialnorm2", &ac3::EncoderConfig::dialnorm2)
                .field("chbwcod", &ac3::EncoderConfig::chbwcod)
                .field("acmod", &ac3::EncoderConfig::acmod)
                .field("lfe", &ac3::EncoderConfig::lfe)
                .field("coupling", &ac3::EncoderConfig::coupling)
                .field("cplbegf", &ac3::EncoderConfig::cplbegf)
                .field("cplendf", &ac3::EncoderConfig::cplendf)
                .field("fast_mdct", &ac3::EncoderConfig::fast_mdct)
                .field("drc", &ac3::EncoderConfig::drc)
                .field("heavy", &ac3::EncoderConfig::heavy)
                .field("drc2", &ac3::EncoderConfig::drc2)
                .field("heavy2", &ac3::EncoderConfig::heavy2)
                .field("cmixlev", &ac3::EncoderConfig::cmixlev)
                .field("surmixlev", &ac3::EncoderConfig::surmixlev)
                .finish();
        }))
        .def_readwrite("sample_rate", &ac3::EncoderConfig::sample_rate)
        .def_readwrite("bitrate_kbps", &ac3::EncoderConfig::bitrate_kbps)
        .def_readwrite("dialnorm", &ac3::EncoderConfig::dialnorm)
        .def_readwrite("dialnorm2", &ac3::EncoderConfig::dialnorm2)
        .def_readwrite("chbwcod", &ac3::EncoderConfig::chbwcod)
        .def_readwrite("acmod", &ac3::EncoderConfig::acmod)
        .def_readwrite("lfe", &ac3::EncoderConfig::lfe)
        .def_readwrite("coupling", &ac3::EncoderConfig::coupling)
        .def_readwrite("cplbegf", &ac3::EncoderConfig::cplbegf)
        .def_readwrite("cplendf", &ac3::EncoderConfig::cplendf)
        .def_readwrite("fast_mdct", &ac3::EncoderConfig::fast_mdct)
        .def_readwrite("drc", &ac3::EncoderConfig::drc)
        .def_readwrite("heavy", &ac3::EncoderConfig::heavy)
        .def_readwrite("drc2", &ac3::EncoderConfig::drc2)
        .def_readwrite("heavy2", &ac3::EncoderConfig::heavy2)
        .def_readwrite("cmixlev", &ac3::EncoderConfig::cmixlev)
        .def_readwrite("surmixlev", &ac3::EncoderConfig::surmixlev);

    py::class_<ac3::FrameEncoder>(m, "FrameEncoder")
        .def(py::init<const ac3::EncoderConfig&>(), py::arg("config"))
        .def(
            "encode_frame",
            [](ac3::FrameEncoder& self, const py::sequence& channels) {
                auto owned = extract_channels(channels, static_cast<std::size_t>(ac3::kSamplesPerFrame));
                if (owned.size() != static_cast<std::size_t>(self.channel_count())) {
                    throw py::value_error("expected " + std::to_string(self.channel_count()) +
                                          " channels (self.channel_count), got " +
                                          std::to_string(owned.size()));
                }
                std::vector<std::byte> bytes;
                {
                    py::gil_scoped_release release;
                    auto spans = as_spans(owned);
                    auto result = self.encode_frame(spans);
                    if (!result) {
                        throw EncodeFailure(result.error());
                    }
                    bytes = std::move(*result);
                }
                return py::bytes(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            },
            py::arg("channels"),
            "channels: a sequence of 1-D float arrays, ac3.SAMPLES_PER_FRAME samples each, AC-3 "
            "channel order (LFE last). Returns one syncframe as bytes.")
        .def_property_readonly("config", &ac3::FrameEncoder::config)
        .def_property_readonly("channel_count", &ac3::FrameEncoder::channel_count)
        .def_property_readonly("latency", &ac3::FrameEncoder::latency)
        .def_property_readonly("latency_samples", &ac3::FrameEncoder::latency_samples);

    // --- decoder ---------------------------------------------------------------
    py::class_<ac3::DecoderConfig>(m, "DecoderConfig")
        .def(py::init([](py::kwargs kwargs) {
            return KwargBinder<ac3::DecoderConfig>(std::move(kwargs))
                .field("drc_scale", &ac3::DecoderConfig::drc_scale)
                .field("heavy_compression", &ac3::DecoderConfig::heavy_compression)
                .finish();
        }))
        .def_readwrite("drc_scale", &ac3::DecoderConfig::drc_scale)
        .def_readwrite("heavy_compression", &ac3::DecoderConfig::heavy_compression);

    py::class_<ac3::DecodedFrame>(m, "DecodedFrame")
        .def_readonly("sample_rate", &ac3::DecodedFrame::sample_rate)
        .def_readonly("bitrate_kbps", &ac3::DecodedFrame::bitrate_kbps)
        .def_readonly("acmod", &ac3::DecodedFrame::acmod)
        .def_readonly("lfe", &ac3::DecodedFrame::lfe)
        .def_readonly("dialnorm", &ac3::DecodedFrame::dialnorm)
        .def_readonly("compr", &ac3::DecodedFrame::compr)
        .def_readonly("dialnorm2", &ac3::DecodedFrame::dialnorm2)
        .def_readonly("compr2", &ac3::DecodedFrame::compr2)
        .def_property_readonly("dynrng", [](const ac3::DecodedFrame& f) { return to_vec(f.dynrng); })
        .def_property_readonly("dynrng2", [](const ac3::DecodedFrame& f) { return to_vec(f.dynrng2); })
        .def_property_readonly("blksw", [](const ac3::DecodedFrame& f) { return blksw_to_list(f.blksw); })
        .def_property_readonly("channels",
                                [](const ac3::DecodedFrame& f) { return channels_to_list(f.channels); })
        .def_property_readonly("channel_labels", [](const ac3::DecodedFrame& f) {
            return ac3_channel_labels(f.acmod, f.lfe);
        });

    py::class_<ac3::DecodedSubstream>(m, "DecodedSubstream")
        .def_readonly("strmtyp", &ac3::DecodedSubstream::strmtyp)
        .def_readonly("substreamid", &ac3::DecodedSubstream::substreamid)
        .def_readonly("sample_rate", &ac3::DecodedSubstream::sample_rate)
        .def_readonly("acmod", &ac3::DecodedSubstream::acmod)
        .def_readonly("lfe", &ac3::DecodedSubstream::lfe)
        .def_readonly("dialnorm", &ac3::DecodedSubstream::dialnorm)
        .def_readonly("compr", &ac3::DecodedSubstream::compr)
        .def_readonly("dialnorm2", &ac3::DecodedSubstream::dialnorm2)
        .def_readonly("compr2", &ac3::DecodedSubstream::compr2)
        .def_readonly("numblkscod", &ac3::DecodedSubstream::numblkscod)
        .def_readonly("chanmap", &ac3::DecodedSubstream::chanmap)
        .def_readonly("last_dependent", &ac3::DecodedSubstream::last_dependent)
        .def_readonly("object_metadata", &ac3::DecodedSubstream::object_metadata)
        .def_property_readonly("dynrng", [](const ac3::DecodedSubstream& s) { return to_vec(s.dynrng); })
        .def_property_readonly("dynrng2", [](const ac3::DecodedSubstream& s) { return to_vec(s.dynrng2); })
        .def_property_readonly("blksw",
                                [](const ac3::DecodedSubstream& s) { return blksw_to_list(s.blksw); })
        .def_property_readonly("channels",
                                [](const ac3::DecodedSubstream& s) { return channels_to_list(s.channels); })
        .def_property_readonly(
            "object_audio", [](const ac3::DecodedSubstream& s) { return channels_to_list(s.object_audio); })
        .def_property_readonly("channel_labels", [](const ac3::DecodedSubstream& s) {
            return chanmap_labels(s.location_map());
        });

    py::class_<ac3::DecodedAccessUnit>(m, "DecodedAccessUnit")
        .def_readonly("sample_rate", &ac3::DecodedAccessUnit::sample_rate)
        .def_readonly("acmod", &ac3::DecodedAccessUnit::acmod)
        .def_readonly("dialnorm", &ac3::DecodedAccessUnit::dialnorm)
        .def_readonly("compr", &ac3::DecodedAccessUnit::compr)
        .def_readonly("numblkscod", &ac3::DecodedAccessUnit::numblkscod)
        .def_readonly("object_metadata", &ac3::DecodedAccessUnit::object_metadata)
        .def_readonly("substream_count", &ac3::DecodedAccessUnit::substream_count)
        .def_property_readonly("dynrng",
                                [](const ac3::DecodedAccessUnit& u) { return to_vec(u.dynrng); })
        .def_property_readonly(
            "object_audio", [](const ac3::DecodedAccessUnit& u) { return channels_to_list(u.object_audio); })
        .def_property_readonly("channels",
                                [](const ac3::DecodedAccessUnit& u) { return channels_to_list(u.channels); })
        .def_property_readonly("channel_labels", [](const ac3::DecodedAccessUnit& u) {
            // Dual mono has no Table E2.5 layout at all (DecodedAccessUnit::layout's own
            // comment) - fall back to the plain AC-3 acmod labels, same as
            // apps/wasm/decoder_bindings.cpp's apply_layout() does.
            if (u.layout.count == 0) {
                return ac3_channel_labels(u.acmod, false);
            }
            std::vector<std::string> labels;
            for (int i = 0; i < u.layout.count; ++i) {
                labels.emplace_back(ac3::eac3::chanmap::name(u.layout[i]));
            }
            return labels;
        });

    py::class_<ac3::FrameDecoder>(m, "FrameDecoder")
        .def(py::init<>())
        .def(py::init<const ac3::DecoderConfig&>(), py::arg("config"))
        .def(
            "decode_frame",
            [](ac3::FrameDecoder& self, const py::buffer& frame) {
                const auto bytes = to_bytes(frame);
                ac3::DecodedFrame result;
                {
                    py::gil_scoped_release release;
                    auto decoded = self.decode_frame(bytes);
                    if (!decoded) {
                        throw DecodeFailure(decoded.error());
                    }
                    result = std::move(*decoded);
                }
                return result;
            },
            py::arg("frame"), "Decode exactly one AC-3 syncframe")
        .def_property_readonly(
            "latency_samples", [](const ac3::FrameDecoder&) { return ac3::FrameDecoder::latency_samples(); },
            "The delay this decoder adds on top of the encoder's budget: always 0 for AC-3.");

    py::class_<ac3::Eac3Decoder>(m, "Eac3Decoder")
        .def(py::init<>())
        .def(py::init<const ac3::DecoderConfig&>(), py::arg("config"))
        .def(
            "decode_substream",
            [](ac3::Eac3Decoder& self, const py::buffer& frame) -> py::object {
                const auto bytes = to_bytes(frame);
                std::optional<ac3::DecodedSubstream> result;
                {
                    py::gil_scoped_release release;
                    auto decoded = self.decode_substream(bytes);
                    if (!decoded) {
                        throw DecodeFailure(decoded.error());
                    }
                    result = std::move(*decoded);
                }
                return result ? py::cast(*result) : py::none();
            },
            py::arg("frame"),
            "Decode one E-AC-3 syncframe. None means the frame's PCM is held back pending "
            "transient pre-noise processing (§3.7) - see flush().")
        .def(
            "decode_access_unit",
            [](ac3::Eac3Decoder& self, const py::buffer& unit) -> py::object {
                const auto bytes = to_bytes(unit);
                std::optional<ac3::DecodedAccessUnit> result;
                {
                    py::gil_scoped_release release;
                    auto decoded = self.decode_access_unit(bytes);
                    if (!decoded) {
                        throw DecodeFailure(decoded.error());
                    }
                    result = std::move(*decoded);
                }
                return result ? py::cast(*result) : py::none();
            },
            py::arg("unit"),
            "Decode one access unit (independent substream + dependents, as split_access_units "
            "delimits them). Same None convention as decode_substream, for the same reason.")
        .def("flush",
             [](ac3::Eac3Decoder& self) {
                 std::vector<ac3::DecodedSubstream> out;
                 {
                     py::gil_scoped_release release;
                     out = self.flush();
                 }
                 return out;
             })
        .def_property_readonly(
            "latency_samples", &ac3::Eac3Decoder::latency_samples,
            "The delay this decoder adds on top of the encoder's budget: 0 until some "
            "substream's frame sets transproce, SAMPLES_PER_FRAME from then on.");

    // --- Atmos objects -----------------------------------------------------
    py::class_<ac3::oba::AtmosConfig>(m, "AtmosConfig")
        .def(py::init([](py::kwargs kwargs) {
            return KwargBinder<ac3::oba::AtmosConfig>(std::move(kwargs))
                .field("sample_rate", &ac3::oba::AtmosConfig::sample_rate)
                .field("bitrate_kbps", &ac3::oba::AtmosConfig::bitrate_kbps)
                .field("dialnorm", &ac3::oba::AtmosConfig::dialnorm)
                .field("num_bands_idx", &ac3::oba::AtmosConfig::num_bands_idx)
                .field("fine_quant", &ac3::oba::AtmosConfig::fine_quant)
                .field("emit_object_metadata", &ac3::oba::AtmosConfig::emit_object_metadata)
                .field("fast_mdct", &ac3::oba::AtmosConfig::fast_mdct)
                .finish();
        }))
        .def_readwrite("sample_rate", &ac3::oba::AtmosConfig::sample_rate)
        .def_readwrite("bitrate_kbps", &ac3::oba::AtmosConfig::bitrate_kbps)
        .def_readwrite("dialnorm", &ac3::oba::AtmosConfig::dialnorm)
        .def_readwrite("num_bands_idx", &ac3::oba::AtmosConfig::num_bands_idx)
        .def_readwrite("fine_quant", &ac3::oba::AtmosConfig::fine_quant)
        .def_readwrite("emit_object_metadata", &ac3::oba::AtmosConfig::emit_object_metadata)
        .def_readwrite("fast_mdct", &ac3::oba::AtmosConfig::fast_mdct);

    py::class_<ac3::oba::AtmosEncoder>(m, "AtmosEncoder")
        .def(py::init<const ac3::oba::AtmosConfig&, int>(), py::arg("config"), py::arg("objects"))
        .def(
            "encode_frame",
            [](ac3::oba::AtmosEncoder& self, const py::sequence& objects,
               std::vector<ac3::oba::ObjectPlacement> placement) {
                auto owned =
                    extract_channels(objects, static_cast<std::size_t>(ac3::kSamplesPerFrame));
                if (owned.size() != static_cast<std::size_t>(self.dynamic_object_count())) {
                    throw py::value_error("expected " + std::to_string(self.dynamic_object_count()) +
                                          " objects (self.dynamic_object_count), got " +
                                          std::to_string(owned.size()));
                }
                std::vector<std::byte> bytes;
                {
                    py::gil_scoped_release release;
                    auto spans = as_spans(owned);
                    auto result = self.encode_frame(spans, placement);
                    if (!result) {
                        throw EncodeFailure(result.error());
                    }
                    bytes = std::move(result->bytes);
                }
                return py::bytes(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            },
            py::arg("objects"), py::arg("placement"),
            "objects: one mono ac3.SAMPLES_PER_FRAME array per object, in construction order. "
            "placement: one ac3.ObjectPlacement per object. Returns one E-AC-3 access unit as bytes.")
        .def_property_readonly("dynamic_object_count", &ac3::oba::AtmosEncoder::dynamic_object_count)
        .def_property_readonly("program", &ac3::oba::AtmosEncoder::program)
        .def_property_readonly(
            "latency", &ac3::oba::AtmosEncoder::latency,
            "The OBJECT path's budget. Its transform term is TRANSFORM_DELAY_SAMPLES plus the "
            "§7.1 QMF filterbank's own delay (576 samples): JOC reconstruction pulls objects "
            "back out of the decoded bed in a 64-band complex QMF domain, not the MDCT's.")
        .def_property_readonly("latency_samples", &ac3::oba::AtmosEncoder::latency_samples)
        .def_property_readonly(
            "bed_latency", &ac3::oba::AtmosEncoder::bed_latency,
            "The 5.1 bed's budget: what a legacy decoder that ignores the container hears.");

    // --- E-AC-3 encoder (ac3::eac3::FrameEncoder / AccessUnitEncoder), roadmap AP6 ------------
    // A real submodule rather than flat top-level names like Eac3Decoder: ac3::FrameEncoder and
    // ac3::eac3::FrameEncoder share a name across C++ namespaces (roadmap AP2), so ac3.FrameEncoder
    // (AC-3) vs ac3.eac3.FrameEncoder (E-AC-3) is what keeps that collision out of the binding
    // surface. pybind11-direct on ac3::eac3::FrameEncoder/AccessUnitEncoder, same policy as every
    // other class here (see this file's own header comment) - not layered on the C API.
    py::module_ eac3 = m.def_submodule(
        "eac3", "ac3::eac3::FrameEncoder/AccessUnitEncoder - E-AC-3 encoding, including wide "
                 "layouts past 5.1 via dependent substreams.");

    // ac3.StreamType (registered above, shared with DecodedSubstream.strmtyp) is reused here
    // rather than re-bound - pybind11 has one C++-type-to-Python-type mapping process-wide, and a
    // submodule is a namespace for lookup, not a second type registry.

    py::enum_<ac3::plan::LayoutId>(
        eac3, "LayoutId",
        "Named speaker layouts (ac3::plan::LayoutId) - the named-layout convenience "
        "access_unit_config_for_layout() below builds a full AccessUnitConfig from, without "
        "hand-building a dependent's chanmap.")
        .value("kMono", ac3::plan::LayoutId::kMono, "1/0 mono")
        .value("kStereo", ac3::plan::LayoutId::kStereo, "2/0 stereo")
        .value("kDualMono", ac3::plan::LayoutId::kDualMono, "1+1 dual mono")
        .value("k51", ac3::plan::LayoutId::k51, "5.1")
        .value("k71", ac3::plan::LayoutId::k71, "7.1 (one dependent)")
        .value("k512", ac3::plan::LayoutId::k512, "5.1.2 (one dependent)")
        .value("k514", ac3::plan::LayoutId::k514, "5.1.4 (one dependent)")
        .value("k714", ac3::plan::LayoutId::k714, "7.1.4 (two dependents)");

    // Not mirrored on ac3.eac3.FrameConfig: the mixmdate/infomdat metadata groups and vbr/ABR
    // (EQ12) - a real gap, not a stable design decision the way the C API's trim is documented to
    // be; and the internal self-check `trace` hook, which is never exposed anywhere in this
    // binding layer.
    py::class_<ac3::eac3::FrameConfig>(
        eac3, "FrameConfig",
        "One substream's config (ac3::eac3::FrameConfig) - sample rate (including the fscod2 "
        "reduced rates), bitrate, acmod/lfe, the Annex E tools, and substream identity.")
        .def(py::init([](py::kwargs kwargs) {
            return KwargBinder<ac3::eac3::FrameConfig>(std::move(kwargs))
                .field("sample_rate", &ac3::eac3::FrameConfig::sample_rate)
                .field("bitrate_kbps", &ac3::eac3::FrameConfig::bitrate_kbps)
                .field("numblkscod", &ac3::eac3::FrameConfig::numblkscod)
                .field("dialnorm", &ac3::eac3::FrameConfig::dialnorm)
                .field("dialnorm2", &ac3::eac3::FrameConfig::dialnorm2)
                .field("chbwcod", &ac3::eac3::FrameConfig::chbwcod)
                .field("acmod", &ac3::eac3::FrameConfig::acmod)
                .field("lfe", &ac3::eac3::FrameConfig::lfe)
                .field("strmtyp", &ac3::eac3::FrameConfig::strmtyp)
                .field("substreamid", &ac3::eac3::FrameConfig::substreamid)
                .field("chanmap", &ac3::eac3::FrameConfig::chanmap)
                .field("last_dependent", &ac3::eac3::FrameConfig::last_dependent)
                .field("drc", &ac3::eac3::FrameConfig::drc)
                .field("heavy", &ac3::eac3::FrameConfig::heavy)
                .field("drc2", &ac3::eac3::FrameConfig::drc2)
                .field("heavy2", &ac3::eac3::FrameConfig::heavy2)
                .field("auto_tools", &ac3::eac3::FrameConfig::auto_tools)
                .field("coupling", &ac3::eac3::FrameConfig::coupling)
                .field("cplbegf", &ac3::eac3::FrameConfig::cplbegf)
                .field("enhanced", &ac3::eac3::FrameConfig::enhanced)
                .field("spx", &ac3::eac3::FrameConfig::spx)
                .field("spxbegf", &ac3::eac3::FrameConfig::spxbegf)
                .field("spx_atten", &ac3::eac3::FrameConfig::spx_atten)
                .field("spxattencod", &ac3::eac3::FrameConfig::spxattencod)
                .field("aht", &ac3::eac3::FrameConfig::aht)
                .field("gaqmod", &ac3::eac3::FrameConfig::gaqmod)
                .field("transient_prenoise", &ac3::eac3::FrameConfig::transient_prenoise)
                .field("fast_mdct", &ac3::eac3::FrameConfig::fast_mdct)
                .field("dither", &ac3::eac3::FrameConfig::dither)
                .field("oba_complexity_index", &ac3::eac3::FrameConfig::oba_complexity_index)
                .finish();
        }))
        .def_readwrite("sample_rate", &ac3::eac3::FrameConfig::sample_rate)
        .def_readwrite("bitrate_kbps", &ac3::eac3::FrameConfig::bitrate_kbps)
        .def_readwrite("numblkscod", &ac3::eac3::FrameConfig::numblkscod)
        .def_readwrite("dialnorm", &ac3::eac3::FrameConfig::dialnorm)
        .def_readwrite("dialnorm2", &ac3::eac3::FrameConfig::dialnorm2)
        .def_readwrite("chbwcod", &ac3::eac3::FrameConfig::chbwcod)
        .def_readwrite("acmod", &ac3::eac3::FrameConfig::acmod)
        .def_readwrite("lfe", &ac3::eac3::FrameConfig::lfe)
        .def_readwrite("strmtyp", &ac3::eac3::FrameConfig::strmtyp)
        .def_readwrite("substreamid", &ac3::eac3::FrameConfig::substreamid)
        .def_readwrite("chanmap", &ac3::eac3::FrameConfig::chanmap)
        .def_readwrite("last_dependent", &ac3::eac3::FrameConfig::last_dependent)
        .def_readwrite("drc", &ac3::eac3::FrameConfig::drc)
        .def_readwrite("heavy", &ac3::eac3::FrameConfig::heavy)
        .def_readwrite("drc2", &ac3::eac3::FrameConfig::drc2)
        .def_readwrite("heavy2", &ac3::eac3::FrameConfig::heavy2)
        .def_readwrite("auto_tools", &ac3::eac3::FrameConfig::auto_tools)
        .def_readwrite("coupling", &ac3::eac3::FrameConfig::coupling)
        .def_readwrite("cplbegf", &ac3::eac3::FrameConfig::cplbegf)
        .def_readwrite("enhanced", &ac3::eac3::FrameConfig::enhanced)
        .def_readwrite("spx", &ac3::eac3::FrameConfig::spx)
        .def_readwrite("spxbegf", &ac3::eac3::FrameConfig::spxbegf)
        .def_readwrite("spx_atten", &ac3::eac3::FrameConfig::spx_atten)
        .def_readwrite("spxattencod", &ac3::eac3::FrameConfig::spxattencod)
        .def_readwrite("aht", &ac3::eac3::FrameConfig::aht)
        .def_readwrite("gaqmod", &ac3::eac3::FrameConfig::gaqmod)
        .def_readwrite("transient_prenoise", &ac3::eac3::FrameConfig::transient_prenoise)
        .def_readwrite("fast_mdct", &ac3::eac3::FrameConfig::fast_mdct)
        .def_readwrite("dither", &ac3::eac3::FrameConfig::dither)
        .def_readwrite("oba_complexity_index", &ac3::eac3::FrameConfig::oba_complexity_index);

    py::class_<ac3::eac3::FrameMetadata>(eac3, "FrameMetadata", "The §7.7 words for one frame")
        .def(py::init([](py::kwargs kwargs) {
            return KwargBinder<ac3::eac3::FrameMetadata>(std::move(kwargs))
                .field("dynrng", &ac3::eac3::FrameMetadata::dynrng)
                .field("compr", &ac3::eac3::FrameMetadata::compr)
                .field("dynrng2", &ac3::eac3::FrameMetadata::dynrng2)
                .field("compr2", &ac3::eac3::FrameMetadata::compr2)
                .finish();
        }))
        .def_readwrite("dynrng", &ac3::eac3::FrameMetadata::dynrng)
        .def_readwrite("compr", &ac3::eac3::FrameMetadata::compr)
        .def_readwrite("dynrng2", &ac3::eac3::FrameMetadata::dynrng2)
        .def_readwrite("compr2", &ac3::eac3::FrameMetadata::compr2);

    py::class_<ac3::eac3::FrameEncoder>(
        eac3, "FrameEncoder",
        "One substream. AccessUnitEncoder below builds several of these to widen past 5.1.")
        .def(py::init<const ac3::eac3::FrameConfig&>(), py::arg("config"))
        .def(
            "encode_frame",
            [](ac3::eac3::FrameEncoder& self, const py::sequence& channels,
               std::optional<ac3::eac3::FrameMetadata> metadata, const py::buffer& aux) {
                auto owned =
                    extract_channels(channels, static_cast<std::size_t>(self.samples_per_frame()));
                if (owned.size() != static_cast<std::size_t>(self.channel_count())) {
                    throw py::value_error("expected " + std::to_string(self.channel_count()) +
                                          " channels (self.channel_count), got " +
                                          std::to_string(owned.size()));
                }
                const auto aux_bytes = to_bytes(aux);
                std::vector<std::byte> bytes;
                {
                    py::gil_scoped_release release;
                    auto spans = as_spans(owned);
                    auto result = metadata ? self.encode_frame(spans, *metadata, aux_bytes)
                                            : self.encode_frame(spans, aux_bytes);
                    if (!result) {
                        throw EncodeFailure(result.error());
                    }
                    bytes = std::move(*result);
                }
                return py::bytes(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            },
            py::arg("channels"), py::arg("metadata") = py::none(), py::arg("aux") = py::bytes(),
            "channels: a sequence of 1-D float arrays, self.samples_per_frame samples each, AC-3 "
            "channel order (LFE last). metadata: explicit §7.7 words (FrameMetadata) in place of "
            "measuring them from `channels` - AccessUnitEncoder needs this so every substream of "
            "one programme agrees; None measures internally. aux: a pre-built EMDF container. "
            "Returns one syncframe as bytes.")
        .def_property_readonly("config", &ac3::eac3::FrameEncoder::config)
        .def_property_readonly("channel_count", &ac3::eac3::FrameEncoder::channel_count)
        .def_property_readonly("samples_per_frame", &ac3::eac3::FrameEncoder::samples_per_frame)
        .def_property_readonly("latency", &ac3::eac3::FrameEncoder::latency)
        .def_property_readonly("latency_samples", &ac3::eac3::FrameEncoder::latency_samples);

    // dependents/additional are std::vector<FrameConfig>/std::vector<ProgrammeConfig> on the C++
    // side; pybind11/stl.h converts a Python list of eac3.FrameConfig directly. `additional`
    // (further independent programmes, I1-I7) is not mirrored - see docs/library/python-api.md.
    py::class_<ac3::eac3::AccessUnitConfig>(
        eac3, "AccessUnitConfig",
        "The independent substream's config plus its dependents', in transmission order.")
        .def(py::init([](py::kwargs kwargs) {
            return KwargBinder<ac3::eac3::AccessUnitConfig>(std::move(kwargs))
                .field("independent", &ac3::eac3::AccessUnitConfig::independent)
                .field("dependents", &ac3::eac3::AccessUnitConfig::dependents)
                .finish();
        }))
        .def_readwrite("independent", &ac3::eac3::AccessUnitConfig::independent)
        .def_readwrite("dependents", &ac3::eac3::AccessUnitConfig::dependents);

    py::class_<ac3::eac3::AccessUnit>(
        eac3, "AccessUnit", "One encoded access unit: every substream's bytes, concatenated.")
        .def_property_readonly("bytes",
                                [](const ac3::eac3::AccessUnit& u) {
                                    return py::bytes(reinterpret_cast<const char*>(u.bytes.data()),
                                                      u.bytes.size());
                                })
        .def_readonly("substream_bytes", &ac3::eac3::AccessUnit::substream_bytes,
                       "Byte length of each substream (independent first); sums to len(bytes).")
        .def_property_readonly("substream_count", &ac3::eac3::AccessUnit::substream_count);

    py::class_<ac3::eac3::AccessUnitEncoder>(
        eac3, "AccessUnitEncoder",
        "Wide layouts past 5.1: one independent substream plus dependents that widen it. See "
        "access_unit_config_for_layout() for building a config from a named LayoutId.")
        .def(py::init<const ac3::eac3::AccessUnitConfig&>(), py::arg("config"))
        .def(
            "encode_access_unit",
            [](ac3::eac3::AccessUnitEncoder& self, const py::sequence& channels,
               const py::buffer& aux) {
                auto owned =
                    extract_channels(channels, static_cast<std::size_t>(ac3::kSamplesPerFrame));
                if (owned.size() != static_cast<std::size_t>(self.channel_count())) {
                    throw py::value_error("expected " + std::to_string(self.channel_count()) +
                                          " channels (self.channel_count), got " +
                                          std::to_string(owned.size()));
                }
                const auto aux_bytes = to_bytes(aux);
                ac3::eac3::AccessUnit result;
                {
                    py::gil_scoped_release release;
                    auto spans = as_spans(owned);
                    auto encoded = self.encode_access_unit(spans, aux_bytes);
                    if (!encoded) {
                        throw EncodeFailure(encoded.error());
                    }
                    result = std::move(*encoded);
                }
                return result;
            },
            py::arg("channels"), py::arg("aux") = py::bytes(),
            "channels: every channel of the access unit grouped by substream in transmission "
            "order - the independent's first (AC-3 order, LFE last), then each dependent's in the "
            "order its chanmap names them - self.channel_count spans total, "
            "ac3.SAMPLES_PER_FRAME samples each. aux: a pre-built EMDF container.")
        .def_property_readonly("config", &ac3::eac3::AccessUnitEncoder::config)
        .def_property_readonly("channel_count", &ac3::eac3::AccessUnitEncoder::channel_count)
        .def_property_readonly("latency", &ac3::eac3::AccessUnitEncoder::latency)
        .def_property_readonly("latency_samples", &ac3::eac3::AccessUnitEncoder::latency_samples);

    eac3.def("access_unit_config_for_layout", &access_unit_config_for_layout, py::arg("layout"),
              py::arg("bitrate_kbps"), py::arg("dependent_bitrate_kbps") = py::none(),
              py::arg("sample_rate") = ac3::SampleRate::k48000,
              "A ready AccessUnitConfig for a named LayoutId (e.g. eac3.LayoutId.k71), without "
              "hand-building a dependent's chanmap - see ac3::plan::channel_plan_for(). "
              "dependent_bitrate_kbps defaults to half of bitrate_kbps, applied to every "
              "dependent.");
}
