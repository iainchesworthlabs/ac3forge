import QtQuick
import QtTest

import ForgeGui

// What becomes of work still running when the controllers are destroyed, which
// is what closing the window during an encode does. Every controller hands its
// slow work to a worker that ends by posting its result back to the
// controller. A controller that did not wait for its worker left that post
// addressed to freed memory, and the process crashed as it exited, in most
// runs, after every test case had passed. The suite starts an encode and ends
// with it still running; nothing is asserted, the process exiting with code 0
// is the check. tst_ac4_decode.qml does the same for the decode, QC and
// object-inspection controllers.
//
// AC-4 because it is the slow codec: the bundled signal takes about a second
// to encode as AC-4 and about a tenth of that as AC-3, so the encode is still
// running when the process starts to exit however quickly it gets there. It is
// also the one encode with nothing that can stop it part-way, so the
// destructor has to wait it out.
TestCase {
    id: testCase
    name: "Teardown"
    when: windowShown

    readonly property url outputUrl: Qt.resolvedUrl("_test_output/tst_teardown.ac4")

    function test_anEncodeStartedAsTheSuiteEnds() {
        EncoderController.loadBundledTestSignal();
        tryCompare(EncoderController, "sourceReady", true, 30000);
        EncoderController.codecIndex = 2;
        compare(EncoderController.codecNames[2], "AC-4");
        EncoderController.encodeTo(outputUrl);
        verify(EncoderController.busy, EncoderController.status);
    }
}
