import QtQuick
import QtTest

import Ac3ForgeCrucible

// Every persisted setting round-trips through the real CrucibleController into
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
        CrucibleController.stop();
        CrucibleController.pinned = "auto";
        CrucibleController.lowLatency = false;
        CrucibleController.bypassCodec = false;
        CrucibleController.splitStereo = false;
        CrucibleController.bitrate = 0;
        CrucibleController.theme = "system";
        CrucibleController.palette = "signal";
        CrucibleController.nullSinkName = "Desktop Atmos";
        CrucibleController.keepRunningWhenClosed = true;
        CrucibleController.moveDefaultOnLaunch = false;
        CrucibleController.driverDir = "";
        CrucibleController.clearKey();
    }

    function test_defaults() {
        compare(CrucibleController.pinned, "auto");
        compare(CrucibleController.lowLatency, false);
        compare(CrucibleController.bypassCodec, false);
        compare(CrucibleController.bitrate, 0);
        compare(CrucibleController.theme, "system");
        compare(CrucibleController.palette, "signal");
        compare(CrucibleController.nullSinkName, "Desktop Atmos");
        compare(CrucibleController.keepRunningWhenClosed, true);
        compare(CrucibleController.moveDefaultOnLaunch, false);
        compare(CrucibleController.keyPath, "");
    }

    function test_settingsRoundTripAndNotify() {
        const spy = createTemporaryObject(spyComponent, testCase, { target: CrucibleController, signalName: "settingsChanged" });
        CrucibleController.lowLatency = true;
        CrucibleController.bypassCodec = true;
        CrucibleController.bitrate = 640;
        CrucibleController.theme = "dark";
        CrucibleController.palette = "ink";
        CrucibleController.nullSinkName = "Elsewhere";
        CrucibleController.keepRunningWhenClosed = false;
        CrucibleController.moveDefaultOnLaunch = true;
        CrucibleController.pinned = "stereo";
        CrucibleController.splitStereo = true;
        compare(CrucibleController.lowLatency, true);
        compare(CrucibleController.bypassCodec, true);
        compare(CrucibleController.splitStereo, true);
        compare(CrucibleController.bitrate, 640);
        compare(CrucibleController.theme, "dark");
        compare(CrucibleController.palette, "ink");
        compare(CrucibleController.nullSinkName, "Elsewhere");
        compare(CrucibleController.keepRunningWhenClosed, false);
        compare(CrucibleController.moveDefaultOnLaunch, true);
        compare(CrucibleController.pinned, "stereo");
        verify(spy.count >= 9, "one settingsChanged per write, got " + spy.count);
        // Writing the same value again is not a change.
        const before = spy.count;
        CrucibleController.bitrate = 640;
        CrucibleController.theme = "dark";
        compare(spy.count, before);
    }

    function test_signingKeyPathIsRememberedNotTheKey() {
        const path = Qt.resolvedUrl("tst_settings.qml").toString();  // any existing file, as a file: URL
        CrucibleController.loadKey(path);
        verify(CrucibleController.keyPath.length > 0);
        verify(CrucibleController.keyPath.indexOf("file:") < 0, "a local path, not a URL: " + CrucibleController.keyPath);
        CrucibleController.clearKey();
        compare(CrucibleController.keyPath, "");
    }

    function test_driverBlockReadsTheMachine() {
        // The kernel answers SystemCodeIntegrityInformation on every
        // supported Windows; the values themselves are the machine's.
        verify(CrucibleController.codeIntegrityKnown);
        // The default folder is the source tree's driver/, which holds the
        // scripts; whether a built package is there depends on the machine,
        // and the property says which.
        verify(CrucibleController.driverDir.length > 0);
        compare(CrucibleController.driverBusy, false);
    }

    function test_driverButtonsRefuseWithoutAPackage() {
        CrucibleController.driverDir = "C:/no/such/folder";
        compare(CrucibleController.driverPackageFound, false);
        CrucibleController.installDriver();
        compare(CrucibleController.driverBusy, false);
        verify(CrucibleController.driverMessage.indexOf("no driver package") === 0, CrucibleController.driverMessage);
        CrucibleController.removeDriver();
        compare(CrucibleController.driverBusy, false);
        // Back to the default folder: the setting is removed, not stored empty.
        CrucibleController.driverDir = "";
        verify(CrucibleController.driverDir.indexOf("no/such") < 0);
    }

    function test_pageShowsTheDriverStateAndFolder() {
        const page = createTemporaryObject(settingsPage, testCase);
        verify(page);
        waitForRendering(page);
        CrucibleController.driverDir = "C:/no/such/folder";
        const folderInput = findChild(page, "driverFolderInput");
        verify(folderInput, "the driver folder field carries objectName driverFolderInput");
        compare(folderInput.text, CrucibleController.driverDir);
        const install = findChild(page, "installDriverButton");
        verify(install);
        compare(install.enabled, false);
    }

    Component { id: spyComponent; SignalSpy {} }
}
