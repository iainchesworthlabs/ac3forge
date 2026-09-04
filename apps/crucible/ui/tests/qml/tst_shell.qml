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
}
