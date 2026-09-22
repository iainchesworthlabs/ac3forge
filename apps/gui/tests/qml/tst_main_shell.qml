import QtQuick
import QtTest

import Ac3Forge

// Proves the harness itself works end to end: the Ac3Forge module this
// binary embeds resolves, Main.qml (the real shell, not a stand-in) loads
// under the offscreen platform, and EncoderController - the real singleton,
// not a mock - is reachable with no source loaded. Every other tst_*.qml
// file builds on this working; if this one fails, look here first.
TestCase {
    id: testCase
    name: "MainShell"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    function test_windowMeetsTheHandoffsFloor() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        compare(win.minimumWidth, 1280);
        compare(win.minimumHeight, 900);
        verify(win.title.length > 0);
    }

    function test_encoderControllerSingletonStartsWithNoSource() {
        compare(EncoderController.sourceReady, false);
        compare(EncoderController.sourcePath, "");
    }

    // Roadmap UX3: the Guided/Advanced/Expert tier control is a
    // SegmentedControl, always present regardless of source state, so it is
    // this suite's own natural place to prove the header actually carries
    // real accessible names rather than none at all - each segment's
    // Accessible.name comes from the same qsTr()'d label object literal the
    // visible Text renders (see tst_accessibility.qml for the fuller,
    // live-data-driven coverage of the reusable controls themselves).
    function test_headerTierControlSegmentsAreAccessible() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        const guided = findChild(win.contentItem, "seg-guided");
        const advanced = findChild(win.contentItem, "seg-advanced");
        const expert = findChild(win.contentItem, "seg-expert");
        verify(guided !== null && advanced !== null && expert !== null);
        verify(guided.Accessible.name.length > 0);
        verify(advanced.Accessible.name.length > 0);
        verify(expert.Accessible.name.length > 0);
        compare(guided.Accessible.role, Accessible.RadioButton);
    }
}
