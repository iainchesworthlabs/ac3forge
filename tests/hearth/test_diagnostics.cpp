#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <vector>

#include "decoder_settings.hpp"
#include "diagnostic_log.hpp"
#include "diagnostics_report.hpp"
#include "engine_thread.hpp"
#include "queue.hpp"

// Hearth's diagnostics file (apps/hearth/engine/diagnostic_log.hpp and
// diagnostics_report.hpp): the ring keeps the newest lines in order and
// counts what it dropped, a note is one line no longer than the cap, and the
// file never carries a path - the Settings page's "pairing keys, codes and
// file paths are left out". What the player and the engine write to the ring
// is test_player.cpp's and test_engine.cpp's.

using ac3::hearth::DecoderSettings;
using ac3::hearth::DiagnosticLog;
using ac3::hearth::EngineStatus;
using ac3::hearth::PlayedItem;
using ac3::hearth::QueueItem;
using ac3::hearth::ReportFacts;
using ac3::hearth::Secrets;

namespace {

bool has(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

bool stamped(const std::string& line) {
    // "+ssss.mmm " and then the note.
    if (line.size() <= DiagnosticLog::kStampBytes || line[0] != '+' || line[5] != '.' ||
        line[9] != ' ') {
        return false;
    }
    for (const std::size_t i : {1U, 2U, 3U, 4U, 6U, 7U, 8U}) {
        if (std::isdigit(static_cast<unsigned char>(line[i])) == 0) {
            return false;
        }
    }
    return true;
}

Secrets withheld(std::initializer_list<const char*> paths) {
    Secrets secrets;
    for (const char* path : paths) {
        ac3::hearth::withhold_path(secrets, path);
    }
    return secrets;
}

QueueItem queued(const std::string& path, const std::string& title, const std::string& unplayable = {}) {
    QueueItem item;
    item.path = path;
    item.title = title;
    item.facts.unplayable_because = unplayable;
    return item;
}

}  // namespace

TEST_CASE("diagnostics: the log keeps the newest lines in order and counts what it dropped",
          "[hearth][diagnostics]") {
    DiagnosticLog log(4);
    CHECK(log.lines().empty());
    // Round the ring more than once.
    for (int i = 1; i <= 11; ++i) {
        log.note("line " + std::to_string(i));
    }
    const auto lines = log.lines();
    REQUIRE(lines.size() == 4);
    CHECK(lines[0].ends_with(" line 8"));
    CHECK(lines[1].ends_with(" line 9"));
    CHECK(lines[2].ends_with(" line 10"));
    CHECK(lines[3].ends_with(" line 11"));
    CHECK(log.dropped() == 7);
    CHECK(log.capacity() == 4);
    for (const auto& line : lines) {
        CHECK(stamped(line));
    }
    // A log with room for nothing still keeps the latest line.
    DiagnosticLog tiny(0);
    tiny.note("first");
    tiny.note("second");
    REQUIRE(tiny.lines().size() == 1);
    CHECK(tiny.lines()[0].ends_with(" second"));
    CHECK(tiny.dropped() == 1);
}

TEST_CASE("diagnostics: a note is one line, cut whole characters short of the cap",
          "[hearth][diagnostics]") {
    DiagnosticLog log(4);
    std::string big(2000, 'x');
    big[10] = '\r';
    big[11] = '\n';
    big[12] = '\t';
    big[13] = '\x7F';
    log.note(big);
    // A two-byte character straddling the cut is left out whole.
    std::string accented(DiagnosticLog::kMaxLine - 5, 'y');
    accented += "\xC3\xA9\xC3\xA9\xC3\xA9";
    log.note(accented);
    log.note("short\r\nnote");
    const auto lines = log.lines();
    REQUIRE(lines.size() == 3);
    for (const auto& line : lines) {
        CHECK(stamped(line));
        CHECK(line.size() <= DiagnosticLog::kMaxLine + DiagnosticLog::kStampBytes);
        for (const char c : line) {
            const auto byte = static_cast<unsigned char>(c);
            CHECK(byte >= 0x20U);
            CHECK(byte != 0x7FU);
        }
    }
    CHECK(lines[0].ends_with("x ..."));
    CHECK(lines[0].substr(DiagnosticLog::kStampBytes, 16) == "xxxxxxxxxx    xx");
    CHECK(lines[1].ends_with("y ..."));
    CHECK_FALSE(has(lines[1], "\xC3"));
    // A short note is neither cut nor marked.
    CHECK(lines[2].substr(DiagnosticLog::kStampBytes) == "short  note");
}

TEST_CASE("diagnostics: a path is withheld by its folders, and its name stays",
          "[hearth][diagnostics]") {
    const Secrets secrets = withheld({"C:\\Users\\Someone\\Music\\a.ec3"});
    // The folder, the folders above it, either separator, any ASCII case.
    CHECK(scrub("no such file: C:\\Users\\Someone\\Music\\a.ec3", secrets) ==
          "no such file: <withheld>\\a.ec3");
    CHECK(scrub("c:/users/someone/MUSIC/b.ec3 and C:\\Users\\Someone\\Videos\\c.mp4", secrets) ==
          "<withheld>/b.ec3 and <withheld>\\Videos\\c.mp4");
    CHECK(scrub("C:\\Users\\Other\\x.ec3", secrets) == "<withheld>\\Other\\x.ec3");
    // The drive is not a secret, and neither is a word that is not a path.
    CHECK(scrub("C: Users, Music", secrets) == "C: Users, Music");

    const Secrets posix = withheld({"/home/someone/music/a.ec3"});
    CHECK(scrub("\"/home/someone/music/a.ec3\" holds no audio.", posix) ==
          "\"<withheld>/a.ec3\" holds no audio.");
    CHECK(scrub("/home/other and /homeless", posix) == "<withheld>/other and <withheld>less");
    CHECK(scrub("the / of a path", posix) == "the / of a path");

    const Secrets unc = withheld({"\\\\server\\share\\a.ec3"});
    CHECK(scrub("\\\\server\\share\\a.ec3 and //server/share/b.ec3", unc) ==
          "<withheld>\\a.ec3 and <withheld>/b.ec3");

    // Nothing to withhold at the top of a drive or of the file system, or in
    // a bare name.
    CHECK(withheld({"C:\\a.ec3", "/a.ec3", "a.ec3", "C:a.ec3"}).strings.empty());

    // The first folder of a relative path only where a separator follows it.
    const Secrets relative = withheld({"music/a.ec3"});
    CHECK(scrub("music/a.ec3, music\\b.ec3, Music Assistant", relative) ==
          "<withheld>a.ec3, <withheld>b.ec3, Music Assistant");
}

TEST_CASE("diagnostics: scrub replaces the longest secret first and never its own marker",
          "[hearth][diagnostics]") {
    const Secrets nested{.strings = {"ab", "abcd", "", "ABCDEF"}};
    CHECK(scrub("xabcdefx abcdx abx", nested) == "x<withheld>x <withheld>x <withheld>x");
    // A secret that is itself the marker's text cannot loop.
    CHECK(scrub("a <withheld> b", Secrets{.strings = {"<withheld>"}}) == "a <withheld> b");
    CHECK(scrub("a <withheld> b", Secrets{.strings = {"withheld"}}) == "a <<withheld>> b");
    CHECK(scrub("plain", Secrets{}) == "plain");
}

TEST_CASE("diagnostics: the file has every section, from an idle engine", "[hearth][diagnostics]") {
    const std::string report =
        ac3::hearth::render_report(ReportFacts{}, EngineStatus{}, DiagnosticLog{}, Secrets{});
    const std::vector<std::string> sections{"# version",  "# platform",
                                            "# output",   "# playback",
                                            "# items that cannot be played (0)",
                                            "# played (oldest first, 0 of 0)",
                                            "# settings", "# recent messages"};
    std::size_t last = 0;
    for (const auto& section : sections) {
        INFO(section);
        auto at = report.find("\n" + section + "\n", last);
        if (at == std::string::npos) {
            // The last header carries its counts on the same line.
            at = report.find("\n" + section + " (", last);
        }
        REQUIRE(at != std::string::npos);
        last = at;
    }
    CHECK(report.starts_with("AC3Forge Hearth diagnostics\n"));
    CHECK(has(report, "chosen: (none)\n"));
    CHECK(has(report, "open: no\n"));
    CHECK(has(report, "times opened: 0\n"));
    CHECK(has(report, "state: stopped\n"));
    CHECK(has(report, "gapless: on\n"));
    CHECK(has(report, "repeat: off\n"));
    CHECK(has(report, "an item fails: skip to the next\n"));
    CHECK(has(report, "queue: 0 items, none current\n"));
    CHECK(has(report, "decoder: " + describe(DecoderSettings{}) + "\n"));
    CHECK(has(report, "# recent messages (oldest first, 0 of 512; 0 dropped)\n"));
}

TEST_CASE("diagnostics: the file says what played and what could not, and never where it lives",
          "[hearth][diagnostics]") {
    EngineStatus status;
    status.state = ac3::hearth::TransportState::kPlaying;
    status.queue = {
        queued("D:\\Private\\Songs\\one.ec3", "One"),
        queued("D:\\Private\\Songs\\two.ac4", "Two",
               "\"D:\\Private\\Songs\\two.ac4\" is AC-4, which has no decoder here"),
        queued("/srv/private/three.ec3", "Three"),
    };
    status.current = 2;
    status.repeat = true;
    status.output = ac3::hearth::OpenOutputFormat{.sample_rate = 48000,
                                                  .channels = 8,
                                                  .mode = ac3::hearth::OutputMode::kLocalPcm};
    status.output_opens = 2;
    status.history = {
        PlayedItem{.queue_index = 0, .title = "One", .first_frame = 0, .frames = 9216,
                   .expected_frames = 9216, .output_opens = 1},
        PlayedItem{.queue_index = ac3::hearth::Queue::kNone, .title = "Gone", .first_frame = 9216,
                   .frames = 1536, .expected_frames = 4608, .output_opens = 1},
    };
    // Free text the report must not read: it names an item that has left
    // the queue.
    status.note = "\"E:\\Elsewhere\\gone.ec3\" holds no audio.";
    status.error = status.note;

    DiagnosticLog log(8);
    // A line the window wrote without withholding anything.
    log.note("the window opened /srv/private/three.ec3 and D:/Private/Songs/one.ec3");

    ReportFacts facts;
    facts.written_at = "2026-09-16T12:00:00Z";
    facts.version = "ac3forge 1.2.3";
    facts.platform.emplace_back("os", "Windows 11");
    facts.output_name = "Speakers\n(USB)";
    facts.output_reason = "the default device";
    facts.settings.emplace_back("playback/gapless", "true");
    facts.settings.emplace_back("pairing/records", "sink-1 psk 00112233");
    facts.settings.emplace_back("queue/items", "D:/Private/Songs/one.ec3");
    facts.settings.emplace_back("network/name", "Living room");
    const Secrets secrets{.strings = {"C:\\Users\\Someone\\AppData\\Hearth", "123-456"}};
    facts.platform.emplace_back("settings", "C:\\Users\\Someone\\AppData\\Hearth\\hearth.ini");
    log.note("pairing code 123-456 shown");

    const std::string report = ac3::hearth::render_report(facts, status, log, secrets);
    INFO(report);
    CHECK_FALSE(has(report, "Private"));
    CHECK_FALSE(has(report, "private"));
    CHECK_FALSE(has(report, "Elsewhere"));
    CHECK_FALSE(has(report, "Someone"));
    CHECK_FALSE(has(report, "123-456"));
    CHECK_FALSE(has(report, "00112233"));

    CHECK(has(report, "written: 2026-09-16T12:00:00Z\n"));
    CHECK(has(report, "\n# version\nac3forge 1.2.3\n"));
    CHECK(has(report, "os: Windows 11\n"));
    CHECK(has(report, "settings: <withheld>\\hearth.ini\n"));
    CHECK(has(report, "chosen: \"Speakers (USB)\"\n"));
    CHECK(has(report, "reason: the default device\n"));
    CHECK(has(report, "open: local PCM, 48000 Hz, 8 channels\n"));
    CHECK(has(report, "times opened: 2\n"));
    CHECK(has(report, "state: playing\n"));
    CHECK(has(report, "repeat: on\n"));
    CHECK(has(report, "queue: 3 items, item 3 current\n"));
    CHECK(has(report,
              "# items that cannot be played (1)\n"
              "item 2 \"Two\": \"<withheld>\\two.ac4\" is AC-4, which has no decoder here\n"));
    CHECK(has(report, "# played (oldest first, 2 of 2)\n"
                      "item 1 \"One\": 9216 of 9216 frames, from frame 0 of output open 1\n"
                      "\"Gone\" (no longer in the queue): 1536 of 4608 frames, from frame 9216 "
                      "of output open 1\n"));
    CHECK(has(report, "playback/gapless = true\n"));
    CHECK(has(report, "pairing/records = <withheld>\n"));
    CHECK(has(report, "queue/items = <withheld>\n"));
    CHECK(has(report, "network/name = Living room\n"));
    CHECK(has(report, "# recent messages (oldest first, 2 of 8; 0 dropped)\n"));
    CHECK(has(report, "the window opened <withheld>/three.ec3 and <withheld>/one.ec3\n"));
    CHECK(has(report, "pairing code <withheld> shown\n"));
    CHECK_FALSE(has(report, "holds no audio"));
}

TEST_CASE("diagnostics: the file's lists are cut to their limit", "[hearth][diagnostics]") {
    const std::size_t limit = ac3::hearth::kReportListLimit;
    EngineStatus status;
    for (std::size_t i = 0; i < limit + 7; ++i) {
        status.queue.push_back(queued("/m/" + std::to_string(i), "t" + std::to_string(i), "no"));
        status.history.push_back(PlayedItem{.queue_index = i, .title = "t" + std::to_string(i)});
    }
    const std::string report =
        ac3::hearth::render_report(ReportFacts{}, status, DiagnosticLog{}, Secrets{});
    // The first items that cannot be played, and the last items played.
    CHECK(has(report, "# items that cannot be played (" + std::to_string(limit + 7) + ")\n"));
    CHECK(has(report, "item " + std::to_string(limit) + " \"t" + std::to_string(limit - 1) +
                          "\": no\n... and 7 more\n"));
    CHECK_FALSE(has(report, "\"t" + std::to_string(limit) + "\": no\n"));
    CHECK(has(report, "# played (oldest first, " + std::to_string(limit) + " of " +
                          std::to_string(limit + 7) + ")\nitem 8 \"t7\": "));
    CHECK_FALSE(has(report, "\"t6\": 0 of"));
}

TEST_CASE("diagnostics: the process log is one log, and survives", "[hearth][diagnostics]") {
    DiagnosticLog& one = ac3::hearth::process_diagnostics();
    DiagnosticLog& two = ac3::hearth::process_diagnostics();
    CHECK(&one == &two);
    CHECK(one.capacity() == DiagnosticLog::kDefaultCapacity);
    one.note("shared note");
    const auto lines = two.lines();
    REQUIRE_FALSE(lines.empty());
    CHECK(lines.back().ends_with(" shared note"));
}

TEST_CASE("diagnostics: decoder settings and items read as the pages name them",
          "[hearth][diagnostics]") {
    CHECK(describe(DecoderSettings{}) ==
          "line mode, stereo fold Lo/Ro, LFE in AC-4's folds only, the stream's mix levels, "
          "dual mono: both, the first programme, objects for height layouts, "
          "objects reconstructed in the QMF domain, concealment: repeat and fade, "
          "fast inverse transform, AC-4: the stream's presentation, dialogue to -31 dBFS, "
          "the DRC mode for the output level, dialogue enhancement 0 dB, dialogue +0.0 dB, "
          "no audio description, the stereo fold's downmix");
    DecoderSettings custom;
    custom.mode = ac3::OperatingMode::kCustom;
    custom.drc_cut = 0.5;
    custom.drc_boost = 0.25;
    custom.heavy_compression = true;
    custom.normalise_dialogue = false;
    custom.stereo_fold = ac3::DownmixTarget::kLtRt;
    custom.ltrt_phase_shift = false;
    custom.mix_lfe = true;
    custom.mix_levels.loro_clev = 0.5;
    custom.mix_levels.ltrt_slev = 0.707;
    custom.mix_levels.lfe_mix_level_db = -3.0;
    custom.dual_mono = ac3::hearth::DualMonoChoice::kSecond;
    custom.programme = 2;
    custom.objects = ac3::render::ObjectsPolicy::kNever;
    custom.concealment = ac3::ConcealmentPolicy::kNone;
    custom.fast_inverse_transform = false;
    custom.ac4.presentation_id = 7;
    custom.ac4.output_level_dbfs = -20.0;
    custom.ac4.drc = ac4::DrcMode::kPortableHeadphones;
    custom.ac4.dialogue_enhancement_db = 6.0;
    custom.ac4.dialogue_db = -3.0;
    custom.ac4.audio_description = true;
    custom.ac4.associated_db = -9.5;
    custom.ac4.preferred_downmix = true;
    CHECK(describe(custom) ==
          "custom mode (cut 0.50, boost 0.25, compr on, dialogue as coded), "
          "stereo fold Lt/Rt (phase shift off), LFE in folds, "
          "mix levels Lo/Ro centre 0.500, Lt/Rt surround 0.707, LFE -3.0 dB, "
          "dual mono: channel 2, programme 2, objects never, "
          "objects reconstructed in the QMF domain, no concealment, "
          "reference inverse transform, AC-4: presentation_id 7, dialogue to -20 dBFS, "
          "portable headphones DRC, dialogue enhancement 6 dB, dialogue -3.0 dB, "
          "audio description at -9.5 dB, the stream's preferred downmix");
    DecoderSettings rf;
    rf.mode = ac3::OperatingMode::kRf;
    rf.mix_levels.loro_slev = 0.0;
    rf.mix_levels.ltrt_clev = 1.0;
    rf.dual_mono = ac3::hearth::DualMonoChoice::kFirst;
    rf.objects = ac3::render::ObjectsPolicy::kAlways;
    rf.concealment = ac3::ConcealmentPolicy::kMute;
    rf.mix_lfe = false;
    rf.ac4.normalise = false;
    rf.ac4.language = "fr";
    CHECK(describe(rf) ==
          "RF mode (ceiling 0.0 dBFS), stereo fold Lo/Ro, no LFE in folds, "
          "mix levels Lo/Ro surround 0.000, Lt/Rt centre 1.000, "
          "dual mono: channel 1, the first programme, objects always, "
          "objects reconstructed in the QMF domain, concealment: mute, "
          "fast inverse transform, AC-4: a presentation in fr, the coded level, no compression, "
          "dialogue enhancement 0 dB, dialogue +0.0 dB, no audio description, the stereo fold's "
          "downmix");

    CHECK(ac3::hearth::describe_item(0, "First") == "item 1 \"First\"");
    CHECK(ac3::hearth::describe_item(ac3::hearth::Queue::kNone, "Gone") ==
          "\"Gone\" (no longer in the queue)");
    CHECK(ac3::hearth::describe_item(ac3::hearth::Queue::kNone, "") ==
          "an item no longer in the queue");
}
