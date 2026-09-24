import QtQuick
import QtTest

import Ac3ForgeHearth
import Ac3ForgeHearthTest

import "HearthTestHelpers.js" as H

// Main.qml's own shell (FEATURE_COVERAGE.md rows 1-8): the header's page
// switch, the Ctrl+1..Ctrl+6 / F1 / Escape shortcuts, the "?" button and
// the output summary's keyboard activation.
//
// Main.qml is an ApplicationWindow of its own, and TestCase.keyClick()
// always delivers to the test case's window, never a second top-level one -
// so every key here goes through TestServices.keyClick(win, ...), which
// sends it to Main's window the way a real key press arrives (a
// ShortcutOverride first, so its Shortcuts fire). Qt Quick only matches a
// window's Shortcuts while it is the focus window, so each case activates it
// first.
TestCase {
    id: testCase
    name: "Shell"
    when: windowShown

    Component { id: mainComponent; Main { } }

    function initTestCase() {
        verify(TestServices.useFakeRoom(), "the fake device room could not be installed");
        HearthController.firstRunSeen = true;
    }

    function openWindow() {
        const win = createTemporaryObject(mainComponent, testCase);
        verify(win !== null);
        tryCompare(win, "visible", true);
        waitForRendering(win.contentItem);
        win.requestActivate();
        tryVerify(function() { return win.active; }, 5000, "Main's window never became the active window");
        return win;
    }

    function key(win, k, modifiers) {
        verify(TestServices.keyClick(win, k, modifiers === undefined ? Qt.NoModifier : modifiers),
               "the key could not be delivered to Main's window");
    }

    function test_headerPageSwitchShowsEachPage() {
        const win = openWindow();
        compare(win.page, "play");
        const pages = ["media", "speakers", "decoder", "network", "settings", "play"];
        for (let i = 0; i < pages.length; ++i) {
            const cell = H.segment(win.header, "Page", pages[i]);
            verify(cell !== null, "no header cell for " + pages[i]);
            mouseClick(cell);
            compare(win.page, pages[i]);
            compare(cell.Accessible.checked, true);
        }
    }

    function test_ctrlDigitShortcutsSwitchPages() {
        const win = openWindow();
        const keys = [Qt.Key_2, Qt.Key_3, Qt.Key_4, Qt.Key_5, Qt.Key_6, Qt.Key_1];
        const pages = ["media", "speakers", "decoder", "network", "settings", "play"];
        for (let i = 0; i < keys.length; ++i) {
            key(win, keys[i], Qt.ControlModifier);
            tryCompare(win, "page", pages[i], 2000);
        }
    }

    function test_f1AndTheHelpButtonOpenKeyboardShortcuts() {
        const win = openWindow();
        const dialog = findChild(win, "shortcutsDialog");
        verify(dialog !== null);
        key(win, Qt.Key_F1);
        tryVerify(function() { return dialog.opened; }, 5000, "F1 did not open Keyboard shortcuts");
        mouseClick(findChild(win, "shortcutsCloseButton"));
        tryVerify(function() { return !dialog.visible; }, 5000);

        mouseClick(findChild(win, "helpButton"));
        tryVerify(function() { return dialog.opened; }, 5000, "the ? button did not open Keyboard shortcuts");
        mouseClick(findChild(win, "shortcutsCloseButton"));
        tryVerify(function() { return !dialog.visible; }, 5000);
    }

    // Escape stops the identify tone wherever it was started from, on any
    // page (Main.qml's own Shortcut comment).
    function test_escapeStopsTheIdentifyTone() {
        const win = openWindow();
        key(win, Qt.Key_3, Qt.ControlModifier);
        tryCompare(win, "page", "speakers", 2000);
        // A Repeater delegate on the Speakers page: found through the visual
        // tree, which findChild() on a Window does not walk.
        const identify = H.find(win.contentItem, function(item) { return item.objectName === "speakersIdentify-0"; });
        verify(identify !== null, "no identify button for the first speaker");
        // The button may sit below the fold of this 800-pixel window, where a
        // click cannot reach it; Space on the focused button starts it the
        // same way (Speakers.qml's own Keys.onSpacePressed).
        identify.forceActiveFocus();
        tryVerify(function() { return identify.activeFocus; }, 2000);
        key(win, Qt.Key_Space);
        tryCompare(HearthController, "identifySlot", 0, 10000);
        key(win, Qt.Key_1, Qt.ControlModifier);
        tryCompare(win, "page", "play", 2000);
        key(win, Qt.Key_Escape);
        tryCompare(HearthController, "identifySlot", -1, 10000);
    }

    // The output summary is a focusable control: Space opens the picker as
    // a click does (tst_output_picker.qml covers the click and the picker).
    function test_outputSummaryOpensThePickerFromTheKeyboard() {
        const win = openWindow();
        const summary = findChild(win, "outputSummaryButton");
        const picker = findChild(win, "outputPicker");
        summary.forceActiveFocus();
        tryVerify(function() { return summary.activeFocus; }, 2000);
        key(win, Qt.Key_Space);
        tryVerify(function() { return picker.opened; }, 5000, "Space on the summary did not open the picker");
        mouseClick(findChild(win, "outputPickerCancel"));
        tryVerify(function() { return !picker.visible; }, 5000);
    }

    // The appearance settings reach Theme at start and on every change.
    function test_themePaletteAndTextSizeFollowTheSettings() {
        HearthController.theme = "dark";
        HearthController.palette = "console";
        HearthController.textScale = "150";
        const win = openWindow();
        compare(Theme.preference, "dark");
        compare(Theme.paletteChoice, "console");
        compare(Theme.fontScale, 1.5);
        HearthController.theme = "light";
        HearthController.palette = "signal";
        HearthController.textScale = "100";
        tryCompare(Theme, "preference", "light", 2000);
        tryCompare(Theme, "paletteChoice", "signal", 2000);
        tryCompare(Theme, "fontScale", 1.0, 2000);
        HearthController.theme = "system";
    }
}
