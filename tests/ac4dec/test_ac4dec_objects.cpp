// The decoder on the object audio streams of ac4dec_objects.hpp: A-JOC
// substreams over a var_channel_element() or a static 5.X downmix, and
// direct-coded dynamic objects, a bed and an intermediate spatial format, with
// their object audio metadata. Each stream reads with the writer's trace,
// record for record, every substream to its end.
//
// The streams under tests/golden/ac4dec/objects/ are the committed cases, byte
// for byte, and tests/golden/ac4dec/ holds tools/references/ac4_syntax.py's
// digests of them, which test_ac4dec_syntax.cpp holds the decoder to. With
// AC4DEC_WRITE_OBJECTS set to a directory, this writes the committed cases
// there instead of comparing them, to commit after a change to the builder.

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac4dec/decoder.hpp"
#include "ac4dec_objects.hpp"

namespace {

namespace fs = std::filesystem;
using ac4dec_test::BuiltObjectStream;
using ac4dec_test::ObjectCase;

constexpr int kFrames = 8;

// Every frame read, with the decoder's records the writer's - the same
// substreams, offsets, widths and values, in the same order - and every
// substream read to its end.
void parse_checked(const BuiltObjectStream& stream) {
    std::vector<ac4::SyntaxRecord> read;
    const auto keep = [&read](const ac4::SyntaxRecord& record) { read.push_back(record); };
    ac4::DecoderConfig config;
    config.syntax = keep;
    ac4::Decoder decoder(config);
    for (std::size_t f = 0; f < stream.frames.size(); ++f) {
        read.clear();
        const auto report = decoder.parse(stream.frames[f]);
        INFO("frame " << f);
        REQUIRE(report.has_value());
        for (const ac4::SubstreamReport& s : report->substreams) {
            INFO("substream " << s.index << ": " << s.refused_reason);
            CHECK_FALSE(s.refused.has_value());
            CHECK(s.bits_read == s.size_bits);
        }
        const std::vector<ac4::SyntaxRecord>& written = stream.traces[f];
        for (std::size_t i = 0; i < std::min(read.size(), written.size()); ++i) {
            const bool same = read[i].substream == written[i].substream &&
                              read[i].bit_offset == written[i].bit_offset &&
                              read[i].bits == written[i].bits && read[i].value == written[i].value;
            if (!same) {
                CAPTURE(i, written[i].name, read[i].name, written[i].substream, read[i].substream,
                        written[i].bit_offset, read[i].bit_offset, written[i].value, read[i].value);
                REQUIRE(same);
            }
        }
        REQUIRE(read.size() == written.size());
    }
}

std::vector<std::byte> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    return bytes;
}

}  // namespace

TEST_CASE("object audio streams read as the encoder's writer wrote them", "[ac4dec][objects]") {
    for (const ObjectCase& c : ac4dec_test::committed_object_cases()) {
        CAPTURE(c.name);
        parse_checked(ac4dec_test::build_objects(c, kFrames));
    }
}

TEST_CASE("A-JOC downmixes of one to seven signals read in both codec modes", "[ac4dec][objects]") {
    // Every var_channel_element() shape: one signal, pairs, and an odd count
    // over each var_coding_config, SIMPLE and ASPX (companding_control() up
    // to five signals), with and without the LFE.
    for (int dmx = 1; dmx <= 7; ++dmx) {
        for (const bool aspx : {false, true}) {
            for (const int config : {0, 1}) {
                if (config == 1 && (dmx % 2 == 0 || dmx == 1)) {
                    continue;
                }
                ObjectCase c;
                c.name = "var";
                c.dmx = dmx;
                c.umx = dmx + 1;
                c.aspx = aspx;
                c.lfe = dmx % 2 == 0;
                c.var_coding_config = config;
                CAPTURE(dmx, aspx, config);
                parse_checked(ac4dec_test::build_objects(c, 5));
            }
        }
    }
}

TEST_CASE("the committed object streams are the builder's", "[ac4dec][objects]") {
    const fs::path committed = fs::path{AC4DEC_GOLDEN_DIR} / "objects";
    const char* write_to = std::getenv("AC4DEC_WRITE_OBJECTS");
    for (const ObjectCase& c : ac4dec_test::committed_object_cases()) {
        CAPTURE(c.name);
        const BuiltObjectStream stream =
            ac4dec_test::build_objects(c, ac4dec_test::kCommittedObjectFrames);
        const std::vector<std::byte> bytes = ac4dec_test::sync_framed(stream);
        if (write_to != nullptr) {
            fs::create_directories(write_to);
            std::ofstream out(fs::path{write_to} / (c.name + ".ac4"), std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
            REQUIRE(out.good());
            continue;
        }
        CHECK(read_file(committed / (c.name + ".ac4")) == bytes);
    }
}
