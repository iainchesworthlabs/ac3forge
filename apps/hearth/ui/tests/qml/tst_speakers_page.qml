import QtQuick
import QtTest

import Ac3ForgeHearth
import Ac3ForgeHearthTest

import "HearthTestHelpers.js" as H

// Speakers.qml with a device actually open (FEATURE_COVERAGE.md rows 43-54):
// every control on the page - layout presets and the as-text field, heights,
// the routing grid by mouse and its two buttons, per-speaker size / trim /
// delay / identify, the crossover and the identify level - driven by a click
// or typed text and read back through the engine's own snapshot.
//
// tst_speakers_routing.qml covers the grid's keyboard navigation on
// whatever this machine has (often no output at all, so only the NONE
// column). Here the fake device room (TestServices.useFakeRoom()) gives the
// engine a 6-output default endpoint to open, so the grid has real output
// columns and a patch can land on one; the long item is played and paused
// first, because routing only exists while an output is open.
TestCase {
    id: testCase
    name: "SpeakersPage"
    when: windowShown
    width: 1200
    // Tall enough that the page never has to scroll: mouseClick() computes
    // coordinates from an item's unscrolled position, so a card below the
    // fold would be unreachable (tst_decoder_settings.qml's own height note).
    height: 2600

    readonly property string repo: Qt.resolvedUrl("../../../../../").toString()

    Component { id: speakersComponent; Speakers { width: 1200; height: 2600 } }

    function initTestCase() {
        verify(TestServices.useFakeRoom(), "the fake device room could not be installed");
        HearthController.start();
        const path = TestServices.stageFixture(repo + "tests/golden/external-baseline/eac3-music-stereo-96/dee.ec3",
                                               "Long stereo.ec3");
        verify(path.length > 0);
        HearthController.addFiles([path]);
        tryVerify(function() { return HearthController.queue.length === 1; }, 10000);
        HearthController.play();
        tryVerify(function() { return TestServices.device().open; }, 10000, "the engine never opened the device");
        HearthController.pause();
        tryCompare(HearthController, "state", "paused", 10000);
        tryCompare(HearthController, "routingOutputs", 6, 10000);
    }

    function init() {
        HearthController.setLayoutText("2.0");
        tryCompare(HearthController, "layoutText", "2.0", 10000);
    }

    function makePage() {
        const page = createTemporaryObject(speakersComponent, testCase.parent);
        verify(page !== null);
        waitForRendering(page);
        return page;
    }

    function click(item, what) {
        verify(item !== null, "no control for " + what);
        mouseClick(item);
    }

    // Replaces a text field's contents the way a person does - click in,
    // select all, type, Return - so the field's own onEditingFinished is
    // what posts the value.
    function typeInto(field, text) {
        verify(field !== null, "no text field");
        mouseClick(field);
        field.selectAll();
        for (let i = 0; i < text.length; ++i) {
            keyClick(text.charAt(i));
        }
        keyClick(Qt.Key_Return);
    }

    // Clicks what `find()` returns until `done()` holds. For a control in a
    // Repeater delegate that the page rebuilds whenever the controller
    // republishes (a queue row, a speaker row, a group member): a click that
    // lands while its delegate is being replaced is lost, as it would be for
    // a person clicking at that instant, so it is simply made again.
    function clickUntil(find, done, message) {
        let ok = false;
        tryVerify(function() {
            ok = ok || done();
            if (!ok) {
                const target = find();
                if (target !== null) {
                    mouseClick(target);
                }
            }
            return ok;
        }, 10000, message);
    }

    function test_layoutPresetsAndTheAsTextField() {
        const page = makePage();
        click(H.segment(page, "Speaker layout", "5.1"), "the 5.1 preset");
        tryCompare(HearthController, "layoutText", "5.1", 10000);
        tryVerify(function() { return HearthController.speakerLabels.length === 6; }, 10000);
        compare(HearthController.layoutHasLfe, true);
        // The routing grid grew a row per speaker.
        tryVerify(function() { return H.byAccessibleName(page, "LFE to no output") !== null; }, 5000,
                  "the grid has no LFE row after choosing 5.1");

        typeInto(findChild(page, "speakersLayoutText"), "7.1");
        tryCompare(HearthController, "layoutText", "7.1", 10000);
        compare(HearthController.speakerLabels.length, 8);
        // An unparseable edit is refused and changes nothing; once focus
        // leaves the field it shows the layout in effect again.
        typeInto(findChild(page, "speakersLayoutText"), "not a layout");
        keyClick(Qt.Key_Tab);
        tryVerify(function() { return findChild(page, "speakersLayoutText").text === "7.1"; }, 5000,
                  "the field did not fall back to the layout in effect");
        compare(HearthController.layoutText, "7.1");
    }

    function test_heightsRealizationNeedsAHeightLayout() {
        const page = makePage();
        const heights = findChild(page, "speakersHeights");
        compare(heights.enabled, false);
        click(H.segment(page, "Speaker layout", "5.1.2"), "the 5.1.2 preset");
        tryCompare(HearthController, "layoutHasHeight", true, 10000);
        tryVerify(function() { return heights.enabled; }, 5000);
        click(H.segment(page, "Height speaker realization", "ceiling"), "In the ceiling");
        tryCompare(HearthController, "heightsRealization", "ceiling", 10000);
        click(H.segment(page, "Height speaker realization", "upfiring"), "Up-firing");
        tryCompare(HearthController, "heightsRealization", "upfiring", 10000);
    }

    function test_routingGridClicksPatchSpeakersToDeviceOutputs() {
        const page = makePage();
        compare(page.outputs, 6);
        // Six numbered columns plus NONE for each of L and R.
        verify(H.byAccessibleName(page, "L to output 6") !== null, "the grid has no sixth output column");

        // The grid's rows are rebuilt each time the engine republishes the
        // speaker setup, so each click waits for its own result before the
        // next looks its cell up afresh.
        clickUntil(function() { return H.byAccessibleName(page, "L to no output"); },
                   function() { return HearthController.routing[0] === -1; }, "NONE did not unpatch L");
        clickUntil(function() { return H.byAccessibleName(page, "R to no output"); },
                   function() { return HearthController.routing[1] === -1; }, "NONE did not unpatch R");

        clickUntil(function() { return H.byAccessibleName(page, "L to output 3"); },
                   function() { return HearthController.routing[0] === 2; }, "L was never patched to output 3");
        tryVerify(function() { return H.byAccessibleName(page, "L to output 3").Accessible.checked; }, 5000);
        clickUntil(function() { return H.byAccessibleName(page, "R to output 4"); },
                   function() { return HearthController.routing[1] === 3; }, "R was never patched to output 4");
        // An output another slot already has is refused whole.
        click(H.byAccessibleName(page, "R to output 3"), "R to output 3");
        tryVerify(function() { return HearthController.routing[1] === 3 && HearthController.routing[0] === 2; }, 5000);

        click(findChild(page, "speakersUseDeviceOrder"), "Use the device's order");
        tryVerify(function() {
            return HearthController.routing.every(function(o, slot) { return o === slot; });
        }, 10000, "Use the device's order did not patch each slot to its own output");
    }

    // "Clear" unpatches every slot of the open device. Regression: it
    // used to post a patch sized for zero outputs, which every PcmSink
    // refuses while a device is open (ac3::audio::PcmOutput::set_routing()
    // checks the output count), so it did nothing at all - it only looked
    // right in tst_speakers_routing.qml, where nothing is open.
    function test_clearRoutingButtonUnpatchesEverySlot() {
        const page = makePage();
        click(findChild(page, "speakersUseDeviceOrder"), "Use the device's order");
        tryVerify(function() { return HearthController.routing[0] === 0; }, 10000);
        click(findChild(page, "speakersClearRouting"), "Clear");
        tryVerify(function() { return HearthController.routing.every(function(o) { return o === -1; }); }, 10000,
                  "Clear left slots patched: " + HearthController.routing);
        // Still a patch for the open device, so a slot can be patched again.
        compare(HearthController.routingOutputs, 6);
        clickUntil(function() { return H.byAccessibleName(page, "L to output 2"); },
                   function() { return HearthController.routing[0] === 1; }, "L could not be patched after Clear");
    }

    function test_sizeTrimAndDelayPerSpeaker() {
        const page = makePage();
        // A small speaker needs an LFE feed to send its bass to.
        click(H.segment(page, "Speaker layout", "5.1"), "the 5.1 preset");
        tryCompare(HearthController, "layoutHasLfe", true, 10000);
        tryVerify(function() { return H.segment(page, "Size for L", "small") !== null; }, 5000);
        clickUntil(function() { return H.segment(page, "Size for L", "small"); },
                   function() { return HearthController.speakerSmall[0] === true; }, "L never became small");
        clickUntil(function() { return H.segment(page, "Size for L", "large"); },
                   function() { return HearthController.speakerSmall[0] === false; }, "L never became large again");

        typeInto(findChild(page, "speakersTrim-1"), "-3.5");
        tryVerify(function() { return HearthController.trimDb[1] === -3.5; }, 10000,
                  "the trim field never reached the engine: " + HearthController.trimDb[1]);
        typeInto(findChild(page, "speakersDelay-2"), "7.5");
        tryVerify(function() { return HearthController.delayMs[2] === 7.5; }, 10000,
                  "the delay field never reached the engine: " + HearthController.delayMs[2]);
        // Out of the validator's range: Return does not commit it.
        typeInto(findChild(page, "speakersTrim-1"), "99");
        keyClick(Qt.Key_Tab);
        compare(HearthController.trimDb[1], -3.5);
        typeInto(findChild(page, "speakersTrim-1"), "0");
        tryVerify(function() { return HearthController.trimDb[1] === 0; }, 10000);
        typeInto(findChild(page, "speakersDelay-2"), "0");
        tryVerify(function() { return HearthController.delayMs[2] === 0; }, 10000);
    }

    function test_crossoverPresetAndExactField() {
        const page = makePage();
        click(H.segment(page, "Crossover preset", "120"), "the 120 Hz preset");
        tryCompare(HearthController, "crossoverHz", 120, 10000);
        typeInto(findChild(page, "speakersCrossoverExact"), "95");
        tryCompare(HearthController, "crossoverHz", 95, 10000);
        click(H.segment(page, "Crossover preset", "80"), "the 80 Hz preset");
        tryCompare(HearthController, "crossoverHz", 80, 10000);
    }

    function test_identifyButtonAndLevel() {
        const page = makePage();
        click(H.segment(page, "Identify level", "-12"), "-12 dB");
        tryCompare(HearthController, "identifyLevelDb", -12, 10000);

        const identify = findChild(page, "speakersIdentify-1");
        click(identify, "Identify R");
        tryCompare(HearthController, "identifySlot", 1, 10000);
        tryVerify(function() { return findChild(page, "speakersIdentifyStatus").text.indexOf("R") >= 0; }, 5000,
                  "the identify status does not name the speaker sounding");
        // The same button, now Stop, ends it.
        click(identify, "Stop");
        tryCompare(HearthController, "identifySlot", -1, 10000);
        click(H.segment(page, "Identify level", "-20"), "-20 dB");
        tryCompare(HearthController, "identifyLevelDb", -20, 10000);
    }
}
