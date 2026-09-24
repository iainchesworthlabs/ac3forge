import QtQuick
import QtTest
import Qt.labs.platform as Platform

import Ac3ForgeCrucible
import Ac3ForgeCrucibleTest

// The window's own chrome, clicked: the header's page switch, the status
// pill, the "?" button and the About and Licences dialogs behind it, the
// status strip's Start/Stop, and every entry of the tray menu - over a
// scripted machine, so the tray's default-output entries move the FAKE
// default device and can be pressed (ui/tests/qml_test_main.cpp).
//
// The tray menu is Qt Labs Platform's, which a headless session never
// shows, so each entry is fired the way the platform menu fires it: its
// triggered() signal, which is what runs the entry's own onTriggered. Quit
// is the one entry not fired, because it ends the process the suite runs in.
TestCase {
    id: testCase
    name: "ShellWorkflow"
    when: windowShown

    Component { id: shell; Main {} }

    function init() {
        CrucibleController.stop();
        CrucibleController.firstRunAcknowledged = true;
        CrucibleController.keepRunningWhenClosed = true;
        CrucibleController.moveDefaultOnLaunch = false;
        CrucibleController.pinned = "auto";
        CrucibleController.roomView = "2d";
        CrucibleController.theme = "system";
        CrucibleController.textScale = "100";
        CrucibleController.nullSinkName = "Desktop Atmos";
    }

    function cleanup() {
        CrucibleController.stop();
        CrucibleController.pinned = "auto";
        CrucibleController.textScale = "100";
        TestServices.clear();
        Theme.fontScale = 1.0;
    }

    function openShell(scriptedMachine) {
        if (scriptedMachine && !TestServices.scriptMachine([{ app: 900, name: "chrome", active: true }], ["avr", "realtek", "null"])) {
            skip("the scripted machine is not available in this harness");
        }
        const window = createTemporaryObject(shell, testCase);
        verify(window);
        tryCompare(window, "visible", true);
        waitForRendering(window.contentItem);
        return window;
    }

    function trayEntry(window, text) {
        const tray = findChild(window, "tray");
        verify(tray && tray.menu, "the tray and its menu");
        const items = tray.menu.items;
        for (let i = 0; i < items.length; ++i) {
            if (items[i].text === text) return items[i];
        }
        return null;
    }

    // --- the header -----------------------------------------------------------------

    function test_theHeaderSwitchesPagesAndThePillOpensTheSignalPath() {
        const window = openShell(false);
        const pages = findChild(window, "pageChoice");
        verify(pages);
        mouseClick(findChild(pages, "seg-settings"));
        compare(window.page, "settings");
        mouseClick(findChild(pages, "seg-room"));
        compare(window.page, "room");
        mouseClick(findChild(window, "signalPathPill"));
        compare(window.page, "output");
        compare(pages.currentValue, "output", "the switch follows");
    }

    function test_thePillSaysWhatIsHeardAndWhereApplicationsPlay() {
        const window = openShell(true);
        if (!CrucibleController.running) {
            skip("the engine did not run over the scripted machine: " + CrucibleController.lastError);
        }
        tryVerify(function() { return CrucibleController.endpointName === "AVR (HDMI)"; }, 5000);
        const pill = findChild(window, "signalPathPill");
        const label = function() { return pill.children[0].children[1].text; };
        // A real default: the warning form, then the hearing.
        tryVerify(function() { return label().indexOf("⚠") === 0 && label().indexOf("AVR (HDMI)") > 0; }, 3000, label());
        CrucibleController.moveDefaultToNullSink();
        tryVerify(function() { return label().indexOf("⚠") < 0 && label().indexOf("APPS →") === 0; }, 3000, label());
        CrucibleController.restoreDefault();
    }

    function test_aboutAndLicencesOpenAndCloseFromTheirButtons() {
        const window = openShell(false);
        mouseClick(findChild(window, "aboutButton"));
        tryCompare(window.aboutDialog, "opened", true);
        const about = window.aboutDialog.contentItem;
        // The version line is the build's own.
        verify(CrucibleController.versionDetails.length > 0);
        mouseClick(findChild(about, "aboutLicencesButton"));
        tryCompare(window.licencesDialog, "opened", true);
        const text = findChild(window.licencesDialog.contentItem, "licenceNoticesText");
        compare(text.text, CrucibleController.licenceNotices);
        mouseClick(findChild(window.licencesDialog.contentItem, "licencesCloseButton"));
        tryCompare(window.licencesDialog, "opened", false);
        verify(window.aboutDialog.opened, "About stays open under Licences");
        mouseClick(findChild(about, "aboutCloseButton"));
        tryCompare(window.aboutDialog, "opened", false);
    }

    // --- the status strip ---------------------------------------------------------------

    function test_theStatusStripStopsAndStartsTheEngine() {
        const window = openShell(true);
        if (!CrucibleController.running) {
            skip("the engine did not run over the scripted machine: " + CrucibleController.lastError);
        }
        const button = findChild(window, "engineButton");
        const status = findChild(window, "engineStatus");
        compare(button.text, "Stop");
        compare(status.text, "engine running");
        mouseClick(button);
        compare(CrucibleController.running, false);
        tryCompare(button, "text", "Start");
        tryCompare(status, "text", "engine stopped");
        mouseClick(button);
        compare(CrucibleController.running, true);
        tryCompare(button, "text", "Stop");
        tryVerify(function() { return CrucibleController.framesEncoded > 0; }, 5000, "and it encodes again");
    }

    // --- the text size ------------------------------------------------------------------

    function test_theTextSizeSettingScalesTheWholeWindow() {
        const window = openShell(false);
        compare(Theme.fontScale, 1.0);
        const title = findChild(window, "titleText");
        const small = title.font.pixelSize;
        CrucibleController.textScale = "175";
        tryCompare(Theme, "fontScale", 1.75);
        verify(title.font.pixelSize > small, "the title grew: " + title.font.pixelSize);
        CrucibleController.textScale = "100";
        tryCompare(Theme, "fontScale", 1.0);
    }

    // --- the tray ------------------------------------------------------------------------

    function test_theTrayMenusPinsFollowAndSetThePin() {
        const window = openShell(true);
        const heading = trayEntry(window, "Signal path · auto");
        verify(heading, "the heading reads the current pin");
        compare(heading.enabled, false);
        for (const entry of [["Atmos", "atmos"], ["Dolby Digital Plus 5.1", "ddplus"], ["Dolby Digital 5.1", "dd"],
                             ["PCM surround", "pcm"], ["Stereo", "stereo"], ["Automatic", "auto"]]) {
            const item = trayEntry(window, entry[0]);
            verify(item, "the tray offers " + entry[0]);
            item.triggered();
            compare(CrucibleController.pinned, entry[1], entry[0]);
            tryCompare(item, "checked", true);
        }
        // Headphones is offered exactly where the Signal path page offers it.
        const headphones = trayEntry(window, "Headphones");
        verify(headphones);
        compare(headphones.visible, CrucibleController.spatialAvailable);
        // And the pin reaches the engine: DD on the receiver.
        if (CrucibleController.running) {
            trayEntry(window, "Dolby Digital 5.1").triggered();
            tryCompare(CrucibleController, "modeKey", "dd", 5000);
            verify(trayEntry(window, "Signal path · dd"), "the heading follows");
        }
    }

    function test_theTrayMovesAndRestoresTheDefaultOutput() {
        const window = openShell(true);
        const move = trayEntry(window, "Move default output to Desktop Atmos");
        verify(move, "the tray offers the move");
        verify(move.enabled, "the scripted machine has a silent device");
        move.triggered();
        tryCompare(CrucibleController, "defaultIsNullSink", true, 3000);
        const now = trayEntry(window, "Default output: Speakers (Desktop Atmos)");
        verify(now, "the entry now says where applications play");
        compare(now.enabled, false);
        const restore = trayEntry(window, "Restore Speakers (Realtek)");
        verify(restore && restore.enabled, "and Restore names the previous output");
        restore.triggered();
        tryCompare(CrucibleController, "defaultIsNullSink", false, 3000);
        compare(CrucibleController.defaultOutputName, "Speakers (Realtek)");
    }

    function test_theTrayOpensTheRoomSettingsAndAbout() {
        const window = openShell(false);
        trayEntry(window, "Settings…").triggered();
        compare(window.page, "settings");
        compare(window.visible, true);
        trayEntry(window, "Open the room").triggered();
        compare(window.page, "room");
        trayEntry(window, "About…").triggered();
        tryCompare(window.aboutDialog, "opened", true);
        window.aboutDialog.close();
        tryCompare(window.aboutDialog, "opened", false);
        // The objects line reads the signing state, and is not a command.
        const objects = trayEntry(window, CrucibleController.objectsEnabled ? "Objects on · key loaded" : "Objects off · no key");
        verify(objects);
        compare(objects.enabled, false);
        verify(trayEntry(window, "Quit"), "Quit is there (and not pressed: it ends this process)");
    }

    function test_aHiddenWindowComesBackWhenTheTrayIconIsClicked() {
        const window = openShell(false);
        // Hidden the way a close hides it while it stays in the tray (a real
        // close on a session with no tray quits, which would end this run).
        window.hide();
        tryCompare(window, "visible", false);
        const tray = findChild(window, "tray");
        tray.activated(Platform.SystemTrayIcon.Trigger);
        tryCompare(window, "visible", true);
        window.hide();
        tryCompare(window, "visible", false);
        tray.activated(Platform.SystemTrayIcon.DoubleClick);
        tryCompare(window, "visible", true);
        // A context-menu press only opens the menu.
        window.hide();
        tray.activated(Platform.SystemTrayIcon.Context);
        wait(50);
        compare(window.visible, false);
    }
}
