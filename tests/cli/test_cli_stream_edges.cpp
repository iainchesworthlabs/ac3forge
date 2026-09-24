#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "ac3/decoder/decoder.hpp"
#include "ac3/meta/bsi.hpp"

// The commands that take an already-encoded stream somewhere else - the
// container wrappers (apps/cli/commands/containers.cpp: mkv, mp4, fmp4, ts,
// demux) and the stream tools (apps/cli/commands/stream_tools.cpp:
// transcode, metadata, normalize, cut, cat) - on the inputs and outputs they
// have to turn away, plus the multi-substream and dual-mono shapes their
// reports describe differently. test_cli_containers.cpp and
// test_cli_stream_tools.cpp hold the round trips; this file holds the edges.
//
// Every input here is made by ac3cli itself (sine/eac3-sine/eac3-silence),
// one second long, so the only fixtures are the committed AC-4 ones.
//
// The stream tools answer every failure - an unreadable input and an
// unwritable output included - with exit code 1, where the encode and
// decode commands use 2 and 3 (apps/cli/exit_codes.hpp). Those cases below
// check only for a non-zero exit, so they stay true whichever way that is
// settled.
//
// run_cli and the helpers below are trimmed copies of test_cli.cpp's own,
// duplicated per this project's per-file test-helper convention - including
// the Windows double-quote wrapping explained in test_cli.cpp's run_cli.

namespace fs = std::filesystem;

namespace {

// See tests/cli/test_cli.cpp's own scratch_dir for the reasoning this copy
// shares, including the PID fold; the leaf name below is this file's own.
std::string scratch_pid_suffix() {
#ifdef _WIN32
    return std::to_string(_getpid());
#else
    return std::to_string(getpid());
#endif
}

fs::path scratch_dir() {
    auto dir = fs::path{AC3FORGE_TEST_SCRATCH_DIR} / ("cli_stream_edges_" + scratch_pid_suffix());
    fs::create_directories(dir);
    return dir;
}

// See test_cli.cpp's child_exit_code: POSIX std::system() hands back a wait()
// status word, not the exit code itself.
int child_exit_code(int system_status) {
#ifdef _WIN32
    return system_status;
#else
    if (system_status == -1) {
        return system_status;
    }
    return WIFEXITED(system_status) ? WEXITSTATUS(system_status)
                                    : 128 + WTERMSIG(system_status);
#endif
}

int run_cli(const std::string& args, const fs::path& log) {
    const std::string command =
        "\"" + std::string(AC3CLI_EXE) + "\" " + args + " > \"" + log.string() + "\" 2>&1";
#ifdef _WIN32
    const std::string wrapped = "\"" + command + "\"";
    return child_exit_code(std::system(wrapped.c_str()));
#else
    return child_exit_code(std::system(command.c_str()));
#endif
}

std::string read_log(const fs::path& log) {
    std::ifstream in{log, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

std::vector<std::byte> read_bytes(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    const std::vector<char> raw{std::istreambuf_iterator<char>{in},
                                std::istreambuf_iterator<char>{}};
    std::vector<std::byte> out(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        out[i] = static_cast<std::byte>(raw[i]);
    }
    return out;
}

std::string quoted(const fs::path& path) { return "\"" + path.string() + "\""; }

// Makes `path` with one ac3cli generator run, once per test process.
fs::path generated(const fs::path& path, std::string_view generator_args) {
    if (!fs::exists(path)) {
        const auto log = fs::path{path}.replace_extension(".gen.log");
        REQUIRE(run_cli(std::string{generator_args.substr(0, generator_args.find(' '))} + " " +
                            quoted(path) + std::string{generator_args.substr(
                                               generator_args.find(' '))},
                        log) == 0);
    }
    return path;
}

// The shared inputs: one-second AC-3 stereo, AC-3 1+1, AC-3 silence and
// E-AC-3 7.1 (an independent substream plus a dependent one).
struct Inputs {
    fs::path stereo;
    fs::path dual_mono;
    fs::path silent;
    fs::path wide;
};

Inputs inputs() {
    const auto dir = scratch_dir();
    return {.stereo = generated(dir / "in_stereo.ac3", "sine 1 192"),
            .dual_mono = generated(dir / "in_dual_mono.ac3", "sine 1 192 1000 50 1+1"),
            .silent = generated(dir / "in_silent.ac3", "silence 1 192"),
            .wide = generated(dir / "in_71.ec3", "eac3-sine 1 384 1000 50 71")};
}

struct Expectation {
    std::string args;
    int exit_code;
    std::string message;
};

void check_rows(std::initializer_list<Expectation> rows, const fs::path& log) {
    for (const auto& row : rows) {
        CAPTURE(row.args);
        const auto rc = run_cli(row.args, log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == row.exit_code);
        CHECK(text.find(row.message) != std::string::npos);
    }
}

// An output path whose parent directory does not exist.
fs::path nowhere(std::string_view name) { return scratch_dir() / "no_such_directory" / name; }

}  // namespace

TEST_CASE("mkv, mp4 and ts refuse an unreadable input, a non-stream one and an unwritable output",
          "[cli][containers]") {
    const auto in = inputs();
    const auto dir = scratch_dir();
    const auto log = dir / "wrap_refused.log";
    const auto missing = dir / "wrap_missing.ac3";
    fs::remove(missing);
    // A stream of zero bytes after a sync word: it frames as nothing.
    const auto junk = dir / "wrap_junk.ac3";
    {
        std::ofstream out{junk, std::ios::binary};
        out << std::string("\x0B\x77", 2) << std::string(3000, '\0');
    }
    for (const std::string_view command : {"mkv", "mp4", "ts"}) {
        CAPTURE(command);
        const std::string cmd{command};
        const auto out_path = dir / ("wrap_refused." + cmd);
        const auto blocked = nowhere("wrapped." + cmd);
        check_rows({{cmd + " " + quoted(missing) + " " + quoted(out_path), 2,
                     "error: cannot read " + missing.string()},
                    {cmd + " " + quoted(junk) + " " + quoted(out_path), 2,
                     "error: lost sync: expected 0x0B77"},
                    {cmd + " " + quoted(in.stereo) + " " + quoted(blocked), 3,
                     "error: cannot write " + blocked.string()}},
                   log);
        CHECK_FALSE(fs::exists(out_path));
    }
}

TEST_CASE("the container wrappers count a dependent substream into each access unit",
          "[cli][containers]") {
    const auto in = inputs();
    const auto dir = scratch_dir();
    const auto log = dir / "wrap_71.log";
    // 7.1 is a 5.1 independent substream plus a dependent carrying the rest
    // (§E3.8), so each access unit is two syncframes and eight channels.
    struct Case {
        std::string_view command;
        std::string_view output;
    };
    for (const auto& c : {Case{"mkv", "wrap_71.mkv"}, Case{"mp4", "wrap_71.mp4"},
                          Case{"ts", "wrap_71.ts"}, Case{"fmp4", "wrap_71_fmp4"}}) {
        CAPTURE(c.command);
        const auto out_path = dir / c.output;
        REQUIRE(run_cli(std::string{c.command} + " " + quoted(in.wide) + " " + quoted(out_path),
                        log) == 0);
        const auto text = read_log(log);
        INFO(text);
        CHECK(text.find("wrote 32 E-AC-3 access units (2 substreams, 8 channels") !=
              std::string::npos);
    }
}

TEST_CASE("ts refuses an unknown broadcast profile and a service association the stream "
          "contradicts",
          "[cli][containers][ts]") {
    const auto in = inputs();
    const auto dir = scratch_dir();
    const auto log = dir / "ts_refused.log";
    const auto out_path = dir / "ts_refused.ts";
    const auto vi = dir / "ts_vi.ac3";
    // bsmod 2 (visually impaired) makes this an associated service.
    REQUIRE(run_cli("metadata " + quoted(in.stereo) + " " + quoted(vi) + " bsmod=vi", log) == 0);
    const std::string head = "ts " + quoted(in.stereo) + " " + quoted(out_path);
    check_rows({{head + " isdb", 1, "error: unknown TS profile 'isdb' (expected dvb or atsc)"},
                {head + " dvb asvc=1", 1,
                 "error: asvc= given but this stream's bsmod (main audio service: complete main "
                 "(CM)) is a main service - did you mean mainid=?"},
                {"ts " + quoted(vi) + " " + quoted(out_path) + " dvb mainid=1", 1,
                 "error: mainid= given but this stream's bsmod"}},
               log);
}

TEST_CASE("AC-4 input is refused where its signalling does not exist and when it stops parsing",
          "[cli][containers][ac4]") {
    const auto dir = scratch_dir();
    const auto log = dir / "ac4_refused.log";
    const fs::path ac4{std::string{AC3FORGE_EXTERNAL_BASELINE_DIR} + "/ac4-stereo-64/dee.ac4"};
    REQUIRE(fs::exists(ac4));
    // The first 3000 bytes: whole TOCs up to a frame cut part-way through.
    const auto cut = dir / "ac4_cut.ac4";
    {
        const auto bytes = read_bytes(ac4);
        std::ofstream out{cut, std::ios::binary};
        out.write(reinterpret_cast<const char*>(bytes.data()), 3000);
    }
    const auto blocked = nowhere("ac4.ts");
    check_rows({{"ts " + quoted(ac4) + " " + quoted(dir / "ac4.ts") + " atsc", 1,
                 "error: AC-4 has no ATSC MPEG-2 TS signalling (A/342-2 is ATSC 3.0's ROUTE/MMT) "
                 "- use the dvb profile"},
                {"ts " + quoted(ac4) + " " + quoted(blocked), 3,
                 "error: cannot write " + blocked.string()},
                {"mp4 " + quoted(ac4) + " " + quoted(nowhere("ac4.mp4")), 3,
                 "error: cannot write " + nowhere("ac4.mp4").string()},
                {"mp4 " + quoted(cut) + " " + quoted(dir / "ac4_cut.mp4"), 2,
                 "error: AC-4 stream stops parsing at byte 2779: truncated"}},
               log);
}

TEST_CASE("fmp4 refuses an unreadable input, a non-stream one and a directory it cannot create",
          "[cli][containers][fmp4]") {
    const auto in = inputs();
    const auto dir = scratch_dir();
    const auto log = dir / "fmp4_refused.log";
    const auto missing = dir / "fmp4_missing.ac3";
    fs::remove(missing);
    const auto text_file = dir / "fmp4_not_a_stream.txt";
    std::ofstream{text_file} << "not an elementary stream\n";
    // A regular file where a parent directory should be: creating the output
    // directory fails the same way for every user, root included.
    const auto blocker = dir / "fmp4_blocker";
    std::ofstream{blocker} << "x";
    check_rows({{"fmp4 " + quoted(missing) + " " + quoted(dir / "fmp4_a"), 2,
                 "error: cannot open " + missing.string()},
                {"fmp4 " + quoted(text_file) + " " + quoted(dir / "fmp4_b"), 2,
                 "error: lost sync: expected 0x0B77"},
                {"fmp4 " + quoted(in.stereo) + " " + quoted(blocker / "out"), 3,
                 "error: cannot create directory " + (blocker / "out").string()}},
               log);
}

TEST_CASE("fmp4 fallback-51 on a stream with no object layer says it has nothing to strip",
          "[cli][containers][fmp4]") {
    const auto in = inputs();
    const auto dir = scratch_dir();
    const auto log = dir / "fmp4_fallback.log";
    const auto out_dir = dir / "fmp4_fallback";
    REQUIRE(run_cli("fmp4 " + quoted(in.stereo) + " " + quoted(out_dir) + " 48 fallback-51", log) ==
            0);
    const auto text = read_log(log);
    INFO(text);
    CHECK(text.find("note: fallback-51 ignored - " + in.stereo.string() +
                    " carries no object layer to strip") != std::string::npos);
    CHECK(fs::exists(out_dir / "init.mp4"));
    CHECK_FALSE(fs::exists(out_dir / "bed51"));
}

namespace {

// `command` (with `extra` after its two paths) against a missing input, a
// text file and an output directory that does not exist.
void check_io_refusals(std::string_view command, std::string_view extra) {
    const auto in = inputs();
    const auto dir = scratch_dir();
    const std::string cmd{command};
    const auto log = dir / ("io_refused_" + cmd + ".log");
    const auto missing = dir / "tools_missing.ac3";
    fs::remove(missing);
    const auto text_file = dir / "tools_not_a_stream.txt";
    std::ofstream{text_file} << "not an elementary stream\n";
    const auto out_path = dir / ("io_refused_" + cmd + ".ac3");
    const auto blocked = nowhere("tools.ac3");
    const std::string tail = extra.empty() ? "" : " " + std::string{extra};
    // exit_codes.hpp's classes: 2 for the input's fault, 3 for the output's.
    check_rows({{cmd + " " + quoted(missing) + " " + quoted(out_path) + tail, 2,
                 "error: cannot read " + missing.string()},
                {cmd + " " + quoted(text_file) + " " + quoted(out_path) + tail, 2,
                 "error: " + text_file.string() + ": lost sync: expected 0x0B77"},
                {cmd + " " + quoted(in.stereo) + " " + quoted(blocked) + tail, 3,
                 "error: cannot open " + blocked.string() + " for writing"}},
               log);
    CHECK_FALSE(fs::exists(out_path));
}

}  // namespace

TEST_CASE("transcode and metadata refuse an unreadable input, a non-stream one and an "
          "unwritable output",
          "[cli][stream-tools]") {
    check_io_refusals("transcode", "");
    check_io_refusals("metadata", "dialnorm=5");
}

TEST_CASE("normalize and cut refuse an unreadable input, a non-stream one and an unwritable "
          "output",
          "[cli][stream-tools]") {
    check_io_refusals("normalize", "");
    check_io_refusals("cut", "0 0.5");
}

TEST_CASE("cat refuses an unreadable input, a non-stream one and an unwritable output",
          "[cli][stream-tools][cat]") {
    const auto in = inputs();
    const auto dir = scratch_dir();
    const auto log = dir / "cat_io_refused.log";
    const auto missing = dir / "cat_missing.ac3";
    fs::remove(missing);
    const auto text_file = dir / "cat_not_a_stream.txt";
    std::ofstream{text_file} << "not an elementary stream\n";
    const auto out_path = dir / "cat_io_refused.ac3";
    const auto blocked = nowhere("cat.ac3");
    // Each input is read after the output opens, so a refusal part-way
    // through has to take the partial output back out again.
    check_rows({{"cat " + quoted(out_path) + " " + quoted(in.stereo) + " " + quoted(missing), 2,
                 "error: cannot read " + missing.string()},
                {"cat " + quoted(out_path) + " " + quoted(in.stereo) + " " + quoted(text_file), 2,
                 "error: " + text_file.string() + ": lost sync: expected 0x0B77"},
                {"cat " + quoted(blocked) + " " + quoted(in.stereo) + " " + quoted(in.stereo), 3,
                 "error: cannot open " + blocked.string() + " for writing"}},
               log);
    CHECK_FALSE(fs::exists(out_path));
}

TEST_CASE("transcode refuses what it cannot measure, name or code", "[cli][stream-tools]") {
    const auto in = inputs();
    const auto dir = scratch_dir();
    const auto log = dir / "transcode_refused.log";
    const auto out_ac3 = dir / "transcode_refused.ac3";
    const auto out_ec3 = dir / "transcode_refused.ec3";
    check_rows(
        {{"transcode " + quoted(in.silent) + " " + quoted(out_ec3) + " dialnorm=auto", 5,
          "error: no audio above the -70 LKFS absolute gate; pass dialnorm=<1..31> explicitly"},
         {"transcode " + quoted(in.stereo) + " " + quoted(out_ec3) + " dialnorm2=auto", 1,
          "error: " + in.stereo.string() +
              " is not 1+1, so there is no Ch2 to measure for dialnorm2=auto"},
         {"transcode " + quoted(in.stereo) + " " + quoted(out_ec3) + " 192 bogus", 1,
          "error: unknown layout 'bogus' (mono | stereo | 1+1 | 51 | 71 | 512 | 514 | 714)"},
         {"transcode " + quoted(in.stereo) + " " + quoted(out_ac3) + " 191", 1,
          "error: AC-3 takes only the 19 nominal rates of Table 5.18"},
         {"transcode " + quoted(in.stereo) + " " + quoted(dir / "transcode.mp3"), 1,
          "error: cannot tell which codec to write from '"}},
        log);
}

TEST_CASE("transcode keeps a 1+1 source as two programmes and folds 7.1 for AC-3",
          "[cli][stream-tools]") {
    const auto in = inputs();
    const auto dir = scratch_dir();
    const auto log = dir / "transcode_shapes.log";

    const auto dual_out = dir / "transcode_dual.ec3";
    REQUIRE(run_cli("transcode " + quoted(in.dual_mono) + " " + quoted(dual_out), log) == 0);
    auto text = read_log(log);
    INFO(text);
    CHECK(text.find("  layout 1+1 dual mono <- 2 source channels") != std::string::npos);

    const auto folded = dir / "transcode_folded.ac3";
    REQUIRE(run_cli("transcode " + quoted(in.wide) + " " + quoted(folded), log) == 0);
    text = read_log(log);
    CHECK(text.find("note: 8 channels have no AC-3 coding mode; folding down to 5.1") !=
          std::string::npos);
    CHECK(text.find("  layout 5.1 <- 8 source channels") != std::string::npos);
    const auto bytes = read_bytes(folded);
    const auto frames = ac3::split_frames(bytes);
    REQUIRE(frames.has_value());
    ac3::FrameDecoder decoder;
    const auto first = decoder.decode_frame(frames->front());
    REQUIRE(first.has_value());
    CHECK(first->acmod == ac3::Acmod::k3_2);
    CHECK(first->lfe);
}

TEST_CASE("metadata refuses an empty edit and dialnorm2=auto, and rewrites 1+1's Ch2 in place",
          "[cli][stream-tools][metadata]") {
    const auto in = inputs();
    const auto dir = scratch_dir();
    const auto log = dir / "metadata_edges.log";
    const auto out_path = dir / "metadata_edges.ac3";
    check_rows({{"metadata " + quoted(in.stereo) + " " + quoted(out_path), 1,
                 "error: nothing to change - give at least one of dialnorm=, dialnorm2=, compr=, "
                 "compr2=, bsmod=, dsurmod="},
                {"metadata " + quoted(in.stereo) + " " + quoted(out_path) + " dialnorm2=auto", 1,
                 "error: dialnorm2=auto needs a measurement - use 'ac3cli normalize'"},
                // compr2 has no bits to rewrite in a stream that never sent it.
                {"metadata " + quoted(in.dual_mono) + " " + quoted(out_path) + " compr2=-2", 1,
                 "error: " + in.dual_mono.string() +
                     ": this stream does not transmit that field, so there are no bits to "
                     "rewrite"}},
               log);

    REQUIRE(run_cli("metadata " + quoted(in.dual_mono) + " " + quoted(out_path) + " dialnorm2=7",
                    log) == 0);
    const auto bytes = read_bytes(out_path);
    CHECK(bytes.size() == read_bytes(in.dual_mono).size());
    const auto frames = ac3::split_frames(bytes);
    REQUIRE(frames.has_value());
    ac3::FrameDecoder decoder;
    const auto first = decoder.decode_frame(frames->front());
    REQUIRE(first.has_value());
    REQUIRE(first->dialnorm2.has_value());
    CHECK(*first->dialnorm2 == 7);
    CHECK(first->dialnorm == 31);

    // dsurmod only exists in a 2/0 bsi (§5.4.2.6), so it is asked of stereo.
    REQUIRE(run_cli("metadata " + quoted(in.stereo) + " " + quoted(out_path) + " dsurmod=on", log) ==
            0);
    const auto stereo_bytes = read_bytes(out_path);
    const auto stereo_frames = ac3::split_frames(stereo_bytes);
    REQUIRE(stereo_frames.has_value());
    ac3::FrameDecoder stereo_decoder;
    const auto stereo_first = stereo_decoder.decode_frame(stereo_frames->front());
    REQUIRE(stereo_first.has_value());
    CHECK(stereo_first->info.dsurmod == ac3::meta::SurroundMode::kDolbySurround);
}

TEST_CASE("normalize measures each 1+1 programme and refuses a silent stream",
          "[cli][stream-tools][normalize]") {
    const auto in = inputs();
    const auto dir = scratch_dir();
    const auto log = dir / "normalize_edges.log";
    const auto out_path = dir / "normalize_edges.ac3";
    check_rows({{"normalize " + quoted(in.silent) + " " + quoted(out_path), 5,
                 "error: no audio above the -70 LKFS absolute gate; nothing to normalise "
                 "against"}},
               log);

    REQUIRE(run_cli("normalize " + quoted(in.dual_mono) + " " + quoted(out_path), log) == 0);
    const auto text = read_log(log);
    INFO(text);
    // A 0.5-amplitude sine on each channel: -9.03 LKFS per mono programme.
    CHECK(text.find("  measured   -9.03 LKFS (BS.1770-4 gated)") != std::string::npos);
    CHECK(text.find("  dialnorm   31 -> 9 (ATSC A/85 \xC2\xA7" "8)") != std::string::npos);
    CHECK(text.find("  dialnorm2  31 -> 9 (Ch2 measured -9.03 LKFS)") != std::string::npos);
    const auto bytes = read_bytes(out_path);
    const auto frames = ac3::split_frames(bytes);
    REQUIRE(frames.has_value());
    ac3::FrameDecoder decoder;
    const auto first = decoder.decode_frame(frames->front());
    REQUIRE(first.has_value());
    CHECK(first->dialnorm == 9);
    REQUIRE(first->dialnorm2.has_value());
    CHECK(*first->dialnorm2 == 9);
}

TEST_CASE("cut refuses a negative start, a non-positive duration and a start past the end",
          "[cli][stream-tools][cut]") {
    const auto in = inputs();
    const auto dir = scratch_dir();
    const auto log = dir / "cut_refused.log";
    const auto out_path = dir / "cut_refused.ac3";
    const std::string head = "cut " + quoted(in.stereo) + " " + quoted(out_path);
    check_rows({{head + " -1", 1, "error: start must not be negative"},
                {head + " 0 0", 1, "error: duration must be positive"},
                {head + " 5", 1,
                 "error: start 5.000 s is past the end of " + in.stereo.string() + " (1.024 s)"}},
               log);
}

TEST_CASE("cat refuses joining a file into itself and streams whose shape differs",
          "[cli][stream-tools][cat]") {
    const auto in = inputs();
    const auto dir = scratch_dir();
    const auto log = dir / "cat_refused.log";
    const auto out_path = dir / "cat_refused.ac3";
    const auto six = generated(dir / "cat_51.ac3", "sine 1 384 1000 50 51");
    const auto ec3_stereo = generated(dir / "cat_stereo.ec3", "eac3-silence 1 192 stereo");

    // An existing output named again as an input would be truncated by the
    // sink before it was read, so it is refused - and left as it was.
    fs::copy_file(in.stereo, out_path, fs::copy_options::overwrite_existing);
    const auto before = read_bytes(out_path);
    check_rows({{"cat " + quoted(out_path) + " " + quoted(in.stereo) + " " + quoted(out_path), 1,
                 "error: " + out_path.string() + " is both an input and the output"}},
               log);
    CHECK(read_bytes(out_path) == before);

    // ...and so is an output that does not exist YET: the sink would create
    // it before the loop reached the input of the same name, which would
    // then read back cat's own half-written output. fs::equivalent cannot
    // compare a path that does not exist, so this used to be joined, exit 0.
    // Spelled differently on each side, so the check is not a string match.
    const auto fresh = dir / "cat_fresh.ac3";
    fs::remove(fresh);
    check_rows({{"cat " + quoted(fresh) + " " + quoted(in.stereo) + " " +
                     quoted(dir / "." / "cat_fresh.ac3"),
                 1, "is both an input and the output"}},
               log);
    CHECK_FALSE(fs::exists(fresh));

    check_rows(
        {{"cat " + quoted(out_path) + " " + quoted(in.stereo) + " " + quoted(in.wide), 1,
          "error: " + in.wide.string() + " differs from " + in.stereo.string() +
              " in codec - a decoder cannot follow that across a join"},
         {"cat " + quoted(out_path) + " " + quoted(in.stereo) + " " + quoted(six), 1,
          "error: " + six.string() + " differs from " + in.stereo.string() +
              " in coding mode (acmod)"},
         {"cat " + quoted(dir / "cat_refused.ec3") + " " + quoted(ec3_stereo) + " " +
              quoted(in.wide),
          1, "error: " + in.wide.string() + " differs from " + ec3_stereo.string()}},
        log);
}
