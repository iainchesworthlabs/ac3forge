import QtQuick
import QtTest
// The platform module Main.qml already imports, so its StandardPaths is
// deployed beside the window wherever the window is; QtCore's would not be.
import Qt.labs.platform as Platform

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
        // The silent device is a real thing on this platform, so the block
        // shows at all. On macOS it would not: the taps mute each
        // application where they tap it and there is no device to install.
        verify(CrucibleController.silentDeviceNeeded);
        // The blocker is the platform's own sentence and is empty when
        // nothing is in the way, so its content is the machine's business
        // and not this test's - what matters is that it is a string the
        // view can show and that the detail lines came through.
        verify(typeof CrucibleController.silentDeviceBlocker === "string");
        verify(CrucibleController.silentDeviceDetail.length > 0);
        // The default folder is the source tree's driver/, which holds the
        // scripts; whether a built package is there depends on the machine,
        // and the property says which.
        verify(CrucibleController.driverDir.length > 0);
        compare(CrucibleController.driverBusy, false);
    }

    function test_driverButtonsRefuseWithoutAPackage() {
        // Only where the device is a driver from a folder. Elsewhere the
        // application makes it, a folder means nothing, and "found" is the
        // platform's word for "can make one" - test_pageShowsTheDriverState
        // covers that shape.
        if (!CrucibleController.silentDeviceFromPackage)
            skip("no driver package on this platform: the application makes the silent device itself");
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
        // Parented to the window's root item, not to the TestCase: the
        // TestCase item is invisible by design, and an Item's visible reads
        // the effective value, so nothing under it can ever be seen. The
        // other tests here only read enabled and text and never noticed.
        const page = createTemporaryObject(settingsPage, testCase.parent);
        verify(page);
        waitForRendering(page);
        // The two-stage note names the device the platform owns, whatever
        // it is called there: the first Linux screenshot said "Desktop
        // Atmos" on a machine that has never had one.
        const note = findChild(page, "silentDeviceNote");
        verify(note, "the two-stage note carries objectName silentDeviceNote");
        verify(note.text.indexOf(CrucibleController.nullSinkName) >= 0, note.text);
        // Open Advanced, so what follows is about what is shown, not about
        // a section that is closed on every platform alike. Set, not
        // clicked: the toggle sits below a 700-pixel page, and a synthetic
        // click on something outside the window opens nothing.
        const advanced = findChild(page, "advancedSection");
        verify(advanced, "the Advanced section carries objectName advancedSection");
        advanced.open = true;
        waitForRendering(page);
        const folderRow = findChild(page, "driverFolderRow");
        verify(folderRow, "the driver folder row carries objectName driverFolderRow");
        const folderInput = findChild(page, "driverFolderInput");
        verify(folderInput, "the driver folder field carries objectName driverFolderInput");
        const install = findChild(page, "installDriverButton");
        verify(install);
        const remove = findChild(page, "removeDriverButton");
        verify(remove);
        if (CrucibleController.silentDeviceFromPackage) {
            // A driver from a folder: the folder is shown, follows the
            // setting, and a bogus one leaves nothing to install.
            verify(folderRow.visible, "the driver folder is shown where the device is a driver");
            CrucibleController.driverDir = "C:/no/such/folder";
            compare(folderInput.text, CrucibleController.driverDir);
            compare(install.enabled, false);
            compare(install.text, "Install driver");
            compare(remove.text, "Remove driver");
        } else {
            // The application's own device: no folder, and the buttons say
            // create and remove, not install.
            verify(!folderRow.visible, "no driver folder where the application makes the device");
            compare(install.text, "Create device");
            compare(remove.text, "Remove device");
            compare(install.enabled, CrucibleController.driverPackageFound && !CrucibleController.driverBusy);
        }
    }

    function test_diagnosticsReportWithholdsTheKeyPath() {
        // A chosen key file: the path is remembered (in the store) and the
        // report says so without carrying it in any spelling, nor the file's
        // own name.
        CrucibleController.loadKey(Qt.resolvedUrl("tst_settings.qml").toString());
        verify(CrucibleController.keyPath.length > 0);
        const report = CrucibleController.diagnosticsReport();
        verify(report.indexOf(CrucibleController.keyPath) < 0, "the native path is in the report");
        verify(report.indexOf(CrucibleController.keyPath.replace(/\\/g, "/")) < 0, "the forward-slash path is in the report");
        verify(report.indexOf("tst_settings.qml") < 0, "the key file's name is in the report");
        verify(report.indexOf("signing/keyPath = <withheld>") >= 0, report);
        verify(report.indexOf("key source: a file chosen in Settings (path withheld)") >= 0, report);
        verify(report.indexOf("output/pinned = auto") >= 0, report);
        verify(report.indexOf("# platform") >= 0, report);
        verify(report.indexOf("# recent messages") >= 0, report);
        verify(report.indexOf("signing key file chosen (path withheld)") >= 0, report);
    }

    function test_exportWritesAFile() {
        // The system temporary folder as a file: URL, whichever form the
        // platform module hands back.
        let base = String(Platform.StandardPaths.writableLocation(Platform.StandardPaths.TempLocation));
        if (base.indexOf("file:") !== 0)
            base = "file://" + (base.indexOf("/") === 0 ? "" : "/") + base;
        const url = base + "/crucible-test-diagnostics.txt";
        verify(CrucibleController.exportDiagnostics(url), CrucibleController.diagnosticsMessage);
        verify(CrucibleController.diagnosticsMessage.indexOf("saved to") === 0, CrucibleController.diagnosticsMessage);
        const suggested = CrucibleController.suggestedDiagnosticsFile();
        verify(suggested.indexOf("file:") === 0, suggested);
        verify(suggested.indexOf("crucible-diagnostics-") > 0, suggested);
        verify(suggested.endsWith(".txt"), suggested);
        // A folder that does not exist is refused with a reason.
        verify(!CrucibleController.exportDiagnostics("file:///no/such/dir/x.txt"));
        verify(CrucibleController.diagnosticsMessage.indexOf("could not write") === 0, CrucibleController.diagnosticsMessage);
    }

    function test_pageCarriesTheDiagnosticsButton() {
        // Parented to the window's root item for the reason
        // test_pageShowsTheDriverStateAndFolder gives.
        const page = createTemporaryObject(settingsPage, testCase.parent);
        verify(page);
        waitForRendering(page);
        const button = findChild(page, "exportDiagnosticsButton");
        verify(button, "the export button carries objectName exportDiagnosticsButton");
        verify(button.enabled);
        compare(button.text, "Save diagnostics…");
        const message = findChild(page, "diagnosticsMessage");
        verify(message, "the outcome note carries objectName diagnosticsMessage");
        // Shown only once an export has run; whether one has depends on
        // which tests ran before this one in the same process.
        compare(message.visible, CrucibleController.diagnosticsMessage.length > 0);
    }

    Component { id: spyComponent; SignalSpy {} }
}
