#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/encoder/plan.hpp"
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

// Rooted at AC3FORGE_TEST_SCRATCH_DIR rather than fs::temp_directory_path() -
// see tests/CMakeLists.txt's comment on that define for why.
fs::path scratch_dir() {
    auto dir = fs::path{AC3FORGE_TEST_SCRATCH_DIR} / "cli_stream_tools";
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

// A/52 §E2.3.1.2's legacy-core delivery: an AC-3 syncframe carrying a 5.1
// bed, immediately followed by the Annex E dependent that extends it to 7.1
// rear (k71Rear replaces the bed's own Ls/Rs and adds Lrs/Rrs - see
// eac3_tables.hpp's own comment on the constant). Built the raw
// FrameEncoder/eac3::FrameEncoder way tests/decoder/test_stream_playback.cpp's
// legacy_core_streams() and tests/cli/test_cli_containers.cpp's
// legacy_core_stream() both are, not AccessUnitEncoder, which always writes
// Annex E syntax for the independent substream too and so cannot produce a
// genuine AC-3-syntax core.
//
// Each channel gets its own stationary tone, phase-continuous across access
// units (n is the absolute sample index). The bed and the dependent draw from
// disjoint frequency ranges so a decoded channel can be told which substream
// it came from without knowing the WAV's own channel order.
constexpr std::array<double, 6> kLegacyCoreBedHz{300.0, 500.0, 700.0, 900.0, 1100.0, 50.0};
constexpr std::array<double, 4> kLegacyCoreRearHz{2000.0, 2200.0, 2400.0, 2600.0};
// Never encoded into either substream - a check against this is a check
// against the noise floor, not against another real tone.
constexpr double kLegacyCoreSilentHz = 5000.0;

std::vector<std::vector<float>> legacy_core_unit_pcm(std::span<const double> hz,
                                                      std::size_t unit) {
    const auto frame = static_cast<std::size_t>(ac3::kSamplesPerFrame);
    std::vector<std::vector<float>> pcm(hz.size(), std::vector<float>(frame));
    for (std::size_t c = 0; c < hz.size(); ++c) {
        for (std::size_t i = 0; i < frame; ++i) {
            const auto n = static_cast<double>(unit * frame + i);
            pcm[c][i] =
                static_cast<float>(0.4 * std::sin(2.0 * std::numbers::pi * hz[c] * n / 48000.0));
        }
    }
    return pcm;
}

// 16 access units (0.512 s at 48 kHz) - the same round figure the cut test
// below uses: never silence, never one frame (see tone_channels above).
fs::path write_legacy_core_stream(const std::string& name) {
    const auto out = scratch_dir() / name;
    if (fs::exists(out)) {
        return out;
    }
    ac3::FrameEncoder core{{.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true}};
    ac3::eac3::FrameEncoder rear{{.bitrate_kbps = 192,
                                  .acmod = ac3::Acmod::k2_2,
                                  .strmtyp = ac3::eac3::StreamType::kDependent,
                                  .substreamid = 0,
                                  .chanmap = ac3::eac3::chanmap::k71Rear,
                                  .last_dependent = true}};
    std::vector<std::byte> stream;
    for (std::size_t unit = 0; unit < 16; ++unit) {
        const auto bed = legacy_core_unit_pcm(kLegacyCoreBedHz, unit);
        const std::vector<std::span<const float>> bed_views{bed.begin(), bed.end()};
        const auto core_frame = core.encode_frame(bed_views);
        REQUIRE(core_frame.has_value());
        stream.insert(stream.end(), core_frame->begin(), core_frame->end());

        const auto dep = legacy_core_unit_pcm(kLegacyCoreRearHz, unit);
        const std::vector<std::span<const float>> dep_views{dep.begin(), dep.end()};
        const auto dep_frame = rear.encode_frame(dep_views);
        REQUIRE(dep_frame.has_value());
        stream.insert(stream.end(), dep_frame->begin(), dep_frame->end());
    }
    std::ofstream file{out, std::ios::binary};
    file.write(reinterpret_cast<const char*>(stream.data()),
               static_cast<std::streamsize>(stream.size()));
    REQUIRE(file.good());
    return out;
}

// The power of one frequency in `x`, whatever its phase - the same matched
// correlation tests/decoder/test_stream_playback.cpp's own tone_power uses.
// Phase-independent, so it needs no compensation for a fixed decode/encode
// latency: a delay only rotates re/im between each other, it does not shrink
// re^2+im^2 once the signal runs thousands of samples (there is no JOC
// reconstruction delay here - a plain chanmap'd bed and dependent are not
// decoded objects).
double tone_power(std::span<const float> x, double hz) {
    double re = 0.0;
    double im = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double phase = 2.0 * std::numbers::pi * hz * static_cast<double>(i) / 48000.0;
        re += static_cast<double>(x[i]) * std::cos(phase);
        im += static_cast<double>(x[i]) * std::sin(phase);
    }
    return re * re + im * im;
}

// The strongest channel at `hz`, so a caller does not need to know which WAV
// position a Table E2.5 location landed at.
double best_channel_power(const ac3::io::WavData& wav, double hz) {
    double best = 0.0;
    for (const auto& channel : wav.channels) {
        best = std::max(best, tone_power(channel, hz));
    }
    return best;
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

TEST_CASE("transcode carries a reserved dmixmod across as not indicated", "[cli][transcode]") {
    // Table D2.2's '11' (TS 102 366 Table D.1.1) is reserved in E-AC-3's
    // mixmdate, as in AC-3's Annex D, and the encoder refuses to write it. A
    // DD+ to DD+ transcode carries mixmdate across, so it has to carry
    // §D2.3.1.2's reading of the code - "not indicated" - or refuse a stream
    // that decodes fine. The source is the '01' and '10' encodes of one tone
    // ORed byte by byte ('01' | '10' is '11', every other bit meets an
    // identical copy) with each syncframe's CRCs re-stamped;
    // tests/meta/test_bsi.cpp checks that this changes nothing but dmixmod.
    const auto dir = scratch_dir();
    const auto ltrt = read_bytes(
        make_stream("tx_dmix_ltrt.ec3", "eac3-encode", "none 51 off mixmeta dmixmod=ltrt"));
    const auto loro = read_bytes(
        make_stream("tx_dmix_loro.ec3", "eac3-encode", "none 51 off mixmeta dmixmod=loro"));
    REQUIRE(ltrt.size() == loro.size());
    std::vector<std::byte> merged(ltrt.size());
    for (std::size_t i = 0; i < merged.size(); ++i) {
        merged[i] = ltrt[i] | loro[i];
    }
    const auto frames = ac3::split_frames(merged);
    REQUIRE(frames.has_value());
    for (const auto frame : *frames) {
        const auto at = static_cast<std::size_t>(frame.data() - merged.data());
        REQUIRE(ac3::io::restamp_crc(std::span{merged}.subspan(at, frame.size())).has_value());
    }
    const auto before = ac3::io::read_frame_metadata(merged);
    REQUIRE(before.has_value());
    REQUIRE(before->mix.has_value());
    REQUIRE(before->mix->dmixmod == ac3::meta::DownmixMode::kReserved);
    const auto source = dir / "tx_dmix_reserved.ec3";
    {
        std::ofstream file{source, std::ios::binary};
        file.write(reinterpret_cast<const char*>(merged.data()),
                   static_cast<std::streamsize>(merged.size()));
        REQUIRE(file.good());
    }

    const auto out = dir / "tx_dmix_out.ec3";
    const auto log = dir / "tx_dmix.log";
    fs::remove(out);
    REQUIRE(run_cli("transcode " + quoted(source) + " " + quoted(out) + " 448", log) == 0);
    INFO(read_log(log));
    const auto after = ac3::io::read_frame_metadata(read_bytes(out));
    REQUIRE(after.has_value());
    CHECK(after->kind == ac3::io::StreamKind::kEac3);
    REQUIRE(after->mix.has_value());
    CHECK(after->mix->dmixmod == ac3::meta::DownmixMode::kNotIndicated);
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

// The bug this guards: decode_and_render's eac3_source check tested
// `scan.kind == kEac3` alone, so a legacy-core stream (kAc3CoreEac3Extension)
// took the plain-AC-3 FrameDecoder branch instead of Eac3Decoder - reading
// only the AC-3 core's own frame at a time and never the Annex E dependent
// riding behind it. That is exactly the trap StreamKind's own comment warns
// about ("callers that only handle the two plain kinds should refuse this one
// explicitly rather than let it fall through a two-way test") - decode.cpp's
// own dispatch never had this bug because it tests the stream's content
// rather than scan().kind; this file was the one place still gated on the
// enum alone.
TEST_CASE("transcode reads a legacy-core stream's Annex E dependent, not just the AC-3 core",
          "[cli][transcode]") {
    const auto dir = scratch_dir();
    const auto source = write_legacy_core_stream("tx_legacy_core.ec3");

    // The premise: scan() reports the third StreamKind, and the union of the
    // core's 3/2+LFE bed with the dependent's k71Rear chanmap renders 8
    // channels - not something an AC-3-only read of this stream could ever
    // produce (the core alone is 6).
    const auto scanned = ac3::io::scan(read_bytes(source));
    REQUIRE(scanned.has_value());
    REQUIRE(scanned->kind == ac3::io::StreamKind::kAc3CoreEac3Extension);
    REQUIRE(scanned->channels == 8);

    // The E-AC-3 target, so the transcode has a layout (k71, "7.1") that
    // carries all 8 channels rather than folding them the way an AC-3 target
    // would (AC-3 has no coding mode past 5.1) - a fold would still exercise
    // the fixed dispatch, but would blend away the very channels this test
    // checks for.
    const auto out = dir / "tx_legacy_core_out.ec3";
    const auto log = dir / "tx_legacy_core.log";
    REQUIRE(run_cli("transcode " + quoted(source) + " " + quoted(out) + " 448", log) == 0);
    // codec_label(loaded.scan.kind) has the same two-way gap as eac3_source -
    // this is its own regression guard, not just a log spot-check.
    CHECK(read_log(log).find("16 E-AC-3 access units") != std::string::npos);

    const auto out_scanned = ac3::io::scan(read_bytes(out));
    REQUIRE(out_scanned.has_value());
    CHECK(out_scanned->kind == ac3::io::StreamKind::kEac3);
    CHECK(out_scanned->channels == 8);

    const auto wav = dir / "tx_legacy_core_out.wav";
    const auto decode_log = dir / "tx_legacy_core_decode.log";
    REQUIRE(run_cli("decode " + quoted(out) + " " + quoted(wav), decode_log) == 0);
    const auto decoded = ac3::io::read_wav(wav.string());
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->channels.size() == 8);

    // Lrs and Rrs (kLegacyCoreRearHz[2], [3]) exist ONLY because the
    // dependent's k71Rear chanmap added them - the bed's own 3/2+LFE has no
    // such location at all, so any power there proves the dependent decoded
    // rather than being dropped (silently, or by the core FrameDecoder
    // refusing/desyncing on the dependent's bytes).
    const auto silent = best_channel_power(*decoded, kLegacyCoreSilentHz);
    for (const double hz : {kLegacyCoreRearHz[2], kLegacyCoreRearHz[3]}) {
        INFO("expecting the dependent's own tone at " << hz << " Hz");
        CHECK(best_channel_power(*decoded, hz) > 100.0 * std::max(silent, 1.0));
    }

    // Left/Right Surround (kLegacyCoreRearHz[0], [1]) are where §E3.8.2 has
    // the dependent's own channels replace the bed's Ls/Rs - so the bed's own
    // surround tones (kLegacyCoreBedHz[3], [4]) must not be what a decoder
    // still finds there.
    for (const double hz : {kLegacyCoreBedHz[3], kLegacyCoreBedHz[4]}) {
        INFO("not expecting the bed's own surround tone at " << hz << " Hz to survive");
        CHECK(best_channel_power(*decoded, hz) < 100.0 * std::max(silent, 1.0));
    }
}

// Regression for the same bug apps/cli/commands/decode.cpp's own
// "decode plays a legacy core's held-back last unit..." case guards
// (tests/cli/test_cli.cpp): decode_and_render's flush() tail used to push
// each flushed substream's channels into the SampleQueue by calling
// SampleQueue::push once per substream per Table E2.5 location, so a bed and
// the dependent that held the last unit back could both push into slots the
// same slot_to_wav position maps to within one flush - the queue's per-
// channel append grows unevenly rather than just mismatching lengths, since
// it has no per-call slot tracking of its own. decode_and_render now builds
// the whole held-back unit first via ac3::apps::held_back_unit
// (apps/common/stream_playback.hpp) and pushes it exactly once per slot,
// same as every other unit. A genuine E-AC-3 bed (not a legacy core - see
// the separately-flagged decode_and_render dispatch gap for
// kAc3CoreEac3Extension) so this exercises transcode's own eac3_source path
// rather than a dispatch question.
TEST_CASE("transcode carries a held-back last unit's samples through, not just the ones "
          "that arrived on time",
          "[cli][transcode]") {
    namespace cm = ac3::eac3::chanmap;
    constexpr auto kFrame = static_cast<std::size_t>(ac3::kSamplesPerFrame);
    constexpr std::size_t kOnsetSample = 960;
    constexpr int kUnits = 5;
    constexpr int kOnsetUnit = 2;
    constexpr std::array<double, 6> kBedTones = {1000.0, 800.0, 1200.0, 600.0, 1400.0, 60.0};
    constexpr std::array<double, 4> kRearTones = {500.0, 1600.0, 400.0, 1800.0};

    const auto unit_pcm = [&](std::span<const double> tones, int unit) {
        const auto onset = static_cast<std::size_t>(kOnsetUnit) * kFrame + kOnsetSample;
        std::vector<std::vector<float>> pcm(tones.size(), std::vector<float>(kFrame, 0.0F));
        for (std::size_t ch = 0; ch < tones.size(); ++ch) {
            for (std::size_t i = 0; i < kFrame; ++i) {
                const auto n = static_cast<std::size_t>(unit) * kFrame + i;
                if (n < onset) {
                    continue;
                }
                const double t = static_cast<double>(n - onset) / 48000.0;
                pcm[ch][i] =
                    static_cast<float>(0.4 * std::cos(2.0 * std::numbers::pi * tones[ch] * t));
            }
        }
        return pcm;
    };

    // A genuine E-AC-3 5.1 bed (transient-pre-noise held) with a k71Rear
    // dependent - same shape as tests/cli/test_cli.cpp's own legacy-core
    // case, except the bed itself is Annex E from the start (strmtyp 0,
    // bsid 16), so ac3::io::scan reports StreamKind::kEac3 rather than
    // kAc3CoreEac3Extension and decode_and_render's eac3_source check
    // routes it through Eac3Decoder as intended.
    ac3::eac3::AccessUnitEncoder encoder{
        {.independent = {.bitrate_kbps = 448,
                         .acmod = ac3::Acmod::k3_2,
                         .lfe = true,
                         .transient_prenoise = true},
         .dependents = {{.bitrate_kbps = 320, .acmod = ac3::Acmod::k2_2, .chanmap = cm::k71Rear}}}};
    std::vector<std::byte> stream;
    for (int unit = 0; unit < kUnits; ++unit) {
        auto pcm = unit_pcm(kBedTones, unit);
        auto rear = unit_pcm(kRearTones, unit);
        pcm.insert(pcm.end(), rear.begin(), rear.end());
        const std::vector<std::span<const float>> views{pcm.begin(), pcm.end()};
        const auto encoded = encoder.encode_access_unit(views);
        REQUIRE(encoded.has_value());
        stream.insert(stream.end(), encoded->bytes.begin(), encoded->bytes.end());
    }

    const auto dir = scratch_dir();
    const auto source = dir / "tx_held_source.ec3";
    {
        std::ofstream out{source, std::ios::binary};
        out.write(reinterpret_cast<const char*>(stream.data()),
                  static_cast<std::streamsize>(stream.size()));
        REQUIRE(out.good());
    }

    const auto out = dir / "tx_held_out.ec3";
    const auto log = dir / "tx_held.log";
    REQUIRE(run_cli("transcode " + quoted(source) + " " + quoted(out) + " 448", log) == 0);
    INFO(read_log(log));

    // Every real unit made it through, including the one only flush()
    // returns - a pre-fix build's uneven queue growth would have left this
    // short (or, depending on which slot grew, silently wrong rather than
    // short - see the WAV-level check below either way).
    const auto out_scan = ac3::io::scan(read_bytes(out));
    REQUIRE(out_scan.has_value());
    CHECK(ac3::io::stream_duration_samples(*out_scan) == static_cast<std::uint64_t>(kUnits) * kFrame);

    // Decode the transcoded output back and check the last unit's audio
    // directly, the same way test_cli.cpp's legacy-core case does: Ls (the
    // dependent's, per k71Rear's own comment - it replaces the bed's Ls/Rs)
    // must carry the dependent's tone, not the bed's now-superseded one.
    const auto wav_out = dir / "tx_held_out.wav";
    const auto decode_log = dir / "tx_held_decode.log";
    REQUIRE(run_cli("decode " + quoted(out) + " " + quoted(wav_out), decode_log) == 0);
    const auto decoded = ac3::io::read_wav(wav_out.string());
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->channels.size() == 8);
    CHECK(decoded->frame_count() == static_cast<std::size_t>(kUnits) * kFrame);

    const auto layout =
        cm::expand(static_cast<std::uint16_t>(cm::acmod_map(ac3::Acmod::k3_2, true) | cm::k71Rear));
    const auto order =
        ac3::plan::wav_order(std::span{layout.items}.first(static_cast<std::size_t>(layout.count)));
    const auto ls_slot = layout.index_of(cm::Location::kLeftSurround);
    REQUIRE(ls_slot >= 0);
    const auto ls_at = std::find(order.begin(), order.end(), static_cast<std::size_t>(ls_slot));
    REQUIRE(ls_at != order.end());
    const auto ls_wav = static_cast<std::size_t>(std::distance(order.begin(), ls_at));

    const auto tone_power = [&](std::span<const float> x, double hz) {
        double re = 0.0;
        double im = 0.0;
        for (std::size_t i = 0; i < x.size(); ++i) {
            const double phase = 2.0 * std::numbers::pi * hz * static_cast<double>(i) / 48000.0;
            re += static_cast<double>(x[i]) * std::cos(phase);
            im += static_cast<double>(x[i]) * std::sin(phase);
        }
        return re * re + im * im;
    };
    const auto last_unit = std::span{decoded->channels[ls_wav]}.last(kFrame);
    CHECK(tone_power(last_unit, kRearTones[0]) > 100.0 * tone_power(last_unit, kBedTones[3]));
}

TEST_CASE("transcode measures dialnorm when told dialnorm=auto", "[cli][transcode]") {
    const auto dir = scratch_dir();
    // dialnorm=23 on the source is a value the fix must not fall back to:
    // dialnorm=auto asks for a fresh measurement, not a carry, and this
    // tone's own loudness is nowhere near what dialnorm 23 implies.
    const auto source = make_stream("tx_auto.ec3", "eac3-encode", "none 51 off dialnorm=23");
    const auto out = dir / "tx_auto.ac3";
    const auto log = dir / "tx_auto.log";

    REQUIRE(run_cli("transcode " + quoted(source) + " " + quoted(out) + " 448 \"\" dialnorm=auto",
                    log) == 0);
    const auto text = read_log(log);
    INFO(text);
    CHECK(text.find("(measured)") != std::string::npos);
    CHECK(text.find("(from dialnorm=)") == std::string::npos);

    // `normalize` measures the same source independently; transcode's
    // dialnorm=auto must agree with it rather than carrying the source's own
    // 23 or leaving plan::Metadata's unmeasured default of 31.
    const auto norm_out = dir / "tx_auto_norm.ec3";
    const auto norm_log = dir / "tx_auto_norm.log";
    REQUIRE(run_cli("normalize " + quoted(source) + " " + quoted(norm_out), norm_log) == 0);
    const auto normalized = ac3::io::read_frame_metadata(read_bytes(norm_out));
    const auto after = ac3::io::read_frame_metadata(read_bytes(out));
    REQUIRE(normalized.has_value());
    REQUIRE(after.has_value());
    CHECK(after->dialnorm == normalized->dialnorm);
    CHECK(after->dialnorm != 23);
    CHECK(after->dialnorm != 31);
}

// §5.4.2.8 reserves a dialnorm of 0. A decoder reads it as 31, so a stream
// carrying one decodes, but neither encoder writes it, and transcode carries
// the source's dialnorm across unless dialnorm= replaces it. The E-AC-3
// encoder refuses when it is built, by coding no channels, and transcode did
// not check for that: decode_and_render sized its channel list from the zero
// and plan::render indexed past the end of it (0xC0000005 on Windows, with
// nothing printed). The AC-3 encoder refuses at the first frame, where
// transcode reported an illegal bitrate whatever the cause.
TEST_CASE("transcode names the reason when the encoder refuses a carried dialnorm of 0",
          "[cli][transcode]") {
    const auto dir = scratch_dir();
    auto bytes = read_bytes(make_stream("tx_dialnorm0_source.ec3", "eac3-encode", "none 51 off"));
    // Table E1.2: syncword (16), strmtyp (2), substreamid (3), frmsiz (11),
    // fscod (2), numblkscod (2), acmod (3), lfeon (1) and bsid (5) put dialnorm
    // at bits 45 to 49 of every syncframe - the low three bits of byte 5 and
    // the top two of byte 6.
    for (std::size_t at = 0; at < bytes.size();) {
        const auto frame = std::span{bytes}.subspan(at);
        const auto meta = ac3::io::read_frame_metadata(frame);
        REQUIRE(meta.has_value());
        frame[5] &= std::byte{0xF8};
        frame[6] &= std::byte{0x3F};
        REQUIRE(ac3::io::restamp_crc(frame).has_value());
        at += meta->bytes;
    }
    const auto carried = ac3::io::read_frame_metadata(bytes);
    REQUIRE(carried.has_value());
    REQUIRE(carried->dialnorm == 0);
    const auto source = dir / "tx_dialnorm0.ec3";
    {
        std::ofstream file{source, std::ios::binary};
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        REQUIRE(file.good());
    }

    const auto refused = [&](const std::string& suffix) {
        INFO("output " << suffix);
        const auto out = dir / ("tx_dialnorm0_out" + suffix);
        const auto log = dir / ("tx_dialnorm0" + suffix + ".log");
        fs::remove(out);
        const auto rc = run_cli("transcode " + quoted(source) + " " + quoted(out) + " 448", log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc != 0);
        CHECK(text.find("dialnorm out of range 1..31") != std::string::npos);
        CHECK(text.find("bitrate") == std::string::npos);
        CHECK_FALSE(fs::exists(out));
    };
    refused(".ec3");
    refused(".ac3");
}

// All five printed their reports with plain fmt::println on the status
// stream, which `quiet` makes nullptr, so under quiet each of them wrote its
// output and then failed on the null FILE* - on Windows the runtime's
// parameter check exits 0xC0000409 - whatever the input. The run without
// quiet goes first: its report shows there is something to silence, and its
// output is what the quiet run's has to match byte for byte.
TEST_CASE("every stream tool says nothing under quiet and writes the same output",
          "[cli][quiet]") {
    const auto dir = scratch_dir();
    const auto log = dir / "quiet_tools.log";
    const auto source =
        make_stream("quiet_tools_source.ec3", "eac3-encode", "none 51 off dialnorm=23 heavy");
    const auto immersive = dir / "quiet_tools_714.ec3";
    REQUIRE(run_cli("eac3-sine " + quoted(immersive) + " 1 768 500 60 714", log) == 0);

    // `before` and `after` are the command line either side of the output
    // path: cat takes its output first, the other four take it second.
    const auto both = [&](const std::string& tag, const std::string& before,
                          const std::string& suffix, const std::string& after) {
        INFO(tag);
        const auto loud = dir / ("quiet_" + tag + "_loud" + suffix);
        const auto quiet = dir / ("quiet_" + tag + suffix);
        REQUIRE(run_cli(before + quoted(loud) + after, log) == 0);
        REQUIRE_FALSE(read_log(log).empty());
        fs::remove(quiet);
        CHECK(run_cli(before + quoted(quiet) + after + " quiet", log) == 0);
        CHECK(read_log(log).empty());
        CHECK(read_bytes(quiet) == read_bytes(loud));
    };
    both("transcode", "transcode " + quoted(source) + " ", ".ac3", " 448");
    // The fold note is printed before anything is encoded, from a different
    // place than the summary is.
    both("transcode_fold", "transcode " + quoted(immersive) + " ", ".ac3", " 448");
    both("metadata", "metadata " + quoted(source) + " ", ".ec3", " dialnorm=20");
    both("normalize", "normalize " + quoted(source) + " ", ".ec3", "");
    both("cut", "cut " + quoted(source) + " ", ".ec3", " 0.512 0.512");
    both("cat", "cat ", ".ec3", " " + quoted(source) + " " + quoted(source));
}
