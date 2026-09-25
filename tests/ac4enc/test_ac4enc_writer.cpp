// The AC-4 encoder's syntax writer (src/ac4enc/src), each piece read back by
// what reads AC-4 here: variable_bits() and every Huffman codeword by the
// decoder's bit reader, the sync frame and its CRC by the inspector's scan,
// and a whole frame by the inspector and the decoder, whose trace must equal
// the writer's record for record.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac4/ac4.hpp"
#include "ac4/syntax.hpp"
#include "ac4dec/decoder.hpp"
#include "ac4enc/encoder.hpp"
#include "asf/layout.hpp"
#include "bit_reader.hpp"
#include "bit_writer.hpp"
#include "frame/frame_writer.hpp"
#include "frame/metadata.hpp"
#include "huffman.hpp"
#include "pcm/drc.hpp"
#include "tables/huffman_codes.hpp"
#include "tables/huffman_tables.hpp"

namespace {

using ac4::detail::BitReader;
using ac4::detail::BitWriter;

std::vector<ac4::SyntaxRecord> records_of(std::span<const ac4::SyntaxRecord> all, int substream) {
    std::vector<ac4::SyntaxRecord> out;
    for (const ac4::SyntaxRecord& r : all) {
        if (r.substream == substream) {
            out.push_back(r);
        }
    }
    return out;
}

void require_same(std::span<const ac4::SyntaxRecord> written, std::span<const ac4::SyntaxRecord> read) {
    REQUIRE(written.size() == read.size());
    for (std::size_t i = 0; i < written.size(); ++i) {
        CAPTURE(i, written[i].name, read[i].name);
        CHECK(written[i].bit_offset == read[i].bit_offset);
        CHECK(written[i].bits == read[i].bits);
        CHECK(written[i].value == read[i].value);
    }
}

}  // namespace

TEST_CASE("variable_bits written by the encoder reads back through the decoder's reader", "[ac4enc][writer]") {
    for (const unsigned n : {2U, 3U, 5U, 7U}) {
        for (std::uint64_t value = 0; value < 3000; value += (value < 300 ? 1 : 37)) {
            CAPTURE(n, value);
            BitWriter w;
            w.write_variable_bits(n, value, "v");
            w.write(3, 5, "tail");
            CHECK(w.bit_position() == ac4::detail::variable_bits_width(n, value) + 3);
            BitReader r(w.bytes(), 0, {});
            CHECK(r.variable_bits(static_cast<int>(n), "v") == value);
            CHECK(r.read(3, "tail") == 5U);
            CHECK_FALSE(r.overflow());
        }
    }
}

TEST_CASE("every codeword of every spectral and scale factor codebook reads back as its index",
          "[ac4enc][writer]") {
    const auto check_book = [](const ac4::detail::Codebook& book, std::span<const ac4::detail::HuffCode> codes) {
        CAPTURE(book.name);
        REQUIRE(codes.size() == book.codebook_length);
        BitWriter w;
        for (std::size_t index = 0; index < codes.size(); ++index) {
            w.write_codeword(codes, index, "cw");
        }
        BitReader r(w.bytes(), 0, {});
        for (std::size_t index = 0; index < codes.size(); ++index) {
            CHECK(ac4::detail::huff_decode(r, book, "cw") == static_cast<int>(index));
        }
    };
    for (std::size_t cb = 1; cb <= 11; ++cb) {
        check_book(*ac4::detail::tables::kAsfSpectrumCodebooks[cb], ac4::detail::tables::kAsfSpectrumCodes[cb]);
    }
    check_book(ac4::detail::tables::kAsfHcbScalefac, ac4::detail::tables::kAsfHcbScalefacCodes);
}

TEST_CASE("a buffered writer's records land where its bits do", "[ac4enc][writer]") {
    std::vector<ac4::SyntaxRecord> records;
    const auto sink = [&](const ac4::SyntaxRecord& r) { records.push_back(r); };
    BitWriter inner = BitWriter::buffered();
    inner.write(4, 9, "a");
    inner.write(0, 0, "empty");  // a zero-width element is not recorded
    inner.write(7, 100, "b");
    BitWriter outer(3, sink);
    outer.write(5, 17, "head");
    outer.append(inner);
    REQUIRE(records.size() == 3);
    CHECK(records[1].bit_offset == 5);
    CHECK(records[1].substream == 3);
    CHECK(records[2].bit_offset == 9);
    CHECK(records[2].value == 100);
    BitReader r(outer.bytes(), 0, {});
    CHECK(r.read(5, "") == 17U);
    CHECK(r.read(4, "") == 9U);
    CHECK(r.read(7, "") == 100U);
}

TEST_CASE("sync frames carry the raw frame and, with 0xAC41, a CRC the inspector accepts", "[ac4enc][writer]") {
    std::vector<std::byte> raw(300);
    for (std::size_t i = 0; i < raw.size(); ++i) {
        raw[i] = static_cast<std::byte>(i * 37 + 11);
    }
    for (const bool crc : {false, true}) {
        CAPTURE(crc);
        std::vector<std::byte> stream = ac4::sync_frame(raw, crc);
        const std::vector<std::byte> second = ac4::sync_frame(raw, crc);
        stream.insert(stream.end(), second.begin(), second.end());
        const ac4::ScanResult scan = ac4::scan(stream);
        REQUIRE(scan.frames.size() == 2);
        CHECK_FALSE(scan.stopped_at.has_value());
        for (const ac4::SyncFrame& frame : scan.frames) {
            CHECK(frame.sync_word == (crc ? 0xAC41 : 0xAC40));
            CHECK(std::equal(frame.raw_ac4_frame.begin(), frame.raw_ac4_frame.end(), raw.begin(), raw.end()));
            if (crc) {
                REQUIRE(frame.crc_ok.has_value());
                CHECK(*frame.crc_ok);
            } else {
                CHECK_FALSE(frame.crc_ok.has_value());
            }
        }
    }
    // A frame past 0xFFFF bytes takes the 24-bit frame_size.
    const std::vector<std::byte> big(70000, std::byte{0x5A});
    const ac4::ScanResult scan = ac4::scan(ac4::sync_frame(big, true));
    REQUIRE(scan.frames.size() == 1);
    CHECK(scan.frames[0].raw_ac4_frame.size() == big.size());
    CHECK(scan.frames[0].crc_ok.value_or(false));
}

TEST_CASE("a frame of the writer's reads to the end of every substream, with the writer's trace",
          "[ac4enc][writer]") {
    for (const bool stereo : {true, false}) {
        for (const std::size_t frame_bytes : {std::size_t{0}, std::size_t{400}, std::size_t{1029}}) {
            CAPTURE(stereo, frame_bytes);
            ac4::detail::FrameFields fields;
            fields.ch_mode = stereo ? 1 : 0;
            fields.sequence_counter = 17;
            fields.dialnorm_bits = 96;
            // An element that codes nothing: a long frame with no bands.
            BitWriter audio = BitWriter::buffered();
            if (stereo) {
                audio.write(2, 0, "stereo_codec_mode");
                audio.write(1, 0, "b_enable_mdct_stereo_proc");
                for (int track = 0; track < 2; ++track) {
                    audio.write(1, 0, "spec_frontend");
                    audio.write(1, 1, "b_long_frame");
                    audio.write(6, 0, "max_sfb");
                }
            } else {
                audio.write(1, 0, "mono_codec_mode");
                audio.write(1, 0, "spec_frontend");
                audio.write(1, 1, "b_long_frame");
                audio.write(6, 0, "max_sfb");
            }
            for (int track = 0; track < (stereo ? 2 : 1); ++track) {
                audio.write(8, 0, "reference_scale_factor");
                audio.write(1, 0, "b_snf_data_exists");
            }
            std::vector<ac4::SyntaxRecord> written;
            const auto sink = [&](const ac4::SyntaxRecord& r) { written.push_back(r); };
            const auto frame = ac4::detail::write_frame(fields, audio, frame_bytes, sink);
            REQUIRE(frame.has_value());
            if (frame_bytes > 0) {
                CHECK(frame->size() == frame_bytes);
            }

            const auto parsed = ac4::parse_raw_frame(*frame);
            REQUIRE(parsed.has_value());
            CHECK(parsed->toc.bitstream_version == 2);
            CHECK(parsed->toc.sequence_counter == 17);
            CHECK(parsed->toc.frame_rate_index == 13);
            CHECK(parsed->toc.wait_frames == 0);
            REQUIRE(parsed->toc.substream_groups.size() == 1);
            const auto& chan = parsed->toc.substream_groups[0].substreams.at(0).chan;
            REQUIRE(chan.has_value());
            CHECK(chan->ch_mode == (stereo ? 1 : 0));

            std::vector<ac4::SyntaxRecord> read;
            const auto reader_sink = [&](const ac4::SyntaxRecord& r) { read.push_back(r); };
            ac4::DecoderConfig config;
            config.syntax = reader_sink;
            ac4::Decoder decoder(config);
            const auto report = decoder.parse(*frame);
            REQUIRE(report.has_value());
            for (const ac4::SubstreamReport& substream : report->substreams) {
                CAPTURE(substream.index, substream.refused_reason);
                CHECK_FALSE(substream.refused.has_value());
                CHECK(substream.bits_read == substream.size_bits);
            }
            require_same(records_of(written, 0), records_of(read, 0));
            require_same(records_of(written, 1), records_of(read, 1));
        }
    }
}

TEST_CASE("Table 109's grouping bit counts are what the layouts write", "[ac4enc][writer]") {
    for (int a = 0; a < 4; ++a) {
        for (int b = 0; b < 4; ++b) {
            CAPTURE(a, b);
            const ac4::detail::FrameLayout layout = ac4::detail::split_layout(2048, {a, b}, {-1, -1});
            CHECK(static_cast<int>(layout.grouping_bits.size()) == ac4::detail::grouping_bit_count({a, b}));
            int total = 0;
            for (const int length : layout.window_length) {
                total += length;
            }
            CHECK(total == 2048);
        }
    }
}

TEST_CASE("a DRC profile sent as a curve is the profile the decoder's Table 162 gives",
          "[ac4enc][writer][drc]") {
    // The writer's Table 162 in Table 166's terms, read back through the
    // decoder's Table 166, against the decoder's own Table 162: two
    // transcriptions of each table meeting.
    using P = ac4::DrcProfile;
    for (const P profile :
         {P::kFilmStandard, P::kFilmLight, P::kMusicStandard, P::kMusicLight, P::kSpeech}) {
        CAPTURE(static_cast<int>(profile));
        const ac4::detail::CurveCodes c = ac4::detail::curve_codes(profile);
        const ac4::detail::DrcCurve sent = ac4::detail::drc_curve(
            ac4::detail::DrcCompressionCurve{.drc_lev_nullband_low = c.lev_nullband_low,
                                             .drc_lev_nullband_high = c.lev_nullband_high,
                                             .drc_gain_max_boost = c.gain_max_boost,
                                             .drc_lev_max_boost = c.lev_max_boost,
                                             .drc_nr_boost_sections = c.nr_boost_sections,
                                             .drc_gain_section_boost = c.gain_section_boost,
                                             .drc_lev_section_boost = c.lev_section_boost,
                                             .drc_gain_max_cut = c.gain_max_cut,
                                             .drc_lev_max_cut = c.lev_max_cut,
                                             .drc_nr_cut_sections = c.nr_cut_sections,
                                             .drc_gain_section_cut = c.gain_section_cut,
                                             .drc_lev_section_cut = c.lev_section_cut,
                                             .drc_tc_default_flag = c.tc_default,
                                             .drc_tc_attack = c.tc_attack,
                                             .drc_tc_release = c.tc_release,
                                             .drc_tc_attack_fast = c.tc_attack_fast,
                                             .drc_tc_release_fast = c.tc_release_fast,
                                             .drc_adaptive_smoothing_flag = c.adaptive_smoothing,
                                             .drc_attack_threshold = c.attack_threshold,
                                             .drc_release_threshold = c.release_threshold});
        const std::optional<ac4::detail::DrcCurve> table =
            ac4::detail::drc_default_curve(static_cast<int>(profile));
        REQUIRE(table.has_value());
        // The curves agree at every level, from far below the null band to
        // far above.
        for (int level = -60; level <= 50; ++level) {
            CAPTURE(level);
            CHECK(sent.gain(level) == table->gain(level));
        }
        CHECK(sent.attack_ms == table->attack_ms);
        CHECK(sent.release_ms == table->release_ms);
        CHECK(sent.attack_fast_ms == table->attack_fast_ms);
        CHECK(sent.release_fast_ms == table->release_fast_ms);
        CHECK(sent.adaptive == table->adaptive);
        CHECK(sent.attack_threshold == table->attack_threshold);
        CHECK(sent.release_threshold == table->release_threshold);
    }
}
