import QtQuick
import QtTest

import Ac3ForgeHearth

// DecoderPage.qml's settings round trip (issue #886): every control writes
// through DecoderEac3.qml/DecoderAc4.qml straight into
// HearthController.decoderSettings, and hearth_controller.cpp's poll() is
// what republishes it - there is no synchronous write-then-read here, so
// every assertion after a write goes through tryVerify()/tryCompare(), never
// a bare compare() (see setAndVerify()'s own comment).
//
// DecoderPage.qml/DecoderEac3.qml/DecoderAc4.qml carry no objectName on any
// control (checked by grep before writing this file) except the shared
// SegmentedControl component's own "seg-<value>" cells (apps/gui/qml/SegmentedControl.qml).
// The mode/stereoFold/dualMono/objects/jocDomain/concealment fields are
// exercised through those real cells with a real mouseClick(); drcCut,
// drcBoost, heavyCompression, normaliseDialogue, rfCeilingDb,
// ltrtPhaseShift, mixLfe and fastInverseTransform have no reachable control
// yet, so they are exercised directly through the same HearthController
// singleton the page itself reads and writes - not a parallel API, the
// identical property and invokable the page's own bindings use.
TestCase {
    id: testCase
    name: "DecoderSettings"
    when: windowShown
    width: 900
    // Tall enough that DecoderEac3.qml's ScrollView never has to scroll:
    // "02 Stereo and mono" (stereoFold) and "06 Errors and transform"
    // (concealment) sit low enough on the page that a real run at a
    // shorter height left them below the fold - mouseClick() computes
    // screen coordinates from each item's current (unscrolled) position, so
    // a segment scrolled out of the viewport is simply unreachable by
    // coordinate, not merely obscured. Confirmed empirically: at 700 both
    // fields' round-trip tests failed with the click silently landing
    // nowhere; at this height every card fits without scrolling and both
    // pass. mode/dualMono/objects/jocDomain happened to sit high enough on
    // the page to be reachable even at 700, which is why only these two
    // fields' tests ever showed it.
    height: 2400

    Component { id: decoderPageComponent; DecoderPage { width: 900; height: 2400 } }

    function init() {
        HearthController.start();
        tryVerify(function() { return Object.keys(HearthController.decoderSettings).length > 0; }, 15000,
                  "decoderSettings was never populated - no engine snapshot arrived");
    }

    // Writes `key` through HearthController.setDecoderSettings() (the same
    // call DecoderEac3.qml's own set() helper makes) and waits for the
    // poll-driven round trip back, rather than assuming it lands the same
    // event-loop turn.
    function setAndVerify(key, value) {
        const next = Object.assign({}, HearthController.decoderSettings);
        next[key] = value;
        HearthController.setDecoderSettings(next);
        tryVerify(function() { return HearthController.decoderSettings[key] === value; }, 15000,
                  "decoderSettings never reflected " + key + " = " + value);
    }

    function clickSegment(page, segmentValue) {
        const segment = findChild(page, "seg-" + segmentValue);
        verify(segment !== null, "no seg-" + segmentValue + " child found");
        mouseClick(segment);
        return segment;
    }

    function test_dualMonoObjectsJocDomainConcealmentRoundTripThroughThePage() {
        const page = createTemporaryObject(decoderPageComponent, testCase.parent);
        verify(page !== null);
        waitForRendering(page);

        clickSegment(page, "second");
        tryVerify(function() { return HearthController.decoderSettings.dualMono === "second"; }, 15000);

        clickSegment(page, "always");
        tryVerify(function() { return HearthController.decoderSettings.objects === "always"; }, 15000);

        clickSegment(page, "mdct");
        tryVerify(function() { return HearthController.decoderSettings.jocDomain === "mdct"; }, 15000);

        const mute = clickSegment(page, "mute");
        tryVerify(function() { return HearthController.decoderSettings.concealment === "mute"; }, 15000);
        // The segment's own Accessible.checked follows decoderSettings back
        // once the binding settles - a real round trip, not just a one-way
        // write.
        compare(mute.Accessible.checked, true);
    }

    // A key the map does not carry keeps the engine's last-known value
    // (decoder_settings_from_map()'s own comment in hearth_controller.cpp) -
    // writing an empty map changes nothing rather than resetting to
    // DecoderSettings{}'s defaults.
    function test_emptyMapChangesNothing() {
        setAndVerify("mode", "custom");
        const before = HearthController.decoderSettings.mode;
        HearthController.setDecoderSettings({});
        wait(150);
        compare(HearthController.decoderSettings.mode, before);
    }

    // DecoderPage.qml's own "01 AC-3 / E-AC-3 · AC-4" switch - the one
    // control on this page reachable by name that this file does not use
    // for anything else, so it is worth its own test rather than only being
    // incidental background to the others.
    function test_formatSwitchTogglesBetweenEac3AndAc4SubPages() {
        const page = createTemporaryObject(decoderPageComponent, testCase.parent);
        verify(page !== null);
        waitForRendering(page);
        compare(page.format, "eac3");

        clickSegment(page, "ac4");
        compare(page.format, "ac4");

        clickSegment(page, "eac3");
        compare(page.format, "eac3");
    }

    function test_modeSegmentedControlRoundTripsThroughThePage() {
        const page = createTemporaryObject(decoderPageComponent, testCase.parent);
        verify(page !== null);
        waitForRendering(page);

        clickSegment(page, "rf");
        tryVerify(function() { return HearthController.decoderSettings.mode === "rf"; }, 15000);

        clickSegment(page, "custom");
        tryVerify(function() { return HearthController.decoderSettings.mode === "custom"; }, 15000);

        const line = clickSegment(page, "line");
        tryVerify(function() { return HearthController.decoderSettings.mode === "line"; }, 15000);
        compare(line.Accessible.checked, true);
    }

    function test_slidersCheckboxesAndTextFieldsRoundTripThroughTheController() {
        setAndVerify("drcCut", 0.25);
        setAndVerify("drcBoost", 0.75);
        setAndVerify("heavyCompression", true);
        setAndVerify("normaliseDialogue", false);
        setAndVerify("rfCeilingDb", -20.0);
        setAndVerify("ltrtPhaseShift", false);
        setAndVerify("mixLfe", true);
        setAndVerify("fastInverseTransform", false);
    }

    function test_stereoFoldSegmentedControlRoundTripsThroughThePage() {
        const page = createTemporaryObject(decoderPageComponent, testCase.parent);
        verify(page !== null);
        waitForRendering(page);

        clickSegment(page, "ltrt");
        tryVerify(function() { return HearthController.decoderSettings.stereoFold === "ltrt"; }, 15000);

        const loro = clickSegment(page, "loro");
        tryVerify(function() { return HearthController.decoderSettings.stereoFold === "loro"; }, 15000);
        compare(loro.Accessible.checked, true);
    }
}
