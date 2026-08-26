#include "ac3/verify/trace_export.hpp"

#include <cstddef>
#include <string>

#include "ac3/core/tables.hpp"  // kBlocksPerFrame

namespace ac3::verify {

namespace {

void append_row(std::string& out, std::uint64_t frame, int substream, int block, int stream,
                std::string_view kind, int index, long long value) {
    out += std::to_string(frame);
    out += ',';
    out += std::to_string(substream);
    out += ',';
    out += std::to_string(block);
    out += ',';
    out += std::to_string(stream);
    out += ',';
    out += kind;
    out += ',';
    out += std::to_string(index);
    out += ',';
    out += std::to_string(value);
    out += '\n';
}

void append_json_row(std::string& out, std::uint64_t frame, int substream, int block, int stream,
                     std::string_view kind, int index, long long value) {
    out += "{\"frame\":";
    out += std::to_string(frame);
    out += ",\"substream\":";
    out += std::to_string(substream);
    out += ",\"block\":";
    out += std::to_string(block);
    out += ",\"stream\":";
    out += std::to_string(stream);
    out += ",\"kind\":\"";
    out += kind;
    out += "\",\"index\":";
    out += std::to_string(index);
    out += ",\"value\":";
    out += std::to_string(value);
    out += "}\n";
}

// One stream's worth of rows, CSV or JSON Lines picked by `emit`. Templated
// on the row function rather than branching per call inside the loop, so
// neither format pays for the other's string literals.
template <typename Emit>
void append_stream_rows(const Emit& emit, std::uint64_t frame, int substream, int block,
                        int stream_index, const StreamTrace& stream) {
    for (std::size_t bin = 0; bin < stream.exponents.size(); ++bin) {
        emit(frame, substream, block, stream_index, "exponent", static_cast<int>(bin),
             static_cast<long long>(stream.exponents[bin]));
    }
    for (std::size_t bin = 0; bin < stream.bap.size(); ++bin) {
        emit(frame, substream, block, stream_index, "bap", static_cast<int>(bin),
             static_cast<long long>(stream.bap[bin]));
    }
    for (std::size_t band = 0; band < stream.mask.size(); ++band) {
        emit(frame, substream, block, stream_index, "mask", static_cast<int>(band),
             static_cast<long long>(stream.mask[band]));
    }
    emit(frame, substream, block, stream_index, "snr_offset", 0,
         static_cast<long long>(stream.snr_offset));
}

template <typename Emit>
void append_stream_rows(const Emit& emit, std::uint64_t frame, int substream, int block,
                        int stream_index, const Eac3StreamTrace& stream) {
    for (std::size_t bin = 0; bin < stream.exponents.size(); ++bin) {
        emit(frame, substream, block, stream_index, "exponent", static_cast<int>(bin),
             static_cast<long long>(stream.exponents[bin]));
    }
    for (std::size_t bin = 0; bin < stream.bap.size(); ++bin) {
        emit(frame, substream, block, stream_index, "bap", static_cast<int>(bin),
             static_cast<long long>(stream.bap[bin]));
    }
    for (std::size_t band = 0; band < stream.mask.size(); ++band) {
        emit(frame, substream, block, stream_index, "mask", static_cast<int>(band),
             static_cast<long long>(stream.mask[band]));
    }
    emit(frame, substream, block, stream_index, "snr_offset", 0,
         static_cast<long long>(stream.snr_offset));
}

template <typename Emit>
void append_ac3_rows(const Emit& emit, const FrameTrace& trace, std::uint64_t frame_index) {
    for (int block = 0; block < kBlocksPerFrame; ++block) {
        const auto& block_trace = trace.blocks[static_cast<std::size_t>(block)];
        if (!block_trace.allocated) {
            continue;
        }
        for (std::size_t s = 0; s < block_trace.streams.size(); ++s) {
            append_stream_rows(emit, frame_index, 0, block, static_cast<int>(s),
                               block_trace.streams[s]);
        }
    }
}

template <typename Emit>
void append_eac3_rows(const Emit& emit, const Eac3AccessUnitTrace& trace,
                      std::uint64_t frame_index) {
    const auto substreams = trace.substreams();
    for (std::size_t sub = 0; sub < substreams.size(); ++sub) {
        const auto& substream = substreams[sub];
        for (int block = 0; block < kBlocksPerFrame; ++block) {
            const auto& block_trace = substream.blocks[static_cast<std::size_t>(block)];
            if (!block_trace.allocated) {
                continue;
            }
            for (std::size_t s = 0; s < block_trace.streams.size(); ++s) {
                append_stream_rows(emit, frame_index, static_cast<int>(sub), block,
                                   static_cast<int>(s), block_trace.streams[s]);
            }
        }
    }
}

}  // namespace

std::string_view trace_csv_header() { return "frame,substream,block,stream,kind,index,value\n"; }

void append_trace_csv(const FrameTrace& trace, std::uint64_t frame_index, std::string& out) {
    append_ac3_rows(
        [&out](auto&&... args) { append_row(out, args...); }, trace, frame_index);
}

void append_trace_csv(const Eac3AccessUnitTrace& trace, std::uint64_t frame_index,
                      std::string& out) {
    append_eac3_rows(
        [&out](auto&&... args) { append_row(out, args...); }, trace, frame_index);
}

void append_trace_json_lines(const FrameTrace& trace, std::uint64_t frame_index,
                             std::string& out) {
    append_ac3_rows(
        [&out](auto&&... args) { append_json_row(out, args...); }, trace, frame_index);
}

void append_trace_json_lines(const Eac3AccessUnitTrace& trace, std::uint64_t frame_index,
                             std::string& out) {
    append_eac3_rows(
        [&out](auto&&... args) { append_json_row(out, args...); }, trace, frame_index);
}

}  // namespace ac3::verify
