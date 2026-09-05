import QtQuick
import QtTest
// The platform module Main.qml already imports, for its SystemTrayIcon: the
// window's own tray item is one of these, and `available` on it is Qt's
// answer to a different question from the seam's (see below).
import Qt.labs.platform as Platform

import Ac3ForgeCrucible
import Ac3ForgeCrucibleLanguage

// The platform seams the window itself reads, asserted on the platform that
// answers them - which is the only place they can be. The Catch2 suite links
// tests/crucible/platform_services_stub.cpp, so no platform directory is
// compiled into it at all; these suites run the real controller over the real
// seams, so this file is where ui/platform/<os>/tray_support.cpp and the
// Linux VirtualDevice are reached.
//
// Three of them live here. The tray, which is a defect being worked around
// rather than a preference and needs saying so. The silent device, whose
// name and whose advice are the platform's own words and were Windows' words
// everywhere on the first Linux run. And the listing rule, the sentence the
// room shows about which applications appear in it, which the two platforms
// disagree about because they are really different.
//
// Nothing here starts the engine or touches an audio device, and nothing
// installs or creates anything: install()/create is what puts a real node in
// a real graph, and a test may not.
TestCase {
    id: testCase
    name: "Platform"
    when: windowShown
    width: 1480
    height: 820

    readonly property bool isLinux: Qt.platform.os === "linux"
    readonly property bool isWindows: Qt.platform.os === "windows"

    Component { id: settingsPage; SettingsPage { width: 1480; height: 700 } }

    // Created only where the platform publishes a tray. On Linux publishing
    // one takes the process down (ui/platform/linux/tray_support.cpp), and
    // while an invisible item is not a published one, a suite that builds
    // one anyway would be a step towards finding that out the hard way.
    Component { id: trayProbe; Platform.SystemTrayIcon { visible: false } }

    function init() {
        // The seam sentences below are read as their source strings, so this
        // suite reads the same text on a machine whose locale is not English
        // - the harness applies the system language at startup
        // (ui/tests/qml_test_main.cpp). tst_language.qml does the same.
        LanguageManager.setLanguage("en");
        CrucibleController.refreshDriver();
    }

    function test_theTraySeamNeverRefusesWithoutSayingWhy() {
        // Read as types first. A property that was renamed or dropped comes
        // back undefined from QML rather than failing, and every conditional
        // in this file would then quietly take its other arm and pass.
        verify(typeof CrucibleController.trayAvailable === "boolean",
               "trayAvailable is " + typeof CrucibleController.trayAvailable);
        verify(typeof CrucibleController.trayAbsentReason === "string",
               "trayAbsentReason is " + typeof CrucibleController.trayAbsentReason);

        // The rule the header states, whichever platform is answering: a
        // tray that is not published carries a sentence the Settings page
        // can print, and one that is published carries none. A greyed
        // setting with no reason beside it is the failure this prevents.
        if (CrucibleController.trayAvailable) {
            compare(CrucibleController.trayAbsentReason, "");
        } else {
            verify(CrucibleController.trayAbsentReason.length > 0,
                   "the tray is not published and no reason was given");
        }
    }

    function test_linuxPublishesNoTrayAndSaysItIsAFault() {
        if (!isLinux) skip("the Linux tray answer is the Linux file's");
        // Not a preference and not a missing feature. Publishing a
        // StatusNotifierItem from this window kills the process: SIGBUS on
        // the main thread inside libQt6Gui, read off a Raspberry Pi 4B
        // (labwc, wf-panel-pi, Qt 6.8.2) on 2026-09-06, surviving 0 to 2
        // launches in ten with the tray and 10 in 10 without it. The icon
        // format, the menu's contents, the icon provider, accessibility, the
        // render loop and the RTKit client were each ruled out by
        // measurement; ui/platform/linux/tray_support.cpp carries the whole
        // record. This assertion is here so that a change which starts
        // publishing one again fails a test rather than a person's session.
        compare(CrucibleController.trayAvailable, false);
        verify(CrucibleController.trayAbsentReason.length > 0);
        // The two things the sentence has to leave a person knowing: that
        // there is no tray here, and that closing the window therefore quits
        // rather than hiding. Fragments and not the whole sentence, so
        // rewording it is free and dropping either fact is not.
        const reason = CrucibleController.trayAbsentReason;
        verify(reason.indexOf("no tray icon") >= 0, reason);
        verify(reason.indexOf("quits") >= 0, reason);

        // And the answer is this build's, not the desktop's. Qt's own
        // question has a different answer on a desktop with a StatusNotifier
        // host - the Pi's own panel owns org.kde.StatusNotifierWatcher and
        // says yes - which is exactly why the window asks the seam instead.
        // Deliberately not asserted as "Qt says yes here": a headless runner
        // has no panel and says no, and the case worth pinning is that ours
        // is false whatever Qt answers.
    }

    function test_windowsFollowsTheNotificationArea() {
        if (!isWindows) skip("the notification area is the Windows file's");
        // Here Qt's question IS the right one, so the seam is Qt's answer
        // and the two must not drift apart.
        const probe = createTemporaryObject(trayProbe, testCase);
        verify(probe, "a SystemTrayIcon was created");
        compare(CrucibleController.trayAvailable, probe.available);
        if (!CrucibleController.trayAvailable) {
            // A session with the notification area turned off: the setting
            // greys and says so rather than offering a hiding place that is
            // not there.
            verify(CrucibleController.trayAbsentReason.indexOf("notification area") >= 0,
                   CrucibleController.trayAbsentReason);
        }
    }

    function test_theKeepRunningRowFollowsTheTraySeam() {
        // Parented to the window's root item rather than to the TestCase,
        // which is invisible by design (tst_settings.qml).
        const page = createTemporaryObject(settingsPage, testCase.parent);
        verify(page);
        waitForRendering(page);
        const check = findChild(page, "keepRunningCheck");
        verify(check, "the keep-running row carries objectName keepRunningCheck");
        // The setting is offered exactly where there is somewhere to keep the
        // window, and the note under it is the platform's own sentence rather
        // than a second copy written in QML.
        compare(check.enabled, CrucibleController.trayAvailable);
        compare(check.note, CrucibleController.trayAbsentReason);
        if (!CrucibleController.trayAvailable) {
            // Whatever the stored setting says, a window with no tray cannot
            // be left running behind one, so the box does not show ticked.
            CrucibleController.keepRunningWhenClosed = true;
            compare(check.checked, false);
        }
    }

    function test_theSilentDeviceIsNamedAndExplainedByThePlatform() {
        // The name the platform's own sound settings show, and what the
        // engine matches by. It reaches the window as the default of the
        // nullSinkName setting, which is why this case does not write that
        // setting first: the first Linux screenshot labelled every station
        // "Desktop Atmos" on a machine that has never had one, because the
        // name lived in the window as a default rather than in the platform
        // that owns it.
        if (isLinux) {
            compare(CrucibleController.nullSinkName, "Crucible (silent)");
        } else if (isWindows) {
            compare(CrucibleController.nullSinkName, "Desktop Atmos");
        }

        // One sentence on how a person gets one, shown by the signal path
        // when there is none. "Install the driver" is Windows advice and
        // wrong everywhere else, which is the whole reason this is a seam.
        const advice = CrucibleController.silentDeviceAdvice;
        verify(advice.length > 0, "the platform gave no advice");
        if (isLinux) {
            verify(advice.indexOf("driver") < 0,
                   "Linux needs no driver and must not be told to install one: " + advice);
            verify(advice.indexOf("nothing to install") >= 0, advice);
        } else if (isWindows) {
            verify(advice.indexOf("driver") >= 0, advice);
        }
    }

    function test_thePlatformSaysWhetherItsDeviceComesFromAPackage() {
        // Windows installs a signed kernel driver from a folder; Linux asks
        // the daemon to make a node and can undo it by exiting. The Settings
        // page shows a folder, a package state and the driver wording only
        // in the first case (tst_settings.qml asserts the page; this asserts
        // the seam under it).
        verify(typeof CrucibleController.silentDeviceFromPackage === "boolean");
        compare(CrucibleController.silentDeviceNeeded, true);
        if (isLinux) {
            compare(CrucibleController.silentDeviceFromPackage, false);
            // Nothing is installed yet in this process, so the application
            // can make one - which is what the first Send does.
            compare(CrucibleController.silentDeviceCanCreate, true);
        } else if (isWindows) {
            compare(CrucibleController.silentDeviceFromPackage, true);
            // A driver is installed from a package, never created, so the
            // "create" branch of the UI is not offered here.
            compare(CrucibleController.silentDeviceCanCreate, false);
        }
        // Whatever the platform reports, nothing is running: refreshDriver()
        // only reads. An install or a remove is the one thing these suites
        // never start.
        compare(CrucibleController.driverBusy, false);
    }

    function test_theListingRuleIsThePlatformsOwn() {
        // The paragraph the room shows about which applications appear in
        // it. The two platforms disagree because they really differ - a
        // Windows session outlives the sound, a PipeWire stream does not -
        // and the room says its own platform's answer rather than asserting
        // Windows' everywhere.
        const rule = CrucibleController.listingRule;
        verify(rule.length > 0, "the platform gave no listing rule");
        if (isLinux) {
            verify(rule.indexOf("while it is playing") >= 0, rule);
            verify(rule.indexOf("PipeWire") >= 0, rule);
        } else if (isWindows) {
            verify(rule.indexOf("with a window is listed") >= 0, rule);
            verify(rule.indexOf("greyed") >= 0, rule);
        }
        // Both say what happens to something already placed, because that is
        // the question a person actually has when a row disappears.
        verify(rule.indexOf("placed") >= 0, rule);
    }
}
