import QtQuick
import QtTest

import Ac3ForgeCrucible

// The notices the About box's Licences view shows are the NOTICES.txt this
// build installs, embedded at build time, so what they say has to agree
// with the two build facts the controller already exposes: which platform
// seam made the silent device, and whether the room's 3D view was compiled
// in. No test here names an operating system; the seam does.
TestCase {
    id: testCase
    name: "About"
    when: windowShown

    Component { id: shell; Main {} }

    function init() {
        CrucibleController.stop();
        CrucibleController.keepRunningWhenClosed = true;
        CrucibleController.moveDefaultOnLaunch = false;
    }

    function cleanup() {
        CrucibleController.stop();
    }

    function test_noticesDescribeThisBuild() {
        const notices = CrucibleController.licenceNotices;
        // The whole file, not the fallback sentence the controller returns
        // when a binary skipped the :/notices resource.
        verify(notices.length > 2000, "the embedded notices are the whole file; got " + notices.length + " characters: " + notices.substring(0, 120));
        verify(notices.indexOf("AC3Forge Crucible") === 0, notices.substring(0, 80));
        // Both platforms compile {fmt} in and embed the OFL faces.
        verify(notices.indexOf("{fmt}") >= 0, "the {fmt} section is missing");
        verify(notices.indexOf("SIL OPEN FONT LICENSE") >= 0, "the OFL text is missing");
    }

    function test_noticesFollowTheSilentDeviceSeam() {
        const notices = CrucibleController.licenceNotices;
        // The MS-PL section exists exactly where the silent device is a
        // driver from a package; the PipeWire section exactly where the
        // application makes the device itself.
        compare(notices.indexOf("Microsoft Public License") >= 0, CrucibleController.silentDeviceFromPackage,
                "the driver's MS-PL section follows silentDeviceFromPackage");
        compare(notices.indexOf("libpipewire") >= 0, !CrucibleController.silentDeviceFromPackage,
                "the PipeWire section follows !silentDeviceFromPackage");
    }

    function test_quick3dNoticeMatchesTheBuild() {
        // The GPL-3 section is present iff the room's 3D view was compiled in.
        compare(CrucibleController.licenceNotices.indexOf("Qt Quick 3D") >= 0, CrucibleController.has3D,
                "the Qt Quick 3D section follows has3D");
    }

    function test_licencesOpenFromAbout() {
        const window = createTemporaryObject(shell, testCase);
        verify(window);
        tryCompare(window, "visible", true);
        window.openAbout();
        tryCompare(window.aboutDialog, "opened", true);
        window.openLicences();
        tryCompare(window.licencesDialog, "opened", true);
        const textArea = findChild(window.licencesDialog.contentItem, "licenceNoticesText");
        verify(textArea, "the notices text carries objectName licenceNoticesText");
        compare(textArea.text, CrucibleController.licenceNotices);
        // The two stack: closing Licences leaves About open beneath it.
        window.licencesDialog.close();
        tryCompare(window.licencesDialog, "opened", false);
        verify(window.aboutDialog.opened, "closing Licences leaves About open");
        window.aboutDialog.close();
        tryCompare(window.aboutDialog, "opened", false);
    }
}
