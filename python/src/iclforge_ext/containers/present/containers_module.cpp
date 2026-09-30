#include "optional_modules.hpp"

#include "iclforge/matroska/matroska.hpp"
#include "iclforge/matroska/reader.hpp"
#include "iclforge/mp4/mp4.hpp"
#include "iclforge/mp4/reader.hpp"
#include "iclforge/mpegts/mpegts.hpp"
#include "iclforge/mpegts/reader.hpp"

#include "binding_support.hpp"

// The variant of the `ac3.containers` submodule compiled when matroska/mp4/mpegts are in this build.
//
// This is the body that used to sit inside bindings.cpp's PYBIND11_MODULE
// behind `#ifdef ICLFORGE_PY_HAVE_CONTAINERS`, moved verbatim. See
// optional_modules.hpp for why it is a translation unit now, and
// python/CMakeLists.txt for the selection that picks this file over the
// absent/ one beside it.

namespace iclforge::python {

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
        "Matroska/MP4/MPEG-TS carriage for encoded frames - the library twins of `forge "
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

    py::class_<iclforge::matroska::AudioTrack>(containers, "MatroskaTrack")
        .def(py::init([](py::kwargs kwargs) {
            return KwargBinder<iclforge::matroska::AudioTrack>(std::move(kwargs))
                .field("codec_id", &iclforge::matroska::AudioTrack::codec_id)
                .field("sample_rate", &iclforge::matroska::AudioTrack::sample_rate)
                .field("channels", &iclforge::matroska::AudioTrack::channels)
                .field("samples_per_frame", &iclforge::matroska::AudioTrack::samples_per_frame)
                .field("language", &iclforge::matroska::AudioTrack::language)
                .finish();
        }))
        .def_readwrite("codec_id", &iclforge::matroska::AudioTrack::codec_id)
        .def_readwrite("sample_rate", &iclforge::matroska::AudioTrack::sample_rate)
        .def_readwrite("channels", &iclforge::matroska::AudioTrack::channels)
        .def_readwrite("samples_per_frame", &iclforge::matroska::AudioTrack::samples_per_frame)
        .def_readwrite("language", &iclforge::matroska::AudioTrack::language);

    py::class_<iclforge::mp4::AudioTrack>(containers, "Mp4Track")
        .def(py::init([](py::kwargs kwargs) {
            // codec_config is bytes, which KwargBinder's field() cannot
            // convert - lifted out of the kwargs first, applied after.
            std::vector<std::byte> config;
            if (kwargs.contains("codec_config")) {
                config = to_bytes(py::cast<py::buffer>(kwargs["codec_config"]));
                kwargs.attr("pop")("codec_config");
            }
            auto track = KwargBinder<iclforge::mp4::AudioTrack>(std::move(kwargs))
                .field("codec_id", &iclforge::mp4::AudioTrack::codec_id)
                .field("sample_rate", &iclforge::mp4::AudioTrack::sample_rate)
                .field("channels", &iclforge::mp4::AudioTrack::channels)
                .field("samples_per_frame", &iclforge::mp4::AudioTrack::samples_per_frame)
                .field("language", &iclforge::mp4::AudioTrack::language)
                .field("rfc6381", &iclforge::mp4::AudioTrack::rfc6381)
                .finish();
            track.codec_config = std::move(config);
            return track;
        }))
        .def_readwrite("codec_id", &iclforge::mp4::AudioTrack::codec_id)
        .def_readwrite("sample_rate", &iclforge::mp4::AudioTrack::sample_rate)
        .def_readwrite("channels", &iclforge::mp4::AudioTrack::channels)
        .def_readwrite("samples_per_frame", &iclforge::mp4::AudioTrack::samples_per_frame)
        .def_property(
            "codec_config",
            [](const iclforge::mp4::AudioTrack& t) {
                return py::bytes(reinterpret_cast<const char*>(t.codec_config.data()),
                                 t.codec_config.size());
            },
            [](iclforge::mp4::AudioTrack& t, const py::buffer& value) {
                t.codec_config = to_bytes(value);
            },
            "The dac3/dec3 sample-entry box payload - build_codec_config_box() produces it.")
        .def_readwrite("language", &iclforge::mp4::AudioTrack::language)
        .def_readwrite("rfc6381", &iclforge::mp4::AudioTrack::rfc6381);

    py::class_<iclforge::mpegts::AudioTrack>(containers, "TsTrack")
        .def(py::init([](py::kwargs kwargs) {
            return KwargBinder<iclforge::mpegts::AudioTrack>(std::move(kwargs))
                .field("codec", &iclforge::mpegts::AudioTrack::codec)
                .field("sample_rate", &iclforge::mpegts::AudioTrack::sample_rate)
                .field("channels", &iclforge::mpegts::AudioTrack::channels)
                .field("samples_per_frame", &iclforge::mpegts::AudioTrack::samples_per_frame)
                .finish();
        }))
        .def_readwrite("codec", &iclforge::mpegts::AudioTrack::codec)
        .def_readwrite("sample_rate", &iclforge::mpegts::AudioTrack::sample_rate)
        .def_readwrite("channels", &iclforge::mpegts::AudioTrack::channels)
        .def_readwrite("samples_per_frame", &iclforge::mpegts::AudioTrack::samples_per_frame);

    py::enum_<iclforge::mpegts::AudioCodec>(containers, "TsCodec")
        .value("kAc3", iclforge::mpegts::AudioCodec::kAc3)
        .value("kEac3", iclforge::mpegts::AudioCodec::kEac3)
        .value("kAc4", iclforge::mpegts::AudioCodec::kAc4);

    py::enum_<iclforge::mpegts::BroadcastProfile>(containers, "TsProfile")
        .value("kDvb", iclforge::mpegts::BroadcastProfile::kDvb)
        .value("kAtsc", iclforge::mpegts::BroadcastProfile::kAtsc);

    containers.def(
        "mux_matroska",
        [frames_to_views](const iclforge::matroska::AudioTrack& track,
                          const std::vector<py::bytes>& frames) {
            std::vector<std::vector<std::byte>> storage;
            const auto views = frames_to_views(frames, storage);
            std::vector<std::byte> file;
            {
                py::gil_scoped_release release;
                auto muxed = iclforge::matroska::mux(track, views);
                if (!muxed) {
                    throw py::value_error(std::string{iclforge::matroska::describe(muxed.error())});
                }
                file = std::move(*muxed);
            }
            return py::bytes(reinterpret_cast<const char*>(file.data()), file.size());
        },
        py::arg("track"), py::arg("frames"),
        "One Matroska file from encoded frames (one block per access unit).");

    containers.def(
        "mux_mp4",
        [frames_to_views](const iclforge::mp4::AudioTrack& track,
                          const std::vector<py::bytes>& frames) {
            std::vector<std::vector<std::byte>> storage;
            const auto views = frames_to_views(frames, storage);
            std::vector<std::byte> file;
            {
                py::gil_scoped_release release;
                auto muxed = iclforge::mp4::mux(track, views);
                if (!muxed) {
                    throw py::value_error(std::string{iclforge::mp4::describe(muxed.error())});
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
        [frames_to_views](const iclforge::mpegts::AudioTrack& track,
                          const std::vector<py::bytes>& frames,
                          iclforge::mpegts::BroadcastProfile profile) {
            std::vector<std::vector<std::byte>> storage;
            const auto views = frames_to_views(frames, storage);
            std::vector<std::byte> file;
            {
                py::gil_scoped_release release;
                auto muxed = iclforge::mpegts::mux(
                    track, views, iclforge::mpegts::MuxOptions{.profile = profile});
                if (!muxed) {
                    throw py::value_error(std::string{iclforge::mpegts::describe(muxed.error())});
                }
                file = std::move(*muxed);
            }
            return py::bytes(reinterpret_cast<const char*>(file.data()), file.size());
        },
        py::arg("track"), py::arg("frames"),
        py::arg("profile") = iclforge::mpegts::BroadcastProfile::kDvb,
        "One MPEG-2 Transport Stream (PAT + PMT + one PES-wrapped audio PID), identified per "
        "the chosen broadcast profile.");

    containers.def(
        "demux_matroska",
        [](const py::buffer& file) {
            const auto bytes = to_bytes(file);
            const auto demuxed = iclforge::matroska::demux(bytes);
            if (!demuxed) {
                throw py::value_error(std::string{iclforge::matroska::describe(demuxed.error())});
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
            const auto demuxed = iclforge::mp4::demux(bytes);
            if (!demuxed) {
                throw py::value_error(std::string{iclforge::mp4::describe(demuxed.error())});
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
            const auto demuxed = iclforge::mpegts::demux(bytes);
            if (!demuxed) {
                throw py::value_error(std::string{iclforge::mpegts::describe(demuxed.error())});
            }
            const auto codec = demuxed->stream.ac4    ? iclforge::mpegts::AudioCodec::kAc4
                               : demuxed->stream.eac3 ? iclforge::mpegts::AudioCodec::kEac3
                                                      : iclforge::mpegts::AudioCodec::kAc3;
            return py::make_tuple(codec, to_bytes_list(demuxed->payloads));
        },
        py::arg("file"),
        "(codec, pes_payloads) back out of a transport stream. Payloads are PES payloads, "
        "not necessarily one access unit each - concatenate and re-split with "
        "ac3.split_access_units for the A/52 codecs.");
}

}  // namespace iclforge::python
