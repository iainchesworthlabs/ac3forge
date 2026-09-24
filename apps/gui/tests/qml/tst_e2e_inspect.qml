import QtQuick
import QtTest

import Ac3Forge

// The three "open what already exists" dialogs, reached only through their
// header buttons and fed only through their own "Choose file…" pickers - the
// UI-driven counterpart to tst_qc_panel.qml / tst_stream_player.qml /
// tst_object_inspector.qml, which call measureFile()/openFile()/inspectFile()
// on the controllers directly and check the report data.
//
// Pickers: same seam as tst_e2e_encode.qml - press the real button, find the
// FileDialog it opened by its objectName, fill selectedFile, emit accepted().
//
// Playback and object audition open a real platform output (MonitorSink).
// This harness has no null sink and CI boxes typically have no audio server,
// so the Play/Audition cases assert whichever honest outcome the machine
// gives: the dialog's own error line naming the output it could not open, or
// a real playing state that the same button then stops. Neither is faked.
TestCase {
    id: testCase
    name: "E2eInspect"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    readonly property url ac3StreamUrl:
        Qt.resolvedUrl("../../../../fuzz/seeds/fuzz_wav_read/roundtrip-stereo.ac3")
    readonly property url atmosStreamUrl: Qt.resolvedUrl("../fixtures/atmos-objects.ec3")
    readonly property url notAStreamUrl:
        Qt.resolvedUrl("../../../../fuzz/seeds/fuzz_wav_read/roundtrip-stereo.wav")
    readonly property url objectsOutUrl: Qt.resolvedUrl("_test_output")

    function init() {
        QcController.presetIndex = 0;
        StreamPlayerController.pause();
        ObjectDecodeController.stopAudition();
    }

    function cleanup() {
        StreamPlayerController.pause();
        ObjectDecodeController.stopAudition();
    }

    // ---- helpers (same shape as tst_e2e_encode.qml's) --------------------

    function findByName(root, name) {
        let found = null;
        tryVerify(() => {
            found = findChild(root, name);
            return found !== null;
        }, 5000, "no object called " + name);
        return found;
    }

    // Key events from keyClick() go to the application's FOCUS window, not
    // to the window an item lives in - and the TestCase's own window starts
    // out as that. So the Main window is activated first, then the control
    // takes focus, exactly as clicking into the window would do.
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

    function pickFile(root, dialogName, url, isFolder) {
        const dialog = findByName(root, dialogName);
        tryCompare(dialog, "visible", true, 5000);
        if (isFolder) {
            dialog.selectedFolder = url;
        } else {
            dialog.selectedFile = url;
        }
        dialog.accepted();  // see pickFile
        dialog.close();
        tryCompare(dialog, "visible", false, 5000);
    }

    function openFromHeader(win, buttonName, dialog) {
        click(findByName(win.contentItem, buttonName));
        tryVerify(() => dialog.opened);
    }

    // What QcDialog's verdict chips SHOULD say, worked out here from the
    // measured numbers and the preset's own limits - not read back from the
    // controller's pass flags, so a chip that disagreed with the numbers it
    // sits beside would fail.
    function expectedLoudnessPass(p, preset) {
        if (preset.loudnessIsCeiling) {
            return p.integratedLkfs <= preset.targetLkfs + 1e-9;
        }
        return Math.abs(p.integratedLkfs - preset.targetLkfs) <= preset.toleranceLu + 1e-9;
    }

    function childTexts(item) {
        const out = [];
        (function walk(node) {
            if (node.text !== undefined && typeof node.text === "string") {
                out.push(node.text);
            }
            for (let i = 0; i < node.children.length; ++i) {
                walk(node.children[i]);
            }
        })(item);
        return out;
    }

    // ---- QC --------------------------------------------------------------

    function test_qcFromTheHeaderButtonShowsAVerdictPerPresetThatMatchesTheNumbers() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        const dialog = win.qcDialogRef;
        openFromHeader(win, "qcOpenButton", dialog);

        click(findByName(dialog.contentItem, "qcChooseFileButton"));
        pickFile(dialog, "qcFileDialog", ac3StreamUrl);
        tryCompare(QcController, "busy", false, 15000);
        compare(QcController.error, "");
        verify(QcController.filePath.endsWith("roundtrip-stereo.ac3"), QcController.filePath);

        const summary = findByName(dialog.contentItem, "qcSummaryText");
        compare(summary.text, QcController.summaryLine);
        const programmes = findByName(dialog.contentItem, "qcProgrammes");
        tryCompare(programmes, "count", 1);

        const p = QcController.programmes[0];
        compare(p.presets.length, 5);
        let sawPass = false;
        let sawFail = false;
        for (let i = 0; i < p.presets.length; ++i) {
            const preset = p.presets[i];
            const loudnessOk = expectedLoudnessPass(p, preset);
            const peakOk = p.truePeakDbtp <= preset.maxTruePeakDbtp + 1e-9;
            compare(preset.loudnessPass, loudnessOk, preset.id + " loudness");
            compare(preset.truePeakPass, peakOk, preset.id + " true peak");
            compare(preset.pass, loudnessOk && peakOk, preset.id + " overall");

            // ...and the row a person reads says the same thing, in words
            // and in colour.
            const row = findByName(dialog.contentItem, "qcPresetRow-" + preset.id);
            const texts = childTexts(row);
            verify(texts.indexOf(loudnessOk ? "loudness PASS" : "loudness FAIL") >= 0,
                   preset.id + ": " + texts);
            verify(texts.indexOf(peakOk ? "true peak PASS" : "true peak FAIL") >= 0,
                   preset.id + ": " + texts);
            const chip = findByName(dialog.contentItem, "qcVerdictChip-" + preset.id);
            compare(Qt.colorEqual(chip.color, preset.pass ? Theme.good : Theme.bad), true,
                    preset.id + " chip colour");
            verify(childTexts(chip).indexOf(preset.pass ? "PASS" : "FAIL") >= 0);
            if (preset.pass) sawPass = true; else sawFail = true;
        }
        // This fixture is loud full-scale test material: it cannot sit in
        // every delivery band at once (EBU -23 and Netflix -27 are 4 LU
        // apart with ±1/±2 tolerances), so at least one verdict is FAIL.
        verify(sawFail, "expected at least one failing preset");
        void sawPass;

        // Narrowing to one preset from the segmented control leaves exactly
        // that row, and the loudness meter picks up its band.
        const presetControl = findByName(dialog.contentItem, "qcPresetControl");
        const atsc = findByName(presetControl, "seg-2");
        click(atsc);
        compare(QcController.presetIndex, 2);
        tryCompare(QcController.programmes[0].presets, "length", 1);
        compare(QcController.programmes[0].presets[0].id, "atsc-a85");
        tryVerify(() => findChild(dialog.contentItem, "qcPresetRow-ebu-r128-s2") === null);
        const meter = findByName(dialog.contentItem, "qcLoudnessMeter");
        const a85 = QcController.programmes[0].presets[0];
        fuzzyCompare(meter.bandLow, a85.targetLkfs - a85.toleranceLu, 1e-9);
        fuzzyCompare(meter.bandHigh, a85.targetLkfs + a85.toleranceLu, 1e-9);
        compare(meter.pass, QcController.programmes[0].presets[0].loudnessPass);

        click(findByName(dialog.contentItem, "qcCloseButton"));
        tryCompare(dialog, "visible", false);
    }

    function test_qcOnAFileThatIsNotAStreamSaysSoInTheDialog() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        const dialog = win.qcDialogRef;
        openFromHeader(win, "qcOpenButton", dialog);
        click(findByName(dialog.contentItem, "qcChooseFileButton"));
        pickFile(dialog, "qcFileDialog", notAStreamUrl);
        tryCompare(QcController, "busy", false, 15000);
        verify(QcController.error.length > 0);
        compare(QcController.hasResult, false);
        const errorText = findByName(dialog.contentItem, "qcErrorText");
        tryCompare(errorText, "visible", true);
        compare(errorText.text, QcController.error);
        dialog.close();
    }

    // ---- stream player ---------------------------------------------------

    function test_playerPlayScrubAndPauseFromItsOwnControls() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        const dialog = win.streamPlayerDialogRef;
        openFromHeader(win, "streamPlayerOpenButton", dialog);
        click(findByName(dialog.contentItem, "spChooseFileButton"));
        pickFile(dialog, "spFileDialog", ac3StreamUrl);
        tryCompare(StreamPlayerController, "busy", false, 15000);
        compare(StreamPlayerController.error, "");
        verify(StreamPlayerController.summaryLine.indexOf("AC-3") === 0);
        compare(findByName(dialog.contentItem, "spSummaryText").text,
                StreamPlayerController.summaryLine);
        const duration = StreamPlayerController.durationSeconds;
        verify(duration > 0);

        // Scrub from the keyboard: a Slider with no stepSize steps by 0.1
        // of a value unit per key press (here seconds), and onMoved seeks the
        // controller there.
        const scrub = findByName(dialog.contentItem, "spScrubSlider");
        compare(scrub.to, duration);
        focusIn(scrub);
        keyClick(Qt.Key_Right);
        fuzzyCompare(StreamPlayerController.positionSeconds, 0.1, 1 / 48000);
        const positionLabel = findByName(dialog.contentItem, "spPositionLabel");
        compare(positionLabel.text, "0.1 / " + duration.toFixed(1) + " s");
        keyClick(Qt.Key_Left);
        compare(StreamPlayerController.positionSeconds, 0);

        const play = findByName(dialog.contentItem, "spPlayButton");
        compare(play.text, "Play");
        click(play);
        tryVerify(() => StreamPlayerController.playing
                        || StreamPlayerController.error.length > 0, 5000);
        if (StreamPlayerController.error.length > 0) {
            // No output on this machine: the press is refused visibly, the
            // button stays a Play button, and nothing claims to be playing.
            verify(StreamPlayerController.error.indexOf("Could not open the playback output") === 0,
                   StreamPlayerController.error);
            tryCompare(findByName(dialog.contentItem, "spErrorText"), "visible", true);
            compare(StreamPlayerController.playing, false);
            compare(play.text, "Play");
        } else {
            compare(play.text, "Pause");
            click(play);
            tryCompare(StreamPlayerController, "playing", false, 5000);
            compare(play.text, "Play");
        }
        // Closing the dialog always stops playback (onVisibleChanged).
        click(findByName(dialog.contentItem, "spCloseButton"));
        tryCompare(dialog, "visible", false);
        compare(StreamPlayerController.playing, false);
    }

    function test_playerExportsOneWavPerObjectFromAnAtmosStream() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        const dialog = win.streamPlayerDialogRef;
        openFromHeader(win, "streamPlayerOpenButton", dialog);
        click(findByName(dialog.contentItem, "spChooseFileButton"));
        pickFile(dialog, "spFileDialog", atmosStreamUrl);
        tryCompare(StreamPlayerController, "busy", false, 15000);
        compare(StreamPlayerController.hasObjects, true);
        const soundfield = findByName(dialog.contentItem, "spSoundfield");
        compare(soundfield.visible, true);

        const exportObjects = findByName(dialog.contentItem, "spExportObjectsButton");
        compare(exportObjects.visible, true);
        click(exportObjects);
        pickFile(dialog, "spExportObjectsDialog", objectsOutUrl, true);
        tryCompare(StreamPlayerController, "exporting", false, 30000);
        compare(StreamPlayerController.exportError, "");
        dialog.close();
    }

    // ---- object inspector ------------------------------------------------

    function test_inspectorChosenFromItsButtonListsObjectsAndScrubsFrames() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        const dialog = win.objectInspectorDialogRef;
        openFromHeader(win, "objectInspectorOpenButton", dialog);
        click(findByName(dialog.contentItem, "oiChooseFileButton"));
        pickFile(dialog, "objFileDialog", atmosStreamUrl);
        tryCompare(ObjectDecodeController, "busy", false, 15000);
        compare(ObjectDecodeController.error, "");
        verify(ObjectDecodeController.frameCount > 1);

        const firstObjects = ObjectDecodeController.frames[0].objects;
        const rows = findByName(dialog.contentItem, "oiObjectRows");
        tryCompare(rows, "count", firstObjects.length);
        compare(findByName(dialog.contentItem, "oiSummaryText").text,
                ObjectDecodeController.summaryLine);

        // Scrub to the last frame from the keyboard (Right, one frame per
        // press - stepSize 1): the rows now describe the LAST frame's
        // positions.
        const scrub = findByName(dialog.contentItem, "oiScrubSlider");
        focusIn(scrub);
        const last = ObjectDecodeController.frameCount - 1;
        for (let i = 0; i < last; ++i) {
            keyClick(Qt.Key_Right);
        }
        tryCompare(dialog, "frameIndex", last);
        const frameLabel = findByName(dialog.contentItem, "oiFrameLabel");
        verify(frameLabel.text.indexOf("frame " + (last + 1) + "/" + (last + 1)) > 0,
               frameLabel.text);
        const lastObjects = ObjectDecodeController.frames[last].objects;
        const row0 = findByName(dialog.contentItem, "oiObjectRow-0");
        const expected = "x " + lastObjects[0].x.toFixed(2) + ", y "
                         + lastObjects[0].y.toFixed(2) + ", z " + lastObjects[0].z.toFixed(2);
        verify(row0.Accessible.description.indexOf(expected) === 0,
               row0.Accessible.description + " vs " + expected);

        // Audition the first object: a real output, or a visible refusal.
        // Pressed from the keyboard: at the window's own 1280px floor the
        // button sits mostly past the dialog's right edge, so a click at its
        // centre lands on the modal dimmer (see
        // test_auditionButtonsFitInsideTheInspectorDialog below).
        const audition = findByName(dialog.contentItem, "oiAuditionButton-0");
        focusIn(audition);
        keyClick(Qt.Key_Space);
        tryVerify(() => ObjectDecodeController.auditioningIndex === 0
                        || ObjectDecodeController.error.length > 0, 5000);
        if (ObjectDecodeController.error.length > 0) {
            verify(ObjectDecodeController.error.indexOf("Could not open the audition output") === 0,
                   ObjectDecodeController.error);
            tryCompare(findByName(dialog.contentItem, "oiErrorText"), "visible", true);
        } else {
            focusIn(audition);
            keyClick(Qt.Key_Space);
            tryCompare(ObjectDecodeController, "auditioningIndex", -1, 5000);
        }
        click(findByName(dialog.contentItem, "oiCloseButton"));
        tryCompare(dialog, "visible", false);
    }

    // Regression: ObjectInspectorDialog was 900px wide, but its room plan
    // (340px) beside an object row of fixed-width columns plus the Audition
    // button needs ~1300px, so at the window's 1280x900 minimum each row's
    // Audition button ran past the dialog's right edge and a click at its
    // centre landed on the modal dimmer. The room and the object list now
    // stack when they do not fit side by side; the button must be inside the
    // dialog and a real mouse click on it must reach it.
    function test_auditionButtonsFitInsideTheInspectorDialog() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        const dialog = win.objectInspectorDialogRef;
        openFromHeader(win, "objectInspectorOpenButton", dialog);
        click(findByName(dialog.contentItem, "oiChooseFileButton"));
        pickFile(dialog, "objFileDialog", atmosStreamUrl);
        tryCompare(ObjectDecodeController, "busy", false, 15000);
        const audition = findByName(dialog.contentItem, "oiAuditionButton-0");
        settle(audition);
        const right = audition.mapToItem(null, audition.width, 0).x;
        const dialogRight = dialog.x + dialog.width;
        verify(right <= dialogRight,
               "Audition button's right edge at " + right + "px, dialog ends at "
               + dialogRight + "px");
        click(audition);
        tryVerify(() => ObjectDecodeController.auditioningIndex === 0
                        || ObjectDecodeController.error.length > 0, 5000);
        if (ObjectDecodeController.auditioningIndex === 0) {
            click(audition);
            tryCompare(ObjectDecodeController, "auditioningIndex", -1, 5000);
        }
        dialog.close();
    }
}
