// Writes the AC-4 decoder's syntax trace of each stream named on the command
// line, for tools/checks/ac4_syntax_differential.py to compare with the Python
// transcription's (tools/references/ac4_syntax.py).
//
//   ac4_syntax_trace [--last-frame] <out_dir> <file.ac4>...
//
// writes <out_dir>/<stem>.cpp.tsv for each file, one line per record or
// status, tab-separated; with --last-frame, for the stream's last frame only
// (every frame is still decoded, so that its state builds up):
//   T  frame offset:size...                          the substream layout the
//                                                    table of contents gives
//   R  frame substream bit_offset width value name   one syntax record
//   S  frame substream kind status reason            one substream's outcome;
//                                                    status is ok, size_mismatch
//                                                    (read to a place other than
//                                                    its end) or the error
//   F  frame error                                   the table of contents failed
//   X  scan stopped                                  the sync frames stopped
//
// Not a CMake target: a development tool, built by hand against a build of
// the two static libraries. With MSVC, from a developer prompt at the repo
// root, against a build tree in <b>:
//
//   cl /nologo /std:c++latest /EHsc /utf-8 /MD /O2 /DAC4_STATIC_DEFINE
//      /DAC4DEC_STATIC_DEFINE /Isrc/ac4/include /Isrc/ac4dec/include
//      /I<b>/src/ac4/generated /I<b>/src/ac4dec/generated
//      tools/checks/ac4_syntax_trace.cpp /link <b>/src/ac4dec/ac4dec_static.lib
//      <b>/src/ac4/ac4_static.lib
//
// With GCC or Clang:
//
//   g++ -std=c++23 -O2 -o ac4_syntax_trace tools/checks/ac4_syntax_trace.cpp
//      -DAC4_STATIC_DEFINE -DAC4DEC_STATIC_DEFINE -Isrc/ac4/include
//      -Isrc/ac4dec/include -I<b>/src/ac4/generated -I<b>/src/ac4dec/generated
//      <b>/src/ac4dec/libac4dec_static.a <b>/src/ac4/libac4_static.a

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "ac4/ac4.hpp"
#include "ac4dec/decoder.hpp"

namespace {

namespace fs = std::filesystem;

const char* kind_name(ac4::SubstreamReport::Kind kind) {
    switch (kind) {
        case ac4::SubstreamReport::Kind::kAudio:
            return "audio";
        case ac4::SubstreamReport::Kind::kPresentation:
            return "presentation";
        case ac4::SubstreamReport::Kind::kEmdfPayloads:
            return "emdf_payloads";
        default:
            return "other";
    }
}

const char* error_name(ac4::DecodeError error) {
    switch (error) {
        case ac4::DecodeError::kTruncated:
            return "truncated";
        case ac4::DecodeError::kInvalidToc:
            return "invalid_toc";
        case ac4::DecodeError::kInvalidStream:
            return "invalid_stream";
        case ac4::DecodeError::kUnsupported:
            return "unsupported";
        case ac4::DecodeError::kMissingIFrame:
            return "missing_iframe";
    }
    return "unknown";
}

std::vector<std::byte> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    return bytes;
}

void trace(const fs::path& path, const fs::path& out_dir, bool last_frame_only) {
    const std::vector<std::byte> bytes = read_file(path);
    std::ofstream out(out_dir / (path.stem().string() + ".cpp.tsv"), std::ios::binary);
    const ac4::ScanResult scan = ac4::scan(bytes);
    const int first_written = last_frame_only ? static_cast<int>(scan.frames.size()) - 1 : 0;
    int frame_index = 0;
    const auto sink = [&](const ac4::SyntaxRecord& r) {
        if (frame_index < first_written) {
            return;
        }
        out << "R\t" << frame_index << '\t' << r.substream << '\t' << r.bit_offset << '\t' << r.bits << '\t'
            << r.value << '\t' << r.name << '\n';
    };
    ac4::DecoderConfig config;
    config.syntax = sink;
    ac4::Decoder decoder(config);
    for (const ac4::SyncFrame& frame : scan.frames) {
        const auto report = decoder.parse(frame.raw_ac4_frame);
        if (frame_index < first_written) {
            ++frame_index;
            continue;
        }
        // The substream layout the table of contents gives, so that a
        // disagreement there is told apart from one in a substream's syntax.
        if (const auto raw = ac4::parse_raw_frame(frame.raw_ac4_frame)) {
            out << "T\t" << frame_index;
            for (const ac4::Substream& s : raw->substreams) {
                out << '\t' << s.offset << ':' << s.size;
            }
            out << '\n';
        }
        if (!report) {
            out << "F\t" << frame_index << '\t' << error_name(report.error()) << '\n';
        } else {
            for (const ac4::SubstreamReport& s : report->substreams) {
                out << "S\t" << frame_index << '\t' << s.index << '\t' << kind_name(s.kind) << '\t';
                if (s.refused) {
                    out << error_name(*s.refused) << '\t' << s.refused_reason << '\n';
                } else if (s.bits_read != s.size_bits) {
                    // The Python transcription requires every substream to
                    // end where its size says; the decoder reports the
                    // difference instead of failing on it.
                    out << "size_mismatch\tread " << s.bits_read << " of " << s.size_bits << " bits\n";
                } else {
                    out << "ok\t\n";
                }
            }
        }
        ++frame_index;
    }
    if (scan.stopped_at) {
        out << "X\tscan stopped\n";
    }
}

}  // namespace

int main(int argc, char** argv) {
    int arg = 1;
    const bool last_frame_only = argc > 1 && std::string_view{argv[1]} == "--last-frame";
    if (last_frame_only) {
        ++arg;
    }
    if (argc - arg < 2) {
        std::fprintf(stderr, "usage: ac4_syntax_trace [--last-frame] <out_dir> <file.ac4>...\n");
        return 2;
    }
    const fs::path out_dir{argv[arg]};
    fs::create_directories(out_dir);
    for (int i = arg + 1; i < argc; ++i) {
        trace(fs::path{argv[i]}, out_dir, last_frame_only);
    }
    return 0;
}
