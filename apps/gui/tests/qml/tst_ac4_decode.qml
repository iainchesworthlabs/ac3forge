import QtQuick
import QtTest

import ForgeGui

// The AC-4 decode pages over a committed stream of eight presentations
// (tests/golden/ac4dec/presentations/encoder-hybrid.ac4: presentation 0 is
// 5.1 and what the decoder takes with no preference, presentation 2 is
// stereo). Each page is opened from the header's own button, the file comes
// through the page's own picker, and the presentation through the page's own
// picker by the keyboard, as `presentation=<n>` chooses it on the command
// line.
TestCase {
    id: testCase
    name: "Ac4Decode"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    readonly property url streamUrl:
        Qt.resolvedUrl("../../../../tests/golden/ac4dec/presentations/encoder-hybrid.ac4")

    // Every test ends with the pickers back at the decoder's own choice, and
    // setting the index reads the open file again on a worker. That has to be
    // finished before the next test starts: measureFile() and openFile() are
    // refused while their controller is busy, so a pick landing on a reload
    // still running would be dropped without a word. The wait before the
    // reset is for a test that failed part-way, whose own job may still be
    // running; a reset that arrives while busy is ignored as well.
    function cleanup() {
        StreamPlayerController.pause();
        settleControllers();
        QcController.presentationIndex = -1;
        StreamPlayerController.presentationIndex = -1;
        settleControllers();
    }

    function settleControllers() {
        tryCompare(QcController, "busy", false, 15000);
        tryCompare(StreamPlayerController, "busy", false, 15000);
        tryCompare(ObjectDecodeController, "busy", false, 15000);
    }

    // The last thing this suite does is leave a measurement, a decode and an
    // inspection running, so the three controllers are destroyed with work in
    // flight, as they are when the window is closed during one. Each worker
    // posts its result back to its controller when it finishes. A controller
    // that did not wait for its worker left that post addressed to freed
    // memory, and ~QGuiApplication crashed on it in most runs, after every test
    // had passed. Nothing is asserted: the process exiting with code 0 is the
    // check.
    function cleanupTestCase() {
        QcController.measureFile(streamUrl);
        StreamPlayerController.openFile(streamUrl);
        ObjectDecodeController.inspectFile(streamUrl);
    }

    function findByName(root, name) {
        let found = null;
        tryVerify(() => {
            found = findChild(root, name);
            return found !== null;
        }, 5000, "no object called " + name);
        return found;
    }

    function focusIn(item) {
        const w = item.Window.window;
        w.requestActivate();
        tryVerify(() => w.active, 5000, "window never became active");
        item.forceActiveFocus();
        tryVerify(() => item.activeFocus, 5000, "control never took focus");
    }

    function settle(item) {
        waitForRendering(item);
        let last = "";
        tryVerify(() => {
            const p = item.mapToItem(null, 0, 0);
            const key = p.x + "," + p.y + "," + item.width + "," + item.height;
            const stable = item.width > 0 && key === last;
            last = key;
            return stable;
        }, 5000, "item never settled");
    }

    function click(item) {
        settle(item);
        mouseClick(item);
    }

    function pickFile(root, dialogName, url) {
        const dialog = findByName(root, dialogName);
        tryCompare(dialog, "visible", true, 5000);
        dialog.selectedFile = url;
        dialog.accepted();
        dialog.close();
        tryCompare(dialog, "visible", false, 5000);
    }

    function test_qcMeasuresThePresentationItsPickerChooses() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        click(findByName(win.contentItem, "qcOpenButton"));
        tryVerify(() => win.qcDialogRef.opened);
        const dialog = win.qcDialogRef.contentItem;
        click(findByName(dialog, "qcChooseFileButton"));
        pickFile(win.qcDialogRef, "qcFileDialog", streamUrl);
        tryCompare(QcController, "busy", false, 15000);
        compare(QcController.error, "");
        compare(QcController.isAc4, true);
        compare(QcController.presentationNames.length, 8);
        compare(QcController.measuredPresentation, 0);
        const firstSummary = QcController.summaryLine;

        const picker = findByName(dialog, "qcPresentation");
        tryCompare(picker, "visible", true);
        compare(picker.currentIndex, 0);
        compare(picker.count, 9);
        // Each step is a pick, measured before the picker takes the next.
        for (let step = 0; step < 3; ++step) {
            focusIn(picker);
            keyClick(Qt.Key_Down);
            compare(QcController.presentationIndex, step);
            tryCompare(QcController, "busy", false, 15000);
            tryCompare(picker, "currentIndex", step + 1);
        }
        compare(QcController.error, "");
        compare(QcController.measuredPresentation, 2);
        verify(QcController.summaryLine !== firstSummary, QcController.summaryLine);
        compare(QcController.programmes.length, 1);
        compare(QcController.programmes[0].hasDialnorm, true);
        win.qcDialogRef.close();
    }

    function test_playerPlaysThePresentationItsPickerChooses() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        click(findByName(win.contentItem, "streamPlayerOpenButton"));
        tryVerify(() => win.streamPlayerDialogRef.opened);
        const dialog = win.streamPlayerDialogRef.contentItem;
        click(findByName(dialog, "spChooseFileButton"));
        pickFile(win.streamPlayerDialogRef, "spFileDialog", streamUrl);
        tryCompare(StreamPlayerController, "busy", false, 15000);
        compare(StreamPlayerController.error, "");
        compare(StreamPlayerController.isAc4, true);
        compare(StreamPlayerController.decodedPresentation, 0);
        compare(StreamPlayerController.channelMeta.length, 6);

        const picker = findByName(dialog, "spPresentation");
        tryCompare(picker, "visible", true);
        for (let step = 0; step < 3; ++step) {
            focusIn(picker);
            keyClick(Qt.Key_Down);
            compare(StreamPlayerController.presentationIndex, step);
            tryCompare(StreamPlayerController, "busy", false, 15000);
            tryCompare(picker, "currentIndex", step + 1);
        }
        compare(StreamPlayerController.error, "");
        compare(StreamPlayerController.decodedPresentation, 2);
        compare(StreamPlayerController.channelMeta.length, 2);
        compare(findByName(dialog, "spMeterRows").count, 2);
        win.streamPlayerDialogRef.close();
    }

    function test_objectPageListsWhatTheDecoderReports() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        click(findByName(win.contentItem, "objectInspectorOpenButton"));
        tryVerify(() => win.objectInspectorDialogRef.opened);
        const dialog = win.objectInspectorDialogRef.contentItem;
        click(findByName(dialog, "oiChooseFileButton"));
        pickFile(win.objectInspectorDialogRef, "objFileDialog", streamUrl);
        tryCompare(ObjectDecodeController, "busy", false, 15000);
        compare(ObjectDecodeController.error, "");
        compare(ObjectDecodeController.isAc4, true);
        compare(ObjectDecodeController.decodedPresentation, 0);
        const rows = findByName(dialog, "oiAc4Presentations");
        compare(rows.count, 8);
        const names = ObjectDecodeController.presentationNames;
        verify(names[0].indexOf("0: L R C LFE Ls Rs") === 0, names[0]);
        verify(names[0].indexOf("id 1") > 0, names[0]);
        verify(names[2].indexOf("2: L R") === 0, names[2]);
        // A channel-based stream has beds as channels, and no objects.
        compare(ObjectDecodeController.ac4BedObjects, 0);
        compare(ObjectDecodeController.ac4DynamicObjects, 0);
        compare(findByName(dialog, "oiAc4Block").visible, true);
        compare(findByName(dialog, "oiAc4ExportNote").visible, true);
        win.objectInspectorDialogRef.close();
    }
}
