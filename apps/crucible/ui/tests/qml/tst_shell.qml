import QtQuick
import QtTest

import Ac3ForgeCrucible
import Ac3ForgeCrucibleLanguage
import Ac3ForgeCrucibleTest

// The window itself: it comes up, switches pages, applies the persisted
// theme, and closing it hides rather than quits while "keep running in the
// tray" is on. Main.qml's Component.onCompleted starts the engine; on a
// machine with no audio endpoint that start refuses and the status strip
// says so, which is a state the shell has to render too.
//
// The last two cases are the window's right-to-left half: the header
// follows the layout direction the language sets, and the plan's speakers
// keep the x they are given, because the plan is a map of a room.
TestCase {
    id: testCase
    name: "Shell"
    when: windowShown

    Component { id: shell; Main {} }

    function init() {
        CrucibleController.stop();
        CrucibleController.theme = "system";
        CrucibleController.palette = "signal";
        CrucibleController.keepRunningWhenClosed = true;
        CrucibleController.moveDefaultOnLaunch = false;
        // The plan rather than the elevation: the right-to-left case below
        // reads the plan's speakers, and every case here starts from the
        // same view whatever an earlier one left behind.
        CrucibleController.roomView = "2d";
        // Seen already, so the first-run dialog does not sit over the shell
        // cases; tst_firstrun.qml is where it is exercised.
        CrucibleController.firstRunAcknowledged = true;
    }

    function cleanup() {
        CrucibleController.stop();
        // The machine back for the one case here that scripts it, before
        // anything else runs against the fake default device: a no-op for
        // every other case (tst_accessibility.qml's cleanup says why this
        // has to happen rather than being left to the next scriptSessions).
        TestServices.clear();
        // A case that switched language leaves the rest of the suite in
        // English, whatever order they run in.
        if (LanguageManager.currentLanguage !== "en") {
            LanguageManager.setLanguage("en");
        }
    }

    function test_windowOpensOnTheRoomAndSwitchesPages() {
        const window = createTemporaryObject(shell, testCase);
        verify(window);
        tryCompare(window, "visible", true);
        compare(window.page, "room");
        window.page = "output";
        compare(window.page, "output");
        window.page = "settings";
        compare(window.page, "settings");
        window.page = "room";
        compare(window.page, "room");
    }

    function test_themeFollowsTheSetting() {
        CrucibleController.theme = "dark";
        CrucibleController.palette = "console";
        const window = createTemporaryObject(shell, testCase);
        verify(window);
        tryCompare(window, "visible", true);
        compare(Theme.preference, "dark");
        compare(Theme.paletteChoice, "console");
        CrucibleController.theme = "light";
        tryCompare(Theme, "preference", "light");
    }

    function test_statusStripReportsTheEngine() {
        const window = createTemporaryObject(shell, testCase);
        verify(window);
        tryCompare(window, "visible", true);
        // Either the engine runs (an audio endpoint exists) or start()
        // refused with a reason; both are rendered, neither is silence.
        verify(CrucibleController.running || CrucibleController.lastError.length > 0,
               "running=" + CrucibleController.running + " lastError=" + CrucibleController.lastError);
        if (CrucibleController.running) {
            tryVerify(function() { return CrucibleController.framesEncoded > 0; }, 5000);
            verify(CrucibleController.tapChannels === 2 || CrucibleController.tapChannels === 6 || CrucibleController.tapChannels === 8);
        }
    }

    // The refusal half of the case above, on a machine scripted to have
    // nothing to play into rather than one that happens to. Which branch the
    // case above takes depends on the seat it runs on, so this is where the
    // refused state is actually rendered and read back.
    function test_statusStripSaysWhyTheEngineCouldNotStart() {
        if (!TestServices.scriptMachineWithNoOutput()) {
            skip("the scripted machine is not available in this harness");
        }
        const window = createTemporaryObject(shell, testCase);
        verify(window);
        tryCompare(window, "visible", true);
        // Main.qml starts the engine as it comes up, and this machine has
        // no endpoint, so that start refuses.
        verify(!CrucibleController.running, "the engine started on a machine with no output");
        verify(CrucibleController.lastError.length > 0, "a refusal with no reason to show");
        const status = findChild(window, "engineStatus");
        verify(status, "the status strip carries objectName engineStatus");
        compare(status.text, CrucibleController.lastError);
        // The reason is the output policy's, which is what says the refusal
        // came from the probe reporting rather than from start() running out
        // of time waiting for a worker that never came up.
        verify(CrucibleController.lastError.indexOf("no render endpoint") >= 0,
               CrucibleController.lastError);
    }

    function test_closingHidesWhileTrayResident() {
        const window = createTemporaryObject(shell, testCase);
        verify(window);
        tryCompare(window, "visible", true);
        window.close();
        tryCompare(window, "visible", false);
        // The engine is left running for the tray; stop() is cleanup's job.
    }

    function starts(report) {
        return report.split("engine started:").length - 1 + (report.split("engine start refused:").length - 1);
    }

    function test_diagnosticsReportCarriesTheEngine() {
        // The ring is process-wide and every suite here starts the engine, so
        // an earlier test's notes would satisfy a bare indexOf. Count what is
        // there first and wait for this engine to add its own.
        const before = starts(CrucibleController.diagnosticsReport());
        const window = createTemporaryObject(shell, testCase);
        verify(window);
        tryCompare(window, "visible", true);
        verify(CrucibleController.running || CrucibleController.lastError.length > 0,
               "running=" + CrucibleController.running + " lastError=" + CrucibleController.lastError);
        // The engine's notes come from its own thread a moment after start()
        // returns, and the signing note follows the start note.
        tryVerify(function() {
            const report = CrucibleController.diagnosticsReport();
            return starts(report) > before && report.indexOf("signing: ") >= 0;
        }, 5000);
        const report = CrucibleController.diagnosticsReport();
        verify(report.indexOf("# engine") >= 0, report);
        // The status line that names the key file never reaches the report.
        verify(report.indexOf("loaded from") < 0, report);
    }

    function test_rightToLeftMirrorsTheHeader() {
        // Arabic sets the application layout direction, and the window
        // root's LayoutMirroring turns that into the arrangement: the
        // title is the first thing in the header row, so it sits at the
        // right edge under Arabic and at the left edge under English.
        verify(LanguageManager.setLanguage("ar"));
        const window = createTemporaryObject(shell, testCase);
        verify(window);
        tryCompare(window, "visible", true);
        const title = findChild(window, "titleText");
        verify(title, "the header title");
        tryVerify(function() { return title.mapToItem(null, 0, 0).x > window.width / 2; }, 3000,
                  "the header title did not move to the right half under Arabic");
        verify(LanguageManager.setLanguage("en"));
        tryVerify(function() { return title.mapToItem(null, 0, 0).x < window.width / 2; }, 3000,
                  "the header title did not come back to the left half under English");
    }

    function test_roomPlanKeepsLeftOnTheLeft() {
        // The plan is a picture of a room: L is where the left speaker
        // is, and mirroring the window must not move it, whatever
        // direction the language reads in. init() has pinned the plan.
        verify(LanguageManager.setLanguage("he"));
        const window = createTemporaryObject(shell, testCase);
        verify(window);
        tryCompare(window, "visible", true);
        compare(window.page, "room");
        const plan = findChild(window, "roomPlan");
        verify(plan, "the room plan");
        const speaker = findChild(plan, "speaker-L");
        verify(speaker, "the plan's L speaker");
        tryVerify(function() { return speaker.mapToItem(plan, 0, 0).x < plan.width / 3; }, 3000,
                  "the L speaker left the left third of the plan under Hebrew");
        verify(LanguageManager.setLanguage("en"));
    }

    function test_stopNeverTouchesTheDefault() {
        // quit() restores the default output this application moved and then
        // ends the process, which no test can call. The invariant it rests
        // on is asserted through stop() instead: what every suite calls in
        // init() and cleanup() must never move a developer's own default output.
        const window = createTemporaryObject(shell, testCase);
        verify(window);
        tryCompare(window, "visible", true);
        const wasNullSink = CrucibleController.defaultIsNullSink;
        const name = CrucibleController.defaultOutputName;
        CrucibleController.stop();
        compare(CrucibleController.defaultIsNullSink, wasNullSink);
        compare(CrucibleController.defaultOutputName, name);
    }
}
