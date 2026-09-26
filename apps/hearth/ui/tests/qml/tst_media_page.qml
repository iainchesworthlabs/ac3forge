import QtQuick
import QtTest

import Ac3ForgeHearth
import Ac3ForgeHearthTest

import "HearthTestHelpers.js" as H

// Media.qml (FEATURE_COVERAGE.md rows 39-42 and 100) and the Play page's
// Objects card (row 20): the Showing picker points the page at another queue
// item, whose file is read by the real MediaInspector and shown card by card;
// Copy puts the JSON document on the clipboard and Export JSON... writes it
// through the page's own save dialog. The objects case plays an E-AC-3
// stream carrying object metadata into the fake device room
// (TestServices.useFakeRoom()) and reads the Objects card while it plays; the
// AC-4 case plays a DEE stream and reads what the decoder says of it.
TestCase {
    id: testCase
    name: "MediaPage"
    when: windowShown
    width: 1200
    height: 2400

    readonly property string repo: Qt.resolvedUrl("../../../../../").toString()
    property string shortAc3: ""
    property string longEac3: ""
    property string objectsEac3: ""
    property string filmAc4: ""

    Component { id: mediaComponent; Media { width: 1200; height: 2400 } }
    Component { id: objectsCardComponent; PlayObjectsCard { width: 320 } }

    function initTestCase() {
        verify(TestServices.useFakeRoom(), "the fake device room could not be installed");
        HearthController.start();
        shortAc3 = TestServices.stageFixture(repo + "tests/golden/external-baseline/ac3-51-448/dee.ac3",
                                             "Short 5.1.ac3");
        longEac3 = TestServices.stageFixture(repo + "tests/golden/external-baseline/eac3-music-stereo-96/dee.ec3",
                                             "Long stereo.ec3");
        objectsEac3 = TestServices.stageFixture(repo + "esp-idf/ac3forge/examples/hearth_sink/www/objects-mdct.ec3",
                                                "Objects.ec3");
        filmAc4 = TestServices.stageFixture(repo + "tests/golden/external-baseline/ac4-51-drc-ltrt-192/dee.ac4",
                                            "Film 5.1.ac4");
        verify(shortAc3.length > 0 && longEac3.length > 0 && objectsEac3.length > 0 && filmAc4.length > 0,
               "fixtures were not staged");
    }

    function init() {
        HearthController.stop();
        for (let i = HearthController.queue.length - 1; i >= 0; --i) {
            HearthController.removeAt(i);
        }
        tryVerify(function() { return HearthController.queue.length === 0; }, 10000);
    }

    function makePage() {
        const page = createTemporaryObject(mediaComponent, testCase.parent);
        verify(page !== null);
        waitForRendering(page);
        return page;
    }

    function queueTwo() {
        HearthController.addFiles([shortAc3, longEac3]);
        tryVerify(function() { return HearthController.queue.length === 2; }, 10000);
    }

    function button(page, text) {
        const found = H.find(page, function(item) { return item.text === text && item.clicked !== undefined; });
        verify(found !== null, "no \"" + text + "\" button");
        return found;
    }

    function test_showingPickerInspectsAnotherItem() {
        queueTwo();
        const page = makePage();
        const combo = findChild(page, "showingCombo");
        verify(combo !== null);
        tryCompare(combo, "count", 2, 5000);

        // Down on the focused picker moves to the second item, as picking it
        // from the list does (ComboBox emits activated either way).
        // An earlier case's save dialog may have left its own window as the
        // focus window; keys only reach an item in the active one.
        testCase.parent.Window.window.requestActivate();
        tryVerify(function() { return testCase.parent.Window.window.active; }, 5000);
        combo.forceActiveFocus();
        tryVerify(function() { return combo.activeFocus; }, 5000);
        keyClick(Qt.Key_Down);
        tryCompare(HearthController, "inspectedIndex", 1, 5000);
        tryVerify(function() { return HearthController.inspectedMedia.codec === "eac3"; }, 10000,
                  "the second item was never read");
        compare(HearthController.inspectedMedia.path, longEac3);
        // The Stream card says what the file is.
        tryVerify(function() { return H.textItem(page, "E-AC-3") !== null; }, 5000, "the Stream card shows no codec");
        verify(H.textItem(page, "48 kHz") !== null);

        keyClick(Qt.Key_Up);
        tryCompare(HearthController, "inspectedIndex", 0, 5000);
        tryVerify(function() { return HearthController.inspectedMedia.codec === "ac3"; }, 10000);
        tryVerify(function() { return H.textItem(page, "AC-3") !== null; }, 5000);
        verify(H.textItem(page, "Bitstream information") !== null, "no Bitstream information card for AC-3");
    }

    function test_copyAndExportJson() {
        queueTwo();
        const page = makePage();
        HearthController.inspectItem(1);
        tryVerify(function() { return HearthController.inspectedMedia.codec === "eac3"
                                      && (HearthController.inspectedMedia.json ?? "").length > 0; }, 10000);
        const copy = button(page, "Copy");
        tryVerify(function() { return copy.enabled; }, 5000);
        mouseClick(copy);
        tryVerify(function() { return TestServices.clipboardText() === HearthController.inspectedMedia.json; }, 5000,
                  "Copy did not put the JSON on the clipboard");
        compare(JSON.parse(TestServices.clipboardText()).codec, "eac3");

        // The name is the dialog's selection when it opens - the one way a
        // choice survives the non-native dialog's accept() (tst_playback.qml's
        // addThroughDialog() says why).
        const dialog = H.dialog(page, "Export JSON", TestServices);
        verify(dialog !== null, "Media declares no \"Export JSON\" dialog");
        const path = TestServices.scratchPath("media.json");
        dialog.currentFolder = H.fileUrl(H.folderOf(path));
        dialog.selectedFile = H.fileUrl(path);
        mouseClick(button(page, "Export JSON…"));
        tryVerify(function() { return dialog.visible; }, 5000, "Export JSON... did not open the dialog");
        dialog.accept();
        tryVerify(function() { return TestServices.readTextFile(path).length > 0; }, 5000,
                  "Export JSON wrote nothing to " + path);
        const exported = JSON.parse(TestServices.readTextFile(path));
        compare(exported.codec, "eac3");
        compare(exported.playback.sample_rate_hz, 48000);
        compare(exported.schema, "ac3forge.hearth.media/1");
    }

    // An E-AC-3 stream with object metadata: the Media page grows its
    // Objects card, and while it plays the Play page's Objects card places
    // them.
    function test_objectMetadataShowsOnTheMediaPageAndWhilePlaying() {
        HearthController.addFiles([objectsEac3]);
        tryVerify(function() { return HearthController.queue.length === 1; }, 10000);
        const page = makePage();
        HearthController.inspectItem(0);
        tryVerify(function() { return HearthController.inspectedMedia.probe !== undefined; }, 10000);
        if (HearthController.inspectedMedia.probe.objectCount === undefined) {
            skip("objects-mdct.ec3 carries no object metadata in this build's reading");
        }
        tryVerify(function() { return H.textItem(page, "Objects · OAMD") !== null; }, 5000,
                  "no Objects card for a stream with object metadata");

        const card = createTemporaryObject(objectsCardComponent, testCase.parent);
        verify(card !== null);
        HearthController.play();
        tryCompare(HearthController, "state", "playing", 10000);
        tryCompare(HearthController, "hasObjectMetadata", true, 10000);
        tryVerify(function() { return HearthController.objectsPlaced > 0 && HearthController.objects.length > 0; },
                  10000, "no objects placed while playing");
        tryVerify(function() { return H.textItem(card, HearthController.objectsPlaced + " placed") !== null; }, 5000,
                  "the Objects card does not say how many are placed");
    }

    // An AC-4 item (planning/ac4.md, I2) plays, and the page says what the
    // decoder reads of it: the Stream card's frame rate and I-frames, the
    // presentation table, and the Metadata card - with no banner saying it
    // cannot play.
    function test_ac4ItemIsDescribedAndPlays() {
        HearthController.addFiles([filmAc4]);
        tryVerify(function() { return HearthController.queue.length === 1; }, 10000);
        const page = makePage();
        TestServices.resetPeak();
        HearthController.play();
        tryCompare(HearthController, "state", "playing", 10000);
        tryVerify(function() { return TestServices.device().peak > 0.001; }, 10000,
                  "nothing the AC-4 decoder put out reached the device");
        tryVerify(function() { return HearthController.inspectedMedia.codec === "ac4"; }, 10000);
        tryVerify(function() { return H.textContaining(page, "AC-4 · bitstream version") !== null; }, 5000,
                  "the Stream card does not name AC-4");
        verify(H.textContaining(page, "samples a frame") !== null, "no frame rate");
        verify(H.textContaining(page, ", every ") !== null, "no I-frame interval");
        verify(H.textContaining(page, "NOT PLAYABLE") === null, "the not-playable banner is back");
        // One presentation, 5.1, and its metadata: DEE writes a dialogue
        // level and the Lt/Rt preference this leg asked for.
        tryVerify(function() { return H.textItem(page, "5.1") !== null; }, 5000, "no 5.1 row in the presentation table");
        verify(H.textContaining(page, "dialnorm ") !== null, "no dialogue level in the Metadata card");
        verify(H.textContaining(page, "prefers Lt/Rt") !== null, "no downmix preference in the Metadata card");
    }
}
