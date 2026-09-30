import QtQuick
import QtTest

import ForgeGui

// AC-4 from the page, end to end: the codec picker and the AC-4 tab set up an
// encode through the keyboard, the Encode button writes it, and the command
// line the run recorded - the one the command bar echoed - is run through
// this build's forge in a folder holding a copy of the source. The two files
// must be the same bytes, for a raw stream and for an MP4 file (planning/
// ac4.md, I3's exit criterion). The page's file is then decoded by the
// player, measured by QC and opened on the object page, each through its own
// picker.
//
// The pickers are the only seam, as in tst_e2e_encode.qml; running forge is
// the second, through qml_test_main.cpp's cliRunner.
TestCase {
    id: testCase
    name: "E2eAc4"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    readonly property url wavUrl:
        Qt.resolvedUrl("../../../../fuzz/seeds/fuzz_wav_read/roundtrip-stereo.wav")
    readonly property url rawOutUrl: Qt.resolvedUrl("_test_output/tst_e2e_ac4.ac4")
    readonly property url mp4OutUrl: Qt.resolvedUrl("_test_output/tst_e2e_ac4.mp4")
    readonly property url cliFolderUrl: Qt.resolvedUrl("_test_output/tst_e2e_ac4_cli")

    function cleanup() {
        StreamPlayerController.pause();
        while (EncoderController.sourceModel.length > 0) {
            EncoderController.removeSource(0);
        }
        EncoderController.codecIndex = 0;
        EncoderController.containerIndex = 0;
        EncoderController.bitrateKbps = 192;
        EncoderController.ac4FrameRateIndex = 13;
        EncoderController.ac4RateModeIndex = 0;
        EncoderController.ac4CodecModeIndex = 0;
        EncoderController.ac4Dialnorm = 31;
        EncoderController.ac4MeasureDialnorm = false;
        EncoderController.ac4LoudnessIndex = 0;
        EncoderController.ac4DrcIndex = 0;
        EncoderController.ac4DialogueLeft = false;
        EncoderController.ac4DialogueRight = false;
        EncoderController.ac4IframeInterval = 24;
        EncoderController.ac4Crc = true;
    }

    // ---- helpers (tst_e2e_encode.qml's) ----------------------------------

    function findByName(root, name) {
        let found = null;
        tryVerify(() => {
            found = findChild(root, name);
            return found !== null;
        }, 5000, "no object called " + name);
        return found;
    }

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
        return dialog;
    }

    function keys(item, key, times) {
        focusIn(item);
        for (let i = 0; i < times; ++i) {
            keyClick(key);
        }
    }

    function pressEncodeAndSaveTo(win, url) {
        const encodeButton = findByName(win.contentItem, "encodeButton");
        tryCompare(encodeButton, "enabled", true);
        click(encodeButton);
        const dialog = findByName(win, "saveDialog");
        tryCompare(dialog, "visible", true, 5000);
        const planned = dialog.selectedFile.toString();
        dialog.selectedFile = url;
        dialog.accepted();
        dialog.close();
        tryCompare(EncoderController, "busy", false, 60000);
        return planned;
    }

    function openTab(win, key) {
        const tab = findByName(win.contentItem, "tab-" + key);
        click(tab);
        compare(win.currentTab, key);
    }

    // The source through the first-run card, the Expert tier, and AC-4 from
    // the Format tab's codec picker by the keyboard: two steps down from AC-3.
    function openAc4Page() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        click(findByName(win.contentItem, "firstRun-file"));
        pickFile(win, "openDialog", wavUrl);
        tryCompare(EncoderController, "sourceReady", true, 10000);
        click(findByName(win.contentItem, "seg-expert"));
        openTab(win, "format");
        const codecBox = findAccessible(win.contentItem, "Codec");
        verify(codecBox !== null, "no Codec combo");
        keys(codecBox, Qt.Key_Down, 2);
        compare(EncoderController.codecIndex, 2);
        compare(codecBox.displayText, "AC-4");
        tryVerify(() => win.visibleTabs.some(tab => tab.key === "ac4"));
        verify(!win.visibleTabs.some(tab => tab.key === "coding"));
        verify(!win.visibleTabs.some(tab => tab.key === "meta"));
        return win;
    }

    // The recorded line, run through forge where the source's copy is, and
    // what it wrote compared with what the page wrote.
    function runEchoedLineAndCompare(line, outName, pageUrl) {
        if (!cliRunner.available()) {
            skip("this build has no forge to run the echoed line through");
        }
        verify(cliRunner.prepare(cliFolderUrl, wavUrl));
        compare(cliRunner.run(line, cliFolderUrl), 0, line);
        const cliUrl = Qt.resolvedUrl("_test_output/tst_e2e_ac4_cli/" + outName);
        verify(cliRunner.size(pageUrl) > 0);
        compare(cliRunner.size(cliUrl), cliRunner.size(pageUrl), line);
        verify(cliRunner.sameBytes(cliUrl, pageUrl), line);
    }

    // ---- cases ----------------------------------------------------------

    function test_rawStreamFromThePageEqualsTheEchoedLine() {
        const win = openAc4Page();
        const encodeButton = findByName(win.contentItem, "encodeButton");
        tryCompare(encodeButton, "text", "Encode to .ac4");

        openTab(win, "ac4");
        const tab = win.contentItem;
        keys(findByName(tab, "ac4FrameRate"), Qt.Key_Up, 11);        // native -> 25 fps
        compare(EncoderController.ac4FrameRateIndex, 2);
        keys(findByName(tab, "ac4RateMode"), Qt.Key_Down, 1);        // average
        keys(findByName(tab, "ac4MeasureDialnorm"), Qt.Key_Space, 1);
        keys(findByName(tab, "ac4Loudness"), Qt.Key_Down, 1);        // EBU R 128
        keys(findByName(tab, "ac4Drc"), Qt.Key_Down, 1);             // film standard
        keys(findByName(tab, "ac4DialogueLeft"), Qt.Key_Space, 1);
        keys(findByName(tab, "ac4DialogueRight"), Qt.Key_Space, 1);
        keys(findByName(tab, "ac4IframeInterval"), Qt.Key_Down, 1);  // 23
        keys(findByName(tab, "ac4Crc"), Qt.Key_Space, 1);

        const echoed = "forge ac4-encode roundtrip-stereo.wav out.ac4 192 frame-rate=25 "
                       + "rate-mode=average dialnorm=auto loudness=ebu-r128 drc=film-standard "
                       + "dialogue-channels=l,r iframe-interval=23 crc=off";
        compare(win.cliLine, echoed);

        const planned = pressEncodeAndSaveTo(win, rawOutUrl);
        verify(planned.endsWith("roundtrip-stereo.ac4"), planned);
        const run = EncoderController.runs[0];
        compare(run.status, "done", run.detail);
        compare(run.cliLine, echoed);
        verify(EncoderController.status.indexOf("stereo AC-4 frames") > 0, EncoderController.status);

        runEchoedLineAndCompare(run.cliLine, "out.ac4", rawOutUrl);

        // The player decodes what the page wrote through ac4::Decoder.
        click(findByName(win.contentItem, "streamPlayerOpenButton"));
        tryVerify(() => win.streamPlayerDialogRef.opened);
        click(findByName(win.streamPlayerDialogRef.contentItem, "spChooseFileButton"));
        pickFile(win.streamPlayerDialogRef, "spFileDialog", rawOutUrl);
        tryCompare(StreamPlayerController, "busy", false, 15000);
        compare(StreamPlayerController.error, "");
        compare(StreamPlayerController.isAc4, true);
        verify(StreamPlayerController.summaryLine.indexOf("AC-4 · ") === 0,
               StreamPlayerController.summaryLine);
        verify(StreamPlayerController.summaryLine.indexOf("48000 Hz") > 0,
               StreamPlayerController.summaryLine);
        compare(StreamPlayerController.channelMeta.length, 2);
        compare(StreamPlayerController.decodedPresentation, 0);
        win.streamPlayerDialogRef.close();

        // QC reads the dialnorm and the loudness values the page sent.
        click(findByName(win.contentItem, "qcOpenButton"));
        tryVerify(() => win.qcDialogRef.opened);
        click(findByName(win.qcDialogRef.contentItem, "qcChooseFileButton"));
        pickFile(win.qcDialogRef, "qcFileDialog", rawOutUrl);
        tryCompare(QcController, "busy", false, 15000);
        compare(QcController.error, "");
        compare(QcController.isAc4, true);
        verify(QcController.summaryLine.indexOf("AC-4 · ") === 0, QcController.summaryLine);
        const programme = QcController.programmes[0];
        compare(programme.hasLoudness, true);
        compare(programme.hasDialnorm, true);
        compare(programme.hasStatedLkfs, true);
        const dialnormLine = findByName(win.qcDialogRef.contentItem, "qcDialnormLine");
        compare(dialnormLine.visible, true);
        win.qcDialogRef.close();

        // The object page shows the one presentation, read-only.
        click(findByName(win.contentItem, "objectInspectorOpenButton"));
        tryVerify(() => win.objectInspectorDialogRef.opened);
        click(findByName(win.objectInspectorDialogRef.contentItem, "oiChooseFileButton"));
        pickFile(win.objectInspectorDialogRef, "objFileDialog", rawOutUrl);
        tryCompare(ObjectDecodeController, "busy", false, 15000);
        compare(ObjectDecodeController.error, "");
        compare(ObjectDecodeController.isAc4, true);
        compare(ObjectDecodeController.presentationNames.length, 1);
        verify(ObjectDecodeController.presentationNames[0].indexOf("0: L R") === 0,
               ObjectDecodeController.presentationNames[0]);
        compare(ObjectDecodeController.ac4DynamicObjects, 0);
        compare(findByName(win.objectInspectorDialogRef.contentItem, "oiAc4ExportNote").visible,
                true);
        win.objectInspectorDialogRef.close();
    }

    function test_mp4FileFromThePageEqualsTheEchoedLine() {
        const win = openAc4Page();
        const containerBox = findAccessible(win.contentItem, "Container");
        verify(containerBox !== null, "no Container combo");
        keys(containerBox, Qt.Key_Down, 3);                           // MP4
        compare(EncoderController.containerIndex, 3);
        const encodeButton = findByName(win.contentItem, "encodeButton");
        tryCompare(encodeButton, "text", "Encode to .mp4");

        openTab(win, "ac4");
        const tab = win.contentItem;
        keys(findByName(tab, "ac4RateMode"), Qt.Key_Down, 2);        // variable
        keys(findByName(tab, "ac4CodecMode"), Qt.Key_Down, 2);       // ASPX
        keys(findByName(tab, "ac4Dialnorm"), Qt.Key_Down, 4);        // 31 -> 30 dB
        compare(EncoderController.ac4Dialnorm, 30);
        // An MP4 sample carries no CRC, so the control is off the table.
        compare(findByName(tab, "ac4Crc").enabled, false);

        const echoed = "forge ac4-encode roundtrip-stereo.wav out.mp4 192 rate-mode=variable "
                       + "codec-mode=aspx dialnorm=30";
        compare(win.cliLine, echoed);

        pressEncodeAndSaveTo(win, mp4OutUrl);
        const run = EncoderController.runs[0];
        compare(run.status, "done", run.detail);
        compare(run.cliLine, echoed);

        runEchoedLineAndCompare(run.cliLine, "out.mp4", mp4OutUrl);

        // QC demuxes the MP4's AC-4 track and reads its dialnorm back.
        click(findByName(win.contentItem, "qcOpenButton"));
        tryVerify(() => win.qcDialogRef.opened);
        click(findByName(win.qcDialogRef.contentItem, "qcChooseFileButton"));
        pickFile(win.qcDialogRef, "qcFileDialog", mp4OutUrl);
        tryCompare(QcController, "busy", false, 15000);
        compare(QcController.error, "");
        compare(QcController.isAc4, true);
        compare(QcController.programmes[0].dialnorm, 30);
        win.qcDialogRef.close();
    }
}
