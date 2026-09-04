import QtQuick
import QtTest

import Ac3ForgeCrucible

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
        CrucibleController.stop();
    }

    function cleanup() {
        CrucibleController.stop();
    }

    function test_emptyRoomWithTheEngineStopped() {
        compare(CrucibleController.apps.length, 0);
        compare(CrucibleController.placedCount, 0);
        compare(CrucibleController.bedCount, 0);
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
        if (!CrucibleController.has3D) {
            skip("this build has no Qt Quick 3D");
        }
        page.threeD = true;
        compare(loader.active, true);
        // The view either comes up or reports why; it never hangs the page.
        tryVerify(function() { return loader.status === Loader.Ready || loader.status === Loader.Error; }, 10000);
        if (loader.status === Loader.Ready) {
            compare(loader.item.apps.length, CrucibleController.apps.length);
        }
        page.threeD = false;
        compare(loader.active, false);
    }

    function test_runningRoomCountsAddUp() {
        CrucibleController.start();
        if (!CrucibleController.running) {
            skip("the engine did not start here: " + CrucibleController.lastError);
        }
        tryVerify(function() { return CrucibleController.framesEncoded > 0; }, 5000);
        // The list is whatever this machine's sessions are; the invariant
        // is that every application is either placed or in the bed.
        compare(CrucibleController.placedCount + CrucibleController.bedCount, CrucibleController.apps.length);
        for (const app of CrucibleController.apps) {
            verify(app.app > 0);
            verify(app.name.length > 0);
            verify(app.slot === -1 || (app.slot >= 0 && app.slot < 10), "slot " + app.slot);
        }
        // Commands for an unknown application are ignored, not fatal.
        CrucibleController.position(4294967295, 0.5, 0.5, 0);
        CrucibleController.unposition(4294967295);
        CrucibleController.reprobe();
        const page = createTemporaryObject(roomPage, testCase);
        verify(page);
        waitForRendering(page);
    }

    function test_splitTakesTwoSlotsAndMonoGivesOneBack() {
        CrucibleController.start();
        if (!CrucibleController.running) {
            skip("the engine did not start here: " + CrucibleController.lastError);
        }
        tryVerify(function() { return CrucibleController.framesEncoded > 0; }, 5000);
        wait(800);  // the fresh engine's first session list reaches the controller on its next polls
        if (CrucibleController.apps.length === 0) {
            skip("no application has an audio session on this machine");
        }
        const id = CrucibleController.apps[0].app;
        function row() { return CrucibleController.apps.find(function(a) { return a.app === id; }); }
        CrucibleController.setSplit(id, true);
        CrucibleController.position(id, 0.5, 0.5, 0);
        tryVerify(function() {
            const app = row();
            return app && app.width === 2 && app.slot >= 0;
        }, 5000, "after split: " + JSON.stringify(row()));
        const placed = CrucibleController.placedCount;
        CrucibleController.setSplit(id, false);
        tryVerify(function() {
            const app = row();
            return app && app.width === 1 && app.slot >= 0;
        }, 5000, "after mono: " + JSON.stringify(row()));
        compare(CrucibleController.placedCount, placed);  // one application either way
        // The object's extent round-trips, clamped to the room.
        CrucibleController.setSize(id, 0.4);
        tryVerify(function() { const app = row(); return app && Math.abs(app.size - 0.4) < 1e-6; }, 5000, "size 0.4");
        CrucibleController.setSize(id, 7);
        tryVerify(function() { const app = row(); return app && app.size === 1; }, 5000, "size clamped to 1");
        CrucibleController.setSize(id, 0);
        CrucibleController.unposition(id);
    }

    function test_placementRoundTripsForALiveApplication() {
        CrucibleController.start();
        if (!CrucibleController.running) {
            skip("the engine did not start here: " + CrucibleController.lastError);
        }
        tryVerify(function() { return CrucibleController.framesEncoded > 0; }, 5000);
        wait(800);
        if (CrucibleController.apps.length === 0) {
            skip("no application has an audio session on this machine");
        }
        const id = CrucibleController.apps[0].app;
        CrucibleController.position(id, 0.25, 0.75, 0.5);
        tryVerify(function() {
            const app = CrucibleController.apps.find(function(a) { return a.app === id; });
            return app && app.slot >= 0 && Math.abs(app.x - 0.25) < 0.02 && Math.abs(app.y - 0.75) < 0.02;
        }, 5000);
        CrucibleController.unposition(id);
        tryVerify(function() {
            const app = CrucibleController.apps.find(function(a) { return a.app === id; });
            return app && app.slot === -1;
        }, 5000);
    }
    // A split pair's objects can be placed on their own; "standard stereo"
    // puts them back at the spread either side of the centre.
    function test_pairSidesPlaceOnTheirOwnAndResetToTheSpread() {
        CrucibleController.start();
        wait(800);
        const app = CrucibleController.apps.length ? CrucibleController.apps[0].app : -1;
        if (app < 0) { skip("no application with sound to place"); return; }
        CrucibleController.setSplit(app, true);
        CrucibleController.position(app, 0.5, 0.5, 0);
        tryVerify(function() { return CrucibleController.apps[0].width === 2 && !CrucibleController.apps[0].pairCustom; }, 5000, "split at the spread");
        tryCompare(CrucibleController.apps[0], "lx", 0.35);
        CrucibleController.positionSide(app, 1, 0.9, 0.9, 0);
        tryVerify(function() { return CrucibleController.apps[0].pairCustom; }, 5000, "custom after a side was placed");
        tryCompare(CrucibleController.apps[0], "rx", 0.9);
        tryCompare(CrucibleController.apps[0], "ry", 0.9);
        CrucibleController.resetPair(app);
        tryVerify(function() { return !CrucibleController.apps[0].pairCustom; }, 5000, "back to standard");
        CrucibleController.setSplit(app, false);
        CrucibleController.stop();
    }

}
