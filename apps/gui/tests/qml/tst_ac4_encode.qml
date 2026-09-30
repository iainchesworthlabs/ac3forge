import QtQuick
import QtTest

import ForgeGui

// The AC-4 tab's controls that tst_e2e_ac4.qml does not drive, each from the
// keyboard: the dialogue enhancement's centre, Mid and largest boost, and the
// stereo downmix a 5.1 source has (whose encode is held to the echoed line's
// bytes too); and the codec's rules - a stereo source leaves the downmix
// disabled, AC-4 refuses a container other than a raw stream or MP4, and a
// preset that needs an extra moves the codec to E-AC-3 as it does from AC-3.
TestCase {
    id: testCase
    name: "Ac4Encode"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    readonly property url stereoUrl:
        Qt.resolvedUrl("../../../../fuzz/seeds/fuzz_wav_read/roundtrip-stereo.wav")
    readonly property url surroundUrl:
        Qt.resolvedUrl("../../../../fuzz/seeds/fuzz_wav_read/roundtrip-51.wav")
    readonly property url surroundOutUrl: Qt.resolvedUrl("_test_output/tst_ac4_encode_51.ac4")
    readonly property url refusedOutUrl: Qt.resolvedUrl("_test_output/tst_ac4_encode.mkv")
    readonly property url cliFolderUrl: Qt.resolvedUrl("_test_output/tst_ac4_encode_cli")

    function cleanup() {
        while (EncoderController.sourceModel.length > 0) {
            EncoderController.removeSource(0);
        }
        EncoderController.codecIndex = 0;
        EncoderController.containerIndex = 0;
        EncoderController.ac4DialogueCentre = false;
        EncoderController.ac4DialogueMid = false;
        EncoderController.ac4DialogueMaxGainIndex = 2;
        EncoderController.ac4CentreIndex = 0;
        EncoderController.ac4SurroundIndex = 0;
        EncoderController.ac4PreferredDownmixIndex = 0;
    }

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
    }

    function openTab(win, key) {
        click(findByName(win.contentItem, "tab-" + key));
        compare(win.currentTab, key);
    }

    function pressEncodeAndSaveTo(win, url) {
        const encodeButton = findByName(win.contentItem, "encodeButton");
        tryCompare(encodeButton, "enabled", true);
        click(encodeButton);
        const dialog = findByName(win, "saveDialog");
        tryCompare(dialog, "visible", true, 5000);
        dialog.selectedFile = url;
        dialog.accepted();
        dialog.close();
        tryCompare(EncoderController, "busy", false, 60000);
    }

    function openAc4With(url) {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        click(findByName(win.contentItem, "firstRun-file"));
        pickFile(win, "openDialog", url);
        tryCompare(EncoderController, "sourceReady", true, 10000);
        click(findByName(win.contentItem, "seg-expert"));
        openTab(win, "format");
        keys(findAccessible(win.contentItem, "Codec"), Qt.Key_Down, 2);
        compare(EncoderController.codecIndex, 2);
        return win;
    }

    function test_dialogueCentreMidAndLargestBoostReachTheEchoedLine() {
        const win = openAc4With(stereoUrl);
        openTab(win, "ac4");
        const tab = win.contentItem;
        keys(findByName(tab, "ac4DialogueCentre"), Qt.Key_Space, 1);
        compare(EncoderController.ac4DialogueCentre, true);
        keys(findByName(tab, "ac4DialogueMid"), Qt.Key_Space, 1);
        compare(EncoderController.ac4DialogueMid, true);
        keys(findByName(tab, "ac4DialogueMaxGain"), Qt.Key_Up, 2);   // 9 -> 3 dB
        compare(EncoderController.ac4DialogueMaxGainIndex, 0);
        compare(EncoderController.ac4Tokens,
                "dialogue-channels=c dialogue-method=mid dialogue-max-gain=3");
        verify(win.cliLine.endsWith(EncoderController.ac4Tokens), win.cliLine);
        // The tab's badge counts the tokens it adds to the line.
        compare(win.visibleTabs.filter(t => t.key === "ac4")[0].badge, "3");
    }

    function test_stereoSourceLeavesTheDownmixDisabled() {
        const win = openAc4With(stereoUrl);
        openTab(win, "ac4");
        compare(EncoderController.ac4DownmixAvailable, false);
        compare(findByName(win.contentItem, "ac4Centre").enabled, false);
        compare(findByName(win.contentItem, "ac4Surround").enabled, false);
        compare(findByName(win.contentItem, "ac4PreferredDownmix").enabled, false);
    }

    function test_surroundDownmixFromThePageEqualsTheEchoedLine() {
        const win = openAc4With(surroundUrl);
        compare(EncoderController.bedLfe, true);
        openTab(win, "ac4");
        const tab = win.contentItem;
        compare(EncoderController.ac4DownmixAvailable, true);
        keys(findByName(tab, "ac4Centre"), Qt.Key_Down, 1);           // +3 dB
        compare(EncoderController.ac4CentreIndex, 1);
        keys(findByName(tab, "ac4Surround"), Qt.Key_Down, 6);         // off
        compare(EncoderController.ac4SurroundIndex, 6);
        keys(findByName(tab, "ac4PreferredDownmix"), Qt.Key_Down, 2); // Lt/Rt
        compare(EncoderController.ac4PreferredDownmixIndex, 2);
        const echoed = "forge ac4-encode roundtrip-51.wav out.ac4 192 cmixlev=+3 surmixlev=off "
                       + "dmixmod=ltrt";
        compare(win.cliLine, echoed);

        pressEncodeAndSaveTo(win, surroundOutUrl);
        const run = EncoderController.runs[0];
        compare(run.status, "done", run.detail);
        verify(EncoderController.status.indexOf("5.1 AC-4 frames") > 0, EncoderController.status);
        if (!cliRunner.available()) {
            skip("this build has no forge to run the echoed line through");
        }
        verify(cliRunner.prepare(cliFolderUrl, surroundUrl));
        compare(cliRunner.run(run.cliLine, cliFolderUrl), 0, run.cliLine);
        verify(cliRunner.sameBytes(Qt.resolvedUrl("_test_output/tst_ac4_encode_cli/out.ac4"),
                                   surroundOutUrl), run.cliLine);
    }

    function test_ac4RefusesAContainerOtherThanRawOrMp4() {
        const win = openAc4With(stereoUrl);
        keys(findAccessible(win.contentItem, "Container"), Qt.Key_Down, 1);   // Matroska
        compare(EncoderController.containerIndex, 1);
        pressEncodeAndSaveTo(win, refusedOutUrl);
        verify(win.refusalText.indexOf("raw stream or an MP4 file") > 0, win.refusalText);
        verify(EncoderController.runs.length === 0 || EncoderController.runs[0].status !== "encoding");
    }

    function test_aPresetNeedingAnExtraMovesAc4ToEac3() {
        const win = openAc4With(stereoUrl);
        click(findByName(win.contentItem, "preset-7.1"));
        compare(EncoderController.codecIndex, 1);
        verify(!win.visibleTabs.some(tab => tab.key === "ac4"));
    }
}
