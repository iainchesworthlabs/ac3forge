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
// Three of them live here. The tray, which asks Qt the same question on every
// platform, and whose menu must stay flat for a reason the Linux file
// records. The silent device, whose name and whose advice are the platform's
// own words and were Windows' words everywhere on the first Linux run. And
// the listing rule, the sentence the room shows about which applications
// appear in it, which the platforms disagree about because they are really
// different.
//
// A third platform joined on 2026-09-06 and none of it has run. macOS's arms
// below assert the sentences and the flags its seams return, which is what a
// suite can hold without a machine; nothing here has been executed on a Mac,
// and passing on the macOS CI leg would say that the seams say what they were
// written to say and nothing about whether any of it works
// (docs/crucible/promotion.md, "What cannot be verified, and why").
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
    // Qt's documented value for macOS is "osx". Both spellings are accepted
    // because nobody here can run this to see which one Qt 6 hands back, and
    // getting it wrong fails quietly rather than loudly: every macOS case
    // below would skip, and a suite of skips reads as a pass. If the macOS
    // cases are all skipping on a Mac, this line is the first thing to check.
    readonly property bool isMacos: Qt.platform.os === "osx" || Qt.platform.os === "macos"

    Component { id: settingsPage; SettingsPage { width: 1480; height: 700 } }
    Component { id: shell; Main {} }

    // Qt's own answer to "has this session somewhere to put an icon", which
    // both platforms' seams now defer to. Invisible: this asks the question,
    // it does not publish anything.
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

    function test_linuxFollowsTheDesktopsTray() {
        if (!isLinux) skip("the Linux tray answer is the Linux file's");
        // Linux publishes a tray again, and asks Qt the same question
        // Windows does, so the seam and Qt must not drift apart. It refused
        // for a while: publishing a StatusNotifierItem killed the window,
        // and ui/platform/linux/tray_support.cpp carries that record and
        // what it was - a Qt type confusion reached only through a Menu
        // nested inside the tray's menu, which is why
        // test_theTrayMenuNestsNoSubmenu below is the assertion that matters
        // now.
        const probe = createTemporaryObject(trayProbe, testCase);
        verify(probe, "a SystemTrayIcon was created");
        compare(CrucibleController.trayAvailable, probe.available);
        if (!CrucibleController.trayAvailable) {
            // A session with no StatusNotifier host - a headless runner is
            // one - greys the setting and says so.
            const reason = CrucibleController.trayAbsentReason;
            verify(reason.indexOf("no system tray") >= 0, reason);
            verify(reason.indexOf("quits") >= 0, reason);
        }
    }

    function test_theTrayMenuNestsNoSubmenu() {
        // The one thing left over from the crash that took the Linux tray
        // away, and the reason that file is still worth reading:
        // QDBusPlatformMenu implements no createSubMenu(), so a Qt.labs
        // Menu nested inside a tray icon's menu is handed Qt Labs Platform's
        // QWidget fallback and then static_cast to the D-Bus one. The window
        // dies on the panel's first request for its layout - nine or ten
        // launches in ten, measured both ways on two machines
        // (ui/platform/linux/tray_support.cpp). The menu is flat everywhere
        // rather than on one platform, because one shape is worth more than
        // a submenu. This is here so that adding one back fails a test
        // rather than a person's session.
        const shellItem = createTemporaryObject(shell, testCase.parent);
        verify(shellItem, "the window was created");
        const trayIcon = findChild(shellItem, "tray");
        verify(trayIcon, "the tray carries objectName tray");
        verify(trayIcon.menu, "the tray has a menu");
        verify(trayIcon.menu.items.length > 0, "the tray menu has items");
        for (let i = 0; i < trayIcon.menu.items.length; ++i) {
            // A nested Menu shows up as the item that opens it, carrying
            // subMenu; a plain item's is null. That is the thing to assert
            // rather than the QML type, because it is what Qt hands the
            // platform.
            const item = trayIcon.menu.items[i];
            verify(!item.subMenu,
                   "the tray menu nests a submenu at item " + i + ": " + item.text);
        }
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

    function test_macosFollowsTheMenuBar() {
        if (!isMacos) skip("the menu bar status area is the macOS file's");
        // Qt's own question is the right one here, as it is on Windows and
        // now on Linux: the menu bar is where an NSStatusItem goes, and a
        // session without one - the offscreen platform these suites run
        // under - answers no. The Linux refusal that this file was written
        // beside is gone, and the fault behind it lands one rung short of
        // this platform rather than nowhere near it
        // (ui/platform/macos/tray_support.cpp carries the reasoning, and the
        // reproducer to run before trusting any of it).
        const probe = createTemporaryObject(trayProbe, testCase);
        verify(probe, "a SystemTrayIcon was created");
        compare(CrucibleController.trayAvailable, probe.available);
        if (!CrucibleController.trayAvailable) {
            verify(CrucibleController.trayAbsentReason.indexOf("menu bar") >= 0,
                   CrucibleController.trayAbsentReason);
            verify(CrucibleController.trayAbsentReason.indexOf("quits") >= 0,
                   CrucibleController.trayAbsentReason);
        }
    }

    function test_macosNeedsNoSilentDeviceAndMovesNothing() {
        if (!isMacos) skip("the no-device answer is the macOS file's");
        // The one platform where the silent device is absent because nothing
        // needs it, rather than missing. A process tap carries
        // muteBehavior, so each application is silenced where it is tapped:
        // nothing is installed, nothing is created, and the system default
        // output is never touched.
        compare(CrucibleController.silentDeviceNeeded, false);
        compare(CrucibleController.silentDeviceFromPackage, false);
        compare(CrucibleController.silentDeviceCanCreate, false);
        compare(CrucibleController.movesDefault, false);
        // The sentence a person is shown has to read as "nothing to do",
        // not as a missing feature: it says so, and it says what happens
        // instead. Fragments and not the whole sentence, so rewording is free
        // and dropping either half is not.
        const advice = CrucibleController.silentDeviceAdvice;
        verify(advice.indexOf("nothing to install") >= 0, advice);
        verify(advice.indexOf("tap") >= 0, advice);
        verify(advice.indexOf("driver") < 0,
               "macOS needs no driver and must not be told to install one: " + advice);
    }

    function test_macosCannotAnswerTheFullScreenQuestion() {
        if (!isMacos) skip("the foreground answer is the macOS file's");
        // NSWorkspace names the application in front; AppKit gives one
        // application no way to ask about another's windows, so there is no
        // full-screen answer here and the seam says so rather than reporting
        // the frontmost pid as if it were one. The same distinction the
        // Wayland arm holds on Linux: "nothing is full-screen" and "this
        // platform cannot tell" are different claims.
        //
        // The report is where that reaches a person (diagnostics.cpp's
        // "full-screen rule" row). Only the unavailability is asserted, not
        // the sentence: this suite never starts the engine, so the seam has
        // not been polled and the reason is still its before-the-first-poll
        // one rather than either of the two NSWorkspace decides between.
        const report = CrucibleController.diagnosticsReport();
        const label = "full-screen rule: ";  // diagnostics.cpp's row() writes "<name>: <value>"
        const at = report.indexOf(label);
        verify(at >= 0, report);
        const value = report.substring(at + label.length);
        verify(value.indexOf("unavailable") === 0,
               "the macOS foreground seam must report itself unavailable, with a reason: " +
               value.substring(0, 120));
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
        } else if (isMacos) {
            // No endpoint, so no name for one. Empty rather than a
            // placeholder, because a placeholder would put a device in the
            // room's vocabulary that is not on the machine; every consumer
            // guards an empty search before using it
            // (engine/platform/macos/virtual_device.cpp lists them).
            compare(CrucibleController.nullSinkName, "");
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
        // Two of the three need a device at all; macOS's own case above holds
        // the third answer, and this one asserts nothing about a device that
        // is not there.
        compare(CrucibleController.silentDeviceNeeded, !isMacos);
        if (isMacos) {
            return;
        }
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
        // it. The platforms disagree because they differ - a Windows session
        // outlives the sound, a PipeWire stream does not - and the room says
        // its own platform's answer rather than asserting Windows'
        // everywhere.
        const rule = CrucibleController.listingRule;
        verify(rule.length > 0, "the platform gave no listing rule");
        if (isLinux) {
            verify(rule.indexOf("while it is playing") >= 0, rule);
            verify(rule.indexOf("PipeWire") >= 0, rule);
        } else if (isWindows) {
            verify(rule.indexOf("with a window is listed") >= 0, rule);
            verify(rule.indexOf("greyed") >= 0, rule);
        } else if (isMacos && rule.indexOf("older than 14.0") >= 0) {
            // Below the version floor: Core Audio has no process object class
            // before macOS 14.0 and this build's deployment target is 13.3
            // (cmake/toolchains/macos.llvm.toolchain.cmake), so a machine that
            // can launch it but not list on it is reachable. The rule says
            // that rather than describing a list nobody will see.
            verify(rule.indexOf("no application can be listed") >= 0, rule);
        } else if (isMacos) {
            // Closer to Linux's than to Windows': the list follows what is
            // using the sound hardware, not what has a window. The greying
            // half is asserted too, because on this platform the two are
            // separate questions - Core Audio holds a process object while a
            // process is using audio and answers separately about whether
            // sound is coming out of it now. Which of those a paused player
            // does is exactly what one launch on a Mac would settle, so the
            // sentence claims neither and neither does this.
            verify(rule.indexOf("sound hardware") >= 0, rule);
            verify(rule.indexOf("greyed") >= 0, rule);
        }
        // Both say what happens to something already placed, because that is
        // the question a person actually has when a row disappears.
        verify(rule.indexOf("placed") >= 0, rule);
    }
}
