import QtQuick
import QtTest

import Ac3Forge

// AC-4 objects on the encoder page (planning/ac4.md, I5b), each new control from the
// keyboard: object mode under the AC-4 codec (the codec picker takes E-AC-3 and AC-4 in
// it, never AC-3), the AC-4 tab's object controls (the coding, the dialnorm in whole dB,
// the CRC) with the controls that describe channels switched off, the writer's limits in
// the Objects tab's own text, the LFE send that AC-4 objects lack, and what an object
// stream refuses before Encode. tst_e2e_ac4_objects.qml holds the echoed line's bytes and
// the round trip through the object page.
TestCase {
    id: testCase
    name: "Ac4Objects"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    readonly property url stereoUrl:
        Qt.resolvedUrl("../../../../fuzz/seeds/fuzz_wav_read/roundtrip-stereo.wav")
    readonly property url surroundUrl:
        Qt.resolvedUrl("../../../../fuzz/seeds/fuzz_wav_read/roundtrip-51.wav")
    readonly property url pathsUrl: Qt.resolvedUrl("_test_output/tst_ac4_objects-paths.json")
    readonly property url refusedOutUrl: Qt.resolvedUrl("_test_output/tst_ac4_objects.mkv")

    function cleanup() {
        EncoderController.atmosEnabled = false;
        while (EncoderController.sourceModel.length > 0) {
            EncoderController.removeSource(0);
        }
        EncoderController.codecIndex = 0;
        EncoderController.containerIndex = 0;
        EncoderController.bitrateKbps = 192;
        EncoderController.ac4ObjectCodingIndex = 0;
        EncoderController.ac4Dialnorm = 31;
        EncoderController.ac4MeasureDialnorm = false;
        EncoderController.ac4Crc = true;
    }

    // No test here starts an encode (every refusal is raised before a run opens), and the
    // controllers are idle as the suite ends: no job of this suite is left running at teardown.
    function cleanupTestCase() {
        tryCompare(EncoderController, "busy", false, 15000);
        tryCompare(ObjectDecodeController, "busy", false, 15000);
    }

    // ---- helpers (tst_ac4_encode.qml's) ----------------------------------

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

    function keys(item, key, times) {
        focusIn(item);
        for (let i = 0; i < times; ++i) {
            keyClick(key);
        }
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

    function openTab(win, key) {
        click(findByName(win.contentItem, "tab-" + key));
        compare(win.currentTab, key);
    }

    // The source through the first-run card, the Expert tier, AC-4 from the Format tab's codec
    // picker by the keyboard, then object mode from the Objects tab's own switch.
    function openAc4Objects(url) {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        click(findByName(win.contentItem, "firstRun-file"));
        pickFile(win, "openDialog", url);
        tryCompare(EncoderController, "sourceReady", true, 10000);
        click(findByName(win.contentItem, "seg-expert"));
        openTab(win, "format");
        keys(findAccessible(win.contentItem, "Codec"), Qt.Key_Down, 2);
        compare(EncoderController.codecIndex, 2);
        openTab(win, "objects");
        const atmosSwitch = findByName(win.contentItem, "atmosSwitch");
        focusIn(atmosSwitch);
        keyClick(Qt.Key_Space);
        compare(EncoderController.atmosEnabled, true);
        return win;
    }

    // ---- cases ----------------------------------------------------------

    function test_objectModeKeepsAc4AndOffersItsTabBesideTheObjectsTab() {
        const win = openAc4Objects(stereoUrl);
        // The codec stayed AC-4: object mode did not move it to E-AC-3.
        compare(EncoderController.codecIndex, 2);
        compare(EncoderController.ac4Objects, true);
        tryVerify(() => win.visibleTabs.some(tab => tab.key === "ac4"));
        verify(win.visibleTabs.some(tab => tab.key === "objects"));
        verify(!win.visibleTabs.some(tab => tab.key === "coding"));
        verify(!win.visibleTabs.some(tab => tab.key === "meta"));
        // The plan, the summary and the button say AC-4 objects, not E-AC-3 over a bed.
        compare(findByName(win.contentItem, "encodeButton").text, "Encode to .ac4");
        verify(win.planLine.indexOf("AC-4 · 2 objects") === 0, win.planLine);
        verify(findByName(win.contentItem, "objectsSummary").text.indexOf("AC-4 objects") > 0,
               findByName(win.contentItem, "objectsSummary").text);
        // The bit rate is the page's to choose: object mode under AC-4 raised nothing.
        compare(EncoderController.bitrateKbps, 192);

        // Off again, AC-4 is channels again, with its own tab and its 5.1 bed of the source.
        focusIn(findByName(win.contentItem, "atmosSwitch"));
        keyClick(Qt.Key_Space);
        compare(EncoderController.atmosEnabled, false);
        compare(EncoderController.ac4Objects, false);
        compare(EncoderController.codecIndex, 2);
    }

    function test_codecPickerInObjectModeTakesEac3AndAc4ButNotAc3() {
        const win = openAc4Objects(stereoUrl);
        openTab(win, "format");
        const codecBox = findAccessible(win.contentItem, "Codec");
        verify(codecBox !== null, "no Codec combo");
        // It is a control in object mode, not the readout it was.
        compare(codecBox.enabled, true);
        keys(codecBox, Qt.Key_Up, 1);
        compare(EncoderController.codecIndex, 1);
        compare(EncoderController.atmosEnabled, true);
        compare(EncoderController.ac4Objects, false);
        // E-AC-3's objects want the frame to hold the JOC payload: the rate went up.
        verify(EncoderController.bitrateKbps >= 384);
        tryCompare(findByName(win.contentItem, "encodeButton"), "text", "Encode to .ec3");
        keys(codecBox, Qt.Key_Down, 1);
        compare(EncoderController.codecIndex, 2);
        compare(EncoderController.ac4Objects, true);
        // AC-3 has no place for objects: asking for it is refused, and the codec stays.
        EncoderController.codecIndex = 0;
        compare(EncoderController.codecIndex, 2);
    }

    function test_ac4TabCarriesTheObjectControlsAndSwitchesOffWhatDescribesChannels() {
        const win = openAc4Objects(stereoUrl);
        openTab(win, "ac4");
        const tab = win.contentItem;
        compare(findByName(tab, "ac4ObjectCoding").enabled, true);
        compare(findByName(tab, "ac4ObjectCoding").currentIndex, 0);
        compare(findByName(tab, "ac4Dialnorm").enabled, true);
        compare(findByName(tab, "ac4Crc").enabled, true);
        // Objects are written at the native frame rate, at a constant rate: those stay as they
        // are, shown as what they are.
        const rate = findByName(tab, "ac4FrameRate");
        compare(rate.enabled, false);
        compare(rate.currentIndex, EncoderController.ac4FrameRateNames.length - 1);
        compare(rate.displayText, EncoderController.ac4FrameRateNames[13]);
        compare(findByName(tab, "ac4RateMode").enabled, false);
        compare(findByName(tab, "ac4CodecMode").enabled, false);
        compare(findByName(tab, "ac4IframeInterval").enabled, false);
        // The loudness values and DRC describe channels; the downmix and dialogue cards are gone.
        compare(findByName(tab, "ac4Loudness").enabled, false);
        compare(findByName(tab, "ac4Drc").enabled, false);
        compare(findByName(tab, "ac4Centre").visible, false);
        compare(findByName(tab, "ac4DialogueLeft").visible, false);

        // Back to channels, they are all controls again.
        EncoderController.atmosEnabled = false;
        tryCompare(findByName(tab, "ac4FrameRate"), "enabled", true);
        compare(findByName(tab, "ac4Loudness").enabled, true);
        compare(findByName(tab, "ac4ObjectCoding").visible, false);
    }

    function test_codingDialnormAndCrcReachTheEchoedLine() {
        const win = openAc4Objects(stereoUrl);
        openTab(win, "ac4");
        const tab = win.contentItem;
        // Nothing chosen: the command is plain.
        compare(EncoderController.ac4Tokens, "");
        compare(win.cliLine,
                "ac3cli atmos-encode roundtrip-stereo.wav out.ac4 192 2 roundtrip-stereo-paths.json "
                + "codec=ac4");

        keys(findByName(tab, "ac4ObjectCoding"), Qt.Key_Down, 1);        // direct-coded
        compare(EncoderController.ac4ObjectCodingIndex, 1);
        compare(EncoderController.ac4Tokens, "coding=direct");
        // Whole dB in object mode: four steps down from 31.
        keys(findByName(tab, "ac4Dialnorm"), Qt.Key_Down, 4);
        compare(EncoderController.ac4Dialnorm, 27);
        keys(findByName(tab, "ac4Crc"), Qt.Key_Space, 1);
        compare(EncoderController.ac4Crc, false);
        compare(EncoderController.ac4Tokens, "coding=direct dialnorm=27 crc=off");
        compare(win.cliLine,
                "ac3cli atmos-encode roundtrip-stereo.wav out.ac4 192 2 roundtrip-stereo-paths.json "
                + "codec=ac4 coding=direct dialnorm=27 crc=off");
        // The tab's badge counts the tokens it adds.
        compare(win.visibleTabs.filter(t => t.key === "ac4")[0].badge, "3");

        // An MP4 sample has no CRC: the token goes, the file is named .mp4.
        EncoderController.containerIndex = 3;
        compare(EncoderController.ac4Tokens, "coding=direct dialnorm=27");
        verify(win.cliLine.indexOf(" out.mp4 192 ") > 0, win.cliLine);
        verify(win.cliLine.indexOf("crc=") < 0, win.cliLine);
        tryCompare(findByName(win.contentItem, "encodeButton"), "text", "Encode to .mp4");
    }

    function test_theWritersLimitsAreOnTheObjectsTabAndTheLfeSendIsOff() {
        const win = openAc4Objects(stereoUrl);
        const tab = win.contentItem;
        const limits = findByName(tab, "ac4ObjectsLimits");
        compare(limits.visible, true);
        verify(limits.text.indexOf("2 048 samples a frame") >= 0, limits.text);
        verify(limits.text.indexOf("64 objects at most") >= 0, limits.text);
        verify(limits.text.indexOf("roundtrip-stereo-paths.json") >= 0, limits.text);
        compare(EncoderController.objectLimit, 64);
        // Nothing is refused for two objects at a plain rate.
        compare(EncoderController.ac4ObjectsRefusal, "");
        compare(findByName(tab, "ac4ObjectsRefusal").visible, false);
        // The LFE send has no AC-4 counterpart: its slider is off, and the page says why.
        const send = findAccessible(tab, "LFE send — object 1");
        verify(send !== null, "no LFE send slider");
        compare(send.enabled, false);
        // E-AC-3's objects still have it, and the limit of fifteen.
        EncoderController.codecIndex = 1;
        compare(EncoderController.objectLimit, 15);
        compare(findByName(tab, "ac4ObjectsLimits").visible, false);
        tryCompare(findAccessible(tab, "LFE send — object 1"), "enabled", true);
    }

    function test_theCountLineCountsAChannelAssignedToASpeakerAsAnObjectNotAsABed() {
        const win = openAc4Objects(stereoUrl);
        const line = findByName(win.contentItem, "objectsCountLine");
        verify(line !== null, "no count line");
        // Nothing assigned: each channel is an object, counted against the writer's 64.
        verify(line.text.indexOf("2 of 64 objects") >= 0, line.text);
        // A channel assigned to a speaker is an object held there. It spends one of the 64, and
        // there is no bed for it to be pinned to.
        EncoderController.setAssignment(0, 0, "L");
        EncoderController.setAssignment(0, 1, "obj");
        tryVerify(() => line.text.indexOf("1 of 63 objects") >= 0);
        verify(line.text.indexOf("1 assigned to speakers") >= 0, line.text);
        verify(line.text.indexOf("bed") < 0, line.text);
        // E-AC-3's own wording and limit of fifteen are as they were.
        EncoderController.codecIndex = 1;
        tryVerify(() => line.text.indexOf("1 pinned to the bed") >= 0);
        verify(line.text.indexOf("of 14 objects") >= 0, line.text);
    }

    function test_aContainerAnObjectStreamCannotBeIsRefusedBesideTheControlsAndBeforeEncode() {
        const win = openAc4Objects(stereoUrl);
        EncoderController.containerIndex = 1;   // Matroska
        verify(EncoderController.ac4ObjectsRefusal.indexOf("raw stream or an MP4 file") >= 0,
               EncoderController.ac4ObjectsRefusal);
        const refusal = findByName(win.contentItem, "ac4ObjectsRefusal");
        tryCompare(refusal, "visible", true);
        verify(refusal.text.indexOf("raw stream or an MP4 file") >= 0, refusal.text);
        // Encode refuses it too, in the banner, before any run opens.
        const encodeButton = findByName(win.contentItem, "encodeButton");
        tryCompare(encodeButton, "enabled", true);
        click(encodeButton);
        pickFile(win, "saveDialog", refusedOutUrl);
        verify(win.refusalText.indexOf("raw stream or an MP4 file") >= 0, win.refusalText);
        verify(EncoderController.runs.length === 0 || EncoderController.runs[0].status !== "encoding");
    }

    function test_oneLfeObjectIsTakenAndTwoAreRefused() {
        const win = openAc4Objects(surroundUrl);
        // 5.1's six channels: a dynamic object, an LFE, a second LFE and three silent.
        EncoderController.setAssignment(0, 0, "obj");
        EncoderController.setAssignment(0, 1, "none");
        EncoderController.setAssignment(0, 2, "none");
        EncoderController.setAssignment(0, 3, "LFE");
        EncoderController.setAssignment(0, 4, "none");
        EncoderController.setAssignment(0, 5, "LFE2");
        verify(EncoderController.ac4ObjectsRefusal.indexOf("2 channels are assigned to an LFE") >= 0,
               EncoderController.ac4ObjectsRefusal);
        tryCompare(findByName(win.contentItem, "ac4ObjectsRefusal"), "visible", true);
        EncoderController.setAssignment(0, 5, "none");
        compare(EncoderController.ac4ObjectsRefusal, "");
        // An LFE alone is no object stream.
        EncoderController.setAssignment(0, 0, "none");
        verify(EncoderController.ac4ObjectsRefusal.indexOf("at least one object that is not the LFE") >= 0,
               EncoderController.ac4ObjectsRefusal);
    }

    function test_aDialnormOffTheWholeDbGridIsRefusedAndMeasuringIsToo() {
        const win = openAc4Objects(stereoUrl);
        EncoderController.ac4Dialnorm = 27.25;
        verify(EncoderController.ac4ObjectsRefusal.indexOf("whole dB") >= 0,
               EncoderController.ac4ObjectsRefusal);
        EncoderController.ac4Dialnorm = 31;
        compare(EncoderController.ac4ObjectsRefusal, "");
        EncoderController.ac4MeasureDialnorm = true;
        verify(EncoderController.ac4ObjectsRefusal.indexOf("no bed to measure") >= 0,
               EncoderController.ac4ObjectsRefusal);
        // Channels measure it as they always did.
        EncoderController.atmosEnabled = false;
        compare(EncoderController.ac4ObjectsRefusal, "");
    }

    function test_exportPathsSuggestsTheNameTheEchoedCommandReads() {
        const win = openAc4Objects(stereoUrl);
        // A button on the timeline's toolbar, in a scrolling page: pressed from the keyboard, as
        // tst_e2e_objects.qml does.
        focusIn(findByName(win.contentItem, "exportPathsButton"));
        keyClick(Qt.Key_Space);
        const dialog = findByName(win, "exportPathsDialog");
        tryCompare(dialog, "visible", true);
        verify(dialog.selectedFile.toString().endsWith("/roundtrip-stereo-paths.json"),
               dialog.selectedFile);
        compare(dialog.defaultSuffix, "json");
        dialog.selectedFile = pathsUrl;
        dialog.accepted();
        dialog.close();
        // The scene AC-4 objects read: a file, written.
        tryVerify(() => cliRunner.size(pathsUrl) > 0);
    }
}
