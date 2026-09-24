import QtQuick
import QtTest

import Ac3Forge

// The whole file workflow, driven the way a person drives it: the first-run
// card or the rail's own button opens the WAV picker, the Encode button opens
// the save picker with the planned name already in it, the run strip reports
// the finished run, and the header's "Open stream…" decodes what was written.
// Every step goes through the real EncoderController / StreamPlayerController
// singletons - nothing here calls encodeTo() or loadSourceFile() itself.
//
// The pickers are the only seam: a FileDialog under the offscreen platform
// has no person to click in it, so each test presses the real button, finds
// the picker that button opened (the objectName each FileDialog carries for
// exactly this), fills selectedFile and emits the picker's accepted() signal - which runs
// the same onAccepted handler a real pick would (see pickFile on why the
// signal and not accept()).
//
// The fixture is 49152 frames of 48 kHz stereo float (1.024 s): exactly 32
// AC-3 frames of 1536 samples, and at the default 192 kbps each frame is
// 192000 * 1536 / 48000 / 8 = 768 bytes - the numbers the stats assertions
// below are computed from rather than copied from a run.
TestCase {
    id: testCase
    name: "E2eEncode"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    readonly property url wavUrl:
        Qt.resolvedUrl("../../../../fuzz/seeds/fuzz_wav_read/roundtrip-stereo.wav")
    readonly property url ac3OutUrl: Qt.resolvedUrl("_test_output/tst_e2e_encode.ac3")
    readonly property url ec3OutUrl: Qt.resolvedUrl("_test_output/tst_e2e_encode.ec3")
    readonly property url toolsOutUrl: Qt.resolvedUrl("_test_output/tst_e2e_encode_tools.ec3")
    readonly property url metaOutUrl: Qt.resolvedUrl("_test_output/tst_e2e_encode_meta.ac3")
    readonly property url decodedWavUrl: Qt.resolvedUrl("_test_output/tst_e2e_encode_decoded.wav")

    readonly property int fixtureFrames: 32
    readonly property int fixtureSampleFrames: 49152

    // Shared singletons: put back everything a case here changes, so the
    // next case (and this binary's other suites) start from defaults.
    function cleanup() {
        StreamPlayerController.pause();
        while (EncoderController.sourceModel.length > 0) {
            EncoderController.removeSource(0);
        }
        EncoderController.codecIndex = 0;
        EncoderController.bitrateKbps = 192;
        EncoderController.coupling = false;
        EncoderController.spx = false;
        EncoderController.aht = false;
        EncoderController.drcIndex = 0;
        EncoderController.dialnorm = 31;
        EncoderController.measureDialnorm = false;
        EncoderController.loudnessTouched = false;
    }

    // ---- helpers --------------------------------------------------------

    function findByName(root, name) {
        let found = null;
        tryVerify(() => {
            found = findChild(root, name);
            return found !== null;
        }, 5000, "no object called " + name);
        return found;
    }

    // The first control under `item` whose accessible name is `name` - for
    // the handful of plain Controls ComboBoxes/CheckBoxes/SpinBoxes that
    // carry a name for screen readers but no objectName.
    function findAccessible(item, name) {
        if (item.Accessible && item.Accessible.name === name) {
            return item;
        }
        const kids = item.children;
        for (let i = 0; i < kids.length; ++i) {
            const hit = findAccessible(kids[i], name);
            if (hit) {
                return hit;
            }
        }
        return null;
    }

    function findByText(item, text) {
        if (item.text === text && item.checkable !== undefined) {
            return item;
        }
        const kids = item.children;
        for (let i = 0; i < kids.length; ++i) {
            const hit = findByText(kids[i], text);
            if (hit) {
                return hit;
            }
        }
        return null;
    }

    // A click needs real geometry: wait for one painted frame of the item
    // and for its window position to read the same value twice running
    // (tst_format_channels.qml's waitForHeaderLayout, generalised).
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

    // The picker `button` opened: it must actually be showing (proof the
    // button opened it), then gets the file and is accepted. Not through
    // accept(): under the offscreen platform the non-native FileDialog's
    // accept() re-reads the selection from its own (never shown, empty)
    // implementation and clears selectedFile before onAccepted runs.
    // Emitting accepted() runs exactly the handler a real pick runs, with
    // the file set; close() then dismisses the picker (it emits rejected(),
    // which no ac3gui picker handles).
    function pickFile(root, dialogName, url) {
        const dialog = findByName(root, dialogName);
        tryCompare(dialog, "visible", true, 5000);
        dialog.selectedFile = url;
        dialog.accepted();  // see pickFile
        dialog.close();
        tryCompare(dialog, "visible", false, 5000);
        return dialog;
    }

    function loadWavFromFirstRun(win) {
        const card = findByName(win.contentItem, "firstRun-file");
        click(card);
        pickFile(win, "openDialog", wavUrl);
        tryCompare(EncoderController, "sourceReady", true, 10000);
    }

    function pressEncodeAndSaveTo(win, url) {
        const encodeButton = findByName(win.contentItem, "encodeButton");
        tryCompare(encodeButton, "enabled", true);
        click(encodeButton);
        const dialog = findByName(win, "saveDialog");
        tryCompare(dialog, "visible", true, 5000);
        // The planned name follows the source and the codec - proof the
        // button went through startEncodeFlow's openSaveDialog, not a
        // generic picker.
        const planned = dialog.selectedFile.toString();
        dialog.selectedFile = url;
        dialog.accepted();  // see pickFile
        dialog.close();
        compare(EncoderController.busy, true);
        tryCompare(EncoderController, "busy", false, 30000);
        return planned;
    }

    function switchTier(win, tier) {
        const seg = findByName(win.contentItem, "seg-" + tier);
        click(seg);
        compare(win.tier, tier);
    }

    function openTab(win, key) {
        const tab = findByName(win.contentItem, "tab-" + key);
        click(tab);
        compare(win.currentTab, key);
    }

    // Opens the header's "Open stream…" player on `url` through its own
    // "Choose file…" button and waits for the decode.
    function decodeInPlayer(win, url) {
        click(findByName(win.contentItem, "streamPlayerOpenButton"));
        tryVerify(() => win.streamPlayerDialogRef.opened);
        click(findByName(win.streamPlayerDialogRef.contentItem, "spChooseFileButton"));
        pickFile(win.streamPlayerDialogRef, "spFileDialog", url);
        tryCompare(StreamPlayerController, "busy", false, 15000);
        compare(StreamPlayerController.error, "");
        compare(StreamPlayerController.hasResult, true);
    }

    // ---- cases ----------------------------------------------------------

    function test_firstRunChooseFileEncodeAc3AndDecodeWhatWasWritten() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        compare(win.everHadSource, false);

        loadWavFromFirstRun(win);
        // The first-run screen gives way to the workbench, and the window
        // names the file the picker handed over.
        tryCompare(win, "everHadSource", true);
        compare(win.sourceLabel, "roundtrip-stereo.wav");
        compare(EncoderController.sourceModel.length, 1);
        compare(EncoderController.sourceModel[0].rate, 48000);
        compare(EncoderController.codecIndex, 0);

        const encodeButton = findByName(win.contentItem, "encodeButton");
        compare(encodeButton.text, "Encode to .ac3");
        const planned = pressEncodeAndSaveTo(win, ac3OutUrl);
        verify(planned.endsWith("roundtrip-stereo.ac3"), planned);

        const run = EncoderController.runs[0];
        compare(run.status, "done");
        compare(run.eac3, false);
        compare(run.filename, "tst_e2e_encode.ac3");
        compare(run.rateText, "192 kbps");
        compare(run.durationText, "0:01");
        // 32 frames x 768 bytes = 24576 bytes = 24 KB, reported both on the
        // status line (which names the layout, "2.0") and on the finished chip.
        const bytes = fixtureFrames * 192 * 4;
        compare(EncoderController.status,
                "Wrote " + fixtureFrames + " 2.0 frames (" + Math.floor(bytes / 1024)
                + " KB) to tst_e2e_encode.ac3");
        compare(run.sizeText, Math.floor(bytes / 1024) + " KB");
        const chip = findByName(win.contentItem, "runChipSummary-" + run.id);
        compare(chip.text, run.id + " · tst_e2e_encode.ac3 · 192 kbps · 0:01 · 24 KB");
        compare(EncoderController.outputIsEac3, false);
        compare(EncoderController.canPlay, true);

        // The file on disk is a real AC-3 stream of the source's shape and
        // length: the player decodes it back to 2 channels at 48 kHz, 32
        // frames long.
        decodeInPlayer(win, ac3OutUrl);
        verify(StreamPlayerController.summaryLine.indexOf("AC-3") === 0,
               StreamPlayerController.summaryLine);
        verify(StreamPlayerController.summaryLine.indexOf("48000 Hz") > 0,
               StreamPlayerController.summaryLine);
        verify(StreamPlayerController.summaryLine.indexOf(" " + fixtureFrames + " frame(s)") > 0,
               StreamPlayerController.summaryLine);
        compare(StreamPlayerController.channelMeta.length, 2);
        fuzzyCompare(StreamPlayerController.durationSeconds,
                     fixtureFrames * 1536 / 48000, 1e-6);
        compare(StreamPlayerController.hasObjects, false);
        const meters = findByName(win.streamPlayerDialogRef.contentItem, "spMeterRows");
        compare(meters.count, 2);
        click(findByName(win.streamPlayerDialogRef.contentItem, "spCloseButton"));
        tryCompare(win.streamPlayerDialogRef, "visible", false);
    }

    function test_expertCodecComboSwitchesToEac3AndEncodesAccessUnits() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        loadWavFromFirstRun(win);

        switchTier(win, "expert");
        openTab(win, "format");
        const codecBox = findAccessible(win.contentItem, "Codec");
        verify(codecBox !== null, "no Codec combo");
        compare(codecBox.enabled, true);
        compare(codecBox.currentIndex, 0);
        // The keyboard path a person without a mouse takes: focus the combo
        // and step it - activated() fires exactly as it does on a pick.
        focusIn(codecBox);
        keyClick(Qt.Key_Down);
        compare(EncoderController.codecIndex, 1);
        compare(codecBox.displayText, EncoderController.codecNames[1]);

        const encodeButton = findByName(win.contentItem, "encodeButton");
        tryCompare(encodeButton, "text", "Encode to .ec3");
        const planned = pressEncodeAndSaveTo(win, ec3OutUrl);
        verify(planned.endsWith("roundtrip-stereo.ec3"), planned);

        const run = EncoderController.runs[0];
        compare(run.status, "done");
        compare(run.eac3, true);
        compare(EncoderController.outputIsEac3, true);
        // E-AC-3 counts access units, not frames; with six blocks each they
        // are the same 1536 samples, so the count is the same 32.
        verify(EncoderController.status.indexOf("Wrote " + fixtureFrames + " 2.0 access units") === 0,
               EncoderController.status);

        decodeInPlayer(win, ec3OutUrl);
        verify(StreamPlayerController.summaryLine.indexOf("E-AC-3") === 0,
               StreamPlayerController.summaryLine);
        compare(StreamPlayerController.channelMeta.length, 2);
        fuzzyCompare(StreamPlayerController.durationSeconds,
                     fixtureFrames * 1536 / 48000, 1e-6);
        win.streamPlayerDialogRef.close();
    }

    // Coding tools are an Expert-only tab of plain CheckBoxes; ticking one
    // from the keyboard must reach both the token the command bar shows and
    // the run it starts.
    function test_codingToolsTabDrivesTheToolsTokenAndTheEncode() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        loadWavFromFirstRun(win);
        switchTier(win, "expert");
        EncoderController.codecIndex = 1;  // the tab's own note says tools need DD+
        openTab(win, "coding");

        const coupling = findByText(win.contentItem, "Channel coupling");
        verify(coupling !== null, "no coupling checkbox");
        compare(coupling.checked, false);
        focusIn(coupling);
        keyClick(Qt.Key_Space);
        compare(EncoderController.coupling, true);

        const spx = findByText(win.contentItem, "Spectral extension");
        focusIn(spx);
        keyClick(Qt.Key_Space);
        compare(EncoderController.spx, true);
        verify(EncoderController.toolsToken.length > 0);
        // The tab badge counts what is on, the same number a person reads.
        const codingTab = win.visibleTabs.filter((t) => t.key === "coding")[0];
        compare(codingTab.badge, "2");
        verify(win.cliLine.indexOf(EncoderController.toolsToken) > 0, win.cliLine);

        // The begin-band SpinBox appears once coupling is on and steps from
        // "auto" up.
        const cplBand = findAccessible(win.contentItem, "Channel coupling begin band");
        tryCompare(cplBand, "visible", true);
        compare(EncoderController.cplBegf, -1);
        focusIn(cplBand);
        keyClick(Qt.Key_Up);
        compare(EncoderController.cplBegf, 0);

        pressEncodeAndSaveTo(win, toolsOutUrl);
        const run = EncoderController.runs[0];
        compare(run.status, "done", run.detail);
        // The snapshotted command line carries the same tools token.
        verify(run.cliLine.indexOf(EncoderController.toolsToken) > 0, run.cliLine);

        decodeInPlayer(win, toolsOutUrl);
        verify(StreamPlayerController.summaryLine.indexOf("E-AC-3") === 0);
        win.streamPlayerDialogRef.close();
    }

    // Metadata set on the Metadata tab ends up IN the stream: dialnorm typed
    // into the Loudness card's spin box is what QC reads back out of the
    // encoded file, opened from the finished run's own QC shortcut.
    function test_metadataDialnormSetFromTheUiIsWhatQcReadsBackFromTheFile() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        loadWavFromFirstRun(win);
        switchTier(win, "expert");
        openTab(win, "meta");

        const dialnormBoxes = [];
        (function collect(item) {
            if (item.Accessible && item.Accessible.name === "dialnorm" && item.visible) {
                dialnormBoxes.push(item);
            }
            for (let i = 0; i < item.children.length; ++i) {
                collect(item.children[i]);
            }
        })(win.contentItem);
        verify(dialnormBoxes.length > 0, "no visible dialnorm box on the Metadata tab");
        const dialnormBox = dialnormBoxes[0];
        compare(dialnormBox.value, 31);
        focusIn(dialnormBox);
        for (let i = 0; i < 7; ++i) {
            keyClick(Qt.Key_Down);
        }
        compare(EncoderController.dialnorm, 24);
        compare(EncoderController.loudnessTouched, true);
        verify(EncoderController.metaTokens.indexOf("dialnorm=24") >= 0,
               EncoderController.metaTokens);

        pressEncodeAndSaveTo(win, metaOutUrl);
        const run = EncoderController.runs[0];
        compare(run.status, "done", run.detail);

        win.openRunInQc(run.path);
        tryVerify(() => win.qcDialogRef.opened);
        tryCompare(QcController, "busy", false, 15000);
        compare(QcController.error, "");
        compare(QcController.programmes[0].dialnorm, 24);
        const line = findByName(win.qcDialogRef.contentItem, "qcDialnormLine");
        verify(line.text.indexOf("dialnorm 24") === 0, line.text);
        click(findByName(win.qcDialogRef.contentItem, "qcCloseButton"));
        tryCompare(win.qcDialogRef, "visible", false);
    }

    // Decode, export the decode as a WAV from the player's own button, and
    // load that WAV back in through the rail: a full round trip through
    // every picker the file workflow has.
    function test_playerExportsDecodedWavThatLoadsBackAsASource() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        loadWavFromFirstRun(win);
        pressEncodeAndSaveTo(win, ac3OutUrl);
        compare(EncoderController.runs[0].status, "done");

        decodeInPlayer(win, ac3OutUrl);
        const dialog = win.streamPlayerDialogRef;
        click(findByName(dialog.contentItem, "spExportWavButton"));
        pickFile(dialog, "spExportWavDialog", decodedWavUrl);
        tryCompare(StreamPlayerController, "exporting", false, 15000);
        compare(StreamPlayerController.exportError, "");
        compare(findByName(dialog.contentItem, "spExportError").visible, false);
        dialog.close();
        tryCompare(dialog, "visible", false);

        // The rail's button now reads "+ Add files…" and opens the ADD
        // picker, not the replace one.
        const railButton = findByName(win.contentItem, "chooseWavButton");
        compare(railButton.text, "+ Add files…");
        click(railButton);
        pickFile(win, "addSourceDialog", decodedWavUrl);
        tryVerify(() => EncoderController.sourceModel.length === 2, 10000);
        const decoded = EncoderController.sourceModel[1];
        compare(decoded.rate, 48000);
        compare(decoded.channels, 2);
        compare(decoded.label, "tst_e2e_encode_decoded.wav");
        // The two-source rail offers per-row remove, and pressing it on the
        // decoded row leaves the original as the only source.
        click(findByName(win.contentItem, "sourceRemove1"));
        tryVerify(() => EncoderController.sourceModel.length === 1);
        compare(win.sourceLabel, "roundtrip-stereo.wav");
    }
}
