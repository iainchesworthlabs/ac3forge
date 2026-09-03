import QtQuick
import QtTest

import Ac3ForgeDesk

// Every persisted setting round-trips through the real DeskController into
// the isolated QSettings store the harness points it at, and the driver
// block reads the machine's real state without ever launching anything.
TestCase {
    id: testCase
    name: "Settings"
    when: windowShown
    width: 1480
    height: 820

    Component { id: settingsPage; SettingsPage { width: 1480; height: 700 } }

    function init() {
        DeskController.stop();
        DeskController.pinned = "auto";
        DeskController.lowLatency = false;
        DeskController.bypassCodec = false;
        DeskController.bitrate = 0;
        DeskController.theme = "system";
        DeskController.palette = "signal";
        DeskController.nullSinkName = "Desktop Atmos";
        DeskController.keepRunningWhenClosed = true;
        DeskController.moveDefaultOnLaunch = false;
        DeskController.driverDir = "";
        DeskController.clearKey();
    }

    function test_defaults() {
        compare(DeskController.pinned, "auto");
        compare(DeskController.lowLatency, false);
        compare(DeskController.bypassCodec, false);
        compare(DeskController.bitrate, 0);
        compare(DeskController.theme, "system");
        compare(DeskController.palette, "signal");
        compare(DeskController.nullSinkName, "Desktop Atmos");
        compare(DeskController.keepRunningWhenClosed, true);
        compare(DeskController.moveDefaultOnLaunch, false);
        compare(DeskController.keyPath, "");
    }

    function test_settingsRoundTripAndNotify() {
        const spy = createTemporaryObject(spyComponent, testCase, { target: DeskController, signalName: "settingsChanged" });
        DeskController.lowLatency = true;
        DeskController.bypassCodec = true;
        DeskController.bitrate = 640;
        DeskController.theme = "dark";
        DeskController.palette = "ink";
        DeskController.nullSinkName = "Elsewhere";
        DeskController.keepRunningWhenClosed = false;
        DeskController.moveDefaultOnLaunch = true;
        DeskController.pinned = "stereo";
        compare(DeskController.lowLatency, true);
        compare(DeskController.bypassCodec, true);
        compare(DeskController.bitrate, 640);
        compare(DeskController.theme, "dark");
        compare(DeskController.palette, "ink");
        compare(DeskController.nullSinkName, "Elsewhere");
        compare(DeskController.keepRunningWhenClosed, false);
        compare(DeskController.moveDefaultOnLaunch, true);
        compare(DeskController.pinned, "stereo");
        verify(spy.count >= 9, "one settingsChanged per write, got " + spy.count);
        // Writing the same value again is not a change.
        const before = spy.count;
        DeskController.bitrate = 640;
        DeskController.theme = "dark";
        compare(spy.count, before);
    }

    function test_signingKeyPathIsRememberedNotTheKey() {
        const path = Qt.resolvedUrl("tst_settings.qml").toString();  // any existing file, as a file: URL
        DeskController.loadKey(path);
        verify(DeskController.keyPath.length > 0);
        verify(DeskController.keyPath.indexOf("file:") < 0, "a local path, not a URL: " + DeskController.keyPath);
        DeskController.clearKey();
        compare(DeskController.keyPath, "");
    }

    function test_driverBlockReadsTheMachine() {
        // The kernel answers SystemCodeIntegrityInformation on every
        // supported Windows; the values themselves are the machine's.
        verify(DeskController.codeIntegrityKnown);
        // The default folder is the source tree's driver/, which holds the
        // scripts; whether a built package is there depends on the machine,
        // and the property says which.
        verify(DeskController.driverDir.length > 0);
        compare(DeskController.driverBusy, false);
    }

    function test_driverButtonsRefuseWithoutAPackage() {
        DeskController.driverDir = "C:/no/such/folder";
        compare(DeskController.driverPackageFound, false);
        DeskController.installDriver();
        compare(DeskController.driverBusy, false);
        verify(DeskController.driverMessage.indexOf("no driver package") === 0, DeskController.driverMessage);
        DeskController.removeDriver();
        compare(DeskController.driverBusy, false);
        // Back to the default folder: the setting is removed, not stored empty.
        DeskController.driverDir = "";
        verify(DeskController.driverDir.indexOf("no/such") < 0);
    }

    function test_pageShowsTheDriverStateAndFolder() {
        const page = createTemporaryObject(settingsPage, testCase);
        verify(page);
        waitForRendering(page);
        DeskController.driverDir = "C:/no/such/folder";
        const folderInput = findChild(page, "driverFolderInput");
        verify(folderInput, "the driver folder field carries objectName driverFolderInput");
        compare(folderInput.text, DeskController.driverDir);
        const install = findChild(page, "installDriverButton");
        verify(install);
        compare(install.enabled, false);
    }

    Component { id: spyComponent; SignalSpy {} }
}
