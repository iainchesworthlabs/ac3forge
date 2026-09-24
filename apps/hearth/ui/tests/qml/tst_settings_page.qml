import QtQuick
import QtTest

import Ac3ForgeHearth
import Ac3ForgeHearthLanguage
import Ac3ForgeHearthTest

import "HearthTestHelpers.js" as H

// Settings.qml driven from its own controls (FEATURE_COVERAGE.md rows 78-86):
// every checkbox, segmented control and field writes through to
// HearthController and persists, and Save diagnostics... writes the report
// through the page's own save dialog. The pairing-records list and its
// Forget are exercised in tst_network_pairing.qml, where a real pairing
// makes a record to forget.
//
// Settings are persisted to the suite's own throwaway QSettings
// (qml_test_main.cpp), and every case puts back what it changed.
TestCase {
    id: testCase
    name: "SettingsPage"
    when: windowShown
    width: 1200
    height: 1600

    Component { id: settingsComponent; Settings { width: 1200; height: 1600 } }

    function initTestCase() {
        HearthController.start();
    }

    function makePage() {
        const page = createTemporaryObject(settingsComponent, testCase.parent);
        verify(page !== null);
        waitForRendering(page);
        return page;
    }

    function click(item, what) {
        verify(item !== null, "no control for " + what);
        mouseClick(item);
    }

    function test_playbackCheckboxesAndFailurePolicy() {
        const page = makePage();
        const gaplessBefore = HearthController.gapless;
        click(findChild(page, "settingsGapless"), "Gapless between items");
        tryCompare(HearthController, "gapless", !gaplessBefore, 5000);
        compare(findChild(page, "settingsGapless").checked, !gaplessBefore);
        click(findChild(page, "settingsGapless"), "Gapless between items");
        tryCompare(HearthController, "gapless", gaplessBefore, 5000);

        const resumeBefore = HearthController.resumeQueue;
        click(findChild(page, "settingsResumeQueue"), "Pick up the queue");
        tryCompare(HearthController, "resumeQueue", !resumeBefore, 5000);
        click(findChild(page, "settingsResumeQueue"), "Pick up the queue");
        tryCompare(HearthController, "resumeQueue", resumeBefore, 5000);

        click(H.segment(page, "When an item fails", "stop"), "Stop on failure");
        tryCompare(HearthController, "onFailure", "stop", 5000);
        click(H.segment(page, "When an item fails", "skip"), "Skip on failure");
        tryCompare(HearthController, "onFailure", "skip", 5000);
    }

    function test_networkNameAndDiscovery() {
        const page = makePage();
        const before = HearthController.networkName;
        const field = findChild(page, "settingsNetworkName");
        verify(field !== null);
        mouseClick(field);
        field.selectAll();
        const typed = "Den";
        for (let i = 0; i < typed.length; ++i) {
            keyClick(typed.charAt(i));
        }
        keyClick(Qt.Key_Return);
        tryCompare(HearthController, "networkName", "Den", 5000);

        const discoverBefore = HearthController.networkDiscover;
        click(findChild(page, "settingsNetworkDiscover"), "Look for Sendspin players");
        tryCompare(HearthController, "networkDiscover", !discoverBefore, 5000);
        click(findChild(page, "settingsNetworkDiscover"), "Look for Sendspin players");
        tryCompare(HearthController, "networkDiscover", discoverBefore, 5000);
        HearthController.networkName = before;
    }

    function test_appearanceThemePaletteAndTextSize() {
        const page = makePage();
        click(H.segment(page, "Theme", "dark"), "Dark");
        tryCompare(HearthController, "theme", "dark", 5000);
        click(H.segment(page, "Palette", "ink"), "Ink");
        tryCompare(HearthController, "palette", "ink", 5000);
        click(H.segment(page, "Text size", "125"), "125%");
        tryCompare(HearthController, "textScale", "125", 5000);
        compare(H.segment(page, "Text size", "125").Accessible.checked, true);

        click(H.segment(page, "Theme", "system"), "System theme");
        tryCompare(HearthController, "theme", "system", 5000);
        click(H.segment(page, "Palette", "signal"), "Signal");
        tryCompare(HearthController, "palette", "signal", 5000);
        click(H.segment(page, "Text size", "100"), "100%");
        tryCompare(HearthController, "textScale", "100", 5000);
    }

    // Down on the focused language box picks the first real language, as
    // choosing it from the list does (ComboBox emits activated either way);
    // Up goes back to System.
    function test_languageChoice() {
        const page = makePage();
        const combo = findChild(page, "settingsLanguage");
        verify(combo !== null);
        if (combo.count < 2) {
            skip("this build lists no language beyond System");
        }
        const before = LanguageManager.currentLanguage;
        let target = 1;
        while (target < combo.count && combo.valueAt(target) === before) {
            ++target;
        }
        if (target >= combo.count) {
            skip("every listed language is the one already in use");
        }
        combo.forceActiveFocus();
        for (let i = 0; i < target; ++i) {
            keyClick(Qt.Key_Down);
        }
        tryCompare(LanguageManager, "currentLanguage", combo.valueAt(target), 5000);
        // Home goes back to the first entry, System.
        keyClick(Qt.Key_Home);
        tryCompare(LanguageManager, "currentLanguage", before, 5000);
    }

    // Save diagnostics... opens a save dialog pre-filled with the suggested
    // file (HearthController.suggestedDiagnosticsFile(), in the Documents
    // folder - qml_test_main.cpp's QStandardPaths test mode points that at a
    // throwaway folder); accepting it writes the report there and the page
    // says so.
    function test_saveDiagnosticsWritesTheReport() {
        const page = makePage();
        const dialog = H.dialog(page, "Save diagnostics", TestServices);
        verify(dialog !== null, "Settings declares no \"Save diagnostics\" dialog");
        click(findChild(page, "settingsDiagnosticsButton"), "Save diagnostics...");
        tryVerify(function() { return dialog.visible; }, 5000, "Save diagnostics... did not open the dialog");
        // Pre-filled with the suggested name (timestamped, so read back off
        // the dialog rather than asked for again a second later).
        const suggested = dialog.selectedFile.toString();
        verify(suggested.indexOf("hearth-diagnostics-") >= 0 && suggested.indexOf(".txt") > 0,
               "the dialog was not pre-filled with the suggested file: " + suggested);
        dialog.accept();
        tryVerify(function() { return !dialog.visible; }, 5000);

        tryVerify(function() { return HearthController.diagnosticsMessage.length > 0; }, 5000,
                  "the export said nothing");
        const message = findChild(page, "settingsDiagnosticsMessage");
        tryVerify(function() { return message.visible && message.text === HearthController.diagnosticsMessage; }, 5000);
        const written = TestServices.readTextFile(suggested);
        verify(written.length > 0, "no diagnostics file at " + suggested + " (" + HearthController.diagnosticsMessage + ")");
        // The same report the controller composes: its first line.
        compare(written.split("\n")[0], HearthController.diagnosticsReport().split("\n")[0]);
        verify(TestServices.removeFile(suggested));
    }
}
