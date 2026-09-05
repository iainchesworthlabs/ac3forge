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
