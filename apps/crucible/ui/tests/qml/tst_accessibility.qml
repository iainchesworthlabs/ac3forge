import QtQuick
import QtTest

import Ac3ForgeCrucible
import Ac3ForgeCrucibleTest

// What a screen reader is told: a role, a name and a description for every
// part of the window a person acts on, all of it composed from the same live
// data the view draws itself from - never a second, hand-typed copy, which
// is what lets the two drift apart without a test noticing.
//
// No case here types a sentence the window is supposed to say. Each one
// compares what Accessible reports against the property the visible thing is
// bound to, so a change of wording changes both together.
TestCase {
    id: testCase
    name: "Accessibility"
    when: windowShown
    width: 1480
    height: 820

    Component { id: spyComponent; SignalSpy {} }
    Component { id: roomView; RoomView { width: 400; height: 400 } }
    Component { id: signalPath; SignalPath { width: 320 } }
    Component { id: appRowComponent; AppRow { width: 300 } }
    Component { id: chipComponent; BedChip {} }
    Component { id: roomPage; RoomPage { width: 1480; height: 700 } }
    Component { id: settingsPage; SettingsPage { width: 1480; height: 700 } }
    Component { id: outputPage; OutputPage { width: 1480; height: 700 } }
    Component { id: shell; Main {} }
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
    }

    function cleanup() {
        CrucibleController.stop();
        Theme.preference = "system";
        Theme.paletteChoice = "signal";
        Theme.fontScale = 1.0;
    }

    // --- the room -------------------------------------------------------------

    function test_markerDescribesItsPositionFromLiveData() {
        const placed = createTemporaryObject(fakeAppComponent, testCase, { app: 7, name: "Chrome", slot: 0, x: 0.2, y: 0.2 });
        const inBed = createTemporaryObject(fakeAppComponent, testCase, { app: 8, name: "Steam", slot: -1 });
        verify(placed && inBed);
        const view = createTemporaryObject(roomView, testCase.parent, { apps: [placed, inBed], selectedApp: 7 });
        verify(view);
        waitForRendering(view);

        const marker = findChild(view, "marker-7");
        verify(marker, "a placed application has a marker named for it");
        compare(marker.Accessible.role, Accessible.Button);
        compare(marker.Accessible.name, placed.name);
        compare(marker.Accessible.description,
                RoomWords.describe(0.2, 0.2, 0) + ", " + RoomWords.coords(0.2, 0.2, 0));
        compare(marker.Accessible.selected, true);
        // It follows the engine, because it reads the same entry the marker
        // is drawn from.
        placed.x = 0.9;
        compare(marker.Accessible.description,
                RoomWords.describe(0.9, 0.2, 0) + ", " + RoomWords.coords(0.9, 0.2, 0));
        verify(marker.Accessible.description.indexOf(RoomWords.describe(0.9, 0.2, 0)) >= 0);
        // The one in the bed is not in the room.
        const bedMarker = findChild(view, "marker-8");
        verify(bedMarker);
        compare(bedMarker.visible, false);
        // And selection is reported, not just drawn.
        view.selectedApp = 8;
        compare(marker.Accessible.selected, false);
    }

    function test_appRowExposesSelectionAndDetail() {
        const app = createTemporaryObject(fakeAppComponent, testCase, { slot: 0 });
        const row = createTemporaryObject(appRowComponent, testCase.parent, { app: app, selected: true });
        verify(row);
        waitForRendering(row);
        compare(row.Accessible.role, Accessible.ListItem);
        compare(row.Accessible.name, app.name);
        compare(row.Accessible.description, row.detail);
        compare(row.Accessible.selected, true);
        // The detail follows the entry: back to the bed, and it says so.
        const placedDetail = row.detail;
        app.slot = -1;
        verify(row.detail !== placedDetail, "the detail changed with the slot");
        compare(row.Accessible.description, row.detail);
        // The level moves every 60 ms and is deliberately not in either
        // string: a reader would be flooded.
        const before = row.Accessible.name + row.Accessible.description;
        app.level = -12;
        compare(row.Accessible.name + row.Accessible.description, before);
    }

    function test_bedChipIsAButtonNamedForItsApplication() {
        const app = createTemporaryObject(fakeAppComponent, testCase, { slot: -1 });
        const chip = createTemporaryObject(chipComponent, testCase.parent, { app: app });
        verify(chip);
        compare(chip.Accessible.role, Accessible.Button);
        verify(chip.Accessible.name.indexOf(app.name) >= 0, chip.Accessible.name);
        const loose = chip.Accessible.description;
        app.fullscreen = true;
        verify(chip.Accessible.description !== loose, "the description says a full-screen application stays put");
    }

    // --- the signal path ------------------------------------------------------

    function test_stationsAreNamedGroupsAndCarryTheirWarning() {
        // Stopped, so the middle station is the one warning.
        const path = createTemporaryObject(signalPath, testCase.parent);
        verify(path);
        waitForRendering(path);
        const station = findChild(path, "station-crucible");
        verify(station, "the middle station carries objectName station-crucible");
        compare(station.Accessible.role, Accessible.Grouping);
        // The name is the station's own kicker and title, so a reader can
        // tell which stage of the path it has reached.
        verify(station.Accessible.name.indexOf(station.kicker) >= 0, station.Accessible.name);
        verify(station.Accessible.name.indexOf(station.title) >= 0, station.Accessible.name);
        // The warning belongs to this station rather than floating loose:
        // it is in the station's own description, with the detail after it.
        compare(station.warn, true);
        verify(station.detail.length > 0, "the station composes its visible lines");
        verify(station.Accessible.description.indexOf(station.detail) > 0,
               "the warning comes first, then the detail: " + station.Accessible.description);

        // The first station's detail follows the machine's own answer about
        // where applications play, whatever that is here.
        const apps = findChild(path, "station-apps");
        verify(apps);
        compare(apps.warn, !CrucibleController.defaultIsNullSink || !CrucibleController.nullSinkPresent);
        verify(apps.Accessible.name.indexOf(apps.title) >= 0, apps.Accessible.name);
        verify(apps.detail.length > 0);
    }

    // --- names on the things that had none ------------------------------------

    function test_listAndControlsCarryNames() {
        const room = createTemporaryObject(roomPage, testCase.parent);
        verify(room);
        waitForRendering(room);
        const list = findChild(room, "appList");
        verify(list);
        compare(list.Accessible.role, Accessible.List);
        verify(list.Accessible.name.length > 0, "the applications list is named");

        const settings = createTemporaryObject(settingsPage, testCase.parent);
        verify(settings);
        waitForRendering(settings);
        const bitrate = findChild(settings, "bitrateBox");
        const language = findChild(settings, "languageBox");
        verify(bitrate && language);
        verify(bitrate.Accessible.name.length > 0, "the bitrate choice is named");
        verify(language.Accessible.name.length > 0, "the language choice is named");
        // The field's name is the row's own label, not a second copy of it.
        const advanced = findChild(settings, "advancedSection");
        verify(advanced);
        advanced.open = true;
        waitForRendering(settings);
        const folderRow = findChild(settings, "driverFolderRow");
        const folderInput = findChild(settings, "driverFolderInput");
        verify(folderRow && folderInput);
        compare(folderInput.Accessible.name, folderRow.label);
        // And the disclosure says which way it is.
        const toggle = findChild(settings, "advancedToggle");
        verify(toggle);
        compare(toggle.Accessible.role, Accessible.Button);
        const expanded = toggle.Accessible.description;
        advanced.open = false;
        verify(toggle.Accessible.description !== expanded, "expanded and collapsed read differently");

        const output = createTemporaryObject(outputPage, testCase.parent);
        verify(output);
        waitForRendering(output);
        const pin = findChild(output, "pinBox");
        verify(pin);
        verify(pin.Accessible.name.length > 0, "the pin choice is named");
    }

    function test_sizeSliderReportsARange() {
        const page = createTemporaryObject(roomPage, testCase.parent);
        verify(page);
        waitForRendering(page);
        const slider = findChild(page, "sizeSlider");
        verify(slider, "the size track carries objectName sizeSlider");
        compare(slider.Accessible.role, Accessible.Slider);
        verify(slider.Accessible.name.length > 0);
        compare(slider.minimumValue, 0);
        compare(slider.maximumValue, 1);
        fuzzyCompare(slider.stepSize, 0.05, 0.001);
        // With nothing selected there is no value to report, and the slider
        // is out of the tab chain.
        compare(page.selected, null);
        compare(slider.activeFocusOnTab, false);
        compare(slider.Accessible.description, "");
    }

    function test_endpointRowsNameTheirEndpointInEveryButton() {
        if (!TestServices.scriptSessions([{ app: 900, name: "Chrome", active: true }])) {
            skip("the scripted machine is not available in this harness");
        }
        CrucibleController.start();
        if (!CrucibleController.running) {
            skip("the engine did not run over the scripted machine here: " + CrucibleController.lastError);
        }
        tryVerify(function() { return CrucibleController.endpoints.length > 0; }, 5000,
                  "the probe reached the controller");
        const page = createTemporaryObject(outputPage, testCase.parent);
        verify(page);
        waitForRendering(page);
        for (const endpoint of CrucibleController.endpoints) {
            const row = findChild(page, "endpointRow-" + endpoint.id);
            verify(row, "endpoint " + endpoint.id + " has a row");
            compare(row.Accessible.role, Accessible.ListItem);
            compare(row.Accessible.name, endpoint.name);
            const hear = findChild(page, "hear-" + endpoint.id);
            const send = findChild(page, "send-" + endpoint.id);
            verify(hear && send);
            // Every row's two buttons read the same without this.
            verify(hear.Accessible.name.indexOf(endpoint.name) >= 0, hear.Accessible.name);
            verify(send.Accessible.name.indexOf(endpoint.name) >= 0, send.Accessible.name);
        }
    }

    // --- what the window says out loud ----------------------------------------

    function test_announceKeepsTheLastMessageAndTicksItsSerial() {
        const spy = createTemporaryObject(spyComponent, testCase, { target: A11y, signalName: "announced" });
        A11y.announce("");
        compare(spy.count, 0, "an empty announcement says nothing");
        A11y.announce("a position");
        compare(A11y.lastMessage, "a position");
        compare(spy.count, 1);
        compare(spy.signalArguments[0][1], false, "polite unless told otherwise");
        const serial = A11y.serial;
        A11y.announce("a position");
        compare(A11y.serial, serial + 1, "the same text said twice is two announcements");
        A11y.announce("an error", true);
        compare(spy.signalArguments[2][1], true, "assertive when it interrupts");
    }

    function test_announcerRelaysStateChangesOnceNotPerPoll() {
        const window = createTemporaryObject(shell, testCase);
        verify(window);
        tryCompare(window, "visible", true);
        const announcer = findChild(window, "announcer");
        verify(announcer, "the shell carries an announcer");
        // The launch state is not a change and is not announced; the item is
        // primed one turn later.
        tryVerify(function() { return announcer.primed; }, 5000, "the announcer primes after the launch");
        const spy = createTemporaryObject(spyComponent, testCase, { target: A11y, signalName: "announced" });
        // A state the controller always changes, whichever way round it is
        // on this machine.
        if (CrucibleController.running) {
            CrucibleController.stop();
        } else {
            CrucibleController.start();
        }
        tryVerify(function() { return spy.count >= 1; }, 3000, "the change is announced");
        // What was said is what the announcer reports as its own name, which
        // is how it reaches the platform's reader.
        verify(A11y.lastMessage.length > 0);
        compare(announcer.Accessible.name, A11y.lastMessage);
        // The controller republishes every property on every 60 ms poll.
        // Nothing is said again for a state that has not changed.
        wait(600);
        const settled = spy.count;
        wait(600);
        compare(spy.count, settled, "ten more polls say nothing new: " + A11y.lastMessage);
    }

    // --- contrast -------------------------------------------------------------

    // Theme.contrast, under a shorter name, because these cases read as a
    // table of ratios and the table is the point.
    function ratio(foreground, background) {
        return Theme.contrast(foreground, background);
    }

    function test_themePalettesMeetTheContrastFloors() {
        const palettes = ["signal", "ink", "console"];
        const modes = ["light", "dark"];
        for (const palette of palettes) {
            for (const mode of modes) {
                Theme.paletteChoice = palette;
                Theme.preference = mode;
                const where = palette + " " + mode + ": ";
                verify(ratio(Theme.text, Theme.bg) >= 7,
                       where + "text on bg is " + ratio(Theme.text, Theme.bg).toFixed(2));
                verify(ratio(Theme.textMuted, Theme.bg) >= 4.5,
                       where + "muted text on bg is " + ratio(Theme.textMuted, Theme.bg).toFixed(2));
                verify(ratio(Theme.textMuted, Theme.surface) >= 4.5,
                       where + "muted text on surface is " + ratio(Theme.textMuted, Theme.surface).toFixed(2));
                verify(ratio(Theme.accentInk, Theme.bg) >= 4.5,
                       where + "accent ink on bg is " + ratio(Theme.accentInk, Theme.bg).toFixed(2));
                verify(ratio(Theme.accentInk, Theme.surface) >= 4.5,
                       where + "accent ink on surface is " + ratio(Theme.accentInk, Theme.surface).toFixed(2));
                verify(ratio(Theme.focusRing, Theme.bg) >= 3,
                       where + "the focus ring on bg is " + ratio(Theme.focusRing, Theme.bg).toFixed(2));
                verify(ratio(Theme.focusRing, Theme.surface) >= 3,
                       where + "the focus ring on surface is " + ratio(Theme.focusRing, Theme.surface).toFixed(2));
                verify(ratio(Theme.divider, Theme.bg) >= 3,
                       where + "a control border on bg is " + ratio(Theme.divider, Theme.bg).toFixed(2));
                verify(ratio(Theme.divider, Theme.surface) >= 3,
                       where + "a control border on surface is " + ratio(Theme.divider, Theme.surface).toFixed(2));
                // The label on an accent fill is the better of the two ends
                // of the palette. The fill is the design system's and is not
                // darkened to suit the label, so in signal and console light
                // this is the best available (3.9 and 4.2:1) rather than a
                // 4.5 pass - docs/crucible/accessibility.md says so plainly.
                const onAccent = ratio(Theme.accentText, Theme.accent);
                verify(onAccent >= 3, where + "a primary label on the accent is " + onAccent.toFixed(2));
                fuzzyCompare(onAccent,
                             Math.max(ratio(Theme.bg, Theme.accent), ratio(Theme.text, Theme.accent)),
                             1e-9,
                             where + "the label takes the better of the two");
            }
        }
    }

    function test_contrastCompositesTheAlphaItIsGiven() {
        Theme.paletteChoice = "signal";
        Theme.preference = "light";
        // textMuted IS the text colour, at an alpha. Measured raw it would
        // report the 15:1 of solid text on the background and every muted
        // line in the window would pass a check it fails on screen.
        verify(Theme.textMuted.a < 1, "the muted token carries an alpha");
        const composited = Theme.contrast(Theme.textMuted, Theme.bg);
        const solid = Theme.contrast(Theme.text, Theme.bg);
        verify(composited < solid - 5, "the alpha is composited, not ignored: " + composited.toFixed(2)
               + " against " + solid.toFixed(2));
    }
}
