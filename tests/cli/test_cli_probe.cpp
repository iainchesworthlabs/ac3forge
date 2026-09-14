#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

// `ac3cli probe` (roadmap IO1), at the level its consumers actually use it:
// the real binary, run as a subprocess, and the text it puts on stdout.
//
// tests/io/test_probe.cpp already holds the library's own contract - what the
// walk concludes about a stream. What is checked HERE is the part a library
// test cannot see: that the JSON document matches the schema docs/cli/
// commands.md publishes, that the exit code is usable as a gate, and that both
// output forms agree with each other about the same file. A sibling chip is
// told to build an HLS/DASH manifest check on this document, so the fields it
// will read are asserted by name rather than by "the output mentions 5.1
// somewhere".
//
// The external-baseline fixtures matter more here than anywhere else in the
// suite: they are FFmpeg- and DEE-encoded, and they exercise syntax this
// encoder never emits (the DEE E-AC-3 leg uses AHT and spectral extension,
// and most of its frames are ones this decoder declines outright). A probe
// that only ever saw its own encoder's output would be describing a dialect,
// not a format - and the "describes a stream it cannot decode" claim is only
// testable against a stream that really does not decode.
//
// AC3CLI_EXE and AC3FORGE_EXTERNAL_BASELINE_DIR come from tests/CMakeLists.txt;
// run_cli below is a trimmed copy of test_cli.cpp's helper of the same name,
// duplicated per this project's own per-file test-helper convention (see
// test_cli_atmos_adm.cpp, which does the same), including its Windows
// double-quote wrapping - see test_cli.cpp's own comment on std::system() and
// cmd.exe's quoting for why that is needed.

namespace fs = std::filesystem;

namespace {

// See tests/cli/test_cli.cpp's own scratch_dir for the reasoning this copy
// shares; the leaf name below is this file's own.
fs::path scratch_dir() {
    auto dir = fs::path{AC3FORGE_TEST_SCRATCH_DIR} / "cli_probe";
    fs::create_directories(dir);
    return dir;
}

int run_cli(const std::string& args, const fs::path& log) {
    const std::string command =
        "\"" + std::string(AC3CLI_EXE) + "\" " + args + " > \"" + log.string() + "\" 2>&1";
#ifdef _WIN32
    const std::string wrapped = "\"" + command + "\"";
    return std::system(wrapped.c_str());
#else
    return std::system(command.c_str());
#endif
}

std::string read_log(const fs::path& log) {
    std::ifstream in{log, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

// The value of one JSON member, as raw text - enough to assert on a documented
// scalar without pulling a JSON parser into this suite for the sake of thirty
// numbers. Deliberately literal about the separator ("key": ), which is
// exactly what JsonWriter emits, so a change to the writer's punctuation shows
// up here rather than passing silently.
std::string json_field(std::string_view document, std::string_view key) {
    const std::string needle = "\"" + std::string{key} + "\": ";
    const auto at = document.find(needle);
    if (at == std::string_view::npos) {
        return {};
    }
    const auto start = at + needle.size();
    const auto end = document.find_first_of(",\n}]", start);
    auto value = document.substr(start, end - start);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\r')) {
        value.remove_suffix(1);
    }
    return std::string{value};
}

// The text of one nested object, brace-balanced from its own key.
//
// JSON keys are scoped to their object, and probe's document uses that: `min`
// and `max` appear under `access_unit_bytes` and under every metadata range,
// `present` under each of those and again under `authenticity`, `total` and
// `dynamic` under `objects`. A flat "first occurrence of this key" lookup
// silently reads whichever came first - which is how the first draft of this
// file managed to assert `access_unit_bytes.min` while claiming to check
// `dialnorm_db.min`. So a scalar is always looked up inside the section that
// owns it.
std::string json_section(std::string_view document, std::string_view key) {
    const std::string needle = "\"" + std::string{key} + "\": {";
    const auto at = document.find(needle);
    if (at == std::string_view::npos) {
        return {};
    }
    const auto start = at + needle.size() - 1;
    int depth = 0;
    for (auto i = start; i < document.size(); ++i) {
        depth += document[i] == '{' ? 1 : 0;
        depth -= document[i] == '}' ? 1 : 0;
        if (depth == 0) {
            return std::string{document.substr(start, i - start + 1)};
        }
    }
    return {};
}

// A JSON array's elements, whitespace stripped - "11,14" for the payload-id
// list. Written this way rather than matched against the pretty-printed text
// so the assertion survives a change of indentation, and does not depend on
// which line ending the document happened to be written with.
std::string json_array(std::string_view document, std::string_view key) {
    const std::string needle = "\"" + std::string{key} + "\": [";
    const auto at = document.find(needle);
    if (at == std::string_view::npos) {
        return "<missing>";
    }
    const auto start = at + needle.size();
    const auto end = document.find(']', start);
    std::string out;
    for (const char c : document.substr(start, end - start)) {
        if (c != ' ' && c != '\n' && c != '\r' && c != '\t') {
            out.push_back(c);
        }
    }
    return out;
}

fs::path baseline(std::string_view leg, std::string_view file) {
    return fs::path{AC3FORGE_EXTERNAL_BASELINE_DIR} / leg / file;
}

// A short stream from ac3cli itself, so a test that only needs "some valid
// AC-3" does not depend on a committed fixture.
fs::path make_ac3(const std::string& name, const std::string& args) {
    const auto out = scratch_dir() / name;
    const auto log = scratch_dir() / (name + ".log");
    REQUIRE(run_cli("sine \"" + out.string() + "\" " + args, log) == 0);
    REQUIRE(fs::exists(out));
    return out;
}

}  // namespace

TEST_CASE("probe's JSON document carries the schema docs/cli/commands.md publishes",
          "[cli][probe]") {
    const auto input = baseline("ac3-51-448", "ffmpeg.ac3");
    REQUIRE(fs::exists(input));
    const auto log = scratch_dir() / "schema.json";
    REQUIRE(run_cli("probe \"" + input.string() + "\" json=1", log) == 0);
    const auto document = read_log(log);
    INFO(document);

    // The version marker is the contract itself: a consumer keys off it, so a
    // change to it is a change to the promise.
    CHECK(json_field(document, "schema") == "\"ac3forge.probe/1\"");

    // Identity. Cross-checked against ffprobe on this same file, which reports
    // codec_name=ac3, sample_rate=48000, channels=6, bit_rate=448000.
    CHECK(json_field(document, "codec") == "\"ac3\"");
    CHECK(json_field(document, "bsid") == "8");
    CHECK(json_field(document, "sample_rate_hz") == "48000");
    CHECK(json_field(document, "reduced_rate") == "false");
    CHECK(json_field(document, "acmod") == "7");
    CHECK(json_field(document, "lfeon") == "true");
    CHECK(json_field(document, "numblkscod") == "3");
    CHECK(json_field(document, "blocks_per_syncframe") == "6");
    CHECK(json_field(document, "coded_channels") == "6");
    CHECK(json_field(document, "rendered_channels") == "6");
    CHECK(json_field(document, "substreams_per_access_unit") == "1");
    CHECK(document.find("\"stream_type\": \"independent\"") != std::string::npos);

    // Extent. 448 kbit/s at 48 kHz is a whole number of bytes per frame, so
    // the measured rate is exact and the stream is genuinely constant - the
    // one case where an equality on a measured figure is safe.
    CHECK(json_field(document, "nominal_bitrate_kbps") == "448");
    CHECK(json_field(document, "bitrate_kbps") == "448.000");
    CHECK(json_field(document, "variable_bitrate") == "false");
    CHECK(json_field(document, "duration_seconds") == "2.528000");  // == ffprobe's

    // Integrity: an untouched fixture must report clean, since the exit code
    // is built on these.
    const auto integrity = json_section(document, "integrity");
    CHECK(json_field(integrity, "crc_failures") == "0");
    CHECK(json_field(integrity, "parse_failures") == "0");
    CHECK(json_field(integrity, "first_parse_error") == "null");

    // Object audio: absent, and said so explicitly rather than omitted - a
    // consumer reading the field must not have to distinguish "no key" from
    // "no objects".
    const auto objects = json_section(document, "objects");
    CHECK(json_field(objects, "oamd") == "false");
    CHECK(json_field(objects, "joc") == "false");
    CHECK(json_field(objects, "complexity_index") == "null");
    CHECK(json_array(objects, "emdf_payload_ids").empty());

    // Metadata ranges, in the documented units: dialnorm as dB, not the code.
    const auto metadata = json_section(document, "metadata");
    const auto dialnorm = json_section(metadata, "dialnorm_db");
    INFO(dialnorm);
    CHECK(json_field(dialnorm, "present") == "true");
    CHECK(json_field(dialnorm, "min") == "-31");
    CHECK(json_field(dialnorm, "max") == "-31");
    CHECK(json_field(json_section(metadata, "compr"), "present") == "false");

    // Tools: this FFmpeg encode couples every block, which is a fact about the
    // fixture rather than about our encoder - exactly why it is asserted here.
    const auto tools = json_section(document, "tools");
    INFO(tools);
    CHECK(json_field(tools, "blocks") == "474");
    CHECK(json_field(tools, "coupling") == "474");
    CHECK(json_field(tools, "spectral_extension") == "0");
    CHECK(json_field(tools, "aht_syncframes") == "0");
}

TEST_CASE("probe reads a foreign E-AC-3 stream at both tiers", "[cli][probe]") {
    // The DEE leg, and the fixture this test was written around: until the
    // Annex E parsing fixes in decoder/eac3_decoder.cpp (the AHT-in-use flags'
    // conditionality, cplfgaincod/cplfsnroffst, the band-structure reuse rule,
    // the first* per-frame states and the coupling-state reset), most of its
    // syncframes were refused outright and this test asserted exactly that -
    // non-zero parse_failures, a first_parse_error, a non-zero exit - to prove
    // the header tier stood on its own when the parse tier did not.
    //
    // It parses cleanly now, and so does every other committed third-party
    // fixture, so the non-zero side of those counters has no committed stream
    // left to reach it; tests/io/test_probe.cpp owns the synthetic side. What
    // this fixture still proves is worth keeping: the layout, rate, duration
    // and substream map below all come off the wire, and the parse tier agrees
    // with them on a stream no encoder here produced.
    const auto input = baseline("eac3-51-256", "dee.ec3");
    REQUIRE(fs::exists(input));
    const auto log = scratch_dir() / "dee.json";
    const int status = run_cli("probe \"" + input.string() + "\" json=1", log);
    const auto document = read_log(log);
    INFO(document);

    CHECK(json_field(document, "codec") == "\"eac3\"");
    CHECK(json_field(document, "bsid") == "16");
    CHECK(json_field(document, "sample_rate_hz") == "48000");
    CHECK(json_field(document, "acmod") == "7");
    CHECK(json_field(document, "lfeon") == "true");
    CHECK(json_field(document, "rendered_channels") == "6");
    CHECK(json_field(document, "syncframes") == "79");
    CHECK(json_field(document, "duration_seconds") == "2.528000");  // == ffprobe's
    CHECK(json_field(document, "bitrate_kbps") == "256.000");       // == ffprobe's
    // E-AC-3 has no declared-rate field at all, unlike AC-3's frmsizecod.
    CHECK(json_field(document, "nominal_bitrate_kbps") == "null");
    // Every frame's CRC is good and every frame parses. The two counters stay
    // separate because they answer different questions - a stream can be
    // bit-intact and still be syntax this decoder cannot read, which is what
    // this fixture was before the Annex E fixes.
    const auto integrity = json_section(document, "integrity");
    CHECK(json_field(integrity, "crc_failures") == "0");
    CHECK(json_field(integrity, "parse_failures") == "0");
    CHECK(json_field(integrity, "first_parse_error") == "null");
    // ...and the exit code says so, which is what makes probe usable as a gate
    // without parsing its output.
    CHECK(status == 0);

    // AHT is Annex E syntax this encoder never emits, so seeing it reported at
    // all is only possible off a foreign stream. Not all 79: this baseline is
    // DEE's real encoder output (tools/generators/gen_external_baseline.py),
    // and it does not use AHT on every syncframe of this particular leg -
    // measured directly off the committed file, not derived from the total.
    CHECK(json_field(json_section(document, "tools"), "aht_syncframes") == "77");
}

TEST_CASE("probe reports the object layer of an Atmos stream", "[cli][probe][atmos]") {
    const auto out = scratch_dir() / "probe_atmos.ec3";
    const auto log = scratch_dir() / "probe_atmos.log";
    REQUIRE(run_cli("atmos \"" + out.string() + "\" 1 448 4", log) == 0);
    const auto json_log = scratch_dir() / "probe_atmos.json";
    REQUIRE(run_cli("probe \"" + out.string() + "\" json=1", json_log) == 0);
    const auto document = read_log(json_log);
    INFO(document);

    // TS 103 420: OAMD (11) and JOC (14) in the EMDF container, the addbsi
    // complexity index beside them, and the program those describe - 4 dynamic
    // objects plus the bed's LFE.
    const auto objects = json_section(document, "objects");
    INFO(objects);
    CHECK(json_array(objects, "emdf_payload_ids") == "11,14");
    CHECK(json_field(objects, "oamd") == "true");
    CHECK(json_field(objects, "joc") == "true");
    CHECK(json_field(objects, "complexity_index") == "5");
    CHECK(json_field(objects, "total") == "5");
    CHECK(json_field(objects, "dynamic") == "4");
    CHECK(json_field(objects, "lfe") == "true");
    // Nothing signed it, and an unsigned Atmos stream must not read as signed
    // just because it has a container to put a tag in.
    const auto authenticity = json_section(document, "authenticity");
    CHECK(json_field(authenticity, "present") == "false");
    CHECK(json_field(authenticity, "tagged_syncframes") == "0");
}

TEST_CASE("probe reports an authenticity tag without being given a key", "[cli][probe][atmos]") {
    // The key is written here, used to SIGN, and then deliberately not passed
    // to probe: whether a frame carries a tag is answerable from the container
    // alone, and only whether the tag is VALID needs the key.
    const auto key = scratch_dir() / "probe_signing.key";
    {
        std::ofstream out{key, std::ios::binary};
        for (int i = 0; i < 32; ++i) {
            out.put(static_cast<char>(i + 1));
        }
    }
    const auto signed_stream = scratch_dir() / "probe_signed.ec3";
    const auto log = scratch_dir() / "probe_signed.log";
    REQUIRE(run_cli("atmos \"" + signed_stream.string() + "\" 1 448 4 sign-objects signing-key=\"" +
                        key.string() + "\"",
                    log) == 0);

    const auto json_log = scratch_dir() / "probe_signed.json";
    REQUIRE(run_cli("probe \"" + signed_stream.string() + "\" json=1", json_log) == 0);
    const auto document = read_log(json_log);
    INFO(document);
    const auto authenticity = json_section(document, "authenticity");
    CHECK(json_field(authenticity, "present") == "true");
    CHECK(json_field(authenticity, "tagged_syncframes") ==
          json_field(json_section(document, "stream"), "syncframes"));
}

TEST_CASE("probe's detail modes add frames and blocks without changing the summary",
          "[cli][probe]") {
    const auto input = baseline("eac3-stereo-192", "ffmpeg.ec3");
    REQUIRE(fs::exists(input));

    const auto plain = scratch_dir() / "detail_none.json";
    const auto frames = scratch_dir() / "detail_frames.json";
    const auto blocks = scratch_dir() / "detail_blocks.json";
    REQUIRE(run_cli("probe \"" + input.string() + "\" json=1", plain) == 0);
    REQUIRE(run_cli("probe \"" + input.string() + "\" json=1 detail=frames", frames) == 0);
    REQUIRE(run_cli("probe \"" + input.string() + "\" json=1 detail=blocks", blocks) == 0);

    const auto without = read_log(plain);
    const auto with_frames = read_log(frames);
    const auto with_blocks = read_log(blocks);

    // The summary is the same document either way - detail ADDS, it does not
    // change what was already being reported.
    // Scoped to the `stream` object: with detail on, `syncframes` is also the
    // name of each access unit's own array and `blocks` of each syncframe's,
    // so an unscoped lookup would compare the summary against those instead.
    const auto summary_without = json_section(without, "stream");
    const auto summary_frames = json_section(with_frames, "stream");
    const auto summary_blocks = json_section(with_blocks, "stream");
    for (const std::string_view key :
         {"syncframes", "duration_seconds", "bitrate_kbps", "rendered_channels"}) {
        INFO(key);
        CHECK(json_field(summary_without, key) == json_field(summary_frames, key));
        CHECK(json_field(summary_without, key) == json_field(summary_blocks, key));
    }
    CHECK(json_field(json_section(summary_without, "tools"), "blocks") ==
          json_field(json_section(summary_blocks, "tools"), "blocks"));

    // Only the detail forms carry the per-unit array...
    CHECK(without.find("\"access_units\": [") == std::string::npos);
    CHECK(with_frames.find("\"access_units\": [") != std::string::npos);
    CHECK(with_blocks.find("\"access_units\": [") != std::string::npos);
    // ...and only detail=blocks carries the per-block dump under it. The keys
    // checked here exist nowhere else in the document; a bare
    // `exponent_strategy` would NOT do, since the stream summary's own tools
    // object has one of those in every document, detail or no detail.
    CHECK(with_frames.find("\"frame_tools\"") == std::string::npos);
    CHECK(with_frames.find("\"coupling_exponent_strategy\"") == std::string::npos);
    CHECK(with_blocks.find("\"frame_tools\"") != std::string::npos);
    CHECK(with_blocks.find("\"coupling_exponent_strategy\"") != std::string::npos);
    // This FFmpeg stereo encode rematrixes and couples; the block dump has to
    // say so per block, not just in the totals.
    CHECK(with_blocks.find("\"rematrixing\": true") != std::string::npos);
    CHECK(with_blocks.find("\"coupling\": true") != std::string::npos);
    // The coupling channel carries an exponent strategy of its own wherever it
    // is in use; which of the three real ones it is depends on the content, so
    // what is asserted is that SOME strategy was reported rather than the
    // "reuse" an unused slot would default to. Held in a named bool because
    // Catch2 refuses to decompose an assertion containing `||`.
    const bool coupling_strategy =
        with_blocks.find("\"coupling_exponent_strategy\": \"D15\"") != std::string::npos ||
        with_blocks.find("\"coupling_exponent_strategy\": \"D25\"") != std::string::npos ||
        with_blocks.find("\"coupling_exponent_strategy\": \"D45\"") != std::string::npos;
    CHECK(coupling_strategy);
}

TEST_CASE("probe's table and JSON forms agree about the same stream", "[cli][probe]") {
    const auto input = make_ac3("probe_sine.ac3", "1 448 1000 50 51");
    const auto table_log = scratch_dir() / "agree_table.txt";
    const auto json_log = scratch_dir() / "agree.json";
    REQUIRE(run_cli("probe \"" + input.string() + "\"", table_log) == 0);
    REQUIRE(run_cli("probe \"" + input.string() + "\" json=1", json_log) == 0);
    const auto table = read_log(table_log);
    const auto document = read_log(json_log);
    INFO(table);
    INFO(document);

    // Two renderings of one walk. Rather than re-asserting every field, check
    // the ones a reader would use to identify the stream appear in both, in
    // each form's own vocabulary.
    CHECK(table.find("AC-3 (bsid 8)") != std::string::npos);
    CHECK(json_field(document, "codec") == "\"ac3\"");
    CHECK(table.find("48000 Hz") != std::string::npos);
    CHECK(json_field(document, "sample_rate_hz") == "48000");
    CHECK(table.find("3/2 + LFE") != std::string::npos);
    CHECK(json_field(document, "layout_label") == "\"3/2 + LFE\"");
    CHECK(table.find("complete main") != std::string::npos);
    CHECK(json_field(document, "bsmod_label") == "\"complete main\"");
    CHECK(table.find("L C R Ls Rs LFE") != std::string::npos);
    CHECK(document.find("\"L\"") != std::string::npos);
}

TEST_CASE("probe rejects malformed json=/detail= tokens", "[cli][probe]") {
    const auto input = make_ac3("probe_opts.ac3", "1 192");
    const auto log = scratch_dir() / "probe_opts.log";

    for (const std::string bad : {"json=yes", "json=", "detail=all", "detail="}) {
        CHECK(run_cli("probe \"" + input.string() + "\" " + bad, log) != 0);
        const auto text = read_log(log);
        INFO(bad);
        INFO(text);
        CHECK(text.find("error:") != std::string::npos);
    }
    // ...and accepts both real values of each, including the off form: a
    // script building its command line programmatically should not have to
    // omit the token to turn the option off.
    for (const std::string good : {"json=1", "json=0", "detail=frames", "detail=blocks"}) {
        INFO(good);
        CHECK(run_cli("probe \"" + input.string() + "\" " + good, log) == 0);
    }
}

TEST_CASE("probe refuses a file that is not an elementary stream", "[cli][probe]") {
    const auto junk = scratch_dir() / "probe_junk.ac3";
    {
        std::ofstream out{junk, std::ios::binary};
        for (int i = 0; i < 4096; ++i) {
            out.put(static_cast<char>(i * 7));
        }
    }
    const auto log = scratch_dir() / "probe_junk.log";
    CHECK(run_cli("probe \"" + junk.string() + "\"", log) != 0);
    const auto text = read_log(log);
    INFO(text);
    CHECK(text.find("error:") != std::string::npos);
}

// --- AC-4 --------------------------------------------------------------
//
// probe's AC-4 path (run_probe_ac4, summarize_ac4, print_ac4_table,
// write_ac4_stream and friends) is a completely separate walk from the
// AC-3/E-AC-3 one above (see probe.cpp's own top comment on why), dispatched
// by peeking the stream's first byte - 0x0B for AC-3/E-AC-3, 0xAC for AC-4 -
// so nothing above exercises a line of it. The real DEE fixture below (the
// same one tests/ac4/test_ac4.cpp and test_cli_containers.cpp already use)
// covers the "chan" substream shape; the two hand-built streams after it
// cover the "ajoc" and "obj" shapes and a substream group's own OAMD flag,
// none of which any committed fixture reaches - see tests/ac4/test_ac4.cpp's
// own "Synthetic object/A-JOC/OAMD vectors" section for the reasoning and the
// BitWriter this is a trimmed copy of, duplicated per this project's own
// per-file test-helper convention rather than shared across the two test
// binaries' worth of test files.

namespace {

class Ac4BitWriter {
   public:
    void put(std::uint32_t value, int n) {
        for (int i = n - 1; i >= 0; --i) {
            bits_.push_back(((value >> i) & 1u) != 0);
        }
    }

    [[nodiscard]] std::vector<std::byte> bytes() const {
        std::vector<bool> padded = bits_;
        while (padded.size() % 8 != 0) {
            padded.push_back(false);
        }
        std::vector<std::byte> out(padded.size() / 8, std::byte{0});
        for (std::size_t i = 0; i < padded.size(); ++i) {
            if (padded[i]) {
                out[i / 8] |= static_cast<std::byte>(0x80U >> (i % 8));
            }
        }
        return out;
    }

   private:
    std::vector<bool> bits_;
};

// tests/ac4/test_ac4.cpp's write_ac4_object_coded_preamble() and
// write_ac4_object_coded_group_preamble() concatenated - see that file for
// the field-by-field trace against parse_toc()/parse_presentation_v1_info()/
// parse_substream_group_info(): bitstream_version 2, a single presentation
// referencing a single, object-coded substream group (group_index 0),
// fs_index/frame_rate_index chosen so nothing downstream reads an extra bit
// for either.
void write_ac4_preamble(Ac4BitWriter& w) {
    w.put(2, 2);   // bitstream_version = 2
    w.put(0, 10);  // sequence_counter
    w.put(0, 1);   // b_wait_frames
    w.put(0, 1);   // fs_index = 0 (44100 Hz)
    w.put(5, 4);   // frame_rate_index = 5
    w.put(0, 1);   // b_iframe_global
    w.put(1, 1);   // b_single_presentation -> n_presentations = 1
    w.put(0, 1);   // b_payload_base = 0
    w.put(0, 1);   // b_program_id = 0
    // ac4_presentation_v1_info():
    w.put(1, 1);  // b_single_substream_group = 1
    w.put(0, 1);  // presentation_version terminator (unary 0 -> version 0)
    w.put(0, 3);  // md_compat
    w.put(0, 1);  // b_presentation_id = 0
    // frame_rate_multiply_info(frame_rate_index=5): reads 0 bits.
    w.put(0, 1);  // frame_rate_fractions_info: frame_rate_factor==1 branch reads 1 bit
    // emdf_info(): version(2)=0, key_id(3)=0, b_payloads_substream_info(1)=0,
    // emdf_reserved: primary(2)=0, secondary(2)=0.
    w.put(0, 2);
    w.put(0, 3);
    w.put(0, 1);
    w.put(0, 2);
    w.put(0, 2);
    w.put(0, 1);  // b_presentation_filter = 0
    w.put(0, 3);  // ac4_sgi_specifier(): group_index = 0
    w.put(0, 1);  // b_pre_virtualized
    w.put(0, 1);  // b_add_emdf_substreams = 0
    w.put(0, 1);  // b_alternative
    w.put(0, 1);  // b_pres_ndot
    w.put(0, 2);  // ac4_presentation_substream_info()'s substream_index_ref
    // ac4_substream_group_info()'s own preamble for a single-substream,
    // object-coded group: b_substreams_present=1, b_hsf_ext=0,
    // b_single_substream=1 (n_lf_substreams=1, no count field),
    // b_channel_coded=0.
    w.put(1, 1);  // b_substreams_present
    w.put(0, 1);  // b_hsf_ext
    w.put(1, 1);  // b_single_substream
    w.put(0, 1);  // b_channel_coded
}

// tests/ac4/test_ac4.cpp's write_ac4_single_empty_substream_index_table():
// n_substreams=1, one zero-length substream_size entry. probe never reads a
// substream's own audio bytes, so a zero-length entry round-trips fine.
void write_ac4_single_empty_substream_index_table(Ac4BitWriter& w) {
    w.put(1, 2);   // n_substreams = 1
    w.put(1, 1);   // b_size_present
    w.put(0, 1);   // b_more_bits
    w.put(0, 10);  // substream_size = 0
}

// Wraps a TOC's raw bytes (preamble + payload + trailer - exactly the span
// ac4::parse_raw_frame() itself expects, and what tests/ac4/test_ac4.cpp
// hands straight to it) into one Annex G.3.1 syncframe: sync_word 0xAC40 (no
// crc_word - summarize_ac4() only counts a transmitted, failing CRC as a
// failure, so omitting it costs this vector nothing) plus a plain 2-byte
// frame_size, matching ac4::scan()'s own reading of both fields.
std::vector<std::byte> wrap_ac4_syncframe(const std::vector<std::byte>& raw_frame) {
    std::vector<std::byte> out;
    out.push_back(std::byte{0xAC});
    out.push_back(std::byte{0x40});
    const auto size = static_cast<std::uint16_t>(raw_frame.size());
    out.push_back(static_cast<std::byte>(size >> 8));
    out.push_back(static_cast<std::byte>(size & 0xFFU));
    out.insert(out.end(), raw_frame.begin(), raw_frame.end());
    return out;
}

void write_bytes(const fs::path& path, const std::vector<std::byte>& data) {
    std::ofstream out{path, std::ios::binary};
    REQUIRE(out.is_open());
    out.write(reinterpret_cast<const char*>(data.data()),
             static_cast<std::streamsize>(data.size()));
}

}  // namespace

TEST_CASE("probe reads a real AC-4 stream, in table and JSON form", "[cli][probe][ac4]") {
    const auto input = baseline("ac4-stereo-64", "dee.ac4");
    REQUIRE(fs::exists(input));

    const auto json_log = scratch_dir() / "ac4_real.json";
    REQUIRE(run_cli("probe \"" + input.string() + "\" json=1", json_log) == 0);
    const auto document = read_log(json_log);
    INFO(document);
    CHECK(json_field(document, "schema") == "\"ac3forge.probe/1\"");
    const auto stream = json_section(document, "stream");
    CHECK(json_field(stream, "codec") == "\"ac4\"");
    // Cross-checked against tests/ac4/test_ac4.cpp's own scan() of this same
    // fixture: 73 sync frames, every one CRC-clean.
    CHECK(json_field(stream, "access_units") == "73");
    CHECK(json_field(stream, "syncframes") == "73");
    const auto integrity = json_section(stream, "integrity");
    // Unlike the AC-3/E-AC-3 schema above, whose "integrity.crc_valid" is a
    // syncframe COUNT, the AC-4 walk's is a bool (write_ac4_stream's own
    // `crc_failures == 0 && sync_frames > 0`) - the two schemas share a key
    // name but not its type, which is exactly why this file's AC-4 section
    // checks its own document rather than assuming the AC-3/E-AC-3 tests
    // above generalise.
    CHECK(json_field(integrity, "crc_valid") == "true");
    CHECK(json_field(integrity, "crc_failures") == "0");
    CHECK(json_field(integrity, "parse_failures") == "0");
    CHECK(json_field(integrity, "first_parse_error") == "null");
    const auto ac4 = json_section(stream, "ac4");
    CHECK(json_field(ac4, "bitstream_version") == "2");
    CHECK(json_field(ac4, "sample_rate_hz") == "48000");
    CHECK(json_field(ac4, "n_presentations") == "1");
    // The real fixture's one presentation is v1 (test_ac4.cpp's own frame-0
    // check: presentations_v1.size() == 1), so the older presentations_v0
    // array this schema also always carries stays empty rather than absent.
    CHECK(json_array(ac4, "presentations_v0").empty());
    CHECK(document.find("\"kind\": \"chan\"") != std::string::npos);
    CHECK(document.find("\"channel_mode_name\": \"Stereo\"") != std::string::npos);

    const auto table_log = scratch_dir() / "ac4_real.txt";
    REQUIRE(run_cli("probe \"" + input.string() + "\"", table_log) == 0);
    const auto table = read_log(table_log);
    INFO(table);
    CHECK(table.find("AC-4") != std::string::npos);
    CHECK(table.find("bs version") != std::string::npos);
    CHECK(table.find("48000 Hz") != std::string::npos);
    CHECK(table.find("Stereo") != std::string::npos);
    CHECK(table.find("73 of 73 valid") != std::string::npos);
}

TEST_CASE("probe reports a hand-built AC-4 stream's A-JOC substream", "[cli][probe][ac4]") {
    // The exact vector tests/ac4/test_ac4.cpp's "parse_substream_info_ajoc:
    // static_dmx, minimal upmix" test already validated field by field -
    // reused verbatim rather than combined with another vector: an earlier
    // draft of this file spliced this payload onto the OAMD vector below's
    // own fields to cover both in one frame, and that combination came out
    // one field short of what ac4_substream_group_info()/
    // ac4_substream_info_ajoc() actually read together, so parse_raw_frame()
    // read past the end of it (probe's own "truncated" refusal, confirmed
    // against this exact build) - two independently-validated vectors, not
    // one hand-spliced guess.
    Ac4BitWriter w;
    write_ac4_preamble(w);
    w.put(0, 1);  // b_oamd_substream = 0
    w.put(1, 1);  // b_ajoc = 1
    w.put(1, 1);  // b_lfe
    w.put(1, 1);  // b_static_dmx (skips dmx assignment; n_fullband_dmx_signals defaults to 5)
    w.put(0, 1);  // b_oamd_common_data_present
    w.put(0, 4);  // n_fullband_upmix_signals_minus1 = 0 -> 1 signal
    w.put(1, 1);  // bed_dyn_obj_assignment(1): b_dyn_objects_only = 1
    w.put(0, 1);  // b_bitrate_info
    w.put(0, 1);  // b_audio_ndot
    w.put(1, 2);  // substream_index = 1
    w.put(0, 1);  // b_content_type = 0
    write_ac4_single_empty_substream_index_table(w);

    const auto path = scratch_dir() / "ac4_ajoc.ac4";
    write_bytes(path, wrap_ac4_syncframe(w.bytes()));

    const auto json_log = scratch_dir() / "ac4_ajoc.json";
    REQUIRE(run_cli("probe \"" + path.string() + "\" json=1", json_log) == 0);
    const auto document = read_log(json_log);
    INFO(document);
    CHECK(document.find("\"kind\": \"ajoc\"") != std::string::npos);
    CHECK(document.find("\"b_lfe\": true") != std::string::npos);
    CHECK(document.find("\"b_static_dmx\": true") != std::string::npos);
    CHECK(document.find("\"n_fullband_dmx_signals\": 5") != std::string::npos);
    CHECK(document.find("\"n_fullband_upmix_signals\": 1") != std::string::npos);
    CHECK(document.find("\"substream_index\": 1") != std::string::npos);
    // No group carries b_oamd_substream here, so the group's own OAMD field
    // stays null - the companion test below covers the flag itself.
    CHECK(document.find("\"oamd\": null") != std::string::npos);

    const auto table_log = scratch_dir() / "ac4_ajoc.txt";
    REQUIRE(run_cli("probe \"" + path.string() + "\"", table_log) == 0);
    const auto table = read_log(table_log);
    INFO(table);
    CHECK(table.find("A-JOC, 5 dmx + 1 upmix signal(s)") != std::string::npos);
}

TEST_CASE("probe reports a hand-built AC-4 stream's group-level OAMD flag", "[cli][probe][ac4]") {
    // ac4_substream_group_info()'s own OAMD flag (b_oamd_substream) is
    // independent of any one substream's kind - the exact vector
    // tests/ac4/test_ac4.cpp's "parse_oamd_substream_info via
    // ac4_substream_group_info's b_oamd_substream" test already validated,
    // reused verbatim (see the A-JOC test above for why this is its own
    // frame rather than spliced onto that one).
    Ac4BitWriter w;
    write_ac4_preamble(w);
    w.put(1, 1);  // b_oamd_substream = 1
    w.put(1, 1);  // b_oamd_ndot
    w.put(2, 2);  // oamd substream_index = 2
    // The group's one substream still has to be parsed - simplest
    // ac4_substream_info_obj() shape: reserved-bytes branch, 0 bytes.
    w.put(0, 1);  // b_ajoc = 0
    w.put(0, 3);  // n_objects_code (unused)
    w.put(0, 1);  // b_dynamic_objects
    w.put(0, 1);  // b_bed_objects
    w.put(0, 1);  // b_isf
    w.put(0, 4);  // res_bytes = 0
    w.put(0, 1);  // b_bitrate_info
    w.put(0, 1);  // b_audio_ndot
    w.put(0, 2);  // substream_index = 0
    w.put(0, 1);  // b_content_type = 0
    write_ac4_single_empty_substream_index_table(w);

    const auto path = scratch_dir() / "ac4_oamd.ac4";
    write_bytes(path, wrap_ac4_syncframe(w.bytes()));

    const auto json_log = scratch_dir() / "ac4_oamd.json";
    REQUIRE(run_cli("probe \"" + path.string() + "\" json=1", json_log) == 0);
    const auto document = read_log(json_log);
    INFO(document);
    const auto oamd = json_section(document, "oamd");
    INFO(oamd);
    CHECK(json_field(oamd, "b_oamd_ndot") == "true");
    CHECK(json_field(oamd, "substream_index") == "2");
}

TEST_CASE("probe reports a hand-built AC-4 stream's Obj substream and its bed/dynamic objects",
          "[cli][probe][ac4]") {
    // Section 6.3.2.10's dynamic-objects-plus-LFE-bed shape - the exact
    // vector tests/ac4/test_ac4.cpp's "parse_substream_info_obj: dynamic
    // objects with an LFE bed object" test already validated field by field.
    Ac4BitWriter w;
    write_ac4_preamble(w);
    w.put(0, 1);  // b_oamd_substream = 0
    w.put(0, 1);  // b_ajoc = 0 -> ac4_substream_info_obj()
    w.put(2, 3);  // n_objects_code = 2 -> num_objects = 2
    w.put(1, 1);  // b_dynamic_objects
    w.put(1, 1);  // b_lfe
    w.put(0, 1);  // b_bitrate_info
    w.put(0, 1);  // b_audio_ndot
    w.put(1, 2);  // substream_index = 1
    w.put(0, 1);  // b_content_type = 0
    write_ac4_single_empty_substream_index_table(w);

    const auto path = scratch_dir() / "ac4_obj.ac4";
    write_bytes(path, wrap_ac4_syncframe(w.bytes()));

    const auto json_log = scratch_dir() / "ac4_obj.json";
    REQUIRE(run_cli("probe \"" + path.string() + "\" json=1", json_log) == 0);
    const auto document = read_log(json_log);
    INFO(document);
    CHECK(document.find("\"kind\": \"obj\"") != std::string::npos);
    CHECK(document.find("\"b_dynamic_objects\": true") != std::string::npos);
    CHECK(document.find("\"substream_index\": 1") != std::string::npos);
    const auto bed_at = document.find("\"kind\": \"bed\"");
    const auto dyn_at = document.find("\"kind\": \"dyn\"");
    REQUIRE(bed_at != std::string::npos);
    REQUIRE(dyn_at != std::string::npos);
    // objects[0] is the LFE bed object, objects[1] the dynamic one - the
    // order ac4_substream_info_obj() emits them in.
    CHECK(bed_at < dyn_at);
    CHECK(document.find("\"lfe\": true", bed_at) < dyn_at);

    const auto table_log = scratch_dir() / "ac4_obj.txt";
    REQUIRE(run_cli("probe \"" + path.string() + "\"", table_log) == 0);
    const auto table = read_log(table_log);
    INFO(table);
    CHECK(table.find("object, 2 object(s) (dynamic)") != std::string::npos);
}
