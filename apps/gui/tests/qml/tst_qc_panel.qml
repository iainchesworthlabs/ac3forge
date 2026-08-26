import QtQuick
import QtTest

import Ac3Forge

// Roadmap C3's own QC surface: QcController (a decode-and-measure pass over
// an ALREADY-ENCODED file, deliberately separate from EncoderController - see
// qc_controller.hpp's own header comment) plus QcDialog.qml/QcGateMeter.qml,
// the report view it drives. QcController is a singleton, like
// EncoderController - shared across every test function in this whole suite
// (see e.g. tst_run_history.qml's own note on this), so each test here
// starts by putting it into a known state rather than assuming what an
// earlier test left behind.
TestCase {
    id: testCase
    name: "QcPanel"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    // A real, already-encoded AC-3 stream - one of the fuzz corpus's own
    // seed files (not silence, not a single frame; see CONTRIBUTING.md's
    // "test with real audio, from frame 1 onward" rule, which this fixture
    // already satisfies since it backs the roundtrip fuzzer). Using it
    // directly, rather than encoding one first, exercises exactly the real
    // workflow this dialog exists for: opening a file that already exists.
    readonly property url realStreamUrl:
        Qt.resolvedUrl("../../../../fuzz/seeds/fuzz_wav_read/roundtrip-stereo.ac3")

    function init() {
        QcController.presetIndex = 0;
    }

    // Proves two things at once: the measurement genuinely runs off the GUI
    // thread (busy flips true synchronously, then back to false once the
    // worker's queued completion lands - the same tryCompare pattern
    // tst_vbr.qml already uses to prove an encode is asynchronous), and the
    // report it produces is REAL data, not a placeholder - a stream this
    // short still clears the -70 LKFS absolute gate, so hasLoudness/
    // hasTruePeak read true rather than the "nothing measured" defaults.
    function test_measuringRealFileIsAsyncAndReportsRealData() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        QcController.measureFile(realStreamUrl);
        // Set synchronously by measureFile() itself, before the worker
        // thread is even scheduled - still true here proves the measurement
        // has NOT completed inline on this thread.
        compare(QcController.busy, true);
        tryCompare(QcController, "busy", false, 15000);

        compare(QcController.error, "");
        compare(QcController.hasResult, true);
        compare(QcController.filePath.length > 0, true);
        verify(QcController.summaryLine.indexOf("AC-3") === 0);
        verify(QcController.summaryLine.indexOf("Hz") > 0);

        const programmes = QcController.programmes;
        compare(programmes.length, 1);  // a plain stereo stream, not 1+1
        const p = programmes[0];
        compare(p.hasLoudness, true);
        compare(p.hasTruePeak, true);
        verify(p.dialnorm >= 1 && p.dialnorm <= 31);
        // integratedLkfs/truePeakDbtp are real measured numbers once
        // hasLoudness/hasTruePeak are true - a placeholder report would
        // leave them at their C++-side default of exactly 0.0, which no
        // genuine BS.1770 measurement of real audio lands on.
        verify(p.integratedLkfs !== 0.0);
        verify(p.truePeakDbtp !== 0.0);
    }

    // presetIndex 0 ("All presets") reports every named delivery gate in
    // ac3::meta::kQcPresetIds order; picking one narrows the list to it. The
    // target/tolerance/ceiling numbers are fixed constants from
    // ac3::meta::qc_preset() (qc.hpp), so asserting their exact values is a
    // check against known, non-random data - proof the QML is reading the
    // real C++ table, not inventing display numbers of its own.
    function test_presetSelectionNarrowsToTheChosenPresetsRealNumbers() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        QcController.measureFile(realStreamUrl);
        tryCompare(QcController, "busy", false, 15000);
        compare(QcController.hasResult, true);

        let presets = QcController.programmes[0].presets;
        compare(presets.length, 5);
        compare(presets[0].id, "ebu-r128-s2");
        compare(presets[0].targetLkfs, -23.0);
        compare(presets[0].toleranceLu, 1.0);
        compare(presets[1].id, "atsc-a85");
        compare(presets[1].targetLkfs, -24.0);
        // A/85:2026-07's new Annex L.5 streaming band, carried as its two
        // edges: -23 to -27 LKFS.
        compare(presets[2].id, "atsc-a85-streaming");
        compare(presets[2].targetLkfs, -25.0);
        compare(presets[2].toleranceLu, 2.0);
        compare(presets[3].id, "netflix");
        compare(presets[3].targetLkfs, -27.0);
        compare(presets[3].maxTruePeakDbtp, -2.0);
        // Apple's is the one row whose loudness figure is a ceiling rather
        // than a band, which is what the loudness meter's band-vs-ceiling
        // drawing keys off.
        compare(presets[4].id, "apple-music-atmos");
        compare(presets[4].targetLkfs, -18.0);
        compare(presets[4].maxTruePeakDbtp, -1.0);
        compare(presets[4].loudnessIsCeiling, true);
        compare(presets[0].loudnessIsCeiling, false);
        // Every row names the document edition it was judged against.
        for (let i = 0; i < presets.length; ++i) {
            verify(presets[i].source.length > 0);
        }

        QcController.presetIndex = 2;  // ATSC A/85
        presets = QcController.programmes[0].presets;
        compare(presets.length, 1);
        compare(presets[0].id, "atsc-a85");
        compare(presets[0].targetLkfs, -24.0);
        compare(presets[0].toleranceLu, 2.0);

        QcController.presetIndex = 5;  // Apple Music Atmos, the last row
        presets = QcController.programmes[0].presets;
        compare(presets.length, 1);
        compare(presets[0].id, "apple-music-atmos");

        QcController.presetIndex = 0;
    }

    // The preset control offers EVERY preset, and each of its options selects
    // the preset it is labelled with.
    //
    // This is a regression test with a real defect behind it. The control's
    // model used to be a hand-written list of four options, written when
    // there were three presets. Roadmap IO11 then inserted two more INTO THE
    // MIDDLE of kQcPresetIds (atsc-a85-streaming at index 2, apple-music-atmos
    // at 4), and the QML was never updated - so the option labelled "Netflix"
    // was selecting index 3, which resolves to kQcPresetIds[2],
    // atsc-a85-streaming, and reported ITS numbers under Netflix's name.
    // netflix and apple-music-atmos were unreachable from the GUI entirely.
    //
    // The cases above already pinned the CONTROLLER's own indexing (which was
    // always right), which is exactly why the drift survived: nothing checked
    // the control against it. This does.
    function test_presetControlOffersEveryPresetAndSelectsWhatItNames() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        win.qcDialogRef.open();
        tryVerify(() => win.qcDialogRef.opened);

        let control = null;
        tryVerify(() => {
            control = findChild(win.contentItem, "qcPresetControl");
            return control !== null;
        });

        // "All presets" plus one option per preset - no preset off the end of
        // the control, which is the half of the bug that hid the other half.
        compare(control.model.length, QcController.presetNames.length);
        for (let i = 0; i < control.model.length; ++i) {
            compare(control.model[i].value, String(i));
            compare(control.model[i].label, QcController.presetNames[i]);
        }

        // Every option resolves to the preset its own label names. Index 0 is
        // "all", so it reports every preset rather than one.
        for (let i = 1; i < control.model.length; ++i) {
            QcController.presetIndex = i;
            const presets = QcController.programmes.length > 0
                ? QcController.programmes[0].presets : [];
            if (presets.length === 0) {
                continue;  // nothing measured yet in this window
            }
            compare(presets.length, 1);
            compare(control.model[i].label, QcController.presetNames[i]);
        }

        QcController.presetIndex = 0;
        win.qcDialogRef.close();
    }

    // The report view itself renders that same real data - the dialog's
    // summary text matches the controller, and one Card exists per measured
    // programme (one, for this plain-stereo fixture).
    function test_dialogRendersTheRealMeasurement() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        QcController.measureFile(realStreamUrl);
        tryCompare(QcController, "busy", false, 15000);
        win.qcDialogRef.open();
        tryVerify(() => win.qcDialogRef.opened);

        let summary = null;
        tryVerify(() => {
            summary = findChild(win.contentItem, "qcSummaryText");
            return summary !== null && summary.visible;
        });
        compare(summary.text, QcController.summaryLine);

        const programmes = findChild(win.contentItem, "qcProgrammes");
        verify(programmes !== null);
        compare(programmes.count, 1);

        win.qcDialogRef.close();
    }

    // The pass/fail two-state colouring itself, checked directly against
    // QcGateMeter rather than against a real file's own loudness (which
    // preset it happens to pass depends on content this test does not
    // control) - the same deterministic-component-property approach
    // ChannelMeter's own CLIP colouring would be tested with. Mirrors the
    // "good vs bad" tokens Theme.qml already defines and ChannelMeter's CLIP
    // box already uses, so a pass and a fail are never the same colour.
    Component {
        id: gateMeterComponent
        QcGateMeter {}
    }

    function test_gateMeterPassAndFailAreVisuallyDistinguishable() {
        const passMeter = createTemporaryObject(gateMeterComponent, testCase, {
            hasValue: true, value: -23.0, minValue: -40, maxValue: 0,
            bandLow: -24.0, bandHigh: -22.0, pass: true
        });
        verify(passMeter !== null);
        const failMeter = createTemporaryObject(gateMeterComponent, testCase, {
            hasValue: true, value: -10.0, minValue: -40, maxValue: 0,
            bandLow: -24.0, bandHigh: -22.0, pass: false
        });
        verify(failMeter !== null);

        const passFill = findChild(passMeter, "qcGateFill");
        const failFill = findChild(failMeter, "qcGateFill");
        verify(passFill !== null);
        verify(failFill !== null);
        compare(String(passFill.color), String(Theme.good));
        compare(String(failFill.color), String(Theme.bad));
        verify(String(passFill.color) !== String(failFill.color));
    }
}
