import QtQuick
import QtQuick.Window
import QtTest

import Ac3ForgeCrucible
import Ac3ForgeCrucibleTest

// The window without a mouse: every control that can be pressed can be
// reached by Tab and pressed by Space or Return, the room's keys move the
// selected application, and a keys-only session gets an application out of
// the bed, across the room and back again.
//
// Everything under test is parented to testCase.parent, not to the TestCase
// itself: the TestCase item is invisible by design, an item that is not
// effectively visible cannot take active focus, and forceActiveFocus() on
// one silently does nothing (tst_settings.qml says the same about visible).
TestCase {
    id: testCase
    name: "Keyboard"
    when: windowShown
    width: 1480
    height: 820

    Component { id: spyComponent; SignalSpy {} }
    Component { id: roomPage; RoomPage { width: 1480; height: 700 } }
    Component { id: shell; Main {} }
    Component { id: aboutDialogComponent; AboutDialog {} }
    Component { id: settingsPage; SettingsPage { width: 1480; height: 700 } }
    Component { id: buttonComponent; CrucibleButton { width: 120; height: 30; text: "Press" } }
    Component { id: checkComponent; CrucibleCheck { width: 300; text: "Check" } }
    Component { id: chipComponent; BedChip {} }
    Component { id: keysComponent; RoomKeys { width: 400; height: 300 } }
    Component {
        id: segmentComponent
        SegmentedControl {
            property string chosen: "a"
            model: [{ label: "A", value: "a" }, { label: "B", value: "b" }, { label: "C", value: "c" }]
            currentValue: chosen
            onSelected: function(value) { chosen = value; }
        }
    }
    // A stand-in for an AppEntry: the same property names the views read,
    // and writable, so a case can put an application in the bed or in
    // full-screen and see what the keys do about it.
    Component {
        id: fakeAppComponent
        QtObject {
            property int app: 7
            property string name: "Chrome"
            property int slot: 0
            property int width: 1
            property real x: 0.5
            property real y: 0.5
            property real z: 0.0
            property real size: 0.0
            property bool fullscreen: false
            property bool silent: false
            property bool active: true
            property bool tapped: true
            property bool background: false
            property string imagePath: ""
            property string iconName: ""
            property string appId: ""
            property real level: -60
        }
    }

    function init() {
        CrucibleController.stop();
        CrucibleController.firstRunAcknowledged = true;
        CrucibleController.keepRunningWhenClosed = true;
        CrucibleController.moveDefaultOnLaunch = false;
        Theme.fontScale = 1.0;
    }

    function cleanup() {
        CrucibleController.stop();
        // The machine back, for the two seams the controller holds rather
        // than hands to the engine. A no-op unless this case scripted it.
        TestServices.clear();
        Theme.fontScale = 1.0;
    }

    // --- the room's keys ------------------------------------------------------

    function makeKeys(appProperties) {
        const app = createTemporaryObject(fakeAppComponent, testCase, appProperties);
        verify(app);
        const keys = createTemporaryObject(keysComponent, testCase.parent, { app: app });
        verify(keys);
        keys.forceActiveFocus();
        verify(keys.activeFocus, "the room's key scope takes active focus");
        return keys;
    }

    function test_roomKeysMoveTheSelectedApplicationWithArrowsAndModifiers() {
        const keys = makeKeys({});
        const moved = createTemporaryObject(spyComponent, testCase, { target: keys, signalName: "moved" });
        const said = createTemporaryObject(spyComponent, testCase, { target: keys, signalName: "announced" });

        keyClick(Qt.Key_Right);
        compare(moved.count, 1);
        compare(moved.signalArguments[0][0], 7);
        fuzzyCompare(moved.signalArguments[0][1], 0.55, 0.001);
        fuzzyCompare(moved.signalArguments[0][2], 0.5, 0.001);
        // The announcement is the same words the card and a reader get, for
        // the position that was just sent - not a second spelling of it.
        compare(said.count, 1);
        verify(said.signalArguments[0][0].indexOf(RoomWords.describe(0.55, 0.5, 0)) >= 0, said.signalArguments[0][0]);

        // Shift is the fine step, and it continues from where the last press
        // asked for rather than from the engine's not-yet-updated position.
        keyClick(Qt.Key_Right, Qt.ShiftModifier);
        fuzzyCompare(moved.signalArguments[1][1], 0.56, 0.001);
        // Control is the coarse step; y 0 is the front of the room.
        keyClick(Qt.Key_Up, Qt.ControlModifier);
        fuzzyCompare(moved.signalArguments[2][2], 0.25, 0.001);
        keyClick(Qt.Key_PageUp);
        fuzzyCompare(moved.signalArguments[3][3], 0.05, 0.001);

        // Twenty more presses do not walk out of the room.
        for (let i = 0; i < 20; ++i) {
            keyClick(Qt.Key_Right);
        }
        const last = moved.signalArguments[moved.count - 1];
        fuzzyCompare(last[1], 1.0, 0.001);

        keyClick(Qt.Key_Home);
        const home = moved.signalArguments[moved.count - 1];
        fuzzyCompare(home[1], 0.5, 0.001);
        fuzzyCompare(home[2], 0.5, 0.001);
        fuzzyCompare(home[3], 0.0, 0.001);
    }

    function test_roomKeysPlaceReturnAndResizeWithEnterDeleteAndPlusMinus() {
        const keys = makeKeys({ slot: -1 });
        const placed = createTemporaryObject(spyComponent, testCase, { target: keys, signalName: "placed" });
        const returned = createTemporaryObject(spyComponent, testCase, { target: keys, signalName: "returned" });
        const resized = createTemporaryObject(spyComponent, testCase, { target: keys, signalName: "resized" });

        keyClick(Qt.Key_Return);
        compare(placed.count, 1);
        compare(placed.signalArguments[0][0], 7);

        keys.app.slot = 0;
        keyClick(Qt.Key_Delete);
        compare(returned.count, 1);
        compare(returned.signalArguments[0][0], 7);

        keys.app.size = 0;
        keyClick(Qt.Key_Plus);
        compare(resized.count, 1);
        fuzzyCompare(resized.signalArguments[0][1], 0.05, 0.001);
        keys.app.size = 0.05;
        keyClick(Qt.Key_Minus);
        fuzzyCompare(resized.signalArguments[1][1], 0.0, 0.001);
    }

    function test_roomKeysRefuseAFullScreenApplicationAndSayWhy() {
        const keys = makeKeys({ fullscreen: true });
        const moved = createTemporaryObject(spyComponent, testCase, { target: keys, signalName: "moved" });
        const said = createTemporaryObject(spyComponent, testCase, { target: keys, signalName: "announced" });
        keyClick(Qt.Key_Right);
        compare(moved.count, 0, "a full-screen application is not moved");
        compare(said.count, 1);
        verify(said.signalArguments[0][0].indexOf(keys.app.name) >= 0, said.signalArguments[0][0]);
    }

    function test_roomKeysStartFromTheCentreForAnUnplacedApplication() {
        // In the bed, and its last position is off to the right: an arrow
        // puts it in the room from the centre, not from where it used to be.
        const keys = makeKeys({ slot: -1, x: 0.9 });
        const moved = createTemporaryObject(spyComponent, testCase, { target: keys, signalName: "moved" });
        keyClick(Qt.Key_Right);
        compare(moved.count, 1);
        fuzzyCompare(moved.signalArguments[0][1], 0.55, 0.001);
        fuzzyCompare(moved.signalArguments[0][2], 0.5, 0.001);
    }

    // --- the controls ---------------------------------------------------------

    function test_buttonsAndChecksActivateFromTheKeyboard() {
        const button = createTemporaryObject(buttonComponent, testCase.parent);
        verify(button);
        compare(button.activeFocusOnTab, true);
        button.forceActiveFocus();
        verify(button.activeFocus);
        const clicked = createTemporaryObject(spyComponent, testCase, { target: button, signalName: "clicked" });
        keyClick(Qt.Key_Space);
        compare(clicked.count, 1);
        keyClick(Qt.Key_Return);
        compare(clicked.count, 2);
        // Disabled: out of the tab chain, and deaf to the same keys.
        button.enabled = false;
        compare(button.activeFocusOnTab, false);
        keyClick(Qt.Key_Space);
        compare(clicked.count, 2);

        const check = createTemporaryObject(checkComponent, testCase.parent);
        verify(check);
        check.forceActiveFocus();
        verify(check.activeFocus);
        const toggled = createTemporaryObject(spyComponent, testCase, { target: check, signalName: "toggled" });
        keyClick(Qt.Key_Space);
        compare(toggled.count, 1);
        compare(toggled.signalArguments[0][0], true);
    }

    function test_segmentedControlMovesWithArrowHomeAndEnd() {
        const segments = createTemporaryObject(segmentComponent, testCase.parent);
        verify(segments);
        segments.forceActiveFocus();
        verify(segments.activeFocus);
        keyClick(Qt.Key_Right);
        compare(segments.chosen, "b");
        keyClick(Qt.Key_End);
        compare(segments.chosen, "c");
        keyClick(Qt.Key_Left);
        compare(segments.chosen, "b");
        keyClick(Qt.Key_Home);
        compare(segments.chosen, "a");
        // Left from the first wraps to the last, so a group of two toggles
        // with either arrow.
        keyClick(Qt.Key_Left);
        compare(segments.chosen, "c");
        keyClick(Qt.Key_Right);
        compare(segments.chosen, "a");
    }

    function test_bedChipPlacesOnEnter() {
        const app = createTemporaryObject(fakeAppComponent, testCase, { slot: -1 });
        const chip = createTemporaryObject(chipComponent, testCase.parent, { app: app });
        verify(chip);
        compare(chip.activeFocusOnTab, true);
        chip.forceActiveFocus();
        verify(chip.activeFocus);
        const place = createTemporaryObject(spyComponent, testCase, { target: chip, signalName: "place" });
        keyClick(Qt.Key_Return);
        compare(place.count, 1);
        // A full-screen application cannot leave the bed, so its chip is not
        // a tab stop and Enter does nothing.
        app.fullscreen = true;
        compare(chip.activeFocusOnTab, false);
        keyClick(Qt.Key_Return);
        compare(place.count, 1);
    }

    function test_focusRingShowsOnlyWithActiveFocus() {
        const first = createTemporaryObject(buttonComponent, testCase.parent);
        const second = createTemporaryObject(buttonComponent, testCase.parent);
        verify(first && second);
        const firstRing = findChild(first, "focusRing");
        const secondRing = findChild(second, "focusRing");
        verify(firstRing, "a button carries a focus ring");
        verify(secondRing);
        compare(firstRing.visible, false);
        first.forceActiveFocus();
        compare(firstRing.visible, true);
        compare(secondRing.visible, false);
        second.forceActiveFocus();
        compare(firstRing.visible, false);
        compare(secondRing.visible, true);
    }

    // The same rule on the other three shapes of control: a check, whose
    // ring is around the box rather than the note; a bed chip; and the room
    // scope, where the ring says the arrows are here.
    function test_focusRingIsOnEveryKindOfControl() {
        const check = createTemporaryObject(checkComponent, testCase.parent);
        const app = createTemporaryObject(fakeAppComponent, testCase, { slot: -1 });
        const chip = createTemporaryObject(chipComponent, testCase.parent, { app: app });
        const keys = createTemporaryObject(keysComponent, testCase.parent, { app: app });
        verify(check && chip && keys);
        const rings = [findChild(check, "focusRing"), findChild(chip, "focusRing"),
                       findChild(keys, "focusRing")];
        const owners = [check, chip, keys];
        for (let i = 0; i < owners.length; ++i) {
            verify(rings[i], "control " + i + " carries a focus ring");
            compare(rings[i].visible, false);
        }
        for (let i = 0; i < owners.length; ++i) {
            owners[i].forceActiveFocus();
            verify(owners[i].activeFocus);
            for (let j = 0; j < rings.length; ++j) {
                compare(rings[j].visible, i === j, "ring " + j + " with the focus on " + i);
            }
        }
    }

    function test_advancedDisclosureTogglesWithSpace() {
        const page = createTemporaryObject(settingsPage, testCase.parent);
        verify(page);
        waitForRendering(page);
        const toggle = findChild(page, "advancedToggle");
        const section = findChild(page, "advancedSection");
        verify(toggle, "the Advanced disclosure carries objectName advancedToggle");
        verify(section);
        compare(section.open, false);
        toggle.forceActiveFocus();
        verify(toggle.activeFocus);
        keyClick(Qt.Key_Space);
        compare(section.open, true);
        keyClick(Qt.Key_Space);
        compare(section.open, false);
    }

    function test_escapeClosesTheAboutDialog() {
        // The dialog on its own, in this window, rather than the shell in a
        // second window: a key goes to the active window, and which window
        // that is under the offscreen platform is not this test's subject.
        const about = createTemporaryObject(aboutDialogComponent, testCase.parent);
        verify(about);
        about.open();
        tryCompare(about, "opened", true);
        const close = findChild(about.contentItem, "aboutCloseButton");
        verify(close, "the Close button carries objectName aboutCloseButton");
        tryVerify(function() { return close.activeFocus; }, 2000, "Close takes focus as the dialog opens");
        keyClick(Qt.Key_Escape);
        tryCompare(about, "opened", false);
    }

    function test_ctrlNumberSwitchesPages() {
        const window = createTemporaryObject(shell, testCase);
        verify(window);
        tryCompare(window, "visible", true);
        window.requestActivate();
        if (!waitForActive(window)) {
            skip("the shell's window never became the active one here, so a window shortcut cannot be delivered to it");
        }
        compare(window.page, "room");
        keySequence("Ctrl+2");
        tryCompare(window, "page", "output");
        keySequence("Ctrl+3");
        tryCompare(window, "page", "settings");
        keySequence("Ctrl+1");
        tryCompare(window, "page", "room");
        // Out of the way before the next case: while the shell's window is
        // the active one, keys go there and not to this test's own window.
        window.visible = false;
        wait(50);
    }

    function waitForActive(window) {
        for (let i = 0; i < 20 && !window.active; ++i) {
            wait(50);
        }
        return window.active;
    }

    // --- the page, walked -----------------------------------------------------

    function test_tabWalksTheRoomPageInReadingOrder() {
        const page = createTemporaryObject(roomPage, testCase.parent);
        verify(page);
        waitForRendering(page);
        const list = findChild(page, "appList");
        const keys = findChild(page, "roomKeys");
        const choice = findChild(page, "roomViewChoice");
        verify(list, "the applications list carries objectName appList");
        verify(keys, "the room's key scope carries objectName roomKeys");
        list.forceActiveFocus();
        verify(list.activeFocus);
        // The documented order (docs/crucible/accessibility.md, "The focus
        // order"): the applications list, the room-view switch where the
        // build has the 3D picture, then the room. Collected as it is
        // walked, from the window's own activeFocusItem, so this asserts the
        // ORDER and the number of presses rather than only the arrival.
        verify(!CrucibleController.has3D || choice, "a 3D build carries the room-view switch");
        const wanted = CrucibleController.has3D ? ["roomViewChoice", "roomKeys"] : ["roomKeys"];
        const walked = [];
        for (let step = 0; step < wanted.length; ++step) {
            keyClick(Qt.Key_Tab);
            walked.push(page.Window.activeFocusItem ? page.Window.activeFocusItem.objectName : "");
        }
        compare(walked.join(" > "), wanted.join(" > "), "the room page in reading order");
        verify(keys.activeFocus, "Tab from the applications list reaches the room in " + wanted.length + " presses");
        // And back the way it came.
        keyClick(Qt.Key_Backtab);
        verify(!keys.activeFocus, "Shift+Tab leaves the room scope");
        compare(page.Window.activeFocusItem ? page.Window.activeFocusItem.objectName : "",
                CrucibleController.has3D ? "roomViewChoice" : "appList",
                "Shift+Tab returns to the item before it");
    }

    function test_textScaleGrowsTheControls() {
        const button = createTemporaryObject(buttonComponent, testCase.parent);
        const check = createTemporaryObject(checkComponent, testCase.parent);
        verify(button && check);
        const app = createTemporaryObject(fakeAppComponent, testCase, { slot: -1 });
        const chip = createTemporaryObject(chipComponent, testCase.parent, { app: app });
        verify(chip);
        waitForRendering(button);
        const smallButton = button.implicitHeight;
        const smallChip = chip.implicitHeight;
        verify(smallButton >= 30, "the mockup's height is the floor: " + smallButton);
        Theme.fontScale = 1.5;
        waitForRendering(button);
        verify(button.implicitHeight > smallButton, "the button grew: " + button.implicitHeight);
        verify(chip.implicitHeight >= smallChip, "the chip is no smaller: " + chip.implicitHeight);
        // The label still fits inside it, which is the point of deriving the
        // height rather than fixing it.
        verify(button.implicitHeight >= Theme.fontBody + 12,
               "the taller text fits: " + button.implicitHeight + " for " + Theme.fontBody + " px text");
        Theme.fontScale = 1.0;
        waitForRendering(button);
        compare(button.implicitHeight, smallButton);
    }

    // --- a keys-only session over a scripted machine ---------------------------

    function scriptedRoom() {
        if (!TestServices.scriptSessions([{ app: 900, name: "Chrome", active: true },
                                          { app: 901, name: "Steam", active: true }])) {
            return false;
        }
        CrucibleController.start();
        if (!CrucibleController.running) {
            return false;
        }
        // Waited for rather than asserted: a machine where the engine runs
        // but the scripted list never arrives is one to skip on, not to
        // fail on.
        for (let i = 0; i < 50 && CrucibleController.apps.length < 2; ++i) {
            wait(100);
        }
        return CrucibleController.apps.length >= 2;
    }

    function test_listSelectsWithArrowsAndEnterPlacesThenTheRoomTakesTheKeys() {
        if (!scriptedRoom()) {
            skip("the engine did not run over the scripted machine here: " + CrucibleController.lastError);
        }
        const page = createTemporaryObject(roomPage, testCase.parent);
        verify(page);
        waitForRendering(page);
        const list = findChild(page, "appList");
        const keys = findChild(page, "roomKeys");
        verify(list && keys);
        list.forceActiveFocus();
        verify(list.activeFocus);
        keyClick(Qt.Key_Down);
        const firstRow = page.indexOfApp(page.selectedApp);
        verify(firstRow >= 0, "Down chooses an application: " + page.selectedApp);
        keyClick(Qt.Key_Down);
        compare(page.indexOfApp(page.selectedApp),
                Math.min(firstRow + 1, CrucibleController.apps.length - 1),
                "Down moves to the next application");
        const chosen = page.selectedApp;

        // Enter places it in the centre and hands the keys to the room.
        keyClick(Qt.Key_Return);
        tryVerify(function() { return keys.activeFocus; }, 2000, "the room takes the keys");
        tryVerify(function() {
            const app = CrucibleController.apps.find(function(entry) { return entry.app === chosen; });
            return app && app.slot >= 0;
        }, 5000, "Enter placed it");

        // And the arrows move it, through the real controller.
        keyClick(Qt.Key_Right);
        tryVerify(function() {
            const app = CrucibleController.apps.find(function(entry) { return entry.app === chosen; });
            return app && Math.abs(app.x - 0.55) < 0.02;
        }, 5000, "Right moved it");

        // Delete puts it back in the bed.
        keyClick(Qt.Key_Delete);
        tryVerify(function() {
            const app = CrucibleController.apps.find(function(entry) { return entry.app === chosen; });
            return app && app.slot < 0;
        }, 5000, "Delete returned it");
    }

    // The rail re-sorts itself whenever an application starts, exits or goes
    // quiet, and a ListView answers a model whose value changed by putting
    // its own currentIndex back to 0. That must not choose an application:
    // the selection is what the room's arrow keys move, and it would walk to
    // the top of the list on its own while a person was placing something.
    // The write below is what the view itself does in that case.
    function test_theListRowFollowsTheSelectionAndNotTheOtherWayAbout() {
        if (!scriptedRoom()) {
            skip("the engine did not run over the scripted machine here: " + CrucibleController.lastError);
        }
        const page = createTemporaryObject(roomPage, testCase.parent);
        verify(page);
        waitForRendering(page);
        const list = findChild(page, "appList");
        verify(list);
        // Nothing is chosen until someone chooses it.
        compare(page.selectedApp, -1);
        compare(list.currentIndex, -1);

        list.forceActiveFocus();
        keyClick(Qt.Key_Down);
        const first = page.selectedApp;
        verify(first >= 0, "Down chooses an application");
        keyClick(Qt.Key_Down);
        const chosen = page.selectedApp;
        verify(chosen !== first, "a second Down moves to the next one");
        compare(list.currentIndex, page.indexOfApp(chosen), "the row followed the selection");

        const away = page.indexOfApp(chosen) === 0 ? 1 : 0;
        list.currentIndex = away;
        compare(page.selectedApp, chosen, "the view's own row does not choose an application");
        compare(list.currentIndex, page.indexOfApp(page.selectedApp),
                "and the row is put back where the selection is");
    }

    function test_sizeSliderRespondsToKeys() {
        if (!scriptedRoom()) {
            skip("the engine did not run over the scripted machine here: " + CrucibleController.lastError);
        }
        const page = createTemporaryObject(roomPage, testCase.parent);
        verify(page);
        waitForRendering(page);
        const chosen = CrucibleController.apps[0].app;
        page.selectedApp = chosen;
        CrucibleController.position(chosen, 0.5, 0.5, 0);
        tryVerify(function() { return page.selected && page.selected.slot >= 0; }, 5000);
        waitForRendering(page);
        const slider = findChild(page, "sizeSlider");
        verify(slider, "the size track carries objectName sizeSlider");
        compare(slider.minimumValue, 0);
        compare(slider.maximumValue, 1);
        fuzzyCompare(slider.stepSize, 0.05, 0.001);
        slider.forceActiveFocus();
        verify(slider.activeFocus);
        keyClick(Qt.Key_Right);
        tryVerify(function() { return page.selected && Math.abs(page.selected.size - 0.05) < 0.001; }, 5000,
                  "Right grew the object");
        keyClick(Qt.Key_End);
        tryVerify(function() { return page.selected && Math.abs(page.selected.size - 1) < 0.001; }, 5000);
        keyClick(Qt.Key_Home);
        tryVerify(function() { return page.selected && page.selected.size === 0; }, 5000);
    }
}
