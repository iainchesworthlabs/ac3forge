import QtQuick
import QtTest

import Ac3ForgeDesk

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
        DeskController.stop();
        DeskController.pinned = "auto";
        DeskController.bypassCodec = false;
    }

    function test_bypassCheckDrivesTheController() {
        const page = createTemporaryObject(outputPage, testCase);
        verify(page);
        waitForRendering(page);
        const check = findChild(page, "bypassCheck");
        verify(check, "the bypass DeskCheck carries objectName bypassCheck");
        compare(check.checked, false);
        check.toggled(true);
        compare(DeskController.bypassCodec, true);
        compare(check.checked, true);
        check.toggled(false);
        compare(DeskController.bypassCodec, false);
    }

    function test_pinnedModeRoundTrips() {
        for (const mode of ["atmos", "ddplus", "dd", "pcm", "headphones", "stereo", "auto"]) {
            DeskController.pinned = mode;
            compare(DeskController.pinned, mode);
        }
    }

    function test_endpointTableIsAListEvenWithNoEngine() {
        // Before start() the engine has probed nothing: an empty list, not
        // undefined, so the page's Repeater has something to bind to.
        // A QVariantList arrives as a sequence, not a JS Array: length is
        // what the Repeater reads.
        compare(DeskController.endpoints.length, 0);
        compare(DeskController.modeKey, "");
        compare(DeskController.running, false);
    }

    function test_pageRendersWithTheEngineStopped() {
        const page = createTemporaryObject(outputPage, testCase);
        verify(page);
        waitForRendering(page);
        verify(page.width > 0 && page.height > 0);
    }
}
