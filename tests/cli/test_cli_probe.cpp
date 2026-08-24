#include <catch2/catch_test_macros.hpp>

#include <cstddef>
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

fs::path scratch_dir() {
    auto dir = fs::temp_directory_path() / "ac3forge_cli_probe_tests";
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

TEST_CASE("probe describes an E-AC-3 stream it cannot fully decode", "[cli][probe]") {
    // The DEE leg. Most of its syncframes are refused by this decoder (a real,
    // pre-existing limitation - 'ac3cli decode' fails outright on this file),
    // which makes it the fixture that proves the header tier stands on its own:
    // the layout, rate, duration and substream map below all come off the wire
    // whatever the parse tier makes of the audio.
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
    // Every frame's CRC is good; what fails is this decoder's own parse. The
    // two counters exist separately precisely so this stream can say so.
    const auto integrity = json_section(document, "integrity");
    CHECK(json_field(integrity, "crc_failures") == "0");
    CHECK(json_field(integrity, "parse_failures") != "0");
    CHECK(json_field(integrity, "first_parse_error") != "null");
    // ...and the exit code reports it, which is what makes probe usable as a
    // gate without parsing its output.
    CHECK(status != 0);

    // AHT is Annex E syntax this encoder never emits, so seeing it reported at
    // all is only possible off a foreign stream.
    CHECK(json_field(json_section(document, "tools"), "aht_syncframes") == "79");
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
