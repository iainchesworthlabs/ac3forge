#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <cmath>
#include <optional>
#include <string>

// apps/hearth/ui/hearth_controller.cpp's own QML-translation layer (issue
// #886, part 2): queue_row(), media_info_to_map() and its field-by-field
// helpers, decoder_settings_to_map()/from_map(), channel_level_to_map(),
// display_object_to_map() and friends are what actually produce the
// QVariantMap keys every Hearth page's QML binds to - a mistyped key, a
// dropped field or a wrong unit conversion currently renders as a
// blank/wrong value with nothing to catch it. Several other bugs found in
// the same review round (dialnorm variance, EMDF payload names, Lt/Rt mix
// levels, reconstructed object counts) were exactly this shape: the
// controller computing something correctly and the map-building function
// not copying it across.
//
// WHY THIS FILE #include's hearth_controller.cpp INSTEAD OF LINKING IT
//
// Every function above is declared inside an ANONYMOUS namespace nested in
// `namespace ac3::hearth::ui { namespace { ... } }` - internal linkage, so
// no other translation unit can call them, and there is no header declaring
// them to link against even if there were. Getting real coverage over the
// REAL functions (not a second, hand-written copy this project's own QML
// harnesses explicitly avoid - see e.g. apps/crucible/ui/tests/qml_test_main.cpp's
// comment on "a parallel fake API is a second thing the real one could
// silently disagree with") means compiling this exact .cpp into this exact
// translation unit, so its anonymous namespace is this file's anonymous
// namespace too.
//
// tests/CMakeLists.txt deliberately does NOT add hearth_controller.cpp to
// ac3tests's own source list for this: ac3tests is Qt-free by design (see
// its own comments on gui_diagnostics.cpp and the Crucible engine sources -
// "exactly so its... contract can be held here... without a QML engine in
// the room"), and hearth_controller.cpp needs QVariantMap/QString (Qt Core)
// plus a QObject-derived HearthController class that also touches Qt Gui
// (QGuiApplication, diagnosticsReport()) and Qt Qml (QML_ELEMENT/
// QML_SINGLETON). This file instead builds into its own small binary,
// ac3hearth_controller_tests, gated on Qt6 Core+Gui+Qml being found - never
// Quick, QuickControls2 or Widgets, and never a live QGuiApplication
// instance: nothing here opens a window, loads a QML file or runs an event
// loop. HearthController's own class methods compile as part of this
// translation unit too (the .cpp is included whole), which is why this
// target also links ac3hearth_engine, item_loader.cpp and
// apps/common/container_input.cpp - what HearthController::start()/
// addFolder() reference has to resolve at link time even though no test
// here calls them.
#include "hearth_controller.cpp"

#include "ac3/analysis/levels.hpp"
#include "ac3/audio/passthrough.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/decoder/output.hpp"
#include "ac3/io/probe.hpp"
#include "ac3/oba/oamd.hpp"
#include "ac3/render/layout.hpp"
#include "container_input.hpp"
#include "decoder_settings.hpp"
#include "media_info.hpp"
#include "queue.hpp"

namespace {

using ac3::hearth::DecoderSettings;
using ac3::hearth::ItemFacts;
using ac3::hearth::MediaBitstream;
using ac3::hearth::MediaInfo;
using ac3::hearth::MediaProgramme;
using ac3::hearth::QueueItem;

// Matches tests/hearth/test_queue.cpp's own QueueItem factory shape (fields
// set one at a time, not one braced initialiser that both reads and moves
// from the same string - that file's own comment says why: a GNU
// -Wnull-dereference false positive at -O3 this project already works
// around for hearth/test_queue.cpp and hearth/test_transport.cpp).
QueueItem queue_item(std::string path, std::string title) {
    QueueItem item;
    item.path = std::move(path);
    item.title = std::move(title);
    return item;
}

}  // namespace

// --- queue_row() ------------------------------------------------------

TEST_CASE("queue_row: a playable item carries its facts and the current flag",
          "[hearth][hearth-controller]") {
    QueueItem item = queue_item("C:/music/programme.ec3", "programme.ec3");
    item.facts.stream = ac3::audio::BitstreamFormat::kEac3;
    item.facts.sample_rate = 48000;
    item.facts.channels = 6;
    item.facts.has_objects = true;
    item.facts.bitrate_kbps = 640.0;
    item.facts.duration = std::chrono::milliseconds(12345);
    item.facts.note = "decoded fine";

    const QVariantMap row = ac3::hearth::ui::queue_row(item, /*current=*/true);

    CHECK(row.value(QStringLiteral("path")).toString().toStdString() == "C:/music/programme.ec3");
    CHECK(row.value(QStringLiteral("title")).toString().toStdString() == "programme.ec3");
    CHECK(row.value(QStringLiteral("playable")).toBool());
    CHECK(row.value(QStringLiteral("note")).toString().toStdString() == "decoded fine");
    CHECK(row.value(QStringLiteral("durationMs")).toLongLong() == 12345);
    CHECK(row.value(QStringLiteral("channels")).toInt() == 6);
    CHECK(row.value(QStringLiteral("sampleRate")).toUInt() == 48000u);
    CHECK(row.value(QStringLiteral("hasObjects")).toBool());
    // stream_kind_name(): has_objects + a stream format reads "E-AC-3 JOC".
    CHECK(row.value(QStringLiteral("streamKind")).toString().toStdString() == "E-AC-3 JOC");
    CHECK(row.value(QStringLiteral("codecBadge")).toString().toStdString() == "E3");
    REQUIRE(row.contains(QStringLiteral("bitrateKbps")));
    CHECK(row.value(QStringLiteral("bitrateKbps")).toDouble() == Catch::Approx(640.0));
    CHECK(row.value(QStringLiteral("current")).toBool());
}

TEST_CASE("queue_row: an unplayable item reports why, and bitrateKbps is absent, not zero",
          "[hearth][hearth-controller]") {
    QueueItem item = queue_item("C:/music/unknown.mkv", "unknown.mkv");
    item.facts.unplayable_because = "no AC-3/E-AC-3 track found";

    const QVariantMap row = ac3::hearth::ui::queue_row(item, /*current=*/false);

    CHECK_FALSE(row.value(QStringLiteral("playable")).toBool());
    CHECK(row.value(QStringLiteral("note")).toString().toStdString() == "no AC-3/E-AC-3 track found");
    CHECK_FALSE(row.contains(QStringLiteral("bitrateKbps")));
    CHECK(row.value(QStringLiteral("durationMs")).toLongLong() == 0);
    CHECK_FALSE(row.value(QStringLiteral("current")).toBool());
    // AC-3 (not E-AC-3), no objects: codec_badge()/stream_kind_name() read
    // "" until item.facts.stream is set at all (an AC-4 item never sets it,
    // and this item's facts here are the same "not yet probed" shape).
    CHECK(row.value(QStringLiteral("streamKind")).toString().isEmpty());
    CHECK(row.value(QStringLiteral("codecBadge")).toString().isEmpty());
}

TEST_CASE("queue_row: a plain AC-3 item's badge is A3, not E3", "[hearth][hearth-controller]") {
    QueueItem item = queue_item("a.ac3", "a.ac3");
    item.facts.stream = ac3::audio::BitstreamFormat::kAc3;

    const QVariantMap row = ac3::hearth::ui::queue_row(item, false);
    CHECK(row.value(QStringLiteral("codecBadge")).toString().toStdString() == "A3");
    CHECK(row.value(QStringLiteral("streamKind")).toString().toStdString() == "AC-3/E-AC-3");
}

// --- media_container_to_map() ------------------------------------------

TEST_CASE("media_container_to_map: an unknown container reads as an elementary stream (empty map)",
          "[hearth][hearth-controller]") {
    const ac3::apps::ContainerFacts facts{};  // kind defaults to kUnknown
    const QVariantMap map = ac3::hearth::ui::media_container_to_map(facts);
    CHECK(map.isEmpty());
}

TEST_CASE("media_container_to_map: an mp4 container carries its codec box",
          "[hearth][hearth-controller]") {
    ac3::apps::ContainerFacts facts;
    facts.kind = ac3::apps::ContainerKind::kMp4;
    facts.track = 2;
    facts.language = "eng";
    facts.sample_rate = 48000;
    facts.channels = 6;
    facts.edits = 3;
    ac3::apps::CodecBox box;
    box.bsid = 16;
    box.bsmod = 0;
    box.acmod = 7;  // 3/2
    box.lfeon = true;
    box.data_rate_kbps = 640;
    box.independent_substreams = 1;
    box.num_dep_sub = 0;
    box.complexity_index = 5;
    facts.codec_box = box;

    const QVariantMap map = ac3::hearth::ui::media_container_to_map(facts);

    CHECK(map.value(QStringLiteral("track")).toInt() == 2);
    CHECK(map.value(QStringLiteral("language")).toString().toStdString() == "eng");
    CHECK(map.value(QStringLiteral("sampleRate")).toUInt() == 48000u);
    CHECK(map.value(QStringLiteral("channels")).toInt() == 6);
    CHECK(map.value(QStringLiteral("edits")).toLongLong() == 3);
    REQUIRE(map.contains(QStringLiteral("codecBox")));
    const QVariantMap codec_box = map.value(QStringLiteral("codecBox")).toMap();
    CHECK(codec_box.value(QStringLiteral("bsid")).toInt() == 16);
    CHECK(codec_box.value(QStringLiteral("lfeon")).toBool());
    CHECK(codec_box.value(QStringLiteral("dataRateKbps")).toInt() == 640);
    REQUIRE(codec_box.contains(QStringLiteral("complexityIndex")));
    CHECK(codec_box.value(QStringLiteral("complexityIndex")).toInt() == 5);
    // mpegts's own nested map must not appear for an mp4 container.
    CHECK_FALSE(map.contains(QStringLiteral("mpegts")));
}

TEST_CASE("media_container_to_map: an mpeg-ts container carries programme/PMT/stream-type, not a codec box",
          "[hearth][hearth-controller]") {
    ac3::apps::ContainerFacts facts;
    facts.kind = ac3::apps::ContainerKind::kMpegTs;
    facts.program_number = 1;
    facts.pmt_pid = 0x100;
    facts.stream_type = 0x81;

    const QVariantMap map = ac3::hearth::ui::media_container_to_map(facts);

    REQUIRE(map.contains(QStringLiteral("mpegts")));
    const QVariantMap ts = map.value(QStringLiteral("mpegts")).toMap();
    CHECK(ts.value(QStringLiteral("programNumber")).toInt() == 1);
    CHECK(ts.value(QStringLiteral("pmtPid")).toInt() == 0x100);
    CHECK(ts.value(QStringLiteral("streamType")).toInt() == 0x81);
    CHECK_FALSE(map.contains(QStringLiteral("codecBox")));
}

// --- media_bitstream_to_map() -------------------------------------------

TEST_CASE("media_bitstream_to_map: Lo/Ro mix levels and the preferred downmix label are exposed",
          "[hearth][hearth-controller]") {
    MediaBitstream bits;
    bits.levels.loro_clev = 0.5;   // -6.02 dB
    bits.levels.loro_slev = 1.0;   // 0 dB
    bits.levels.lfe_mix_level_db = 3.0;

    const QVariantMap map = ac3::hearth::ui::media_bitstream_to_map(bits);
    REQUIRE(map.contains(QStringLiteral("mixLevels")));
    const QVariantMap mix = map.value(QStringLiteral("mixLevels")).toMap();
    CHECK(mix.value(QStringLiteral("centreDb")).toDouble() ==
          Catch::Approx(20.0 * std::log10(0.5)));
    CHECK(mix.value(QStringLiteral("surroundDb")).toDouble() == Catch::Approx(0.0).margin(1e-9));
    REQUIRE(mix.contains(QStringLiteral("lfeDb")));
    CHECK(mix.value(QStringLiteral("lfeDb")).toDouble() == Catch::Approx(3.0));
}

TEST_CASE("media_bitstream_to_map: KNOWN GAP - Lt/Rt mix levels (ltrt_clev/ltrt_slev) are never "
          "copied into the map, only Lo/Ro",
          "[hearth][hearth-controller][known-gap]") {
    // ac3::MixLevels carries ltrt_clev/ltrt_slev (src/forge/include/ac3/decoder/output.hpp)
    // alongside loro_clev/loro_slev, and media_bitstream_to_map() reads only
    // the loro_* pair into mixLevels.centreDb/surroundDb - confirmed by
    // reading the function body directly (hearth_controller.cpp). This
    // characterises today's real behaviour rather than papering over it:
    // setting ltrt_clev/ltrt_slev to values that would produce clearly
    // different dB readings than the loro_* pair still yields the SAME
    // centreDb/surroundDb the previous test got from loro_clev/loro_slev
    // alone - i.e. no Lt/Rt-specific key exists in the map at all. This is
    // exactly the class of bug issue #886 itself names as the reason this
    // test file exists ("Lt/Rt mix levels dropped"), and is tracked as a
    // real, already-filed gap in issue #904 (point 1 there, same function,
    // same line). Left as a documented, deliberately-not-fixed gap here:
    // fixing hearth_controller.cpp's behaviour belongs to #904, a separate
    // change from standing up its test coverage, and
    // apps/hearth/ui/hearth_controller.cpp is one of the hottest files in
    // this repo's current review round. If this test starts failing
    // because ltrtCentreDb/ltrtSurroundDb (or similar) keys have been
    // added, that is #904 being closed - update or remove this test case
    // then, it is not a regression.
    MediaBitstream bits;
    bits.levels.loro_clev = 0.5;
    bits.levels.loro_slev = 1.0;
    bits.levels.ltrt_clev = 0.25;  // a clearly different value from loro_clev
    bits.levels.ltrt_slev = 0.25;

    const QVariantMap map = ac3::hearth::ui::media_bitstream_to_map(bits);
    const QVariantMap mix = map.value(QStringLiteral("mixLevels")).toMap();

    CHECK(mix.value(QStringLiteral("centreDb")).toDouble() ==
          Catch::Approx(20.0 * std::log10(0.5)));
    CHECK_FALSE(mix.contains(QStringLiteral("ltrtCentreDb")));
    CHECK_FALSE(mix.contains(QStringLiteral("ltrtSurroundDb")));
}

// --- media_probe_to_map() ------------------------------------------------

TEST_CASE("media_probe_to_map: a constant dialnorm carries no dialnormMaxDb; a varying one does",
          "[hearth][hearth-controller]") {
    ac3::io::ProbeReport report;
    report.dialnorm.seen = true;
    report.dialnorm.min = -27;
    report.dialnorm.max = -27;

    QVariantMap map = ac3::hearth::ui::media_probe_to_map(report);
    CHECK(map.value(QStringLiteral("dialnormConstant")).toBool());
    CHECK_FALSE(map.contains(QStringLiteral("dialnormMaxDb")));

    report.dialnorm.max = -20;
    map = ac3::hearth::ui::media_probe_to_map(report);
    CHECK_FALSE(map.value(QStringLiteral("dialnormConstant")).toBool());
    REQUIRE(map.contains(QStringLiteral("dialnormMaxDb")));
    // dialnorm_db() is a straightforward negation of the raw §-dB word -
    // min/max ordering in the map follows report.dialnorm.min/.max exactly,
    // not sorted by dB value.
    CHECK(map.value(QStringLiteral("dialnormDb")).toDouble() != Catch::Approx(map.value(QStringLiteral("dialnormMaxDb")).toDouble()));
}

TEST_CASE("media_probe_to_map: EMDF payload ids and the reconstructed object count are both exposed",
          "[hearth][hearth-controller]") {
    ac3::io::ProbeReport report;
    report.emdf_payload_ids = {2, 6, 118};
    ac3::oba::Program program{};
    program.dynamic_objects = 5;
    report.program = program;

    const QVariantMap map = ac3::hearth::ui::media_probe_to_map(report);

    REQUIRE(map.contains(QStringLiteral("emdfPayloadIds")));
    const QVariantList ids = map.value(QStringLiteral("emdfPayloadIds")).toList();
    REQUIRE(ids.size() == 3);
    CHECK(ids.at(0).toInt() == 2);
    CHECK(ids.at(1).toInt() == 6);
    CHECK(ids.at(2).toInt() == 118);

    REQUIRE(map.contains(QStringLiteral("objectCount")));
    CHECK(map.value(QStringLiteral("objectCount")).toInt() == 5);
}

TEST_CASE("media_probe_to_map: KNOWN GAP (issue #904 point 3) - compr/dynrng collapse to a bare "
          "seen flag, the real min/max range is discarded",
          "[hearth][hearth-controller][known-gap]") {
    // ProbeReport::compr/dynrng are MinMax (seen + min + max), the same
    // shape dialnorm above carries its own min/max through - but
    // media_probe_to_map() only ever reads report.compr.seen/report.dynrng.seen
    // (confirmed by reading the function body directly), never .min/.max.
    // Two reports with identical .seen but very different ranges therefore
    // produce IDENTICAL comprSeen/dynrngSeen output, and no comprMinDb/
    // comprMaxDb/dynrngMinDb/dynrngMaxDb key exists at all - matching #904's
    // own description ("collapse to a bare boolean... discarding the real
    // min/max range"). If this test starts failing because such keys have
    // been added, that is #904 being closed - update or remove it then.
    ac3::io::ProbeReport narrow;
    narrow.compr.seen = true;
    narrow.compr.min = 0;
    narrow.compr.max = 0;
    narrow.dynrng.seen = true;
    narrow.dynrng.min = 0;
    narrow.dynrng.max = 0;

    ac3::io::ProbeReport wide;
    wide.compr.seen = true;
    wide.compr.min = -80;
    wide.compr.max = 80;
    wide.dynrng.seen = true;
    wide.dynrng.min = -128;
    wide.dynrng.max = 127;

    const QVariantMap narrow_map = ac3::hearth::ui::media_probe_to_map(narrow);
    const QVariantMap wide_map = ac3::hearth::ui::media_probe_to_map(wide);

    CHECK(narrow_map.value(QStringLiteral("comprSeen")).toBool());
    CHECK(wide_map.value(QStringLiteral("comprSeen")).toBool());
    CHECK(narrow_map.value(QStringLiteral("dynrngSeen")).toBool());
    CHECK(wide_map.value(QStringLiteral("dynrngSeen")).toBool());
    CHECK_FALSE(narrow_map.contains(QStringLiteral("comprMinDb")));
    CHECK_FALSE(narrow_map.contains(QStringLiteral("comprMaxDb")));
    CHECK_FALSE(narrow_map.contains(QStringLiteral("dynrngMinDb")));
    CHECK_FALSE(narrow_map.contains(QStringLiteral("dynrngMaxDb")));
}

TEST_CASE("media_probe_to_map: tool-usage counters and crc/parse failures round trip",
          "[hearth][hearth-controller]") {
    ac3::io::ProbeReport report;
    report.bitrate_kbps = 384.0;
    report.access_units = 900;
    report.syncframes = 900;
    report.crc_failures = 2;
    report.parse_failures = 1;
    report.tools.blocks = 5400;
    report.tools.block_switch = 12;
    report.tools.coupling = 30;
    report.tools.aht_frames = 0;

    const QVariantMap map = ac3::hearth::ui::media_probe_to_map(report);
    CHECK(map.value(QStringLiteral("measuredBitrateKbps")).toDouble() == Catch::Approx(384.0));
    CHECK(map.value(QStringLiteral("accessUnits")).toLongLong() == 900);
    CHECK(map.value(QStringLiteral("crcFailures")).toLongLong() == 2);
    CHECK(map.value(QStringLiteral("parseFailures")).toLongLong() == 1);
    CHECK(map.value(QStringLiteral("blocksParsed")).toLongLong() == 5400);
    CHECK(map.value(QStringLiteral("blockSwitchBlocks")).toLongLong() == 12);
    CHECK(map.value(QStringLiteral("couplingBlocks")).toLongLong() == 30);
}

// --- channel_level_to_map() ----------------------------------------------

TEST_CASE("channel_level_to_map: peak/hold/rms/clipped copy across unchanged",
          "[hearth][hearth-controller]") {
    ac3::analysis::ChannelLevel level;
    level.peak_db = -3.5;
    level.hold_db = -1.0;
    level.rms_db = -12.25;
    level.clipped = true;

    const QVariantMap map = ac3::hearth::ui::channel_level_to_map(level);
    CHECK(map.value(QStringLiteral("peakDb")).toDouble() == Catch::Approx(-3.5));
    CHECK(map.value(QStringLiteral("holdDb")).toDouble() == Catch::Approx(-1.0));
    CHECK(map.value(QStringLiteral("rmsDb")).toDouble() == Catch::Approx(-12.25));
    CHECK(map.value(QStringLiteral("clipped")).toBool());
}

// --- display_object_to_map() ----------------------------------------------

TEST_CASE("display_object_to_map: position/gain/flags copy across, and raised follows z > 0",
          "[hearth][hearth-controller]") {
    ac3::oba::DisplayObject object;
    object.position = {.x = 0.25, .y = 0.75, .z = 0.5};
    object.gain_db = -6.0;
    object.snap = true;
    object.active = true;
    object.label = "L";

    const QVariantMap map = ac3::hearth::ui::display_object_to_map(object);
    CHECK(map.value(QStringLiteral("x")).toDouble() == Catch::Approx(0.25));
    CHECK(map.value(QStringLiteral("y")).toDouble() == Catch::Approx(0.75));
    CHECK(map.value(QStringLiteral("z")).toDouble() == Catch::Approx(0.5));
    CHECK(map.value(QStringLiteral("gainDb")).toDouble() == Catch::Approx(-6.0));
    CHECK(map.value(QStringLiteral("snap")).toBool());
    CHECK(map.value(QStringLiteral("active")).toBool());
    CHECK(map.value(QStringLiteral("label")).toString().toStdString() == "L");
    CHECK(map.value(QStringLiteral("raised")).toBool());

    object.position.z = 0.0;
    const QVariantMap floor_map = ac3::hearth::ui::display_object_to_map(object);
    CHECK_FALSE(floor_map.value(QStringLiteral("raised")).toBool());

    object.position.z = -0.8;
    const QVariantMap below_map = ac3::hearth::ui::display_object_to_map(object);
    CHECK_FALSE(below_map.value(QStringLiteral("raised")).toBool());
}

TEST_CASE("display_object_to_map: a dynamic object's label is empty", "[hearth][hearth-controller]") {
    ac3::oba::DisplayObject object;
    object.label = "";  // dynamic (unlabelled) objects, per this function's own comment
    const QVariantMap map = ac3::hearth::ui::display_object_to_map(object);
    CHECK(map.value(QStringLiteral("label")).toString().isEmpty());
}

// --- decoder_settings_to_map() / decoder_settings_from_map() -------------

TEST_CASE("decoder settings: every control round-trips through the map", "[hearth][hearth-controller]") {
    DecoderSettings settings;
    settings.mode = ac3::OperatingMode::kRf;
    settings.rf_ceiling_db = -18.0;
    settings.drc_cut = 0.3;
    settings.drc_boost = 0.6;
    settings.heavy_compression = true;
    settings.normalise_dialogue = false;
    settings.stereo_fold = ac3::DownmixTarget::kLtRt;
    settings.ltrt_phase_shift = false;
    settings.mix_lfe = true;
    settings.dual_mono = ac3::hearth::DualMonoChoice::kSecond;
    settings.objects = ac3::render::ObjectsPolicy::kAlways;
    settings.joc_domain = ac3::oba::joc::Domain::kMdctBand;
    settings.concealment = ac3::ConcealmentPolicy::kMute;
    settings.fast_inverse_transform = false;

    const QVariantMap map = ac3::hearth::ui::decoder_settings_to_map(settings);
    CHECK(map.value(QStringLiteral("mode")).toString().toStdString() == "rf");
    CHECK(map.value(QStringLiteral("rfCeilingDb")).toDouble() == Catch::Approx(-18.0));
    CHECK(map.value(QStringLiteral("drcCut")).toDouble() == Catch::Approx(0.3));
    CHECK(map.value(QStringLiteral("drcBoost")).toDouble() == Catch::Approx(0.6));
    CHECK(map.value(QStringLiteral("heavyCompression")).toBool());
    CHECK_FALSE(map.value(QStringLiteral("normaliseDialogue")).toBool());
    CHECK(map.value(QStringLiteral("stereoFold")).toString().toStdString() == "ltrt");
    CHECK_FALSE(map.value(QStringLiteral("ltrtPhaseShift")).toBool());
    CHECK(map.value(QStringLiteral("mixLfe")).toBool());
    CHECK(map.value(QStringLiteral("dualMono")).toString().toStdString() == "second");
    CHECK(map.value(QStringLiteral("objects")).toString().toStdString() == "always");
    CHECK(map.value(QStringLiteral("jocDomain")).toString().toStdString() == "mdct");
    CHECK(map.value(QStringLiteral("concealment")).toString().toStdString() == "mute");
    CHECK_FALSE(map.value(QStringLiteral("fastInverseTransform")).toBool());

    const DecoderSettings round_tripped = ac3::hearth::ui::decoder_settings_from_map(map, DecoderSettings{});
    CHECK(round_tripped == settings);
}

TEST_CASE("decoder_settings_from_map: a key the map does not carry keeps base's value",
          "[hearth][hearth-controller]") {
    DecoderSettings base;
    base.mode = ac3::OperatingMode::kCustom;
    base.rf_ceiling_db = -9.0;
    base.dual_mono = ac3::hearth::DualMonoChoice::kFirst;

    // An empty map: every field of `base` survives untouched - the same
    // "unknown or missing keys keep the engine's last-known value" contract
    // decoder_settings_from_map()'s own header comment states, and what a
    // settings page's "apply what changed" always relies on.
    const DecoderSettings unchanged = ac3::hearth::ui::decoder_settings_from_map(QVariantMap{}, base);
    CHECK(unchanged == base);

    // A map with exactly one key changes only that field.
    QVariantMap partial;
    partial[QStringLiteral("rfCeilingDb")] = -3.0;
    const DecoderSettings one_field_changed = ac3::hearth::ui::decoder_settings_from_map(partial, base);
    CHECK(one_field_changed.rf_ceiling_db == Catch::Approx(-3.0));
    CHECK(one_field_changed.mode == ac3::OperatingMode::kCustom);
    CHECK(one_field_changed.dual_mono == ac3::hearth::DualMonoChoice::kFirst);
}

TEST_CASE("decoder settings: mode/downmix/dual-mono/objects/joc-domain/concealment names are stable",
          "[hearth][hearth-controller]") {
    // The exact string values QML compares against (DecoderEac3.qml's
    // SegmentedControl models) - a rename here is a silent breakage there,
    // which is exactly the shape of bug this file exists to catch.
    DecoderSettings settings;
    for (const auto mode : {ac3::OperatingMode::kLine, ac3::OperatingMode::kRf, ac3::OperatingMode::kCustom}) {
        settings.mode = mode;
        const QString name = ac3::hearth::ui::decoder_settings_to_map(settings).value(QStringLiteral("mode")).toString();
        const DecoderSettings back = ac3::hearth::ui::decoder_settings_from_map(
            QVariantMap{{QStringLiteral("mode"), name}}, DecoderSettings{});
        CHECK(back.mode == mode);
    }
    CHECK(ac3::hearth::ui::decoder_settings_to_map(DecoderSettings{}).value(QStringLiteral("mode"))
              .toString().toStdString() == "line");
}

// --- output_format_to_map() -----------------------------------------------

TEST_CASE("output_format_to_map: sample rate, channels and every OutputMode name",
          "[hearth][hearth-controller]") {
    ac3::hearth::OpenOutputFormat format;
    format.sample_rate = 48000;
    format.channels = 6;
    format.mode = ac3::hearth::OutputMode::kBitstreamAsAc3;

    const QVariantMap map = ac3::hearth::ui::output_format_to_map(format);
    CHECK(map.value(QStringLiteral("sampleRate")).toUInt() == 48000u);
    CHECK(map.value(QStringLiteral("channels")).toUInt() == 6u);
    CHECK(map.value(QStringLiteral("mode")).toString().toStdString() == "bitstreamAsAc3");

    format.mode = ac3::hearth::OutputMode::kNone;
    CHECK(ac3::hearth::ui::output_format_to_map(format).value(QStringLiteral("mode"))
              .toString().toStdString() == "none");
}

// --- output_device_row() --------------------------------------------------

TEST_CASE("output_device_row: id/name/channels/speakers/rates/passthrough flags all copy across",
          "[hearth][hearth-controller]") {
    ac3::audio::RenderDeviceInfo device;
    device.id = "{device-guid}";
    device.name = "Realtek Digital Output";
    device.is_default = true;
    device.channels = 8;
    device.speakers = 0x63Fu;
    device.sample_rates = {48000, 96000};
    device.supports_ac3_passthrough = true;
    device.supports_eac3_passthrough = false;

    const QVariantMap row = ac3::hearth::ui::output_device_row(device);
    CHECK(row.value(QStringLiteral("id")).toString().toStdString() == "{device-guid}");
    CHECK(row.value(QStringLiteral("name")).toString().toStdString() == "Realtek Digital Output");
    CHECK(row.value(QStringLiteral("isDefault")).toBool());
    CHECK(row.value(QStringLiteral("channels")).toInt() == 8);
    REQUIRE(row.contains(QStringLiteral("sampleRates")));
    const QVariantList rates = row.value(QStringLiteral("sampleRates")).toList();
    REQUIRE(rates.size() == 2);
    CHECK(rates.at(0).toUInt() == 48000u);
    CHECK(rates.at(1).toUInt() == 96000u);
    CHECK(row.value(QStringLiteral("supportsAc3")).toBool());
    CHECK_FALSE(row.value(QStringLiteral("supportsEac3")).toBool());
}

// --- media_info_to_map() (the composite) -----------------------------------

TEST_CASE("media_info_to_map: path/codec/duration/streamSamples/programmes/nested maps all appear",
          "[hearth][hearth-controller]") {
    MediaInfo info;
    info.path = "C:/music/programme.ec3";
    info.codec = ac3::hearth::MediaCodec::kEac3;
    info.sample_rate = 48000;
    info.stream_samples = 96000;  // 2 seconds at 48 kHz, no skip

    MediaProgramme programme;
    programme.substreamid = 0;
    programme.channels = 6;
    programme.bsid = 16;
    info.programmes.push_back(programme);

    MediaBitstream bits;
    bits.levels.loro_clev = 1.0;
    info.bitstream = bits;

    ac3::io::ProbeReport probe;
    probe.bitrate_kbps = 640.0;
    info.probe = probe;

    const QVariantMap map = ac3::hearth::ui::media_info_to_map(info);
    CHECK(map.value(QStringLiteral("path")).toString().toStdString() == "C:/music/programme.ec3");
    REQUIRE(map.contains(QStringLiteral("codec")));
    CHECK(map.value(QStringLiteral("sampleRate")).toUInt() == 48000u);
    REQUIRE(map.contains(QStringLiteral("durationSeconds")));
    CHECK(map.value(QStringLiteral("durationSeconds")).toDouble() == Catch::Approx(2.0));
    CHECK(map.value(QStringLiteral("streamSamples")).toLongLong() == 96000);
    CHECK(map.value(QStringLiteral("container")).toMap().isEmpty());  // ContainerFacts{} defaults to kUnknown
    REQUIRE(map.value(QStringLiteral("programmes")).toList().size() == 1);
    CHECK(map.value(QStringLiteral("programmes")).toList().at(0).toMap()
              .value(QStringLiteral("channels")).toInt() == 6);
    REQUIRE(map.contains(QStringLiteral("bitstream")));
    REQUIRE(map.contains(QStringLiteral("probe")));
    CHECK(map.value(QStringLiteral("probe")).toMap().value(QStringLiteral("measuredBitrateKbps"))
              .toDouble() == Catch::Approx(640.0));
    CHECK_FALSE(map.contains(QStringLiteral("ac4")));  // no ac4 summary on this item
    // json carries the whole media_info_json() document - not re-parsed
    // here (that document's own shape is media_info_json()'s contract, not
    // this map-building layer's), just confirmed present and non-empty so a
    // dropped `json` key would still be caught.
    REQUIRE(map.contains(QStringLiteral("json")));
    CHECK_FALSE(map.value(QStringLiteral("json")).toString().isEmpty());
}

TEST_CASE("media_info_to_map: no sample rate means no durationSeconds key at all",
          "[hearth][hearth-controller]") {
    MediaInfo info;
    info.path = "x.ec3";
    // sample_rate left at its default (0).
    const QVariantMap map = ac3::hearth::ui::media_info_to_map(info);
    CHECK_FALSE(map.contains(QStringLiteral("sampleRate")));
    CHECK_FALSE(map.contains(QStringLiteral("durationSeconds")));
}

TEST_CASE("media_info_to_map: error and note strings only appear when non-empty",
          "[hearth][hearth-controller]") {
    MediaInfo info;
    info.path = "x.ec3";
    info.error = "could not open file";
    const QVariantMap map = ac3::hearth::ui::media_info_to_map(info);
    REQUIRE(map.contains(QStringLiteral("error")));
    CHECK(map.value(QStringLiteral("error")).toString().toStdString() == "could not open file");
    CHECK_FALSE(map.contains(QStringLiteral("note")));
}
