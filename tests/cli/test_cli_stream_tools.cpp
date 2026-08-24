#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <string>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/io/metadata_edit.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/meta/drc.hpp"

// The DC9 stream tools, driven the same way tests/cli/test_cli.cpp drives
// every other command: the real built ac3cli.exe as a subprocess, inspecting
// what it actually wrote.
//
// What each of these is really checking is a claim the CLI makes about NOT
// changing something:
//
//   metadata/normalize  the audio comes back bit-identical, only bsi moved
//   cut + cat           the two together reproduce their input byte for byte
//   transcode           the source's dialnorm and compr survive the re-encode
//
// So most assertions here compare a file against another file rather than
// against a number - a claim of "unchanged" is only worth as much as the
// comparison behind it.

namespace fs = std::filesystem;

namespace {

fs::path scratch_dir() {
    auto dir = fs::temp_directory_path() / "ac3forge_cli_stream_tool_tests";
    fs::create_directories(dir);
    return dir;
}

// Same subprocess plumbing (and the same Windows quoting workaround) as
// tests/cli/test_cli.cpp's run_cli - see its comment for why the extra outer
// quote pair is needed there and must not appear on POSIX.
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

std::vector<std::byte> read_bytes(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    const std::string raw{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
    }
    return bytes;
}

std::string quoted(const fs::path& path) { return "\"" + path.string() + "\""; }

// Real programme material, never silence and never one frame: this project's
// own testing convention (see CONTRIBUTING.md's validation discipline) is
// that a silent or single-frame fixture passes even when the thing under test
// is badly broken.
std::vector<std::vector<float>> tone_channels(std::size_t channels, std::size_t frames,
                                              std::uint32_t sample_rate) {
    std::vector<std::vector<float>> out(channels, std::vector<float>(frames));
    for (std::size_t c = 0; c < channels; ++c) {
        const double hz = 200.0 * std::pow(2.0, static_cast<double>(c) * 0.4);
        for (std::size_t n = 0; n < frames; ++n) {
            out[c][n] = static_cast<float>(
                0.4 * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) /
                               static_cast<double>(sample_rate)));
        }
    }
    return out;
}

// A 5.1 AC-3 or E-AC-3 stream of ~1.4 s (44 access units) built from a real
// WAV, with whatever extra options the caller wants on the encode.
fs::path make_stream(const std::string& name, const std::string& command,
                     const std::string& extra = {}) {
    const auto dir = scratch_dir();
    const auto wav = dir / "stream_tools_source.wav";
    if (!fs::exists(wav)) {
        const auto channels = tone_channels(6, 68000, 48000);
        REQUIRE(ac3::io::write_wav_f32(wav.string(), channels, 48000).has_value());
    }
    const auto out = dir / name;
    const auto log = dir / (name + ".log");
    REQUIRE(run_cli(command + " " + quoted(wav) + " " + quoted(out) + " 448 " + extra, log) == 0);
    REQUIRE(fs::exists(out));
    return out;
}

// Decoding both streams and comparing the WAVs is the only way to say "the
// audio did not change" about a bitstream whose bsi bytes deliberately did.
void require_same_audio(const fs::path& a, const fs::path& b, const std::string& tag) {
    const auto dir = scratch_dir();
    const auto wav_a = dir / (tag + "_a.wav");
    const auto wav_b = dir / (tag + "_b.wav");
    const auto log = dir / (tag + ".log");
    REQUIRE(run_cli("decode " + quoted(a) + " " + quoted(wav_a), log) == 0);
    REQUIRE(run_cli("decode " + quoted(b) + " " + quoted(wav_b), log) == 0);
    const auto bytes_a = read_bytes(wav_a);
    const auto bytes_b = read_bytes(wav_b);
    REQUIRE(bytes_a.size() == bytes_b.size());
    CHECK(bytes_a == bytes_b);
}

}  // namespace

TEST_CASE("metadata rewrites bsi and leaves the audio bit-identical", "[cli][metadata]") {
    const auto dir = scratch_dir();
    const auto source = make_stream("meta_source.ac3", "encode", "51");
    const auto out = dir / "meta_out.ac3";
    const auto log = dir / "meta.log";

    REQUIRE(run_cli("metadata " + quoted(source) + " " + quoted(out) + " dialnorm=20 bsmod=2",
                    log) == 0);
    const auto text = read_log(log);
    INFO(text);
    CHECK(text.find("audio untouched") != std::string::npos);

    // Same length: an in-place rewrite cannot add or remove a byte.
    CHECK(fs::file_size(out) == fs::file_size(source));

    const auto edited = ac3::io::read_frame_metadata(read_bytes(out));
    REQUIRE(edited.has_value());
    CHECK(edited->dialnorm == 20);
    REQUIRE(edited->bsmod.has_value());
    CHECK(*edited->bsmod == 2);

    require_same_audio(source, out, "meta_audio");
}

TEST_CASE("metadata refuses a field the stream does not carry", "[cli][metadata]") {
    const auto dir = scratch_dir();
    // No `heavy` on the encode, so compre is clear and there are no compr
    // bits to overwrite - the documented limit of an in-place rewrite.
    const auto source = make_stream("meta_nocompr.ac3", "encode", "51");
    const auto out = dir / "meta_nocompr_out.ac3";
    const auto log = dir / "meta_nocompr.log";

    // Removed first: the scratch directory survives between runs, so an
    // "it wrote nothing" check is only meaningful against a clean slate.
    fs::remove(out);
    REQUIRE(run_cli("metadata " + quoted(source) + " " + quoted(out) + " compr=-6", log) != 0);
    const auto text = read_log(log);
    INFO(text);
    CHECK(text.find("does not transmit that field") != std::string::npos);
    // Refused before writing: no half-rewritten output left behind.
    CHECK_FALSE(fs::exists(out));
}

TEST_CASE("metadata stamps compr onto a stream that carries one", "[cli][metadata]") {
    const auto dir = scratch_dir();
    const auto source = make_stream("meta_compr.ac3", "encode", "51 heavy");
    const auto out = dir / "meta_compr_out.ac3";
    const auto log = dir / "meta_compr.log";

    REQUIRE(run_cli("metadata " + quoted(source) + " " + quoted(out) + " compr=-6", log) == 0);
    const auto before = ac3::io::read_frame_metadata(read_bytes(source));
    const auto after = ac3::io::read_frame_metadata(read_bytes(out));
    REQUIRE(before.has_value());
    REQUIRE(after.has_value());
    REQUIRE(before->compr.has_value());
    REQUIRE(after->compr.has_value());
    CHECK(*after->compr != *before->compr);
    // §7.7.2's own rounding rule: the stamped word is the largest gain that
    // does NOT exceed the requested one, so the ceiling stays a ceiling.
    // A small tolerance for the dB conversion itself, not for the rounding
    // rule: the stamped word must not represent a gain ABOVE the request.
    CHECK(ac3::meta::to_db(ac3::meta::compr_gain(*after->compr)) <= -6.0 + 1e-9);
    require_same_audio(source, out, "meta_compr_audio");
}

TEST_CASE("normalize writes the dialnorm the measurement implies", "[cli][normalize]") {
    const auto dir = scratch_dir();
    const auto source = make_stream("norm_source.ac3", "encode", "51");
    const auto out = dir / "norm_out.ac3";
    const auto log = dir / "norm.log";

    REQUIRE(run_cli("normalize " + quoted(source) + " " + quoted(out), log) == 0);
    const auto text = read_log(log);
    INFO(text);
    CHECK(text.find("BS.1770-4 gated") != std::string::npos);
    CHECK(fs::file_size(out) == fs::file_size(source));

    // Whatever it measured, the written dialnorm must be a legal §5.4.2.8
    // value and must agree with what `qc` independently derives from the same
    // stream - the two must not be able to disagree.
    const auto written = ac3::io::read_frame_metadata(read_bytes(out));
    REQUIRE(written.has_value());
    CHECK(written->dialnorm >= 1);
    CHECK(written->dialnorm <= 31);

    const auto qc_log = dir / "norm_qc.log";
    REQUIRE(run_cli("qc " + quoted(out), qc_log) == 0);
    const auto qc_text = read_log(qc_log);
    INFO(qc_text);
    CHECK(qc_text.find("(matches)") != std::string::npos);

    require_same_audio(source, out, "norm_audio");
}

TEST_CASE("cut then cat reproduces the source byte for byte", "[cli][cut][cat]") {
    const auto dir = scratch_dir();
    const auto source = make_stream("cut_source.ac3", "encode", "51");
    const auto head = dir / "cut_head.ac3";
    const auto tail = dir / "cut_tail.ac3";
    const auto rejoined = dir / "cut_rejoined.ac3";
    const auto log = dir / "cut.log";

    // 0.512 s is exactly 16 access units at 48 kHz (1536 samples each), so
    // the split lands on a boundary without any snapping - and the two halves
    // together are the whole thing. This is the strongest statement available
    // about a frame-aligned cut: not "it sounds continuous", but "the bytes
    // are the bytes".
    REQUIRE(run_cli("cut " + quoted(source) + " " + quoted(head) + " 0 0.512", log) == 0);
    REQUIRE(run_cli("cut " + quoted(source) + " " + quoted(tail) + " 0.512", log) == 0);
    REQUIRE(run_cli("cat " + quoted(rejoined) + " " + quoted(head) + " " + quoted(tail), log) ==
            0);

    CHECK(fs::file_size(head) + fs::file_size(tail) == fs::file_size(source));
    CHECK(read_bytes(rejoined) == read_bytes(source));
    require_same_audio(source, rejoined, "cut_audio");
}

TEST_CASE("cut snaps to access-unit boundaries and refuses a start past the end",
          "[cli][cut]") {
    const auto dir = scratch_dir();
    const auto source = make_stream("cut_snap.ac3", "encode", "51");
    const auto out = dir / "cut_snap_out.ac3";
    const auto log = dir / "cut_snap.log";

    // A start inside an access unit names that whole unit - a cut is never a
    // split. 0.040 s falls inside unit 1 (units are 0.032 s each).
    REQUIRE(run_cli("cut " + quoted(source) + " " + quoted(out) + " 0.040 0.064", log) == 0);
    const auto scanned = ac3::io::scan(read_bytes(out));
    REQUIRE(scanned.has_value());
    CHECK(scanned->access_units.size() == 2);
    const auto text = read_log(log);
    INFO(text);
    CHECK(text.find("access-unit aligned") != std::string::npos);

    SECTION("a start past the end is refused, not silently empty") {
        const auto empty = dir / "cut_past_end.ac3";
        fs::remove(empty);
        REQUIRE(run_cli("cut " + quoted(source) + " " + quoted(empty) + " 60", log) != 0);
        CHECK(text.find("access-unit aligned") != std::string::npos);
        CHECK_FALSE(fs::exists(empty));
    }

    SECTION("a duration shorter than one access unit still writes one") {
        const auto tiny = dir / "cut_tiny.ac3";
        REQUIRE(run_cli("cut " + quoted(source) + " " + quoted(tiny) + " 0 0.001", log) == 0);
        const auto tiny_scan = ac3::io::scan(read_bytes(tiny));
        REQUIRE(tiny_scan.has_value());
        CHECK(tiny_scan->access_units.size() == 1);
        CHECK(read_log(log).find("shorter than one access unit") != std::string::npos);
    }
}

TEST_CASE("cat refuses inputs a decoder could not follow across the join", "[cli][cat]") {
    const auto dir = scratch_dir();
    const auto surround = make_stream("cat_51.ac3", "encode", "51");
    const auto stereo = make_stream("cat_stereo.ac3", "encode", "stereo");
    const auto out = dir / "cat_mismatch.ac3";
    const auto log = dir / "cat_mismatch.log";

    fs::remove(out);
    REQUIRE(run_cli("cat " + quoted(out) + " " + quoted(surround) + " " + quoted(stereo), log) !=
            0);
    const auto text = read_log(log);
    INFO(text);
    CHECK(text.find("coding mode") != std::string::npos);
    CHECK_FALSE(fs::exists(out));
}

TEST_CASE("cat joins an E-AC-3 stream by whole access units", "[cli][cat]") {
    const auto dir = scratch_dir();
    const auto source = make_stream("cat_eac3.ec3", "eac3-encode", "none 51");
    const auto out = dir / "cat_eac3_out.ec3";
    const auto log = dir / "cat_eac3.log";

    REQUIRE(run_cli("cat " + quoted(out) + " " + quoted(source) + " " + quoted(source), log) == 0);
    const auto one = ac3::io::scan(read_bytes(source));
    const auto two = ac3::io::scan(read_bytes(out));
    REQUIRE(one.has_value());
    REQUIRE(two.has_value());
    CHECK(two->access_units.size() == one->access_units.size() * 2);
    CHECK(ac3::io::stream_duration_samples(*two) ==
          ac3::io::stream_duration_samples(*one) * 2);
}

TEST_CASE("transcode carries dialnorm and compr from DD+ into DD", "[cli][transcode]") {
    const auto dir = scratch_dir();
    const auto source = make_stream("tx_source.ec3", "eac3-encode", "none 51 off dialnorm=23 heavy");
    const auto out = dir / "tx_out.ac3";
    const auto log = dir / "tx.log";

    REQUIRE(run_cli("transcode " + quoted(source) + " " + quoted(out) + " 448", log) == 0);
    const auto text = read_log(log);
    INFO(text);
    CHECK(text.find("carried from the source") != std::string::npos);
    CHECK(text.find("carried across verbatim") != std::string::npos);

    const auto before = ac3::io::read_frame_metadata(read_bytes(source));
    const auto after = ac3::io::read_frame_metadata(read_bytes(out));
    REQUIRE(before.has_value());
    REQUIRE(after.has_value());
    CHECK(after->kind == ac3::io::StreamKind::kAc3);
    CHECK(after->dialnorm == 23);
    CHECK(after->dialnorm == before->dialnorm);
    REQUIRE(before->compr.has_value());
    REQUIRE(after->compr.has_value());
    // Verbatim, not re-derived: the ceiling §7.7.2 promises describes the
    // programme, not this generation's coding.
    CHECK(*after->compr == *before->compr);

    // Same programme length either way - one AC-3 frame out per E-AC-3
    // access unit in, since both code 1536 samples here.
    const auto in_scan = ac3::io::scan(read_bytes(source));
    const auto out_scan = ac3::io::scan(read_bytes(out));
    REQUIRE(in_scan.has_value());
    REQUIRE(out_scan.has_value());
    CHECK(ac3::io::stream_duration_samples(*out_scan) ==
          ac3::io::stream_duration_samples(*in_scan));
}

TEST_CASE("transcode overrides the carried dialnorm when told to", "[cli][transcode]") {
    const auto dir = scratch_dir();
    const auto source = make_stream("tx_override.ec3", "eac3-encode", "none 51 off dialnorm=23");
    const auto out = dir / "tx_override.ac3";
    const auto log = dir / "tx_override.log";

    REQUIRE(run_cli("transcode " + quoted(source) + " " + quoted(out) + " 448 51 dialnorm=12",
                    log) == 0);
    const auto after = ac3::io::read_frame_metadata(read_bytes(out));
    REQUIRE(after.has_value());
    CHECK(after->dialnorm == 12);
    CHECK(read_log(log).find("(from dialnorm=)") != std::string::npos);
}

TEST_CASE("transcode folds a layout AC-3 cannot code down to 5.1, and says so",
          "[cli][transcode]") {
    const auto dir = scratch_dir();
    const auto source = dir / "tx_714.ec3";
    const auto out = dir / "tx_714_out.ac3";
    const auto log = dir / "tx_714.log";
    REQUIRE(run_cli("eac3-sine " + quoted(source) + " 1 768 500 60 714", log) == 0);

    REQUIRE(run_cli("transcode " + quoted(source) + " " + quoted(out) + " 448", log) == 0);
    const auto text = read_log(log);
    INFO(text);
    CHECK(text.find("no AC-3 coding mode") != std::string::npos);

    const auto scanned = ac3::io::scan(read_bytes(out));
    REQUIRE(scanned.has_value());
    CHECK(scanned->kind == ac3::io::StreamKind::kAc3);
    CHECK(scanned->channels == 6);
    CHECK(scanned->acmod == ac3::Acmod::k3_2);
    CHECK(scanned->lfe);
}

TEST_CASE("transcode needs to be told the codec when the name cannot say it",
          "[cli][transcode]") {
    const auto dir = scratch_dir();
    const auto source = make_stream("tx_suffix.ec3", "eac3-encode", "none 51");
    const auto out = dir / "tx_suffix_out.bin";
    const auto log = dir / "tx_suffix.log";

    // This one writes `out` for real further down, so the previous run's
    // copy is still there unless it goes first.
    fs::remove(out);
    REQUIRE(run_cli("transcode " + quoted(source) + " " + quoted(out), log) != 0);
    CHECK(read_log(log).find("codec=ac3|eac3") != std::string::npos);
    CHECK_FALSE(fs::exists(out));

    REQUIRE(run_cli("transcode " + quoted(source) + " " + quoted(out) + " 448 \"\" codec=ac3",
                    log) == 0);
    const auto scanned = ac3::io::scan(read_bytes(out));
    REQUIRE(scanned.has_value());
    CHECK(scanned->kind == ac3::io::StreamKind::kAc3);
}

TEST_CASE("transcode also goes the other way, DD into DD+", "[cli][transcode]") {
    const auto dir = scratch_dir();
    const auto source = make_stream("tx_up_source.ac3", "encode", "51 dialnorm=27");
    const auto out = dir / "tx_up.ec3";
    const auto log = dir / "tx_up.log";

    REQUIRE(run_cli("transcode " + quoted(source) + " " + quoted(out) + " 448", log) == 0);
    const auto after = ac3::io::read_frame_metadata(read_bytes(out));
    REQUIRE(after.has_value());
    CHECK(after->kind == ac3::io::StreamKind::kEac3);
    CHECK(after->dialnorm == 27);
}
