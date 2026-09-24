import QtQuick
import QtTest

import Ac3ForgeHearth
import Ac3ForgeHearthTest

import "HearthTestHelpers.js" as H

// The window's dialogs (FEATURE_COVERAGE.md rows 87-90): the first-run
// dialog on a first start and its two ways out, and the "? -> Keyboard
// shortcuts -> About -> Licences" chain Main.qml wires together, each hop
// through the button a person would press.
//
// First-run state lives in HearthController.firstRunSeen, persisted through
// the suite's own throwaway QSettings (qml_test_main.cpp), so each case sets
// it before it opens the window rather than relying on what an earlier
// case (alphabetical order) left behind.
TestCase {
    id: testCase
    name: "Dialogs"
    when: windowShown

    Component { id: mainComponent; Main { } }

    function initTestCase() {
        verify(TestServices.useFakeRoom(), "the fake device room could not be installed");
    }

    function openWindow() {
        const win = createTemporaryObject(mainComponent, testCase);
        verify(win !== null);
        tryCompare(win, "visible", true);
        waitForRendering(win.contentItem);
        return win;
    }

    function test_firstRunShowsOnceAndNotNowRemembersIt() {
        HearthController.firstRunSeen = false;
        const win = openWindow();
        const notNow = findChild(win, "firstRunNotNow");
        verify(notNow !== null);
        // Opened one event-loop turn after the window comes up.
        tryVerify(function() { return notNow.visible; }, 5000, "the first-run dialog did not open on a first start");
        // Its first step names the output it will play to.
        verify(findChild(win, "firstRunOutputRow") !== null);
        mouseClick(notNow);
        tryVerify(function() { return !notNow.visible; }, 5000);
        tryCompare(HearthController, "firstRunSeen", true, 2000);
        compare(win.page, "play");

        // A second window on the same settings does not show it again.
        const again = openWindow();
        const againNotNow = findChild(again, "firstRunNotNow");
        // Give the window its Qt.callLater() turn before reading.
        waitForRendering(again.contentItem);
        verify(!againNotNow.visible, "the first-run dialog showed again after Not now");
    }

    function test_firstRunOpenSpeakersGoesToTheSpeakersPage() {
        HearthController.firstRunSeen = false;
        const win = openWindow();
        const open = findChild(win, "firstRunOpenSpeakers");
        verify(open !== null);
        tryVerify(function() { return open.visible; }, 5000, "the first-run dialog did not open on a first start");
        mouseClick(open);
        tryVerify(function() { return !open.visible; }, 5000);
        compare(win.page, "speakers");
        tryCompare(HearthController, "firstRunSeen", true, 2000);
    }

    function test_shortcutsAboutLicencesChain() {
        HearthController.firstRunSeen = true;
        const win = openWindow();
        const shortcuts = findChild(win, "shortcutsDialog");
        const about = findChild(win, "aboutDialog");
        const licences = findChild(win, "licencesDialog");
        verify(shortcuts !== null && about !== null && licences !== null);

        mouseClick(findChild(win, "helpButton"));
        tryVerify(function() { return shortcuts.opened; }, 5000, "? did not open Keyboard shortcuts");
        // It lists every page's shortcut.
        verify(H.textItem(shortcuts.contentItem, "Media information") !== null);

        mouseClick(findChild(win, "shortcutsAboutButton"));
        tryVerify(function() { return about.opened; }, 5000, "About... did not open About");
        // The version line is built from HearthController.versionDetails,
        // which is never empty (ac3::version_details()).
        verify(HearthController.versionDetails.length > 0);
        verify(H.textItem(about.contentItem, "AC3Forge Hearth") !== null);
        verify(H.textItem(about.contentItem, about.compactVersion(HearthController.versionDetails)) !== null,
               "About does not show the version");

        mouseClick(findChild(win, "aboutLicencesButton"));
        tryVerify(function() { return licences.opened; }, 5000, "Licences... did not open Licences");
        const notices = findChild(win, "licenceNoticesText");
        verify(notices !== null);
        verify(notices.text.length > 0, "the licences view is empty");
        compare(notices.text, HearthController.licenceNotices);

        mouseClick(findChild(win, "licencesCloseButton"));
        tryVerify(function() { return !licences.visible; }, 5000);
        mouseClick(findChild(win, "aboutCloseButton"));
        tryVerify(function() { return !about.visible; }, 5000);
        mouseClick(findChild(win, "shortcutsCloseButton"));
        tryVerify(function() { return !shortcuts.visible; }, 5000);
    }
}
