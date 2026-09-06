import QtQuick
import QtTest

import Ac3Forge

// The window without a mouse, and the window at a larger text size.
//
// Both used to be claimed rather than true: Theme.qml said every size came
// from its scale and that "a literal pixelSize in a view is a size that
// cannot follow the person's text-size setting, so there are none", while
// ac3gui held 377 of them and offered no text-size setting at all; and the
// chips a person has to press to choose a bed, a low-frequency count or a
// tab were Rectangles with a MouseArea over them and no way in from the
// keyboard. This suite is what stops either drifting back.
//
// The assertions here are on the properties that MAKE the window operable -
// the tab chain, the ring, the names - rather than on synthesised key events.
// The controls under test live several layouts deep inside an
// ApplicationWindow rather than being separately constructible (Crucible's
// own tst_keyboard.qml instantiates its components into the TestCase's
// window and can therefore keyClick them), and an item on a StackLayout page
// that is not current cannot take active focus, so a focus-driven case here
// would be testing the harness rather than the window.
TestCase {
    id: testCase
    name: "Keyboard"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    // Theme is a process-wide singleton and fontScale is writable, so a case
    // that moves it puts it back: a later case reading a 175% window would
    // fail for a reason that has nothing to do with it.
    function init() {
        Theme.fontScale = 1.0;
    }

    function cleanup() {
        Theme.fontScale = 1.0;
        EncoderController.atmosEnabled = false;
    }

    // The workbench is behind the first-run screen until a source has ever
    // been chosen, and the tabbed panel is behind Guided - neither destroys
    // its delegates, so the chips exist and their bindings hold whether or
    // not they are on screen (the same "still there, just hidden" behaviour
    // tst_format_channels.qml already relies on).
    function openWorkbench() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        win.everHadSource = true;
        win.tier = "expert";
        return win;
    }

    function findByName(win, name) {
        let found = null;
        tryVerify(() => {
            found = findChild(win.contentItem, name);
            return found !== null;
        }, 5000, "no item called " + name);
        return found;
    }

    // ---- the text size setting ---------------------------------------------

    function test_theScaleMovesEverySizeInTheType() {
        compare(Theme.fontScale, 1.0);
        const before = [Theme.fontTitle, Theme.fontHeading, Theme.fontNormal, Theme.fontBody,
                        Theme.fontSmall, Theme.fontMono, Theme.fontMicro, Theme.fontFine];
        Theme.fontScale = 1.5;
        const after = [Theme.fontTitle, Theme.fontHeading, Theme.fontNormal, Theme.fontBody,
                       Theme.fontSmall, Theme.fontMono, Theme.fontMicro, Theme.fontFine];
        for (let i = 0; i < before.length; i++) {
            verify(after[i] > before[i],
                   "rung " + i + " did not follow the scale: " + before[i] + " to " + after[i]);
        }
        // The arithmetic itself, so a rung cannot quietly stop being a
        // multiple of the scale.
        compare(Theme.fontBody, Math.round(13 * 1.5));
        compare(Theme.fontFine, Math.round(9 * 1.5));
    }

    function test_textSizeSettingReachesTheTheme_data() {
        return [
            { tag: "100%", choice: "100", scale: 1.0 },
            { tag: "125%", choice: "125", scale: 1.25 },
            { tag: "150%", choice: "150", scale: 1.5 },
            { tag: "175%", choice: "175", scale: 1.75 },
            // The store is a plain string nothing validates on the way in.
            // A hand-edited one must not leave fontScale as NaN and blank
            // every label in the window.
            { tag: "nonsense", choice: "enormous", scale: 1.0 },
            { tag: "out of range", choice: "900", scale: 1.0 },
            { tag: "empty", choice: "", scale: 1.0 },
        ];
    }

    function test_textSizeSettingReachesTheTheme(data) {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        win.settings.textScale = data.choice;
        win.applyTextScale();
        compare(Theme.fontScale, data.scale);
        // Put the store back: it outlives this window (see qml_test_main.cpp
        // on why the settings store persists here rather than living in memory).
        win.settings.textScale = "100";
        win.applyTextScale();
    }

    function test_systemTextSizeReadsThePlatformsOwnPointSize() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        win.settings.textScale = "system";
        win.applyTextScale();
        // 9 pt is 100%, and the floor is 100% - the window never starts
        // SMALLER than it is drawn at, whatever the desktop reports. A
        // platform reporting a pixel size instead has no point size to read
        // and stays at 1.0, which is the same assertion.
        verify(Theme.fontScale >= 1.0, "scale was " + Theme.fontScale);
        verify(Theme.fontScale <= 2.0, "scale was " + Theme.fontScale);
        win.settings.textScale = "100";
        win.applyTextScale();
        compare(Theme.fontScale, 1.0);
    }

    // A label in the window itself, not a token read back from the theme:
    // the sweep that put 377 literal sizes onto the scale is only worth
    // anything if the window's own Texts actually bind to it.
    function test_aLabelInTheWindowFollowsTheScale() {
        const win = openWorkbench();
        const note = findByName(win, "noteBedAlways");
        compare(note.font.pixelSize, Theme.fontMono);
        Theme.fontScale = 1.75;
        compare(note.font.pixelSize, Theme.fontMono);
        verify(Theme.fontMono > 11, "the scale did not move the rung this label reads");
    }

    // ---- the tab chain -----------------------------------------------------

    function test_theHandDrawnControlsAreTabStopsWithARing_data() {
        return [
            { tag: "bed", name: "bed-3/2" },
            { tag: "dual bed", name: "bed-1+1" },
            { tag: "no low frequency", name: "lfeCount-0" },
            { tag: "one low frequency", name: "lfeCount-1" },
            { tag: "format tab", name: "tab-format" },
            { tag: "command line chip", name: "commandBar" },
        ];
    }

    function test_theHandDrawnControlsAreTabStopsWithARing(data) {
        const win = openWorkbench();
        const control = findByName(win, data.name);
        verify(control.activeFocusOnTab, data.name + " is not in the tab chain");
        // The ring is FocusRing.qml's own objectName, so this asserts the
        // shared component is there rather than that something was drawn.
        verify(findChild(control, "focusRing") !== null, data.name + " has no focus ring");
        verify(control.Accessible.name.length > 0, data.name + " has no accessible name");
    }

    // A control that cannot be pressed must not be a tab stop either, or Tab
    // walks a person into something that ignores them. Object mode locks the
    // bed and the low-frequency count, which is the state that says so.
    function test_aLockedChipLeavesTheTabChain() {
        const win = openWorkbench();
        const bed = findByName(win, "bed-3/2");
        const lfe = findByName(win, "lfeCount-2");
        EncoderController.atmosEnabled = false;
        verify(bed.activeFocusOnTab);

        EncoderController.atmosEnabled = true;
        compare(bed.activeFocusOnTab, false);
        compare(bed.Accessible.focusable, false);
        compare(lfe.activeFocusOnTab, false);

        EncoderController.atmosEnabled = false;
        verify(bed.activeFocusOnTab);
        compare(bed.Accessible.focusable, true);
    }

    // ---- names that follow their data --------------------------------------
    // Same discipline as tst_accessibility.qml: change the BACKING DATA and
    // assert the accessible text changes with it, never compare against a
    // literal typed into the test.

    function test_theExtrasCheckboxTakesItsNameFromTheModel() {
        const win = openWorkbench();
        const model = EncoderController.extrasModel;
        verify(model.length > 1);
        const first = findByName(win, "extra-" + model[0].id);
        const second = findByName(win, "extra-" + model[1].id);
        compare(first.Accessible.name, model[0].label);
        compare(second.Accessible.name, model[1].label);
        verify(first.Accessible.name !== second.Accessible.name,
               "two extras cannot share one name - that is the bug this covers");
        // The description carries the channel tokens the row prints, so a
        // reader hears which channels an extra actually adds.
        verify(first.Accessible.description.indexOf(model[0].tokens) >= 0,
               first.Accessible.description);
    }

    // A segmented control reports Accessible.Grouping, and a group with no
    // name is announced as an unnamed group of radio buttons - the segments
    // are named, the thing they are choosing between is not. Every instance
    // in the window carries one; these are the ones reachable by objectName
    // from the window itself (Preferences' four live on the Overlay). The
    // four in the Guided wizard bind theirs to the Text in the column beside
    // them, so the assertion that matters is that they are there and that
    // they differ - four identical names would be no better than none.
    function test_everyNamedSegmentedGroupHasItsOwnName() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        const names = [];
        // Not `name` or `objectName` for the loop variable: both are
        // properties of this TestCase, and a local of either spelling reads
        // as a shadow rather than as a string.
        for (const groupName of ["tierChoice", "inputModeChoice", "roomFronts", "roomCentre",
                                 "roomSurround", "roomSubs"]) {
            const group = findByName(win, groupName);
            verify(group.Accessible.name.length > 0, groupName + " is an unnamed group");
            names.push(group.Accessible.name);
        }
        for (let i = 0; i < names.length; i++) {
            for (let j = i + 1; j < names.length; j++) {
                verify(names[i] !== names[j],
                       "two groups share the name \"" + names[i] + "\"");
            }
        }
    }

    function test_theBedChipsNameFollowsTheChoiceItDraws() {
        const win = openWorkbench();
        const choices = EncoderController.bedChoices;
        verify(choices.length > 2);
        const plain = findByName(win, "bed-" + choices[choices.length - 1].id);
        const dual = findByName(win, "bed-1+1");
        compare(plain.Accessible.name, choices[choices.length - 1].id);
        verify(dual.Accessible.name !== plain.Accessible.name);
        compare(plain.Accessible.role, Accessible.Button);
        compare(plain.Accessible.checkable, true);
    }
}
