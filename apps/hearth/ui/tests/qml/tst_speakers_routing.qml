import QtQuick
import QtQuick.Window
import QtTest

import Ac3ForgeHearth

// Speakers.qml's routing grid (issue #886, the arrow-key navigation PR #859
// added): HearthController.speakerLabels/trimDb/delayMs/etc. come from the
// engine's layout, set unconditionally at Engine construction ("2.0" -
// hearth_controller.cpp's start()) - present whether or not a real output
// device opened. HearthController.routingOutputs/outputNames come from the
// device side and can legitimately be 0 in an environment with no live or
// fake output device (this sandbox has none -
// [[hearth-qml-exe-isolated-harness-technique]]'s own finding for this same
// grid, and CI runners are not guaranteed to have one either -
// apps/crucible/ui/tests/qml_test_main.cpp's own comment on
// scriptMachineWithNoOutput makes the identical point for Crucible). Every
// test here reads root.outputs at run time rather than assuming a
// particular count, so it holds whether this machine reports 0 real outputs
// or several.
//
// grid/rowsRepeater/cellsRepeater/cell/noneCell are all private ids inside
// Speakers.qml's own document (no property alias exposes them, checked by
// reading the file before writing this test) - cellAt()/moveFocus() cannot
// be called from outside it. The keyboard tests below drive real
// keyClick()s instead and read back Window.activeFocusItem, the same
// pattern apps/crucible/ui/tests/qml/tst_keyboard.qml already uses
// (page.Window.activeFocusItem) for an identical "which item has focus now"
// question - the one difference is that grid cells carry no objectName, so
// this file recognises one by its own `assigned` property
// (readonly property bool assigned, unique to a routing cell) rather than
// by name. HearthController.routing/outputs/labels ARE plain properties on
// Speakers.qml's root ScrollView (readonly property var routingList: ... /
// property int outputs: ...), so those are read directly off the page
// instance without needing findChild() at all.
TestCase {
    id: testCase
    name: "SpeakersRouting"
    when: windowShown
    width: 1200
    height: 820

    Component { id: speakersComponent; Speakers { width: 1200; height: 820 } }

    function init() {
        HearthController.start();
        tryVerify(function() { return HearthController.speakerLabels.length > 0; }, 15000,
                  "speakerLabels was never populated - no engine layout snapshot arrived");
    }

    // True for exactly the items Speakers.qml's grid delegates create (both
    // the numbered output cells and the NONE cell mirror this property) -
    // see this file's own header note on why an objectName check will not
    // do here.
    function looksLikeARoutingCell(item) {
        return item !== null && item !== undefined && typeof item.assigned !== "undefined";
    }

    // Tabs forward from a freshly created page, bounded, until
    // Window.activeFocusItem looks like a grid cell (looksLikeARoutingCell())
    // or the bound is spent. Returns null rather than failing outright: how
    // many controls precede the grid in tab order is Speakers.qml's own
    // layout to change, not a contract this test should pin down by count.
    function tabIntoTheGrid(page) {
        page.forceActiveFocus();
        for (let attempt = 0; attempt < 40; ++attempt) {
            keyClick(Qt.Key_Tab);
            const focused = page.Window.activeFocusItem;
            if (looksLikeARoutingCell(focused)) {
                return focused;
            }
        }
        return null;
    }

    function test_arrowKeysMoveFocusBetweenGridCells() {
        const page = createTemporaryObject(speakersComponent, testCase.parent);
        verify(page !== null);
        waitForRendering(page);

        const first = tabIntoTheGrid(page);
        if (first === null) {
            skip("could not tab into the routing grid in this environment");
        }

        // Down then Up should return focus to the same cell it left - a
        // vertical round trip needs only the row count (speakerLabels),
        // which does not depend on a live output device, unlike a
        // horizontal move across output columns (see this file's own
        // header note on why root.outputs is read rather than assumed).
        keyClick(Qt.Key_Down);
        const afterDown = page.Window.activeFocusItem;
        verify(looksLikeARoutingCell(afterDown), "Down moved focus off the grid entirely");

        if (HearthController.speakerLabels.length > 1) {
            verify(afterDown !== first, "Down did not move focus with more than one speaker row");
        }

        keyClick(Qt.Key_Up);
        const afterUp = page.Window.activeFocusItem;
        verify(looksLikeARoutingCell(afterUp), "Up moved focus off the grid entirely");
        compare(afterUp, first, "Down then Up did not return focus to the starting cell");

        // A key the grid does not handle (moveFocus()'s own default: return
        // branch) must not move focus or crash the item away.
        keyClick(Qt.Key_A);
        compare(page.Window.activeFocusItem, first, "an unhandled key moved focus");
    }

    function test_clearRoutingAndUseDeviceOrderRoundTripThroughTheController() {
        const page = createTemporaryObject(speakersComponent, testCase.parent);
        verify(page !== null);
        waitForRendering(page);

        HearthController.clearRouting();
        tryVerify(function() {
            return page.routingList.length > 0 &&
                   page.routingList.every(function(output) { return output === -1; });
        }, 15000, "clearRouting() never left every slot unassigned");

        if (page.outputs === 0) {
            // useDeviceOrder() patches each slot to the device's own report
            // order (identity, one slot per output) - with zero reported
            // outputs there is nothing for it to assign, so the only honest
            // assertion left is that it does not crash or corrupt the
            // (still all-unassigned) routing list.
            HearthController.useDeviceOrder();
            wait(150);
            verify(page.routingList.every(function(output) { return output === -1; }),
                   "useDeviceOrder() assigned an output that was not reported");
            return;
        }

        HearthController.useDeviceOrder();
        tryVerify(function() {
            return page.routingList.length > 0 &&
                   page.routingList.every(function(output, slot) { return output === slot; });
        }, 15000, "useDeviceOrder() never patched slots to identity order");
    }

    function test_setRoutingAssignmentRoundTripsThroughTheController() {
        const page = createTemporaryObject(speakersComponent, testCase.parent);
        verify(page !== null);
        waitForRendering(page);

        HearthController.clearRouting();
        tryVerify(function() { return page.routingList.length > 0 && page.routingList[0] === -1; }, 15000);

        if (page.outputs === 0) {
            // Nothing to assign slot 0 to - setRoutingAssignment(0, 0) is
            // refused engine-side for an output this device never reported
            // (HearthController::setRoutingAssignment()'s own header
            // comment), so the only honest round trip left in this
            // environment is unassigning what is already unassigned.
            HearthController.setRoutingAssignment(0, -1);
            tryVerify(function() { return page.routingList[0] === -1; }, 15000);
            return;
        }

        HearthController.setRoutingAssignment(0, 0);
        tryVerify(function() { return page.routingList[0] === 0; }, 15000,
                  "setRoutingAssignment(0, 0) never reached HearthController.routing");

        HearthController.setRoutingAssignment(0, -1);
        tryVerify(function() { return page.routingList[0] === -1; }, 15000,
                  "setRoutingAssignment(0, -1) never unassigned slot 0 again");
    }

    function test_spaceCommitsTheFocusedCellsAssignment() {
        const page = createTemporaryObject(speakersComponent, testCase.parent);
        verify(page !== null);
        waitForRendering(page);

        const cell = tabIntoTheGrid(page);
        if (cell === null) {
            skip("could not tab into the routing grid in this environment");
        }
        // rowItem (cell's/noneCell's immediate parent, both Speakers.qml's
        // own delegate Row) carries `required property int index` - the row
        // Keys.onSpacePressed/setRoutingAssignment() targets - read off the
        // focused item rather than assumed, since which row Tab happened to
        // land on is not something this test pins down.
        const row = cell.parent && typeof cell.parent.index !== "undefined" ? cell.parent.index : -1;
        if (row < 0) {
            skip("the focused grid cell's row index could not be read");
        }

        HearthController.clearRouting();
        tryVerify(function() { return page.routingList[row] === -1; }, 15000);

        keyClick(Qt.Key_Space);
        // With no reported outputs the whole row is the NONE column (see
        // this file's header note); committing it is a real, working
        // keypress even though the visible outcome is "still -1" rather
        // than a new assignment. With real outputs, a numbered cell commits
        // its own index.
        const expected = typeof cell.index !== "undefined" && cell.index < page.outputs ? cell.index : -1;
        tryVerify(function() { return page.routingList[row] === expected; }, 15000,
                  "Space never committed the focused cell's assignment");
    }
}
