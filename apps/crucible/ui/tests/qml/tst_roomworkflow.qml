import QtQuick
import QtTest

import Ac3ForgeCrucible
import Ac3ForgeCrucibleTest

// The Room, driven the way a person drives it: over a scripted machine (the
// fake session monitor, devices, default device and full-screen seam the
// engine's Catch2 cases use, ui/tests/qml_test_main.cpp), the real
// controller and the real engine run, and every case acts through the
// page's own controls - a click on a row, a press on a button, a drag of a
// marker or a bed chip - and then waits for the ENGINE's answer to come
// back through the controller's poll. Nothing here calls position() itself.
//
// Everything is parented to testCase.parent, the window's root item, for the
// reason tst_keyboard.qml gives: the TestCase item is invisible by design,
// and nothing under it can be seen, focused or hit by a synthetic click.
TestCase {
    id: testCase
    name: "RoomWorkflow"
    when: windowShown
    width: 1480
    height: 820

    Component { id: roomPage; RoomPage { width: 1480; height: 780 } }
    Component { id: settingsPage; SettingsPage { width: 1480; height: 780 } }
    Component { id: spyComponent; SignalSpy {} }

    readonly property var chrome: ({ app: 900, name: "chrome", active: true })
    readonly property var steam: ({ app: 901, name: "steam", active: true })
    readonly property var zoom: ({ app: 902, name: "zoom", active: false })

    function init() {
        CrucibleController.stop();
        CrucibleController.firstRunAcknowledged = true;
        CrucibleController.roomView = "2d";
        CrucibleController.showSilentApps = true;
        CrucibleController.showBackgroundApps = false;
        CrucibleController.splitStereo = false;
    }

    function cleanup() {
        CrucibleController.stop();
        TestServices.clear();
        CrucibleController.roomView = "2d";
        CrucibleController.showSilentApps = true;
        CrucibleController.showBackgroundApps = false;
    }

    // --- helpers --------------------------------------------------------------

    // The scripted room, running. Skips (not fails) where the engine will
    // not run over it, which is the same refusal every engine suite honours.
    function scripted(apps) {
        if (!TestServices.scriptSessions(apps)) {
            skip("the scripted machine is not available in this harness");
        }
        CrucibleController.start();
        if (!CrucibleController.running) {
            skip("the engine did not run over the scripted machine: " + CrucibleController.lastError);
        }
        tryVerify(function() { return CrucibleController.apps.length === visibleCount(apps); }, 5000,
                  "the scripted applications reach the rail");
    }

    function visibleCount(apps) {
        let count = 0;
        for (const a of apps) {
            const background = a.window === false;
            const silent = a.session === false;
            if (background && !CrucibleController.showBackgroundApps) continue;
            if (silent && !CrucibleController.showSilentApps) continue;
            ++count;
        }
        return count;
    }

    function entry(id) {
        return CrucibleController.apps.find(function(a) { return a.app === id; }) || null;
    }

    function makePage() {
        const page = createTemporaryObject(roomPage, testCase.parent);
        verify(page);
        waitForRendering(page);
        return page;
    }

    // The nearest Flickable above an item, scrolled so the item is on
    // screen: the selected-application card sits below a 780-pixel page,
    // and a synthetic click outside a clipping Flickable reaches nothing.
    function reveal(item) {
        // Layouts place their children at the next polish, so a control that
        // has just appeared (the card, on a selection) is where it will be
        // only once every layout above it has been polished.
        for (let up = item; up; up = up.parent) waitForItemPolished(up);
        let flick = item.parent;
        while (flick && flick.contentY === undefined) flick = flick.parent;
        if (!flick) return;
        const top = item.mapToItem(flick.contentItem, 0, 0).y;
        const wanted = Math.max(0, Math.min(flick.contentHeight - flick.height, top - flick.height / 3));
        // Only a scroll draws a new frame, and waitForRendering waits for one.
        if (Math.abs(flick.contentY - wanted) > 0.5) {
            flick.contentY = wanted;
            waitForRendering(item);
        }
    }

    function click(item) {
        reveal(item);
        verify(item.enabled, (item.objectName || item.text) + " is enabled");
        mouseClick(item);
    }

    // A room view's field: the rectangle the markers and the drop area are
    // drawn in, under the view's caption row (RoomView.qml).
    function fieldOf(view) {
        const field = view.children[0].children[1];
        verify(field && field.width > 100, "the room view's field");
        return field;
    }

    // The elevation: the other RoomView in the plan's grid.
    function elevationOf(page) {
        const plan = findChild(page, "roomPlan");
        const elevation = plan.parent.children[1].children[0];
        verify(elevation && elevation.elevation === true, "the elevation view");
        return elevation;
    }

    // A press-move-release from one point of `ref` to another, in steps, so
    // a drag threshold is crossed and every intermediate position is seen.
    //
    // The first move is a short one past the drag threshold: a MouseArea
    // drag with drag.smoothed (the default, and the bed chip's) does not
    // jump by the move that crossed the threshold, so a long first step
    // would leave the chip that far behind the pointer - which is not how a
    // hand moves and not what this suite is measuring.
    function drag(ref, fromX, fromY, toX, toY) {
        mousePress(ref, fromX, fromY);
        const length = Math.max(1, Math.hypot(toX - fromX, toY - fromY));
        mouseMove(ref, fromX + (toX - fromX) * 6 / length, fromY + (toY - fromY) * 6 / length);
        const steps = 8;
        for (let i = 1; i <= steps; ++i) {
            mouseMove(ref, fromX + (toX - fromX) * i / steps, fromY + (toY - fromY) * i / steps);
        }
        mouseRelease(ref, toX, toY);
    }

    function select(page, id) {
        const list = findChild(page, "appList");
        const row = list.itemAtIndex(page.indexOfApp(id));
        verify(row, "a row for " + id);
        mouseClick(row);
        compare(page.selectedApp, id, "a click on the row selects it");
    }

    function placeFromTheCard(page, id) {
        select(page, id);
        click(findChild(page, "placeButton"));
        tryVerify(function() { const a = entry(id); return a && a.slot >= 0; }, 5000, "placed by the card's button");
    }

    // --- the list -------------------------------------------------------------

    function test_applicationsAppearInTheListAndTheBed() {
        scripted([zoom, steam, chrome]);
        const page = makePage();
        const list = findChild(page, "appList");
        verify(list);
        tryCompare(list, "count", 3);
        // Sound first, then by name; the stem is shown capitalised.
        const names = [];
        for (let i = 0; i < list.count; ++i) names.push(list.itemAtIndex(i).app.name);
        compare(names.join(","), "Chrome,Steam,Zoom");
        compare(CrucibleController.soundingCount, 2);
        compare(CrucibleController.bedCount, 3);
        // Every unplaced application has a chip in the bed.
        for (const id of [900, 901, 902]) {
            const chip = findChild(page, "chip-" + id);
            verify(chip, "chip for " + id);
            verify(chip.visible, "chip " + id + " is shown in the bed");
        }
        // An idle one says so in its row.
        verify(list.itemAtIndex(2).detail.indexOf("idle") >= 0, list.itemAtIndex(2).detail);

        // One leaves, another arrives: the rail follows the machine.
        verify(TestServices.setSessions([chrome, zoom, { app: 903, name: "vlc", active: true }]));
        tryVerify(function() { return entry(901) === null && entry(903) !== null; }, 5000, "Steam left and VLC arrived");
        tryCompare(list, "count", 3);
        tryVerify(function() { return findChild(page, "chip-903") !== null; }, 3000, "VLC's chip in the bed");
    }

    function test_silentAndBackgroundApplicationsFollowTheirSettings() {
        const quiet = { app: 904, name: "notes", session: false };
        const hidden = { app: 905, name: "vmhost", window: false };
        scripted([chrome, quiet, hidden]);
        // Defaults: silent applications listed (greyed), background ones not.
        verify(entry(904) !== null, "the silent application is listed");
        compare(entry(904).silent, true);
        compare(entry(905), null, "the background process is not listed");

        // Turned over from the Settings page's own checks.
        const settings = createTemporaryObject(settingsPage, testCase.parent);
        verify(settings);
        waitForRendering(settings);
        const background = findByText(settings, "Show background processes in the room");
        const silent = findByText(settings, "Show applications with no audio");
        verify(background && silent, "the two behaviour checks");
        click(background);
        compare(CrucibleController.showBackgroundApps, true);
        tryVerify(function() { return entry(905) !== null; }, 5000, "the background process is listed now");
        compare(entry(905).background, true);
        click(silent);
        compare(CrucibleController.showSilentApps, false);
        tryVerify(function() { return entry(904) === null; }, 5000, "the silent application is hidden now");
    }

    function findByText(root, text) {
        if (root.text === text && root.toggled !== undefined) return root;
        const kids = root.children || [];
        for (let i = 0; i < kids.length; ++i) {
            const found = findByText(kids[i], text);
            if (found) return found;
        }
        if (root.contentItem && root.contentItem !== root) {
            return findByText(root.contentItem, text);
        }
        return null;
    }

    // --- placing from the card ------------------------------------------------

    function test_theCardPlacesMovesCentresAndReturnsAnApplication() {
        scripted([chrome, steam]);
        const page = makePage();
        const place = findChild(page, "placeButton");
        verify(place);
        verify(!place.enabled, "nothing selected, nothing to place");

        placeFromTheCard(page, 900);
        const a = entry(900);
        fuzzyCompare(a.x, 0.5, 0.02);
        fuzzyCompare(a.y, 0.5, 0.02);
        compare(CrucibleController.placedCount, 1);
        compare(place.text, "Send to bed");
        // The marker is on the plan, where the engine put it.
        const plan = findChild(page, "roomPlan");
        const marker = findChild(plan, "marker-900");
        verify(marker);
        tryVerify(function() { return marker.visible; }, 3000, "the marker shows once placed");
        verify(!findChild(page, "chip-900").visible, "and its chip has left the bed");

        // The quick placements.
        click(findChild(page, "put-0.1-0.5-0"));
        tryVerify(function() { return Math.abs(entry(900).x - 0.1) < 0.02; }, 5000, "left");
        click(findChild(page, "put-0.85-0.85-0"));
        tryVerify(function() { return Math.abs(entry(900).x - 0.85) < 0.02 && Math.abs(entry(900).y - 0.85) < 0.02; }, 5000, "rear right");
        click(findChild(page, "put-0.5-0.5-0.8"));
        tryVerify(function() { return Math.abs(entry(900).z - 0.8) < 0.02; }, 5000, "overhead");
        // The marker follows the engine: the plan draws x across.
        const field = fieldOf(plan);
        tryVerify(function() { return Math.abs(marker.x - field.width * entry(900).x) < 2; }, 3000, "the marker follows");

        click(findChild(page, "centreButton"));
        tryVerify(function() { const e = entry(900); return Math.abs(e.x - 0.5) < 0.02 && Math.abs(e.y - 0.5) < 0.02 && Math.abs(e.z) < 0.02; }, 5000, "centred");

        click(place);
        tryVerify(function() { return entry(900).slot < 0; }, 5000, "sent back to the bed");
        compare(CrucibleController.placedCount, 0);
        tryVerify(function() { return findChild(page, "chip-900").visible; }, 3000, "the chip is back in the bed");
    }

    // --- placing by dragging ----------------------------------------------------

    function test_aBedChipDraggedIntoThePlanIsPlacedWhereItLands() {
        scripted([chrome]);
        const page = makePage();
        const plan = findChild(page, "roomPlan");
        const field = fieldOf(plan);
        const chip = findChild(page, "chip-900");
        verify(chip && chip.visible);
        reveal(chip);
        // From the chip's centre (its drag hot spot) to a quarter of the way
        // in from the left, a third of the way back.
        const from = chip.mapToItem(page, chip.width / 2, chip.height / 2);
        // The plan may have scrolled off with the chip in view; both are in
        // the same Flickable, and a drag across it is what a person does.
        const to = field.mapToItem(page, field.width * 0.25, field.height * 0.3);
        drag(page, from.x, from.y, to.x, to.y);
        tryVerify(function() { const a = entry(900); return a && a.slot >= 0; }, 5000, "the drop placed it");
        const a = entry(900);
        fuzzyCompare(a.x, 0.25, 0.03);
        fuzzyCompare(a.y, 0.3, 0.03);
        compare(page.selectedApp, 900, "and the dropped application is the selected one");
    }

    function test_aMarkerDraggedInThePlanMovesAndADoubleClickReturnsIt() {
        scripted([chrome]);
        const page = makePage();
        placeFromTheCard(page, 900);
        const plan = findChild(page, "roomPlan");
        reveal(plan);
        const field = fieldOf(plan);
        const marker = findChild(plan, "marker-900");
        tryVerify(function() { return marker.visible && Math.abs(marker.x - field.width / 2) < 2; }, 3000);
        const from = marker.mapToItem(page, 0, 0);
        const to = field.mapToItem(page, field.width * 0.8, field.height * 0.2);
        drag(page, from.x, from.y, to.x, to.y);
        tryVerify(function() { const a = entry(900); return Math.abs(a.x - 0.8) < 0.03 && Math.abs(a.y - 0.2) < 0.03; }, 5000,
                  "dragged to the front right: " + entry(900).x + ", " + entry(900).y);
        // Dragged past the wall, it stops at the wall.
        const at = marker.mapToItem(page, 0, 0);
        const beyond = field.mapToItem(page, field.width + 80, field.height * 0.2);
        drag(page, at.x, at.y, beyond.x, beyond.y);
        tryVerify(function() { return Math.abs(entry(900).x - 1.0) < 0.01; }, 5000, "clamped to the right wall");
        // A double-click on it is meant to send it back to the bed (the bed
        // tray's hint says so). The view does ask for that...
        const now = marker.mapToItem(page, 0, 0);
        const returned = createTemporaryObject(spyComponent, testCase, { target: plan, signalName: "returned" });
        const moved = createTemporaryObject(spyComponent, testCase, { target: plan, signalName: "moved" });
        mouseDoubleClickSequence(page, now.x, now.y);
        compare(returned.count, 1, "the double-click asks for the application back");
        // Regression: the release that ends a double-click used to send the
        // marker's position again (the second press had set dragging), and
        // the engine, given unposition then position, put the application
        // straight back. Only the first click's release may send a move.
        compare(moved.count, 1, "only the first click's release sent a move");
        tryVerify(function() { return entry(900).slot < 0; }, 1000, "double-click returned it");
    }

    function test_theSizeTrackGivesUpTheFocusWhenItsApplicationLeaves() {
        // Regression: the size track left the tab chain (activeFocusOnTab
        // false) while it still held the focus, which Qt refuses with a
        // warning, when the selected application went away.
        failOnWarning(/Cannot set activeFocusOnTab/);
        scripted([chrome, steam]);
        const page = makePage();
        placeFromTheCard(page, 900);
        const track = findChild(page, "sizeSlider");
        verify(track);
        reveal(track);
        mouseClick(track, track.width * 0.5, track.height / 2);
        tryVerify(function() { return track.activeFocus; }, 3000, "the track has the focus");
        verify(TestServices.setSessions([steam]));
        tryVerify(function() { return entry(900) === null; }, 5000, "chrome left");
        tryVerify(function() { return !track.activeFocus && !track.activeFocusOnTab; }, 3000,
                  "the track gave up the focus and left the tab chain");
    }

    function test_theElevationDragMovesDepthAndHeight() {
        scripted([chrome]);
        const page = makePage();
        placeFromTheCard(page, 900);
        const elevation = elevationOf(page);
        reveal(elevation);
        const field = fieldOf(elevation);
        const marker = findChild(elevation, "marker-900");
        verify(marker);
        tryVerify(function() { return marker.visible; }, 3000);
        const from = marker.mapToItem(page, 0, 0);
        // Across is depth (front at the left), up is height: a quarter of the
        // way down the field is half-way to the ceiling.
        const to = field.mapToItem(page, field.width * 0.7, field.height * 0.25);
        drag(page, from.x, from.y, to.x, to.y);
        tryVerify(function() { const a = entry(900); return Math.abs(a.y - 0.7) < 0.03 && Math.abs(a.z - 0.5) < 0.05; }, 5000,
                  "depth and height from the elevation: y " + entry(900).y + " z " + entry(900).z);
        fuzzyCompare(entry(900).x, 0.5, 0.02);  // the elevation leaves x alone
    }

    // --- split pairs and size ---------------------------------------------------

    function test_splitPairSidesDragOnTheirOwnAndStandardStereoResetsThem() {
        scripted([chrome]);
        const page = makePage();
        placeFromTheCard(page, 900);
        const split = findChild(page, "splitButton");
        compare(split.text, "Split");
        click(split);
        tryVerify(function() { return entry(900).width === 2; }, 5000, "split into a pair");
        tryCompare(split, "text", "Mono");
        const standard = findChild(page, "standardStereoButton");
        verify(!standard.visible, "a pair at the standard spread has nothing to reset");

        // The right object of the pair, on the plan, dragged to the rear.
        const plan = findChild(page, "roomPlan");
        reveal(plan);
        const field = fieldOf(plan);
        const marker = findChild(plan, "marker-900");
        let right = null;
        tryVerify(function() {
            for (let i = 0; i < marker.children.length; ++i) {
                if (marker.children[i].side === 1) right = marker.children[i];
            }
            return right !== null;
        }, 3000, "the pair's right object has a marker of its own");
        tryVerify(function() { return Math.abs(right.fieldX - field.width * entry(900).rx) < 2; }, 3000);
        const from = right.mapToItem(page, 0, 0);
        const to = field.mapToItem(page, field.width * 0.9, field.height * 0.9);
        drag(page, from.x, from.y, to.x, to.y);
        tryVerify(function() { const a = entry(900); return a.pairCustom && Math.abs(a.rx - 0.9) < 0.03 && Math.abs(a.ry - 0.9) < 0.03; }, 5000,
                  "the right object moved on its own");
        tryVerify(function() { return standard.visible; }, 3000, "Standard stereo is offered for a moved pair");
        click(standard);
        tryVerify(function() { return !entry(900).pairCustom; }, 5000, "back to the standard spread");
        click(split);
        tryVerify(function() { return entry(900).width === 1; }, 5000, "mono again");
    }

    function test_theSizeTrackSetsTheObjectsExtent() {
        scripted([chrome]);
        const page = makePage();
        placeFromTheCard(page, 900);
        const track = findChild(page, "sizeSlider");
        verify(track);
        reveal(track);
        mouseClick(track, track.width * 0.4, track.height / 2);
        tryVerify(function() { return Math.abs(entry(900).size - 0.4) < 0.02; }, 5000, "size from the click: " + entry(900).size);
        // Dragged past the end, it stops at the whole room.
        drag(track, track.width * 0.4, track.height / 2, track.width + 50, track.height / 2);
        tryVerify(function() { return entry(900).size === 1; }, 5000, "clamped to 1");
    }

    // --- keys, through the real controller ----------------------------------------

    function test_roomKeysMoveHeightRecentreResizeAndReturnThroughTheEngine() {
        scripted([chrome]);
        const page = makePage();
        placeFromTheCard(page, 900);
        const keys = findChild(page, "roomKeys");
        keys.forceActiveFocus();
        verify(keys.activeFocus);
        keyClick(Qt.Key_PageUp, Qt.ControlModifier);
        tryVerify(function() { return Math.abs(entry(900).z - 0.25) < 0.02; }, 5000, "Ctrl+Page Up raised it a quarter");
        keyClick(Qt.Key_Left, Qt.ShiftModifier);
        tryVerify(function() { return Math.abs(entry(900).x - 0.49) < 0.005; }, 5000, "Shift+Left nudged it");
        keyClick(Qt.Key_Home);
        tryVerify(function() { const a = entry(900); return Math.abs(a.x - 0.5) < 0.005 && Math.abs(a.z) < 0.005; }, 5000, "Home recentred it");
        keyClick(Qt.Key_Plus);
        tryVerify(function() { return Math.abs(entry(900).size - 0.05) < 0.005; }, 5000, "plus grew it");
        keyClick(Qt.Key_Minus);
        tryVerify(function() { return entry(900).size === 0; }, 5000, "minus shrank it");
        keyClick(Qt.Key_Delete);
        tryVerify(function() { return entry(900).slot < 0; }, 5000, "Delete returned it");
    }

    function test_enterOnABedChipPlacesItAndHandsTheKeysToTheRoom() {
        scripted([chrome]);
        const page = makePage();
        const chip = findChild(page, "chip-900");
        reveal(chip);
        mouseClick(chip);
        verify(chip.activeFocus, "a click on a chip takes focus");
        compare(page.selectedApp, 900);
        keyClick(Qt.Key_Return);
        tryVerify(function() { const a = entry(900); return a.slot >= 0 && Math.abs(a.x - 0.5) < 0.02; }, 5000, "Enter placed it in the centre");
        tryVerify(function() { return findChild(page, "roomKeys").activeFocus; }, 2000, "the room has the keys");
        keyClick(Qt.Key_Down);
        tryVerify(function() { return Math.abs(entry(900).y - 0.55) < 0.02; }, 5000, "Down moved it back");
    }

    // --- the full-screen rule -----------------------------------------------------

    function test_aFullScreenApplicationIsLockedInTheBed() {
        scripted([chrome, steam]);
        const page = makePage();
        placeFromTheCard(page, 900);
        verify(TestServices.setFullscreen(900));
        tryVerify(function() { return entry(900).fullscreen; }, 5000, "the engine sees it full-screen");
        tryVerify(function() { return entry(900).slot < 0; }, 5000, "and takes it out of the room");
        // Nothing on the card can place it while it lasts.
        compare(findChild(page, "placeButton").enabled, false);
        compare(findChild(page, "put-0.1-0.5-0").enabled, false);
        const chip = findChild(page, "chip-900");
        compare(chip.activeFocusOnTab, false, "its chip leaves the tab chain");
        const row = findChild(page, "appList").itemAtIndex(page.indexOfApp(900));
        verify(row.detail.indexOf("full-screen") >= 0, row.detail);
        // Dragging its chip does nothing either.
        const plan = findChild(page, "roomPlan");
        const field = fieldOf(plan);
        reveal(chip);
        const from = chip.mapToItem(page, chip.width / 2, chip.height / 2);
        const to = field.mapToItem(page, field.width * 0.3, field.height * 0.3);
        drag(page, from.x, from.y, to.x, to.y);
        wait(200);  // a poll or two: nothing is expected to arrive
        compare(entry(900).slot, -1, "a full-screen chip cannot be dragged into the room");
        // When it leaves full-screen it can be placed again.
        verify(TestServices.setFullscreen(0));
        tryVerify(function() { return !entry(900).fullscreen; }, 5000);
        tryVerify(function() { return findChild(page, "placeButton").enabled; }, 3000);
    }

    // --- the 3D view -----------------------------------------------------------

    function test_theRoomViewSwitchPersistsAndTheThreeDViewTakesADrop() {
        if (!CrucibleController.has3D) {
            skip("this build has no Qt Quick 3D");
        }
        scripted([chrome]);
        const page = makePage();
        const choice = findChild(page, "roomViewChoice");
        const loader = findChild(page, "room3dLoader");
        verify(choice && loader);
        mouseClick(findChild(choice, "seg-3d"));
        compare(CrucibleController.roomView, "3d");
        compare(String(TestServices.storedSetting("appearance/roomView")), String("3d"), "the choice is in the store");
        tryVerify(function() { return loader.status === Loader.Ready || loader.status === Loader.Error; }, 10000);
        compare(loader.status, Loader.Ready, "the 3D view loads under this backend");
        const view = loader.item;
        waitForRendering(page);  // the plan's grid gives way to the 3D view
        compare(view.apps.length, 1);
        compare(view.layout, CrucibleController.objectsEnabled ? "7.1.4" : "5.1", "auto layout follows the stream");
        CrucibleController.roomLayout = "7.1";
        compare(view.layout, "7.1");
        CrucibleController.roomLayout = "auto";

        // A chip dropped on the 3D room lands where the ray through the drop
        // meets the floor: the camera's own projection, no rendering needed.
        const moved = createTemporaryObject(spyComponent, testCase, { target: view, signalName: "moved" });
        const chip = findChild(page, "chip-900");
        reveal(chip);
        const from = chip.mapToItem(page, chip.width / 2, chip.height / 2);
        const to = view.mapToItem(page, view.width / 2, view.height / 2);
        drag(page, from.x, from.y, to.x, to.y);
        if (moved.count === 0) {
            skip("the drop reached the 3D view but mapTo3DScene answered nothing under this backend");
        }
        tryVerify(function() { const a = entry(900); return a && a.slot >= 0; }, 5000, "the 3D drop placed it");
        verify(entry(900).x > 0 && entry(900).x < 1, "somewhere on the floor: " + entry(900).x);
        compare(page.selectedApp, 900);

        // And back to the plan, which the store remembers too.
        mouseClick(findChild(choice, "seg-2d"));
        compare(CrucibleController.roomView, "2d");
        compare(String(TestServices.storedSetting("appearance/roomView")), String("2d"));
        verify(loader.active, "the 3D view is kept loaded once shown, so its camera survives");
    }

    // --- the rail's other cards ---------------------------------------------------

    function test_chooseOnTheSignalPathRailAsksForTheOutputPage() {
        scripted([chrome]);
        const page = makePage();
        const spy = createTemporaryObject(spyComponent, testCase, { target: page, signalName: "openOutput" });
        const choose = findChild(page, "chooseEndpointButton");
        verify(choose && choose.visible);
        mouseClick(choose);
        compare(spy.count, 1);
        // The rail's middle station counts what the room holds.
        placeFromTheCard(page, 900);
        const station = findChild(page, "station-crucible");
        tryVerify(function() { return station.title.indexOf("1 placed") >= 0; }, 3000, station.title);
    }
}
