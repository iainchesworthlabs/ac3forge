import QtQuick
import QtTest

import Ac3Forge

// Roadmap UX2 - the rail's own DropArea and `ac3gui <file...>`'s launch-time
// argv handling (main.cpp) both funnel through Main.qml's own
// window.openDroppedFile(url), so this suite exercises that single dispatch
// point directly rather than trying to simulate an OS-level drag-and-drop or
// spawn a second ac3gui process - the same "call the window function
// directly" shape tst_stream_player.qml's own run-chip "More…" menu test
// uses, and for the identical reason: a DropArea's onDropped handler is a
// thin wrapper around this call, not where the interesting behaviour lives.
TestCase {
    id: testCase
    name: "DesktopIntegration"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    readonly property url wavFixtureUrl:
        Qt.resolvedUrl("../../../../fuzz/seeds/fuzz_wav_read/roundtrip-stereo.wav")
    // The same real Atmos E-AC-3 fixture tst_stream_player.qml/
    // tst_object_inspector.qml already use - .ec3 exercises the same
    // ".ac3"/".ec3" branch a plain .ac3 file would, and reusing it avoids a
    // third copy of the fixture.
    readonly property url ec3FixtureUrl:
        Qt.resolvedUrl("../fixtures/atmos-objects.ec3")

    function test_droppingAWavAddsItAsASource() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        // EncoderController is a singleton shared across every test
        // function in this suite (the same reason tst_object_inspector.qml
        // and tst_run_history.qml's own tests never assume a fresh/empty
        // state either) - a prior test's own source(s) may already be
        // loaded, so this asserts growth by exactly one rather than an
        // absolute count.
        const before = EncoderController.sourceModel.length;

        win.openDroppedFile(wavFixtureUrl);
        tryCompare(EncoderController, "sourceReady", true);

        compare(EncoderController.sourceModel.length, before + 1);
    }

    function test_droppingASecondWavAddsRatherThanReplaces() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        win.openDroppedFile(wavFixtureUrl);
        tryCompare(EncoderController, "sourceReady", true);
        const before = EncoderController.sourceModel.length;

        win.openDroppedFile(wavFixtureUrl);
        tryVerify(() => EncoderController.sourceModel.length === before + 1);
    }

    function test_droppingAnEc3StreamOpensTheStreamPlayer() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        compare(win.streamPlayerDialogRef.opened, false);

        win.openDroppedFile(ec3FixtureUrl);

        tryVerify(() => win.streamPlayerDialogRef.opened);
        tryCompare(StreamPlayerController, "busy", false, 15000);
        compare(StreamPlayerController.hasResult, true);
        verify(StreamPlayerController.filePath.indexOf("atmos-objects.ec3") >= 0);
    }

    function test_windowExposesADropAreaAcceptingFileUrls() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        const dropArea = findChild(win.contentItem, "windowDropArea");
        verify(dropArea !== null);
        verify(dropArea.keys.indexOf("text/uri-list") >= 0);
    }
}
