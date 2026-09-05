import QtQuick
import QtTest

import Ac3ForgeCrucible

// The Output page: the mode pin and the codec bypass reach the controller,
// and the page renders the controller's endpoint table whatever the
// machine has.
TestCase {
    id: testCase
    name: "Output"
    when: windowShown
    width: 1480
    height: 820

    Component { id: outputPage; OutputPage { width: 1480; height: 700 } }

    function init() {
        CrucibleController.stop();
        CrucibleController.pinned = "auto";
        CrucibleController.bypassCodec = false;
    }

    function test_bypassCheckDrivesTheController() {
        const page = createTemporaryObject(outputPage, testCase);
        verify(page);
        waitForRendering(page);
        const check = findChild(page, "bypassCheck");
        verify(check, "the bypass CrucibleCheck carries objectName bypassCheck");
        compare(check.checked, false);
        check.toggled(true);
        compare(CrucibleController.bypassCodec, true);
        compare(check.checked, true);
        check.toggled(false);
        compare(CrucibleController.bypassCodec, false);
    }

    function test_pinnedModeRoundTrips() {
        for (const mode of ["atmos", "ddplus", "dd", "pcm", "headphones", "stereo", "auto"]) {
            CrucibleController.pinned = mode;
            compare(CrucibleController.pinned, mode);
        }
    }

    // The headphones pin hands decoded objects to the platform's own object
    // renderer, and the policy refuses it outright on a build without one
    // (src/audio/src/backend/*/spatial.cpp, every backend but Windows), so
    // the entry is offered exactly where it can be honoured, and the page
    // says why where it is not.
    //
    // Parented to the window's root item, not to the TestCase, because the
    // note is asserted by `visible`: a TestCase item is invisible by design
    // and an Item's `visible` reads the effective value, so nothing under one
    // can ever be seen (tst_settings.qml's driver test says the same, and
    // docs/crucible/promotion.md records the sitting that cost). The other
    // tests in this file read only the model and stay where they are.
    function test_pinOffersHeadphonesOnlyWhereTheRendererIs() {
        const page = createTemporaryObject(outputPage, testCase.parent);
        verify(page);
        waitForRendering(page);
        const pin = findChild(page, "pinBox");
        verify(pin, "the pin ComboBox carries objectName pinBox");
        compare(pin.indexOfValue("headphones") >= 0, CrucibleController.spatialAvailable,
                "the headphones entry is offered exactly where the build has an object renderer");
        compare(pin.count, CrucibleController.spatialAvailable ? 7 : 6,
                "and it is the only entry the platform can take away");
        compare(CrucibleController.spatialAbsentReason.length > 0,
                !CrucibleController.spatialAvailable,
                "the backend gives a reason for the shorter list exactly when it is shorter");
        // And the reason is on the page, in the backend's own words, exactly
        // where the entry is missing.
        const note = findChild(page, "spatialAbsentNote");
        verify(note, "the note carries objectName spatialAbsentNote");
        compare(note.visible, !CrucibleController.spatialAvailable,
                "the note is shown exactly where the headphones entry is not");
        if (note.visible) {
            verify(note.text.indexOf(CrucibleController.spatialAbsentReason) >= 0, note.text);
        }
        // A stored pin whose entry this platform does not carry reads as
        // automatic; the setting itself is left as it was found, and the
        // engine is given no pin (CrucibleController's engine_pin).
        CrucibleController.pinned = "headphones";
        compare(pin.currentIndex,
                CrucibleController.spatialAvailable ? pin.indexOfValue("headphones") : 0,
                "a pin with no entry here reads as automatic rather than blank");
        compare(CrucibleController.pinned, "headphones", "and the stored setting is untouched");
    }

    function test_endpointTableIsAListEvenWithNoEngine() {
        // Before start() the engine has probed nothing: an empty list, not
        // undefined, so the page's Repeater has something to bind to.
        // A QVariantList arrives as a sequence, not a JS Array: length is
        // what the Repeater reads.
        compare(CrucibleController.endpoints.length, 0);
        compare(CrucibleController.modeKey, "");
        compare(CrucibleController.running, false);
    }

    function test_pageRendersWithTheEngineStopped() {
        const page = createTemporaryObject(outputPage, testCase);
        verify(page);
        waitForRendering(page);
        verify(page.width > 0 && page.height > 0);
    }
}
