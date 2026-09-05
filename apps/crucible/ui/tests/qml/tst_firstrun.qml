import QtQuick
import QtQuick.Controls
import QtTest

import Ac3ForgeCrucible

// The first-run explanation: it opens once over a fresh settings store and
// not again, says what it says in the platform seams' words, offers Send
// only where the default output moves, and every way out counts as seen.
// Nothing here presses Send: the harness isolates the settings store, not
// the machine, and that button moves the developer's own default output.
TestCase {
    id: testCase
    name: "FirstRun"
    when: windowShown

    Component { id: shell; Main {} }

    function init() {
        CrucibleController.stop();
        CrucibleController.theme = "system";
        CrucibleController.palette = "signal";
        CrucibleController.keepRunningWhenClosed = true;
        CrucibleController.moveDefaultOnLaunch = false;
        CrucibleController.firstRunAcknowledged = false;
    }

    function cleanup() {
        CrucibleController.moveDefaultOnLaunch = false;
        CrucibleController.stop();
    }

    // Main.qml defers the dialog one event-loop turn (so a capture run can
    // suppress it), which is why nothing here reads `opened` synchronously
    // after creating the window.
    function openShell(properties) {
        const window = createTemporaryObject(shell, testCase, properties || {});
        verify(window);
        tryCompare(window, "visible", true);
        return window;
    }

    function openDialog() {
        const window = openShell();
        tryVerify(function() { return window.firstRunDialogRef.opened; });
        return window;
    }

    function child(window, name) {
        const item = findChild(window.firstRunDialogRef.contentItem, name);
        verify(item, "the dialog carries objectName " + name);
        return item;
    }

    function test_opensOnceOnAFreshStore() {
        const window = openDialog();
        child(window, "firstRunLater").clicked();
        tryVerify(function() { return !window.firstRunDialogRef.opened; });
        // closed() arrives when the exit transition ends, hence tryCompare.
        tryCompare(CrucibleController, "firstRunAcknowledged", true);
        // The next launch: acknowledged, so no dialog.
        const second = openShell();
        wait(300);
        compare(second.firstRunDialogRef.opened, false);
    }

    function test_textNamesThePlatformsDevice() {
        const window = openDialog();
        const dialog = window.firstRunDialogRef;
        compare(dialog.movesDefault, CrucibleController.movesDefault && CrucibleController.silentDeviceNeeded);
        const row = child(window, "firstRunDeviceRow");
        if (!dialog.movesDefault) {
            // The platform that never moves the default (or the leg with no
            // platform half): the row says nothing changes and names no device.
            verify(row.body.indexOf("Nothing in your sound settings changes") === 0, row.body);
            return;
        }
        // The device by the platform's own name, never a literal, so this
        // holds on Windows and on the Pi alike.
        verify(row.body.indexOf(CrucibleController.nullSinkName) >= 0, row.body);
        const status = child(window, "firstRunDeviceStatus");
        verify(status.text.indexOf(CrucibleController.nullSinkName) >= 0, status.text);
        const blocker = child(window, "firstRunDeviceBlocker");
        if (CrucibleController.nullSinkPresent) {
            compare(blocker.visible, false);
        } else {
            // How this platform gets one, and what stands in the way, both
            // verbatim from the seams.
            verify(status.text.indexOf(CrucibleController.silentDeviceAdvice) >= 0, status.text);
            compare(blocker.visible, CrucibleController.silentDeviceBlocker.length > 0);
            if (CrucibleController.silentDeviceBlocker.length > 0) {
                compare(blocker.text, CrucibleController.silentDeviceBlocker);
            }
        }
    }

    function test_sendIsOfferedOnlyWhereTheDefaultMoves() {
        const window = openDialog();
        const dialog = window.firstRunDialogRef;
        const send = child(window, "firstRunSend");
        const check = child(window, "firstRunMoveOnLaunch");
        const restore = child(window, "firstRunRestoreRow");
        compare(send.visible, dialog.movesDefault);
        compare(check.visible, dialog.movesDefault);
        compare(restore.visible, dialog.movesDefault);
        if (send.visible) {
            compare(send.enabled, CrucibleController.nullSinkPresent || CrucibleController.silentDeviceCanCreate);
        }
        // Never send.clicked(): it would move this machine's default output.
    }

    function test_notNowLeavesTheDefaultAlone() {
        const window = openDialog();
        const wasNullSink = CrucibleController.defaultIsNullSink;
        const message = CrucibleController.defaultMessage;
        child(window, "firstRunLater").clicked();
        tryVerify(function() { return !window.firstRunDialogRef.opened; });
        compare(CrucibleController.defaultIsNullSink, wasNullSink);
        compare(CrucibleController.defaultMessage, message);
        compare(CrucibleController.moveDefaultOnLaunch, false);
        tryCompare(CrucibleController, "firstRunAcknowledged", true);
    }

    function test_moveOnLaunchCheckWritesTheSetting() {
        const window = openDialog();
        if (!window.firstRunDialogRef.movesDefault) {
            skip("the default output never moves on this platform: no launch-time move to offer");
        }
        const check = child(window, "firstRunMoveOnLaunch");
        compare(check.checked, false);
        check.toggled(true);
        compare(CrucibleController.moveDefaultOnLaunch, true);
        compare(check.checked, true);
        check.toggled(false);
        compare(CrucibleController.moveDefaultOnLaunch, false);
    }

    function test_openSettingsSwitchesPage() {
        const window = openDialog();
        child(window, "firstRunSettings").clicked();
        tryCompare(window, "page", "settings");
        tryVerify(function() { return !window.firstRunDialogRef.opened; });
        tryCompare(CrucibleController, "firstRunAcknowledged", true);
    }

    function test_anyCloseCountsAsSeen() {
        const window = openDialog();
        const dialog = window.firstRunDialogRef;
        // Escape is what the close policy admits. The key itself would land
        // in the TestCase's own window rather than the shell's, so the close
        // it causes is called directly and the policy is asserted beside it.
        verify((dialog.closePolicy & Popup.CloseOnEscape) !== 0, "Escape closes the dialog");
        dialog.close();
        tryVerify(function() { return !dialog.opened; });
        tryCompare(CrucibleController, "firstRunAcknowledged", true);
    }

    function test_suppressedForCaptures() {
        // A --shot run sets this before the first event-loop turn: no dialog,
        // and nothing recorded as seen.
        const window = openShell({ suppressFirstRun: true });
        wait(300);
        compare(window.firstRunDialogRef.opened, false);
        compare(CrucibleController.firstRunAcknowledged, false);
    }

    function test_migratedLaunchWaitsForTheDialog() {
        // A store carried over from the demo with the launch-time move on
        // and no acknowledgement: the automatic move waits behind the
        // dialog, and Not now leaves it for the next launch.
        CrucibleController.moveDefaultOnLaunch = true;
        const wasNullSink = CrucibleController.defaultIsNullSink;
        const message = CrucibleController.defaultMessage;
        const window = openDialog();
        compare(CrucibleController.defaultIsNullSink, wasNullSink);
        compare(CrucibleController.defaultMessage, message);
        child(window, "firstRunLater").clicked();
        tryVerify(function() { return !window.firstRunDialogRef.opened; });
        compare(CrucibleController.defaultIsNullSink, wasNullSink);
        compare(CrucibleController.moveDefaultOnLaunch, true);
    }
}
