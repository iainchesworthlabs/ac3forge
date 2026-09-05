import QtQuick
import QtTest

import Ac3ForgeCrucible

// The window itself: it comes up, switches pages, applies the persisted
// theme, and closing it hides rather than quits while "keep running in the
// tray" is on. Main.qml's Component.onCompleted starts the engine; on a
// machine with no audio endpoint that start refuses and the status strip
// says so, which is a state the shell has to render too.
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
        // Seen already, so the first-run dialog does not sit over the shell
        // cases; tst_firstrun.qml is where it is exercised.
        CrucibleController.firstRunAcknowledged = true;
    }

    function cleanup() {
        CrucibleController.stop();
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
