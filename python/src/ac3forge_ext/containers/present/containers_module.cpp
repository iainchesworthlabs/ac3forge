#include "optional_modules.hpp"

#include "matroska/matroska.hpp"
#include "matroska/reader.hpp"
#include "mp4/mp4.hpp"
#include "mp4/reader.hpp"
#include "mpegts/mpegts.hpp"
#include "mpegts/reader.hpp"

#include "binding_support.hpp"

// The variant of the `ac3.containers` submodule compiled when matroska/mp4/mpegts are in this build.
//
// This is the body that used to sit inside bindings.cpp's PYBIND11_MODULE
// behind `#ifdef AC3FORGE_PY_HAVE_CONTAINERS`, moved verbatim. See
// optional_modules.hpp for why it is a translation unit now, and
// python/CMakeLists.txt for the selection that picks this file over the
// absent/ one beside it.

namespace ac3::python {

namespace py = pybind11;
using detail::to_bytes;
using detail::to_bytes_list;
using detail::KwargBinder;

void register_containers(py::module_& m) {
    // --- Containers (Python bindings completeness) ------------------------------------------
    //
    // The three writers and the batch read side, bytes in / bytes out. The
    // incremental Reader/Writer classes and the fragmented-MP4/HLS/DASH
    // surface stay C++-only for now - recorded in docs/library/python-api.md
    // as the boundary, not silently missing.
    auto containers = m.def_submodule(
        "containers",
        "Matroska/MP4/MPEG-TS carriage for encoded frames - the library twins of `ac3cli "
        "mkv`/`mp4`/`ts`/`demux`.");

    const auto frames_to_views = [](const std::vector<py::bytes>& frames,
                                    std::vector<std::vector<std::byte>>& storage) {
        storage.reserve(frames.size());
        for (const auto& frame : frames) {
            std::string_view view = frame;
            const auto* data = reinterpret_cast<const std::byte*>(view.data());
            storage.emplace_back(data, data + view.size());
        }
        std::vector<std::span<const std::byte>> views;
        views.reserve(storage.size());
        for (const auto& owned : storage) {
            views.emplace_back(owned);
        }
        return views;
    };

    py::class_<matroska::AudioTrack>(containers, "MatroskaTrack")
        .def(py::init([](py::kwargs kwargs) {
            return KwargBinder<matroska::AudioTrack>(std::move(kwargs))
                .field("codec_id", &matroska::AudioTrack::codec_id)
                .field("sample_rate", &matroska::AudioTrack::sample_rate)
                .field("channels", &matroska::AudioTrack::channels)
                .field("samples_per_frame", &matroska::AudioTrack::samples_per_frame)
                .field("language", &matroska::AudioTrack::language)
                .finish();
        }))
        .def_readwrite("codec_id", &matroska::AudioTrack::codec_id)
        .def_readwrite("sample_rate", &matroska::AudioTrack::sample_rate)
        .def_readwrite("channels", &matroska::AudioTrack::channels)
        .def_readwrite("samples_per_frame", &matroska::AudioTrack::samples_per_frame)
        .def_readwrite("language", &matroska::AudioTrack::language);

    py::class_<mp4::AudioTrack>(containers, "Mp4Track")
        .def(py::init([](py::kwargs kwargs) {
            // codec_config is bytes, which KwargBinder's field() cannot
            // convert - lifted out of the kwargs first, applied after.
            std::vector<std::byte> config;
            if (kwargs.contains("codec_config")) {
                config = to_bytes(py::cast<py::buffer>(kwargs["codec_config"]));
                kwargs.attr("pop")("codec_config");
            }
            auto track = KwargBinder<mp4::AudioTrack>(std::move(kwargs))
                .field("codec_id", &mp4::AudioTrack::codec_id)
                .field("sample_rate", &mp4::AudioTrack::sample_rate)
                .field("channels", &mp4::AudioTrack::channels)
                .field("samples_per_frame", &mp4::AudioTrack::samples_per_frame)
                .field("language", &mp4::AudioTrack::language)
                .field("rfc6381", &mp4::AudioTrack::rfc6381)
                .finish();
            track.codec_config = std::move(config);
            return track;
        }))
        .def_readwrite("codec_id", &mp4::AudioTrack::codec_id)
        .def_readwrite("sample_rate", &mp4::AudioTrack::sample_rate)
        .def_readwrite("channels", &mp4::AudioTrack::channels)
        .def_readwrite("samples_per_frame", &mp4::AudioTrack::samples_per_frame)
        .def_property(
            "codec_config",
            [](const mp4::AudioTrack& t) {
                return py::bytes(reinterpret_cast<const char*>(t.codec_config.data()),
                                 t.codec_config.size());
            },
            [](mp4::AudioTrack& t, const py::buffer& value) {
                t.codec_config = to_bytes(value);
            },
            "The dac3/dec3 sample-entry box payload - build_codec_config_box() produces it.")
        .def_readwrite("language", &mp4::AudioTrack::language)
        .def_readwrite("rfc6381", &mp4::AudioTrack::rfc6381);

    py::class_<mpegts::AudioTrack>(containers, "TsTrack")
        .def(py::init([](py::kwargs kwargs) {
            return KwargBinder<mpegts::AudioTrack>(std::move(kwargs))
                .field("codec", &mpegts::AudioTrack::codec)
                .field("sample_rate", &mpegts::AudioTrack::sample_rate)
                .field("channels", &mpegts::AudioTrack::channels)
                .field("samples_per_frame", &mpegts::AudioTrack::samples_per_frame)
                .finish();
        }))
        .def_readwrite("codec", &mpegts::AudioTrack::codec)
        .def_readwrite("sample_rate", &mpegts::AudioTrack::sample_rate)
        .def_readwrite("channels", &mpegts::AudioTrack::channels)
        .def_readwrite("samples_per_frame", &mpegts::AudioTrack::samples_per_frame);

    py::enum_<mpegts::AudioCodec>(containers, "TsCodec")
        .value("kAc3", mpegts::AudioCodec::kAc3)
        .value("kEac3", mpegts::AudioCodec::kEac3)
        .value("kAc4", mpegts::AudioCodec::kAc4);

    py::enum_<mpegts::BroadcastProfile>(containers, "TsProfile")
        .value("kDvb", mpegts::BroadcastProfile::kDvb)
        .value("kAtsc", mpegts::BroadcastProfile::kAtsc);

    containers.def(
        "mux_matroska",
        [frames_to_views](const matroska::AudioTrack& track,
                          const std::vector<py::bytes>& frames) {
            std::vector<std::vector<std::byte>> storage;
            const auto views = frames_to_views(frames, storage);
            std::vector<std::byte> file;
            {
                py::gil_scoped_release release;
                auto muxed = matroska::mux(track, views);
                if (!muxed) {
                    throw py::value_error(std::string{matroska::describe(muxed.error())});
                }
                file = std::move(*muxed);
            }
            return py::bytes(reinterpret_cast<const char*>(file.data()), file.size());
        },
        py::arg("track"), py::arg("frames"),
        "One Matroska file from encoded frames (one block per access unit).");

    containers.def(
        "mux_mp4",
        [frames_to_views](const mp4::AudioTrack& track, const std::vector<py::bytes>& frames) {
            std::vector<std::vector<std::byte>> storage;
            const auto views = frames_to_views(frames, storage);
            std::vector<std::byte> file;
            {
                py::gil_scoped_release release;
                auto muxed = mp4::mux(track, views);
                if (!muxed) {
                    throw py::value_error(std::string{mp4::describe(muxed.error())});
                }
                file = std::move(*muxed);
            }
            return py::bytes(reinterpret_cast<const char*>(file.data()), file.size());
        },
        py::arg("track"), py::arg("frames"),
        "One MP4/ISOBMFF file from encoded frames (one sample per access unit). "
        "track.codec_config must hold the dac3/dec3 payload - see build_codec_config_box().");

    containers.def(
        "mux_mpegts",
        [frames_to_views](const mpegts::AudioTrack& track, const std::vector<py::bytes>& frames,
                          mpegts::BroadcastProfile profile) {
            std::vector<std::vector<std::byte>> storage;
            const auto views = frames_to_views(frames, storage);
            std::vector<std::byte> file;
            {
                py::gil_scoped_release release;
                auto muxed = mpegts::mux(track, views, mpegts::MuxOptions{.profile = profile});
                if (!muxed) {
                    throw py::value_error(std::string{mpegts::describe(muxed.error())});
                }
                file = std::move(*muxed);
            }
            return py::bytes(reinterpret_cast<const char*>(file.data()), file.size());
        },
        py::arg("track"), py::arg("frames"),
        py::arg("profile") = mpegts::BroadcastProfile::kDvb,
        "One MPEG-2 Transport Stream (PAT + PMT + one PES-wrapped audio PID), identified per "
        "the chosen broadcast profile.");

    containers.def(
        "demux_matroska",
        [](const py::buffer& file) {
            const auto bytes = to_bytes(file);
            const auto demuxed = matroska::demux(bytes);
            if (!demuxed) {
                throw py::value_error(std::string{matroska::describe(demuxed.error())});
            }
            return py::make_tuple(demuxed->track.codec_id,
                                  to_bytes_list(demuxed->frames));
        },
        py::arg("file"),
        "(codec_id, frames) back out of a Matroska file - the read twin of mux_matroska.");

    containers.def(
        "demux_mp4",
        [](const py::buffer& file) {
            const auto bytes = to_bytes(file);
            const auto demuxed = mp4::demux(bytes);
            if (!demuxed) {
                throw py::value_error(std::string{mp4::describe(demuxed.error())});
            }
            const auto& config = demuxed->track.codec_config;
            return py::make_tuple(
                demuxed->track.codec_id,
                py::bytes(reinterpret_cast<const char*>(config.payload.data()),
                          config.payload.size()),
                to_bytes_list(demuxed->samples));
        },
        py::arg("file"),
        "(codec_id, codec_config_payload, samples) back out of an MP4 - the read twin of "
        "mux_mp4. codec_config_payload is the raw dac3/dec3/dac4 box body, verbatim.");

    containers.def(
        "demux_mpegts",
        [](const py::buffer& file) {
            const auto bytes = to_bytes(file);
            const auto demuxed = mpegts::demux(bytes);
            if (!demuxed) {
                throw py::value_error(std::string{mpegts::describe(demuxed.error())});
            }
            const auto codec = demuxed->stream.ac4    ? mpegts::AudioCodec::kAc4
                               : demuxed->stream.eac3 ? mpegts::AudioCodec::kEac3
                                                      : mpegts::AudioCodec::kAc3;
            return py::make_tuple(codec, to_bytes_list(demuxed->payloads));
        },
        py::arg("file"),
        "(codec, pes_payloads) back out of a transport stream. Payloads are PES payloads, "
        "not necessarily one access unit each - concatenate and re-split with "
        "ac3.split_access_units for the A/52 codecs.");
}

}  // namespace ac3::python
