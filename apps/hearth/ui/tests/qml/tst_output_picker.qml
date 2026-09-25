import QtQuick
import QtTest

import Ac3ForgeHearth
import Ac3ForgeHearthTest

import "HearthTestHelpers.js" as H

// OutputPicker.qml driven inside the real window (FEATURE_COVERAGE.md rows
// 6, 7, 22, 33-38): Main.qml's header summary and the Play page's signal-path
// "Choose..." open it, it lists the endpoints HearthController.
// refreshOutputDevices() reads, and "Play here" moves what is playing to the
// row picked - asserted on the fake device room (TestServices.useFakeRoom(),
// test_room.hpp), which is what the engine actually reopened, not only on a
// property.
//
// The room is three endpoints: "Test speakers" (default, 6 ch), "Test
// headphones" (2 ch) and "Test receiver" (8 ch, says it takes AC-3/E-AC-3
// passthrough, so it is also the passthrough section's one row). Main.qml is
// its own top-level window; mouseClick() reaches any window's items, so
// nothing here needs keyboard delivery into it (tst_shell.qml does).
TestCase {
    id: testCase
    name: "OutputPicker"
    when: windowShown

    readonly property string repo: Qt.resolvedUrl("../../../../../").toString()
    property string longEac3: ""

    Component { id: mainComponent; Main { } }

    function initTestCase() {
        verify(TestServices.useFakeRoom(), "the fake device room could not be installed");
        // Seen already, so the first-run dialog does not sit over the
        // window; tst_dialogs.qml is where it is exercised.
        HearthController.firstRunSeen = true;
        longEac3 = TestServices.stageFixture(repo + "tests/golden/external-baseline/eac3-music-stereo-96/dee.ec3",
                                             "Long stereo.ec3");
        verify(longEac3.length > 0);
    }

    function cleanup() {
        HearthController.stop();
        for (let i = HearthController.queue.length - 1; i >= 0; --i) {
            HearthController.removeAt(i);
        }
        tryVerify(function() { return HearthController.queue.length === 0; }, 10000);
    }

    function openWindow() {
        const win = createTemporaryObject(mainComponent, testCase);
        verify(win !== null);
        tryCompare(win, "visible", true);
        waitForRendering(win.contentItem);
        return win;
    }

    // Queues the long item by dropping it on the Play page and presses the
    // transport bar's Play, then waits for the device to be open.
    function playSomething(win) {
        const queueList = findChild(win, "queueList");
        verify(queueList !== null);
        verify(TestServices.dropFiles(queueList, [longEac3]), "the Play page refused the drop");
        tryVerify(function() { return HearthController.queue.length === 1; }, 10000);
        mouseClick(findChild(win, "transportPlayPause"));
        tryCompare(HearthController, "state", "playing", 10000);
        tryVerify(function() { return TestServices.device().open; }, 10000, "the engine never opened the device");
    }

    function openPicker(win) {
        const picker = findChild(win, "outputPicker");
        verify(picker !== null);
        mouseClick(findChild(win, "outputSummaryButton"));
        tryVerify(function() { return picker.opened; }, 5000, "the header summary did not open the picker");
        // Rows are laid out once refreshOutputDevices() has filled the
        // Repeater on open; click only once they have settled.
        tryVerify(function() { return rowFor(picker, "fake-receiver") !== null; }, 5000);
        waitForRendering(picker.contentItem);
        return picker;
    }

    function rowFor(picker, id) {
        return H.find(picker.contentItem, function(item) {
            return item.objectName.indexOf("outputDeviceRow-") === 0 && item.modelData.id === id;
        });
    }

    function test_headerSummaryShowsTheOutputDecisionAndOpensThePicker() {
        const win = openWindow();
        playSomething(win);
        // The summary is the output decision's own sentence once something
        // has opened, not the placeholder.
        tryVerify(function() { return HearthController.outputReason.length > 0; }, 10000);
        verify(H.textItem(findChild(win, "outputSummaryButton"), HearthController.outputReason) !== null,
               "the header summary does not show the output reason");

        const picker = openPicker(win);
        // Every endpoint of the room is a row; the default one is labelled
        // so, and the one playing now carries the "playing here" pill.
        tryVerify(function() { return HearthController.outputDevices.length === 3; }, 5000);
        verify(rowFor(picker, "fake-speakers") !== null && rowFor(picker, "fake-headphones") !== null
               && rowFor(picker, "fake-receiver") !== null, "a room endpoint has no picker row");
        verify(H.textItem(picker.contentItem, "Test speakers · default") !== null, "the default row is not marked");
        verify(H.textItem(rowFor(picker, "fake-speakers"), "playing here") !== null,
               "the open endpoint's row has no \"playing here\" pill");
        verify(H.textItem(rowFor(picker, "fake-headphones"), "playing here") === null);
        // Its summary line: width, speakers, rates.
        verify(H.textContaining(rowFor(picker, "fake-headphones"), "2 ch") !== null);
        // The passthrough section lists the one endpoint that says it takes
        // a bitstream.
        verify(H.textContaining(picker.contentItem, "AC-3 ✓ · E-AC-3 ✓") !== null,
               "the passthrough section does not list the receiver");
        // The current endpoint starts selected.
        compare(picker.selectedDeviceId, "fake-speakers");
        mouseClick(findChild(win, "outputPickerCancel"));
        tryVerify(function() { return !picker.visible; }, 5000);
    }

    function test_playHereMovesPlaybackToThePickedEndpoint() {
        const win = openWindow();
        playSomething(win);
        compare(TestServices.device().endpoint, "fake-speakers");
        const opensBefore = TestServices.device().opens;

        const picker = openPicker(win);
        mouseClick(rowFor(picker, "fake-headphones"));
        compare(picker.selectedDeviceId, "fake-headphones");
        mouseClick(findChild(win, "outputPickerPlayHere"));
        tryVerify(function() { return !picker.visible; }, 5000, "Play here did not close the picker");

        // The engine reopened on the headphones: the device says so, and so
        // does every property the window reads the output from.
        tryVerify(function() { return TestServices.device().endpoint === "fake-headphones"; }, 10000,
                  "playback never moved to the headphones (device on " + TestServices.device().endpoint + ")");
        verify(TestServices.device().opens > opensBefore);
        compare(TestServices.device().channels, 2);
        tryCompare(HearthController, "currentDeviceId", "fake-headphones", 10000);
        tryCompare(HearthController, "deviceName", "Test headphones", 10000);
        tryCompare(HearthController, "routingOutputs", 2, 10000);
        compare(HearthController.state, "playing");
        const heard = TestServices.device().framesHeard;
        tryVerify(function() { return TestServices.device().framesHeard > heard + 4800; }, 10000,
                  "the headphones are open but not playing");

        // Reopened, the picker marks the new row as the one playing.
        const again = openPicker(win);
        compare(again.selectedDeviceId, "fake-headphones");
        verify(H.textItem(rowFor(again, "fake-headphones"), "playing here") !== null);
        verify(H.textItem(rowFor(again, "fake-speakers"), "playing here") === null);
        mouseClick(findChild(win, "outputPickerCancel"));
        tryVerify(function() { return !again.visible; }, 5000);
    }

    // Picking a row and then Cancel changes nothing.
    function test_cancelLeavesTheOutputWhereItWas() {
        const win = openWindow();
        playSomething(win);
        const where = TestServices.device().endpoint;
        const opens = TestServices.device().opens;
        const picker = openPicker(win);
        mouseClick(rowFor(picker, "fake-receiver"));
        compare(picker.selectedDeviceId, "fake-receiver");
        mouseClick(findChild(win, "outputPickerCancel"));
        tryVerify(function() { return !picker.visible; }, 5000);
        // Cancel posts nothing, so there is nothing to wait for; a fresh
        // poll's worth of the engine still says the same endpoint.
        tryVerify(function() { return HearthController.currentDeviceId === where; }, 5000);
        compare(TestServices.device().endpoint, where);
        compare(TestServices.device().opens, opens);
    }

    // A group made on the Network page is a row in the picker's network
    // section (FEATURE_COVERAGE.md row 91): Play here pins playback to it,
    // the reopened picker starts on it, and a device row picked again moves
    // playback back to this computer. Main.qml starts the network itself.
    function groupRowFor(picker, id) {
        return H.find(picker.contentItem, function(item) {
            return item.objectName.indexOf("outputGroupRow-") === 0 && item.modelData.id === id;
        });
    }

    function test_groupRowPinsPlaybackToTheGroup() {
        const win = openWindow();
        const groupId = NetworkController.createGroup("Kitchen and lounge");
        verify(groupId.length > 0, "the network did not start, so no group could be made");
        const picker = openPicker(win);
        let row = null;
        tryVerify(function() { row = groupRowFor(picker, groupId); return row !== null; }, 5000,
                  "the picker lists no row for the group");
        verify(H.textContaining(row, "Kitchen and lounge") !== null);
        verify(H.textContaining(row, "No members") !== null, "an empty group's row does not say so");
        mouseClick(row);
        compare(picker.selectedGroupId, groupId);
        compare(picker.selectedDeviceId, "");
        mouseClick(findChild(win, "outputPickerPlayHere"));
        tryVerify(function() { return !picker.visible; }, 5000, "Play here did not close the picker");
        tryCompare(HearthController, "outputGroupName", groupId, 5000);

        const again = openPicker(win);
        compare(again.selectedGroupId, groupId);
        tryVerify(function() { return H.textItem(groupRowFor(again, groupId), "playing here") !== null; }, 5000,
                  "the pinned group's row has no \"playing here\" pill");
        mouseClick(rowFor(again, "fake-speakers"));
        compare(again.selectedGroupId, "");
        mouseClick(findChild(win, "outputPickerPlayHere"));
        tryVerify(function() { return !again.visible; }, 5000);
        tryCompare(HearthController, "outputGroupName", "", 5000);
        NetworkController.deleteGroup(groupId);
    }

    function test_networkPageButtonSwitchesToTheNetworkPage() {
        const win = openWindow();
        const picker = openPicker(win);
        mouseClick(findChild(win, "outputPickerNetworkPage"));
        tryVerify(function() { return !picker.visible; }, 5000);
        compare(win.page, "network");
    }

    // The Play page's signal-path card has its own "Choose..." that opens
    // the same dialog (the sidebar's copy - the monitor column's is hidden
    // at this width).
    function test_signalPathChooseOpensThePicker() {
        const win = openWindow();
        const choose = H.find(win.contentItem, function(item) {
            return item.objectName === "playChooseOutput" && item.visible;
        });
        verify(choose !== null, "no visible Choose... on the Play page");
        mouseClick(choose);
        const picker = findChild(win, "outputPicker");
        tryVerify(function() { return picker.opened; }, 5000, "Choose... did not open the picker");
        mouseClick(findChild(win, "outputPickerCancel"));
        tryVerify(function() { return !picker.visible; }, 5000);
    }
}
