// The AC-4 decoder's syntax layer, checked against a second transcription.
//
// tools/references/ac4_syntax.py reads the same substream syntax, transcribed
// separately from ETSI TS 103 190-1 and -2, and writes one digest line per
// frame and substream: how many syntax elements it read, the bit at which the
// last ended, and a CRC-32 over every element's (bit offset, width, value).
// Those lines are committed under tests/golden/ac4dec/, one file per stream,
// and this test requires the decoder's own trace to produce them exactly. A
// field read in the wrong order, at the wrong width or with a different value
// changes the CRC; a count that goes wrong changes the number of elements.
// tools/checks/test_ac4_syntax_digests.py holds the Python parser to the same
// files.
//
// With AC4DEC_TRACE_DIR set, every record is also written there, one file per
// stream, in the same shape the Python parser's `trace` command writes, so a
// disagreement can be found by comparing the two files line by line.
//
// AC4DEC_GOLDEN_DIR and AC4DEC_STREAM_DIR, when both are set, replace the
// committed digests and streams with another set, such as the whole local
// census, whose header lines name streams relative to AC4DEC_STREAM_DIR.
// Such a set may hold streams with syntax this phase refuses, so refusals
// are then compared through the digests alone.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac4/ac4.hpp"
#include "ac4dec/decoder.hpp"

namespace {

namespace fs = std::filesystem;

constexpr const char* kGoldenDir = AC4DEC_GOLDEN_DIR;
constexpr const char* kStreamDir = AC3FORGE_GOLDEN_EXTERNAL_BASELINE_DIR;

struct Directories {
    fs::path golden;
    fs::path streams;
    bool committed = true;  // the committed digests, where no refusal is expected
};

Directories directories() {
    const char* golden = std::getenv("AC4DEC_GOLDEN_DIR");
    const char* streams = std::getenv("AC4DEC_STREAM_DIR");
    if (golden != nullptr && streams != nullptr) {
        return {fs::path{golden}, fs::path{streams}, false};
    }
    return {fs::path{kGoldenDir}, fs::path{kStreamDir}, true};
}

// zlib's CRC-32 (reflected, polynomial 0xEDB88320), the one Python's
// zlib.crc32 computes.
class Crc32 {
   public:
    void update(const std::uint8_t* data, std::size_t size) {
        for (std::size_t i = 0; i < size; ++i) {
            crc_ ^= data[i];
            for (int bit = 0; bit < 8; ++bit) {
                crc_ = (crc_ & 1U) != 0 ? (crc_ >> 1U) ^ 0xEDB88320U : crc_ >> 1U;
            }
        }
    }
    [[nodiscard]] std::uint32_t value() const { return crc_ ^ 0xFFFFFFFFU; }

   private:
    std::uint32_t crc_ = 0xFFFFFFFFU;
};

struct Digest {
    std::uint64_t records = 0;
    std::uint64_t end_bit = 0;
    Crc32 crc;
};

std::string hex32(std::uint32_t value) {
    std::ostringstream out;
    out << "0x";
    for (int shift = 28; shift >= 0; shift -= 4) {
        out << "0123456789abcdef"[(value >> static_cast<unsigned>(shift)) & 0xFU];
    }
    return out.str();
}

std::string_view kind_name(ac4::SubstreamReport::Kind kind) {
    switch (kind) {
        case ac4::SubstreamReport::Kind::kAudio:
            return "audio";
        case ac4::SubstreamReport::Kind::kPresentation:
            return "presentation";
        case ac4::SubstreamReport::Kind::kEmdfPayloads:
            return "emdf_payloads";
        case ac4::SubstreamReport::Kind::kHsfExt:
            return "hsf_ext";
        case ac4::SubstreamReport::Kind::kOamd:
            return "oamd";
        default:
            return "other";
    }
}

std::vector<std::byte> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> out(bytes.size());
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        out[i] = static_cast<std::byte>(bytes[i]);
    }
    return out;
}

struct Golden {
    std::string stream;               // relative to the external-baseline directory
    std::vector<std::string> lines;   // data lines, in file order
};

Golden read_golden(const fs::path& path) {
    Golden golden;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        if (line.front() == '#') {
            const std::string tag = "# ac4-syntax-digest/1 ";
            if (line.starts_with(tag)) {
                golden.stream = line.substr(tag.size());
            }
            continue;
        }
        golden.lines.push_back(line);
    }
    return golden;
}

// How many times something went wrong, and where it first did.
struct Problems {
    int count = 0;
    std::string first;
    void add(std::string what) {
        if (count++ == 0) {
            first = std::move(what);
        }
    }
};

// Parses every frame of the stream a golden file names and requires its
// digest lines; every substream read to its exact end or refused as
// unsupported; and, for a committed stream, nothing refused at all.
void check_stream(const Directories& dirs, const fs::path& golden_path, const char* trace_dir) {
    const Golden golden = read_golden(golden_path);
    INFO("golden file " << golden_path.filename().string() << ", stream " << golden.stream);
    REQUIRE_FALSE(golden.stream.empty());
    const std::vector<std::byte> bytes = read_file(dirs.streams / golden.stream);
    REQUIRE_FALSE(bytes.empty());

    const ac4::ScanResult scan = ac4::scan(bytes);
    REQUIRE_FALSE(scan.stopped_at.has_value());

    std::ofstream trace;
    if (trace_dir != nullptr) {
        trace.open(fs::path{trace_dir} / (golden_path.stem().string() + ".cpp.trace.tsv"));
    }

    int frame_index = 0;
    std::map<int, Digest> digests;
    const auto sink = [&](const ac4::SyntaxRecord& record) {
        Digest& digest = digests[record.substream];
        ++digest.records;
        digest.end_bit = std::uint64_t{record.bit_offset} + record.bits;
        std::array<std::uint8_t, 14> packed{};
        for (int i = 0; i < 4; ++i) {
            packed[static_cast<std::size_t>(i)] =
                static_cast<std::uint8_t>(record.bit_offset >> static_cast<unsigned>(8 * i));
        }
        packed[4] = static_cast<std::uint8_t>(record.bits & 0xFFU);
        packed[5] = static_cast<std::uint8_t>(record.bits >> 8U);
        for (int i = 0; i < 8; ++i) {
            packed[static_cast<std::size_t>(6 + i)] =
                static_cast<std::uint8_t>(record.value >> static_cast<unsigned>(8 * i));
        }
        digest.crc.update(packed.data(), packed.size());
        if (trace.is_open()) {
            trace << frame_index << '\t' << record.substream << '\t' << record.bit_offset << '\t'
                  << record.bits << '\t' << record.value << '\t' << record.name << '\n';
        }
    };
    ac4::DecoderConfig config;
    config.syntax = sink;
    ac4::Decoder decoder(config);

    std::vector<std::string> produced;
    // Refusals other than kUnsupported, and substreams read to a place other
    // than their end: either means a misread even where the digests agree,
    // since the Python parser checks the same.
    Problems refusals;
    Problems unsupported;
    Problems short_reads;
    // A table of contents the inspector refuses yields no substreams and so
    // no digest lines, as the Python parser skips one it cannot read; the
    // digests then say whether the two refused the same frames.
    Problems toc_refusals;
    for (const ac4::SyncFrame& frame : scan.frames) {
        digests.clear();
        const auto report = decoder.parse(frame.raw_ac4_frame);
        if (!report) {
            toc_refusals.add("frame " + std::to_string(frame_index) + ": " +
                             std::string{ac4::describe(report.error())});
            ++frame_index;
            continue;
        }
        for (const ac4::SubstreamReport& substream : report->substreams) {
            const std::string where =
                "frame " + std::to_string(frame_index) + ", substream " + std::to_string(substream.index);
            if (!substream.refused) {
                if (substream.bits_read != substream.size_bits) {
                    short_reads.add(where + ": read " + std::to_string(substream.bits_read) + " of " +
                                    std::to_string(substream.size_bits) + " bits");
                }
            } else if (*substream.refused == ac4::DecodeError::kUnsupported) {
                unsupported.add(where + ": " + std::string{substream.refused_reason});
            } else {
                refusals.add(where + ": " + std::string{substream.refused_reason});
            }
            const auto found = digests.find(substream.index);
            if (found == digests.end()) {
                continue;
            }
            std::ostringstream line;
            line << frame_index << '\t' << substream.index << '\t' << kind_name(substream.kind) << '\t'
                 << found->second.records << '\t' << found->second.end_bit << '\t'
                 << hex32(found->second.crc.value());
            produced.push_back(line.str());
        }
        ++frame_index;
    }

    // The first disagreement only: a misread field usually misaligns
    // every substream after it, and the full trace is where to look.
    const auto [ours, theirs] = std::mismatch(produced.begin(), produced.end(),
                                              golden.lines.begin(), golden.lines.end());
    if (ours != produced.end() || theirs != golden.lines.end()) {
        const auto line = static_cast<std::size_t>(ours - produced.begin());
        INFO("first difference at data line " << line << " (frame, substream, kind, records, end_bit, crc32)");
        CHECK((ours == produced.end() ? std::string{"<no line>"} : *ours) ==
              (theirs == golden.lines.end() ? std::string{"<no line>"} : *theirs));
    }
    CHECK(produced.size() == golden.lines.size());

    {
        INFO("first: " << short_reads.first);
        CHECK(short_reads.count == 0);
    }
    {
        INFO("first: " << refusals.first);
        CHECK(refusals.count == 0);
    }
    // Every committed stream is channel-coded 2.0, 5.1 or IMS, all of which
    // this phase reads to the end. Another set may hold syntax it refuses.
    if (dirs.committed) {
        INFO("first: " << unsupported.first);
        CHECK(unsupported.count == 0);
        INFO("first: " << toc_refusals.first);
        CHECK(toc_refusals.count == 0);
    }
}

}  // namespace

TEST_CASE("ac4dec syntax digests match the independent Python transcription", "[ac4dec][syntax]") {
    const Directories dirs = directories();
    std::vector<fs::path> goldens;
    for (const auto& entry : fs::directory_iterator(dirs.golden)) {
        if (entry.path().extension() == ".tsv") {
            goldens.push_back(entry.path());
        }
    }
    std::sort(goldens.begin(), goldens.end());
    REQUIRE_FALSE(goldens.empty());

    const char* trace_dir = std::getenv("AC4DEC_TRACE_DIR");
    for (const fs::path& golden_path : goldens) {
        DYNAMIC_SECTION(golden_path.filename().string()) {
            check_stream(dirs, golden_path, trace_dir);
        }
    }
}
