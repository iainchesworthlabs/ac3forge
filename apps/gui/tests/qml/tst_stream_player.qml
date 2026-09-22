import QtQuick
import QtTest

import Ac3Forge

// StreamPlayerController (roadmap UX1) - the GUI twin of `ac3cli monitor`/
// `ac3cli decode`, opened from the header's "Open stream…" button and from a
// finished run chip's own "More…" menu (see tst_run_history.qml's sibling
// coverage of the encode side that menu's items reach into). Like
// tst_object_inspector.qml's own note: the playback path itself (play()/
// pause(), which opens a real MonitorSink) is not exercised here - it opens
// a real platform audio device this offscreen suite does not assume is
// present. seek() IS exercised: while nothing is playing it is a pure
// in-memory position update, no device involved.
TestCase {
    id: testCase
    name: "StreamPlayer"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    // The same real, already-encoded Dolby Atmos E-AC-3 fixture
    // tst_object_inspector.qml uses - see that file's own comment on why
    // this one and not the similarly-named fuzz corpus seed.
    readonly property url atmosStreamUrl:
        Qt.resolvedUrl("../fixtures/atmos-objects.ec3")
    readonly property url wavFixtureUrl:
        Qt.resolvedUrl("../../../../fuzz/seeds/fuzz_wav_read/roundtrip-stereo.wav")
    readonly property url outputUrl:
        Qt.resolvedUrl("_test_output/tst_stream_player.ec3")

    function init() {
        StreamPlayerController.pause();
    }

    function test_openingRealAtmosStreamDecodesAsyncAndReportsObjects() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        StreamPlayerController.openFile(atmosStreamUrl);
        compare(StreamPlayerController.busy, true);
        tryCompare(StreamPlayerController, "busy", false, 15000);

        compare(StreamPlayerController.error, "");
        compare(StreamPlayerController.hasResult, true);
        verify(StreamPlayerController.summaryLine.indexOf("E-AC-3") === 0);
        verify(StreamPlayerController.summaryLine.indexOf("Hz") > 0);
        compare(StreamPlayerController.hasObjects, true);
        verify(StreamPlayerController.objectCount > 0);
        verify(StreamPlayerController.durationSeconds > 0);
    }

    function test_dialogRendersOneMeterRowPerChannelAndOffersObjectExport() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        StreamPlayerController.openFile(atmosStreamUrl);
        tryCompare(StreamPlayerController, "busy", false, 15000);
        compare(StreamPlayerController.hasResult, true);

        win.streamPlayerDialogRef.open();
        tryVerify(() => win.streamPlayerDialogRef.opened);

        const meterRows = findChild(win.contentItem, "spMeterRows");
        verify(meterRows !== null);
        compare(meterRows.count, StreamPlayerController.channelMeta.length);

        const exportObjectsButton = findChild(win.contentItem, "spExportObjectsButton");
        verify(exportObjectsButton !== null);
        compare(exportObjectsButton.visible, true);

        const summary = findChild(win.contentItem, "spSummaryText");
        verify(summary !== null);
        compare(summary.text, StreamPlayerController.summaryLine);

        win.streamPlayerDialogRef.close();
    }

    // seek() while nothing is playing is a pure in-memory position update -
    // see StreamPlayerController::seek's own comment - so this is safe to
    // exercise without an audio device.
    function test_seekWhilePausedMovesPositionImmediately() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        StreamPlayerController.openFile(atmosStreamUrl);
        tryCompare(StreamPlayerController, "busy", false, 15000);
        compare(StreamPlayerController.playing, false);

        const target = StreamPlayerController.durationSeconds / 2;
        StreamPlayerController.seek(target);
        compare(StreamPlayerController.positionSeconds, target);

        StreamPlayerController.seek(-5);
        compare(StreamPlayerController.positionSeconds, 0);

        StreamPlayerController.seek(StreamPlayerController.durationSeconds + 5);
        compare(StreamPlayerController.positionSeconds, StreamPlayerController.durationSeconds);
    }

    // A finished run's own "More…" menu (Main.qml's runMoreMenu) is the run-
    // chip shortcut docs/gui/qc.md and docs/gui/inspect-objects.md used to
    // both say did not exist yet. The menu itself is a Popup, so its
    // MenuItems are not Item-derived and findChild - which only walks
    // Item.children, the same reason every OTHER findChild in this suite
    // targets a Button/Text/Repeater/etc - can never locate one; Main.qml's
    // window.openRunInQc/openRunInInspector exist specifically so this test
    // (and any other caller) can invoke exactly what a MenuItem's own
    // onTriggered does without reaching into the live popup. The button that
    // opens the menu IS an ordinary Item, so its own presence/gating is
    // checked directly.
    function test_runChipMoreMenuOpensQcAndInspectForThisRunsFile() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        EncoderController.loadSourceFile(wavFixtureUrl);
        tryCompare(EncoderController, "sourceReady", true);
        EncoderController.codecIndex = 1;  // E-AC-3, so runMoreInspect- is visible
        EncoderController.encodeTo(outputUrl);
        tryCompare(EncoderController, "busy", false, 10000);
        EncoderController.codecIndex = 0;

        const run = EncoderController.runs[0];
        compare(run.status, "done");
        compare(run.eac3, true);

        const moreButton = findChild(win.contentItem, "runMore-" + run.id);
        verify(moreButton !== null);
        compare(moreButton.visible, true);

        win.openRunInQc(run.path);
        tryVerify(() => win.qcDialogRef.opened);
        tryCompare(QcController, "busy", false, 15000);
        compare(QcController.filePath, run.path);
        win.qcDialogRef.close();

        win.openRunInInspector(run.path);
        tryVerify(() => win.objectInspectorDialogRef.opened);
        tryCompare(ObjectDecodeController, "busy", false, 15000);
        compare(ObjectDecodeController.filePath, run.path);
        win.objectInspectorDialogRef.close();
    }
}
