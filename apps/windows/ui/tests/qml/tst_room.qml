import QtQuick
import QtTest

import Ac3ForgeDesk

// The room: the session list the controller publishes, the placed/bed
// counts that go with it, and position/unposition commands. With the
// engine stopped the list is empty and the counts are zero; with it
// running (a machine with an audio endpoint) the counts add up to the list
// and a placement round-trips. The engine-dependent case skips where
// start() refuses.
TestCase {
    id: testCase
    name: "Room"
    when: windowShown
    width: 1480
    height: 820

    Component { id: roomPage; RoomPage { width: 1480; height: 700 } }

    function init() {
        DeskController.stop();
    }

    function cleanup() {
        DeskController.stop();
    }

    function test_emptyRoomWithTheEngineStopped() {
        compare(DeskController.apps.length, 0);
        compare(DeskController.placedCount, 0);
        compare(DeskController.bedCount, 0);
        const page = createTemporaryObject(roomPage, testCase);
        verify(page);
        waitForRendering(page);
    }

    function test_threeDToggleLoadsTheViewWhenBuilt() {
        const page = createTemporaryObject(roomPage, testCase);
        verify(page);
        waitForRendering(page);
        const choice = findChild(page, "roomViewChoice");
        const loader = findChild(page, "room3dLoader");
        verify(choice);
        verify(loader);
        // Effective visibility is false for everything under a TestCase
        // (the TestCase item itself is hidden), so the toggle's presence is
        // the check; its own visible binding follows has3D.
        compare(loader.active, false);
        if (!DeskController.has3D) {
            skip("this build has no Qt Quick 3D");
        }
        page.threeD = true;
        compare(loader.active, true);
        // The view either comes up or reports why; it never hangs the page.
        tryVerify(function() { return loader.status === Loader.Ready || loader.status === Loader.Error; }, 10000);
        if (loader.status === Loader.Ready) {
            compare(loader.item.apps.length, DeskController.apps.length);
        }
        page.threeD = false;
        compare(loader.active, false);
    }

    function test_runningRoomCountsAddUp() {
        DeskController.start();
        if (!DeskController.running) {
            skip("the engine did not start here: " + DeskController.lastError);
        }
        tryVerify(function() { return DeskController.framesEncoded > 0; }, 5000);
        // The list is whatever this machine's sessions are; the invariant
        // is that every application is either placed or in the bed.
        compare(DeskController.placedCount + DeskController.bedCount, DeskController.apps.length);
        for (const app of DeskController.apps) {
            verify(app.app > 0);
            verify(app.name.length > 0);
            verify(app.slot === -1 || (app.slot >= 0 && app.slot < 10), "slot " + app.slot);
        }
        // Commands for an unknown application are ignored, not fatal.
        DeskController.position(4294967295, 0.5, 0.5, 0);
        DeskController.unposition(4294967295);
        DeskController.reprobe();
        const page = createTemporaryObject(roomPage, testCase);
        verify(page);
        waitForRendering(page);
    }

    function test_splitTakesTwoSlotsAndMonoGivesOneBack() {
        DeskController.start();
        if (!DeskController.running) {
            skip("the engine did not start here: " + DeskController.lastError);
        }
        tryVerify(function() { return DeskController.framesEncoded > 0; }, 5000);
        wait(800);  // the fresh engine's first session list reaches the controller on its next polls
        if (DeskController.apps.length === 0) {
            skip("no application has an audio session on this machine");
        }
        const id = DeskController.apps[0].app;
        function row() { return DeskController.apps.find(function(a) { return a.app === id; }); }
        DeskController.setSplit(id, true);
        DeskController.position(id, 0.5, 0.5, 0);
        tryVerify(function() {
            const app = row();
            return app && app.width === 2 && app.slot >= 0;
        }, 5000, "after split: " + JSON.stringify(row()));
        const placed = DeskController.placedCount;
        DeskController.setSplit(id, false);
        tryVerify(function() {
            const app = row();
            return app && app.width === 1 && app.slot >= 0;
        }, 5000, "after mono: " + JSON.stringify(row()));
        compare(DeskController.placedCount, placed);  // one application either way
        // The object's extent round-trips, clamped to the room.
        DeskController.setSize(id, 0.4);
        tryVerify(function() { const app = row(); return app && Math.abs(app.size - 0.4) < 1e-6; }, 5000, "size 0.4");
        DeskController.setSize(id, 7);
        tryVerify(function() { const app = row(); return app && app.size === 1; }, 5000, "size clamped to 1");
        DeskController.setSize(id, 0);
        DeskController.unposition(id);
    }

    function test_placementRoundTripsForALiveApplication() {
        DeskController.start();
        if (!DeskController.running) {
            skip("the engine did not start here: " + DeskController.lastError);
        }
        tryVerify(function() { return DeskController.framesEncoded > 0; }, 5000);
        wait(800);
        if (DeskController.apps.length === 0) {
            skip("no application has an audio session on this machine");
        }
        const id = DeskController.apps[0].app;
        DeskController.position(id, 0.25, 0.75, 0.5);
        tryVerify(function() {
            const app = DeskController.apps.find(function(a) { return a.app === id; });
            return app && app.slot >= 0 && Math.abs(app.x - 0.25) < 0.02 && Math.abs(app.y - 0.75) < 0.02;
        }, 5000);
        DeskController.unposition(id);
        tryVerify(function() {
            const app = DeskController.apps.find(function(a) { return a.app === id; });
            return app && app.slot === -1;
        }, 5000);
    }
    // A split pair's objects can be placed on their own; "standard stereo"
    // puts them back at the spread either side of the centre.
    function test_pairSidesPlaceOnTheirOwnAndResetToTheSpread() {
        DeskController.start();
        wait(800);
        const app = DeskController.apps.length ? DeskController.apps[0].app : -1;
        if (app < 0) { skip("no application with sound to place"); return; }
        DeskController.setSplit(app, true);
        DeskController.position(app, 0.5, 0.5, 0);
        tryVerify(function() { return DeskController.apps[0].width === 2 && !DeskController.apps[0].pairCustom; }, 5000, "split at the spread");
        tryCompare(DeskController.apps[0], "lx", 0.35);
        DeskController.positionSide(app, 1, 0.9, 0.9, 0);
        tryVerify(function() { return DeskController.apps[0].pairCustom; }, 5000, "custom after a side was placed");
        tryCompare(DeskController.apps[0], "rx", 0.9);
        tryCompare(DeskController.apps[0], "ry", 0.9);
        DeskController.resetPair(app);
        tryVerify(function() { return !DeskController.apps[0].pairCustom; }, 5000, "back to standard");
        DeskController.setSplit(app, false);
        DeskController.stop();
    }

}
