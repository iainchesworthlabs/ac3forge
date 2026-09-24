#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

#include "platform/process.hpp"

#include "ac3/io/wav.hpp"

// The two reading commands - probe (apps/cli/commands/probe.cpp and the JSON
// document apps/common/probe_json.cpp writes through apps/cli/json.cpp) and
// decode (apps/cli/commands/decode.cpp) - on damaged input, on outputs they
// cannot write, and on the stream shapes whose report lines only appear for
// that shape: 1+1's second programme, an object layer, a channel-based bed,
// a dependent substream, Annex D, and E-AC-3's own informational and mixing
// fields. test_cli_probe.cpp holds probe's documented schema; this file
// holds its edges and decode's.
//
// Every stream is made by ac3cli itself, a second or less long; the only
// fixture is one committed AC-4 file.
//
// run_cli and the helpers below are trimmed copies of test_cli.cpp's own,
// duplicated per this project's per-file test-helper convention - including
// the Windows double-quote wrapping explained in test_cli.cpp's run_cli.

namespace fs = std::filesystem;

namespace {

// See tests/cli/test_cli.cpp's own scratch_dir for the reasoning this copy
// shares, including the PID fold; the leaf name below is this file's own.
std::string scratch_pid_suffix() {
    return ac3::test::platform::process_id();
}

fs::path scratch_dir() {
    auto dir = fs::path{AC3FORGE_TEST_SCRATCH_DIR} / ("cli_inspect_edges_" + scratch_pid_suffix());
    fs::create_directories(dir);
    return dir;
}

int run_cli(const std::string& args, const fs::path& log) {
    const std::string command =
        "\"" + std::string(AC3CLI_EXE) + "\" " + args + " > \"" + log.string() + "\" 2>&1";
    return ac3::test::platform::run_shell(command);
}

std::string read_log(const fs::path& log) {
    std::ifstream in{log, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

std::vector<char> read_raw(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

void write_raw(const fs::path& path, const std::vector<char>& bytes) {
    std::ofstream out{path, std::ios::binary};
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string quoted(const fs::path& path) { return "\"" + path.string() + "\""; }

// Makes `path` with one ac3cli run - `command` and `path`, then `rest` -
// once per test process.
fs::path generated(const fs::path& path, std::string_view command, std::string_view rest) {
    if (!fs::exists(path)) {
        const auto log = fs::path{path}.replace_extension(".gen.log");
        REQUIRE(run_cli(std::string{command} + " " + quoted(path) + " " + std::string{rest}, log) ==
                0);
    }
    return path;
}

// A short WAV, one tone per channel, for the encodes a case needs.
fs::path tone_wav(const fs::path& path, std::size_t channels) {
    if (!fs::exists(path)) {
        constexpr std::uint32_t kRate = 48000;
        constexpr std::size_t kFrames = 4800;
        std::vector<std::vector<float>> data(channels, std::vector<float>(kFrames));
        for (std::size_t c = 0; c < channels; ++c) {
            for (std::size_t n = 0; n < kFrames; ++n) {
                data[c][n] = static_cast<float>(
                    0.2 * std::sin(2.0 * std::numbers::pi * (301.0 + 97.0 * static_cast<double>(c)) *
                                   static_cast<double>(n) / kRate));
            }
        }
        REQUIRE(ac3::io::write_wav_f32(path.string(), data, kRate).has_value());
    }
    return path;
}

// A one-second stereo AC-3 stream with one byte of its fourth syncframe
// flipped, so exactly one CRC fails.
fs::path corrupt_ac3() {
    const auto dir = scratch_dir();
    const auto path = dir / "corrupt.ac3";
    if (!fs::exists(path)) {
        auto bytes = read_raw(generated(dir / "clean.ac3", "sine", "1 192"));
        bytes[768 * 3 + 100] = static_cast<char>(bytes[768 * 3 + 100] ^ 0xFF);
        write_raw(path, bytes);
    }
    return path;
}

// Runs `args`, checks the exit code, and returns everything it printed.
std::string run_expecting(const std::string& args, const fs::path& log, int exit_code) {
    const auto rc = run_cli(args, log);
    auto text = read_log(log);
    INFO(text);
    CHECK(rc == exit_code);
    return text;
}

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("probe refuses a missing file, an empty one and a stream that loses sync",
          "[cli][probe]") {
    const auto dir = scratch_dir();
    const auto log = dir / "probe_refused.log";
    const auto missing = dir / "probe_missing.ac3";
    fs::remove(missing);
    CHECK(contains(run_expecting("probe " + quoted(missing), log, 1),
                   "error: cannot open " + missing.string()));

    const auto empty = dir / "probe_empty.ac3";
    write_raw(empty, {});
    CHECK(contains(run_expecting("probe " + quoted(empty), log, 1), "error: no frames in stream"));

    // Two whole syncframes and a run of zeroes where the third should start.
    // The offset is where sync was lost - the third frame's start, 1536 -
    // not the start of the last good syncframe (768), which is what it used
    // to name: an AC-3 access unit is only closed by peeking at the next
    // frame, and the reader reported the unit it was assembling.
    auto bytes = read_raw(generated(dir / "clean.ac3", "sine", "1 192"));
    bytes.resize(768 * 2);
    bytes.resize(768 * 2 + 500, '\0');
    const auto lost = dir / "probe_lost_sync.ac3";
    write_raw(lost, bytes);
    CHECK(contains(run_expecting("probe " + quoted(lost), log, 1),
                   "error: lost sync: expected 0x0B77 at byte 1536"));
}

TEST_CASE("probe counts a corrupt syncframe as a CRC and parse failure and fails the exit code",
          "[cli][probe]") {
    const auto dir = scratch_dir();
    const auto log = dir / "probe_corrupt.log";
    const auto path = corrupt_ac3();
    const auto text = run_expecting("probe " + quoted(path), log, 1);
    CHECK(contains(text, "CRC             31 of 32 syncframe(s) valid"));
    CHECK(contains(text, "parse errors    1 syncframe(s) refused by the parser (first: the "
                         "frame's CRC does not check out)"));

    const auto json = run_expecting("probe " + quoted(path) + " json=1", log, 1);
    CHECK(contains(json, "\"crc_failures\": 1"));
    CHECK(contains(json, "\"parse_failures\": 1"));
    CHECK(contains(json, "\"first_parse_error\": \"the frame's CRC does not check out\""));

    // Frame by frame, the one bad syncframe names its own failure.
    const auto frames = run_expecting("probe " + quoted(path) + " detail=frames", log, 1);
    CHECK(contains(frames, "    parse error: the frame's CRC does not check out"));
    const auto frames_json =
        run_expecting("probe " + quoted(path) + " detail=frames json=1", log, 1);
    CHECK(contains(frames_json, "\"parse_error\": \"the frame's CRC does not check out\""));
}

TEST_CASE("probe describes a 1+1 stream's second programme and its compr words",
          "[cli][probe]") {
    const auto dir = scratch_dir();
    const auto log = dir / "probe_dual.log";
    // heavy/heavy2 make the encoder send compr for Ch1 and compr2 for Ch2.
    const auto path = generated(dir / "dual_heavy.ac3", "sine", "1 192 1000 50 1+1 heavy heavy2");
    const auto text = run_expecting("probe " + quoted(path), log, 0);
    CHECK(contains(text, "renders         2 channel(s), no Table E2.5 layout (dual mono)"));
    CHECK(contains(text, "dialnorm2       -31 dB"));
    CHECK(contains(text, "compr           241"));
    CHECK(contains(text, "compr2          241"));

    const auto frames_json = run_expecting("probe " + quoted(path) + " detail=frames json=1", log, 0);
    CHECK(contains(frames_json, "\"compr\": 241"));
}

TEST_CASE("probe describes an object layer: its EMDF payloads, objects and complexity",
          "[cli][probe][atmos]") {
    const auto dir = scratch_dir();
    const auto log = dir / "probe_objects.log";
    const auto path = generated(dir / "objects.ec3", "atmos", "1 448 2");
    const auto text = run_expecting("probe " + quoted(path), log, 0);
    CHECK(contains(text, "EMDF            payload id(s) 11 (OAMD), 14 (JOC)"));
    CHECK(contains(text, "object audio    3 object(s): bed LFE only, 2 dynamic, in 32 frame(s)"));
    CHECK(contains(text, "complexity      3"));

    const auto frames = run_expecting("probe " + quoted(path) + " detail=frames", log, 0);
    CHECK(contains(frames, "    objects: 3 total, 2 dynamic, bed LFE only"));
    const auto frames_json = run_expecting("probe " + quoted(path) + " detail=frames json=1", log, 0);
    CHECK(contains(frames_json, "\"objects\": {\n"));
    CHECK(contains(frames_json, "\"bed\": \"LFE only\""));
}

TEST_CASE("probe names a channel-based bed by its layout and mask", "[cli][probe][atmos]") {
    const auto dir = scratch_dir();
    const auto log = dir / "probe_bed.log";
    const auto wav = tone_wav(dir / "bed_10ch.wav", 10);
    // atmos-cbi takes its input first, so it is run here rather than through
    // generated().
    const auto bed = dir / "bed_514_cbi.ec3";
    REQUIRE(run_cli("atmos-cbi " + quoted(wav) + " " + quoted(bed) + " 768 5.1.4", log) == 0);
    const auto text = run_expecting("probe " + quoted(bed), log, 0);
    CHECK(contains(text, "object audio    10 object(s): bed 5.1.4 bed, 0 dynamic"));
    const auto json = run_expecting("probe " + quoted(bed) + " json=1", log, 0);
    CHECK(contains(json, "\"bed\": \"5.1.4 bed\""));
    // L/R, C, Ls/Rs, LFE and the four top channels' bits (TS 103 420 Table 5).
    CHECK(contains(json, "\"bed_mask\": 980"));
    CHECK(contains(json, "\"dynamic\": 0"));
}

TEST_CASE("probe lists a dependent substream with its own channel map", "[cli][probe]") {
    const auto dir = scratch_dir();
    const auto log = dir / "probe_dependent.log";
    const auto path = generated(dir / "wide_71.ec3", "eac3-sine", "1 384 1000 50 71");
    const auto json = run_expecting("probe " + quoted(path) + " json=1", log, 0);
    CHECK(contains(json, "\"stream_type\": \"dependent\""));
    // The dependent's Table E2.5 chanmap: decimal in the JSON document, hex
    // in the table, the same 16 bits either way.
    CHECK(contains(json, "\"chanmap\": 6656"));
    CHECK(contains(json, "\"substreams_per_access_unit\": 2"));
    const auto text = run_expecting("probe " + quoted(path), log, 0);
    CHECK(contains(text, "chanmap 0x1a00"));
}

TEST_CASE("probe labels each associated-service bsmod by its Table 5.7 name", "[cli][probe]") {
    const auto dir = scratch_dir();
    const auto log = dir / "probe_bsmod.log";
    const auto clean = generated(dir / "clean.ac3", "sine", "1 192");
    struct Case {
        std::string_view token;
        std::string_view label;
    };
    for (const auto& c : {Case{"vi", "visually impaired"}, Case{"dialogue", "dialogue"},
                          Case{"emergency", "emergency"}}) {
        CAPTURE(c.token);
        const auto stamped = dir / ("bsmod_" + std::string{c.token} + ".ac3");
        REQUIRE(run_cli("metadata " + quoted(clean) + " " + quoted(stamped) + " bsmod=" +
                            std::string{c.token},
                        log) == 0);
        const auto json = run_expecting("probe " + quoted(stamped) + " json=1", log, 0);
        CHECK(contains(json, "\"bsmod_label\": \"" + std::string{c.label} + "\""));
    }
}

TEST_CASE("probe reports an AC-4 stream cut mid-frame and one with a failed CRC",
          "[cli][probe][ac4]") {
    const auto dir = scratch_dir();
    const auto log = dir / "probe_ac4.log";
    const fs::path ac4{std::string{AC3FORGE_EXTERNAL_BASELINE_DIR} + "/ac4-stereo-64/dee.ac4"};
    const auto bytes = read_raw(ac4);
    REQUIRE(bytes.size() > 3000);

    auto cut_bytes = bytes;
    cut_bytes.resize(3000);
    const auto cut = dir / "ac4_cut.ac4";
    write_raw(cut, cut_bytes);
    const auto text = run_expecting("probe " + quoted(cut), log, 1);
    CHECK(contains(text, "parse error     truncated: a declared length or size runs past the end "
                         "of the data"));
    const auto json = run_expecting("probe " + quoted(cut) + " json=1", log, 1);
    CHECK(contains(json, "\"first_parse_error\": \"truncated\""));

    auto flipped = bytes;
    flipped[600] = static_cast<char>(flipped[600] ^ 0x55);
    const auto damaged = dir / "ac4_crc.ac4";
    write_raw(damaged, flipped);
    const auto crc_log = dir / "probe_ac4_crc.log";
    REQUIRE(run_cli("probe " + quoted(damaged), crc_log) >= 0);
    const auto crc_text = read_log(crc_log);
    INFO(crc_text);
    CHECK(contains(crc_text, "CRC             72 of 73 valid"));
}

TEST_CASE("probe's JSON escapes every character a file name can carry that JSON cannot",
          "[cli][probe][json]") {
    // Quote, backslash, the five named control escapes and a bare control
    // byte - none of which Windows allows in a file name, so a filesystem
    // that refuses the name skips the case rather than a preprocessor branch
    // hiding it (tools/checks/check_platform_macros.ps1). Single-quoted for
    // sh, which leaves every one of these characters alone inside '...'.
    const auto dir = scratch_dir();
    const std::string name = std::string{"odd\"name\\"} + "\t\n\r\b\f\x01" + ".ac3";
    const auto path = dir / name;
    std::error_code ec;
    fs::copy_file(generated(dir / "clean.ac3", "sine", "1 192"), path,
                  fs::copy_options::overwrite_existing, ec);
    if (ec) {
        SKIP("this filesystem does not allow control characters in a file name");
    }
    const auto log = dir / "probe_escape.log";
    const auto json = run_expecting("probe '" + path.string() + "' json=1", log, 0);
    CHECK(contains(json, "odd\\\"name\\\\\\t\\n\\r\\b\\f\\u0001.ac3\""));
    fs::remove(path);
}

TEST_CASE("decode refuses a stream too short to frame, one that loses sync and a corrupt frame",
          "[cli][decode]") {
    const auto dir = scratch_dir();
    const auto log = dir / "decode_refused.log";
    const auto out_path = dir / "decode_refused.wav";

    const auto tiny = dir / "decode_tiny.ac3";
    write_raw(tiny, {'\x0B', '\x77', '\x00'});
    const auto zeros = dir / "decode_zeros.ac3";
    std::vector<char> zero_bytes(3000, '\0');
    zero_bytes[0] = '\x0B';
    zero_bytes[1] = '\x77';
    write_raw(zeros, zero_bytes);
    const auto corrupt = corrupt_ac3();
    auto ec3 = read_raw(generated(dir / "clean.ec3", "eac3-silence", "1 192 stereo"));
    ec3[768 * 3 + 100] = static_cast<char>(ec3[768 * 3 + 100] ^ 0xFF);
    const auto corrupt_ec3 = dir / "corrupt.ec3";
    write_raw(corrupt_ec3, ec3);

    struct Case {
        fs::path in;
        std::string message;
    };
    for (const auto& c :
         {Case{tiny, "error: " + tiny.string() + " is too short to hold a syncframe"},
          Case{zeros, "error: " + zeros.string() + ": no 0x0B77 sync word where a frame should "
                                                   "begin"},
          Case{corrupt, "error: " + corrupt.string() + ": the frame's CRC does not check out"},
          Case{corrupt_ec3, "error: decode failed: the frame's CRC does not check out"}}) {
        CAPTURE(c.in.string());
        fs::remove(out_path);
        CHECK(contains(run_expecting("decode " + quoted(c.in) + " " + quoted(out_path), log, 2),
                       c.message));
        CHECK_FALSE(fs::exists(out_path));
    }
}

TEST_CASE("decode refuses an output, census or object directory it cannot write",
          "[cli][decode]") {
    const auto dir = scratch_dir();
    const auto log = dir / "decode_unwritable.log";
    const auto ac3_in = generated(dir / "clean.ac3", "sine", "1 192");
    const auto ec3_in = generated(dir / "clean.ec3", "eac3-silence", "1 192 stereo");
    const auto objects = generated(dir / "objects.ec3", "atmos", "1 448 2");
    const auto nowhere = dir / "no_such_directory" / "out.wav";
    for (const auto& in : {ac3_in, ec3_in}) {
        CAPTURE(in.string());
        CHECK(contains(run_expecting("decode " + quoted(in) + " " + quoted(nowhere), log, 3),
                       "error: cannot open " + nowhere.string() + " for writing"));
    }
    const auto census = dir / "no_such_directory" / "census.json";
    CHECK(contains(run_expecting("decode " + quoted(ac3_in) + " " + quoted(dir / "census.wav") +
                                     " bap-census=" + quoted(census),
                                 log, 3),
                   "error: cannot open bap-census output " + census.string()));
    // A regular file where the objects directory's parent should be.
    const auto blocker = dir / "objects_blocker";
    std::ofstream{blocker} << "x";
    CHECK(contains(run_expecting("decode " + quoted(objects) + " " + quoted(dir / "objects.wav") +
                                     " " + quoted(blocker / "objects"),
                                 log, 3),
                   "error: cannot create directory " + (blocker / "objects").string()));
}

TEST_CASE("decode warns when object or ADM output is asked of a stream with no object layer",
          "[cli][decode]") {
    const auto dir = scratch_dir();
    const auto log = dir / "decode_no_objects.log";
    const auto objects_dir = dir / "unused_objects";
    const auto adm = dir / "unused_adm.wav";

    const auto ac3_in = generated(dir / "clean.ac3", "sine", "1 192");
    auto text = run_expecting("decode " + quoted(ac3_in) + " " + quoted(dir / "plain_ac3.wav") +
                                  " " + quoted(objects_dir) + " " + quoted(adm),
                              log, 0);
    CHECK(contains(text, "warning: objects_dir given but " + ac3_in.string() +
                             " is plain AC-3 - it has no object layer"));
    CHECK(contains(text, "warning: " + adm.string() + " given but " + ac3_in.string() +
                             " is plain AC-3 - it has no object layer"));

    const auto ec3_in = generated(dir / "clean.ec3", "eac3-silence", "1 192 stereo");
    text = run_expecting("decode " + quoted(ec3_in) + " " + quoted(dir / "plain_ec3.wav") + " " +
                             quoted(objects_dir) + " " + quoted(adm),
                         log, 0);
    CHECK(contains(text, "warning: " + adm.string() +
                             " given but no dynamic-object-only Atmos programme was decoded"));
    CHECK(contains(text,
                   "warning: objects_dir given but there is no reconstructed object audio to "
                   "export"));
    CHECK_FALSE(fs::exists(adm));
}

TEST_CASE("decode's DRC report says what each drcmode did with dynrng and compr",
          "[cli][decode][drc]") {
    const auto dir = scratch_dir();
    const auto log = dir / "decode_drcmode.log";
    const auto path = generated(dir / "dual_heavy.ac3", "sine", "1 192 1000 50 1+1 heavy heavy2");
    auto text = run_expecting("decode " + quoted(path) + " " + quoted(dir / "rf.wav") +
                                  " drcmode=rf",
                              log, 0);
    CHECK(contains(text, "dialnorm 31 (dialogue at -31 dBFS), normalised to the -31 dBFS "
                         "reference (+0.00 dB)"));
    CHECK(contains(text, "applied only where no compr word exists (drcmode=rf, \xC2\xA7" "7.7.2.1)"));
    CHECK(contains(text, "over 32 frames, applied with RF mode's +11 dB (drcmode=rf)"));
    CHECK(contains(text, "compr2 present"));

    text = run_expecting("decode " + quoted(path) + " " + quoted(dir / "line.wav") +
                             " drcmode=line",
                         log, 0);
    CHECK(contains(text, "dynrng +0.00 .. +0.00 dB, applied in full (drcmode=line)"));
    CHECK(contains(text, "not applied (drcmode=line uses dynrng)"));
}

TEST_CASE("decode reports E-AC-3's informational and mixing metadata as it was authored",
          "[cli][decode][metadata]") {
    const auto dir = scratch_dir();
    const auto log = dir / "decode_eac3_meta.log";
    const auto stereo = tone_wav(dir / "meta_stereo.wav", 2);
    const auto mono = tone_wav(dir / "meta_mono.wav", 1);
    const auto encode = [&](const fs::path& in, const fs::path& out, std::string_view rest) {
        REQUIRE(run_cli("eac3-encode " + quoted(in) + " " + quoted(out) + " " + std::string{rest},
                        log) == 0);
        return out;
    };

    const auto info = encode(stereo, dir / "meta_info.ec3",
                             "192 none stereo dsurmod=on dheadphonmod=on mixlevel=100 "
                             "roomtyp=small adconvtyp=hdcd");
    auto text = run_expecting("decode " + quoted(info) + " " + quoted(dir / "meta_info.wav"), log, 0);
    CHECK(contains(text, "  dsurmod: Dolby Surround encoded"));
    CHECK(contains(text, "  dheadphonmod: Dolby Headphone encoded"));
    CHECK(contains(text, "  mixed at 100 dB SPL, small room, flat monitor, A/D HDCD"));

    const auto premix = encode(mono, dir / "meta_premix.ec3",
                               "96 none mono paninfo=10 mixdef=premix premixcmp=compr:local:3");
    text = run_expecting("decode " + quoted(premix) + " " + quoted(dir / "meta_premix.wav"), log, 0);
    CHECK(contains(text, "  mixdef 1 (premix compression)"));
    CHECK(contains(text, "    premix compression: compr word, this substream source, 3/6"));
    // panmean 10 is 15 degrees: 1.5 degrees a step (§E2.3.1.54).
    CHECK(contains(text, "  pan: 15.0 degrees clockwise from centre (paninfo 0)"));

    const auto reserved =
        encode(mono, dir / "meta_reserved.ec3", "96 none mono mixdef=reserved mixdata=291");
    text = run_expecting("decode " + quoted(reserved) + " " + quoted(dir / "meta_reserved.wav"),
                         log, 0);
    CHECK(contains(text, "  mixdef 2 (reserved): 0x123"));
}

TEST_CASE("decode reports an AC-3 Annex D stream's alternate bsi", "[cli][decode][metadata]") {
    const auto dir = scratch_dir();
    const auto log = dir / "decode_annexd.log";
    const auto stereo = tone_wav(dir / "annexd_stereo.wav", 2);
    const auto path = dir / "annexd.ac3";
    REQUIRE(run_cli("encode " + quoted(stereo) + " " + quoted(path) +
                        " 192 stereo dheadphonmod=on dmixmod=ltrt",
                    log) == 0);
    const auto text = run_expecting("decode " + quoted(path) + " " + quoted(dir / "annexd.wav"),
                                    log, 0);
    CHECK(contains(text, "bsid 6 (Annex D alternate syntax)"));
    CHECK(contains(text, "  dheadphonmod: Dolby Headphone encoded"));
    CHECK(contains(text, "  xbsi1: preferred downmix Lt/Rt"));
}
