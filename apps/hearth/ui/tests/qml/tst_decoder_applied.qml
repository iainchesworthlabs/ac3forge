import QtQuick
import QtTest

import Ac3ForgeHearth
import Ac3ForgeHearthTest

import "HearthTestHelpers.js" as H

// The Decoder page's controls tst_decoder_settings.qml could only reach
// through the controller (FEATURE_COVERAGE.md rows 57-59, 61, 63-66): the
// Cut/Boost sliders, the four checkboxes, the RF ceiling field and the
// controls the AC-4 sub-page shares with this one, each now driven by a real
// click, drag or typed value - and the settings shown to reach what is
// actually playing: the This stream card reads the playing item, and
// switching Line to RF on the page changes what the fake device is handed.
// tst_decoder_ac4.qml drives the rest of the AC-4 sub-page.
//
// DecoderEac3.qml's sliders carry no name of their own, only the "Cut" /
// "Boost" Text beside them, so they are found by that label
// (HearthTestHelpers.besideLabel()); the checkboxes by their own text.
TestCase {
    id: testCase
    name: "DecoderApplied"
    when: windowShown
    width: 900
    // Every card on the page without scrolling - tst_decoder_settings.qml's
    // own height note explains why a scrolled-away control is unclickable.
    height: 2400

    readonly property string repo: Qt.resolvedUrl("../../../../../").toString()
    property string shortAc3: ""

    Component { id: decoderPageComponent; DecoderPage { width: 900; height: 2400 } }

    function initTestCase() {
        verify(TestServices.useFakeRoom(), "the fake device room could not be installed");
        HearthController.start();
        tryVerify(function() { return Object.keys(HearthController.decoderSettings).length > 0; }, 15000,
                  "decoderSettings was never populated");
        shortAc3 = TestServices.stageFixture(repo + "tests/golden/external-baseline/ac3-51-448/dee.ac3",
                                             "Short 5.1.ac3");
        verify(shortAc3.length > 0);
    }

    function cleanup() {
        HearthController.stop();
        for (let i = HearthController.queue.length - 1; i >= 0; --i) {
            HearthController.removeAt(i);
        }
        tryVerify(function() { return HearthController.queue.length === 0; }, 10000);
    }

    function makePage() {
        const page = createTemporaryObject(decoderPageComponent, testCase.parent);
        verify(page !== null);
        waitForRendering(page);
        return page;
    }

    function click(item, what) {
        verify(item !== null, "no control for " + what);
        mouseClick(item);
    }

    function setting(key) {
        return HearthController.decoderSettings[key];
    }

    function test_customModeSlidersAndCheckboxes() {
        const page = makePage();
        click(H.segment(page, "Mode", "custom"), "Custom");
        tryVerify(function() { return setting("mode") === "custom"; }, 10000);

        // Custom enables the sliders; a press near an end of each sets it
        // (QQC Slider moves on press), and onMoved writes it.
        const cut = H.besideLabel(page, "Cut", H.isSlider);
        const boost = H.besideLabel(page, "Boost", H.isSlider);
        verify(cut !== null && boost !== null, "no Cut/Boost sliders");
        tryVerify(function() { return cut.enabled && boost.enabled; }, 5000);
        mouseClick(cut, cut.leftPadding + 1, cut.height / 2);
        tryVerify(function() { return setting("drcCut") < 0.1; }, 10000, "Cut never went down: " + setting("drcCut"));
        mouseClick(boost, boost.leftPadding + boost.availableWidth / 2, boost.height / 2);
        tryVerify(function() { return Math.abs(setting("drcBoost") - 0.5) < 0.1; }, 10000,
                  "Boost never reached half: " + setting("drcBoost"));
        // The readout beside each follows.
        verify(H.besideLabel(page, "Cut", function(item) { return item.text === Math.round(setting("drcCut") * 100) + "%"; })
               !== null, "the Cut readout does not follow the slider");

        const heavy = H.checkBox(page, "Heavy compression");
        const dialogue = H.checkBox(page, "Dialogue normalisation");
        verify(heavy !== null && dialogue !== null);
        const heavyBefore = setting("heavyCompression");
        const dialogueBefore = setting("normaliseDialogue");
        click(heavy, "Heavy compression");
        tryVerify(function() { return setting("heavyCompression") === !heavyBefore; }, 10000);
        click(dialogue, "Dialogue normalisation");
        tryVerify(function() { return setting("normaliseDialogue") === !dialogueBefore; }, 10000);
        compare(heavy.checked, !heavyBefore);
        click(heavy, "Heavy compression");
        tryVerify(function() { return setting("heavyCompression") === heavyBefore; }, 10000);
        click(dialogue, "Dialogue normalisation");
        tryVerify(function() { return setting("normaliseDialogue") === dialogueBefore; }, 10000);
        // Line mode greys the custom controls out again.
        click(H.segment(page, "Mode", "line"), "Line");
        tryVerify(function() { return !cut.enabled && !heavy.enabled; }, 10000);
    }

    function test_rfCeilingFieldTakesATypedValueInRfMode() {
        const page = makePage();
        click(H.segment(page, "Mode", "rf"), "RF");
        tryVerify(function() { return setting("mode") === "rf"; }, 10000);
        const field = H.byAccessibleName(page, "RF ceiling, dBFS");
        verify(field !== null);
        tryVerify(function() { return field.enabled; }, 5000);
        mouseClick(field);
        field.selectAll();
        keyClick("-");
        keyClick("6");
        keyClick(Qt.Key_Return);
        tryVerify(function() { return setting("rfCeilingDb") === -6; }, 10000,
                  "the RF ceiling never reached the engine: " + setting("rfCeilingDb"));
        click(H.segment(page, "Mode", "line"), "Line");
        tryVerify(function() { return !field.enabled; }, 10000);
    }

    function test_stereoAndTransformCheckboxes() {
        const page = makePage();
        const boxes = [["Phase-shift the surround sum", "ltrtPhaseShift"], ["Mix the LFE in", "mixLfe"],
                       ["Fast inverse transform", "fastInverseTransform"]];
        for (let i = 0; i < boxes.length; ++i) {
            const box = H.checkBox(page, boxes[i][0]);
            verify(box !== null, "no \"" + boxes[i][0] + "\" checkbox");
            // The LFE in a fold is absent until set, and this page shows
            // that as off (DecoderSettings::mix_lfe).
            const before = setting(boxes[i][1]) ?? false;
            click(box, boxes[i][0]);
            tryVerify(function() { return setting(boxes[i][1]) === !before; }, 10000,
                      boxes[i][0] + " never reached the engine");
            // Space on the focused box toggles it back.
            box.forceActiveFocus();
            keyClick(Qt.Key_Space);
            tryVerify(function() { return setting(boxes[i][1]) === before; }, 10000);
        }
    }

    // AC-4's sub-page writes the fields it shares with this one through its
    // own controls ("One control for both formats", planning/ac4.md), and
    // has no operating mode of its own to write: its dynamic range is its own
    // (decision 12), so "mode" is left as it was.
    function test_ac4SubPageSharesTheDownmixAndNotTheMode() {
        const page = makePage();
        click(H.segment(page, "Mode", "rf"), "RF");
        tryVerify(function() { return setting("mode") === "rf"; }, 10000);
        click(findChild(page, "seg-ac4"), "the AC-4 sub-page");
        compare(page.format, "ac4");
        compare(H.segment(page, "Mode", "line"), null);
        click(H.segment(page, "Downmix", "ltrt"), "AC-4 Lt/Rt");
        tryVerify(function() { return setting("stereoFold") === "ltrt"; }, 10000);
        click(H.segment(page, "Bad frame", "mute"), "AC-4 Mute");
        tryVerify(function() { return setting("concealment") === "mute"; }, 10000);
        compare(setting("mode"), "rf");
        // The AC-3 and E-AC-3 sub-page shows what AC-4's set.
        click(findChild(page, "seg-eac3"), "the AC-3 and E-AC-3 sub-page");
        compare(page.format, "eac3");
        tryVerify(function() { return H.segment(page, "Downmix", "ltrt") !== null; }, 5000);
        click(H.segment(page, "Downmix", "loro"), "Lo/Ro");
        tryVerify(function() { return setting("stereoFold") === "loro"; }, 10000);
        click(H.segment(page, "Bad frame", "repeatFade"), "Repeat and fade");
        tryVerify(function() { return setting("concealment") === "repeatFade"; }, 10000);
        click(H.segment(page, "Mode", "line"), "Line");
        tryVerify(function() { return setting("mode") === "line"; }, 10000);
    }

    // The page's own readout cards follow the item playing, and a mode
    // change made on the page is heard: RF mode plays the same programme
    // louder than Line (dialogue at -20 rather than -31 dBFS), so what the
    // fake device is handed gets louder too.
    function test_settingsReachWhatIsPlaying() {
        const page = makePage();
        click(H.segment(page, "Mode", "line"), "Line");
        tryVerify(function() { return setting("mode") === "line"; }, 10000);
        HearthController.addFiles([shortAc3]);
        tryVerify(function() { return HearthController.queue.length === 1; }, 10000);
        TestServices.setClockSpeed(0.5);
        HearthController.play();
        tryCompare(HearthController, "state", "playing", 10000);
        // This stream / Programme now describe the playing file.
        tryVerify(function() { return H.textItem(page, "Short 5.1.ac3") !== null; }, 10000,
                  "the This stream card does not name the playing item");
        tryVerify(function() { return H.textContaining(page, "applied in full in Line mode") !== null
                                      || H.textItem(page, "not carried") !== null; }, 10000);

        TestServices.resetPeak();
        tryVerify(function() { return TestServices.device().peak > 0.001; }, 10000);
        const linePeak = TestServices.device().peak;

        click(H.segment(page, "Mode", "rf"), "RF");
        tryVerify(function() { return setting("mode") === "rf"; }, 10000);
        let louder = false;
        tryVerify(function() {
            const peak = TestServices.device().peak;
            TestServices.resetPeak();
            louder = louder || peak > linePeak * 1.5;
            return louder;
        }, 10000, "RF mode was not louder at the device than Line (line peak " + linePeak + ")");
        TestServices.setClockSpeed(1.0);
        click(H.segment(page, "Mode", "line"), "Line");
        tryVerify(function() { return setting("mode") === "line"; }, 10000);
    }
}
