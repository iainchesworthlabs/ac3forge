import QtQuick
import QtTest

import Ac3ForgeHearth

// Network.qml built with the qmlcachegen-compiled (AOT) bindings this binary
// - and ac3hearth itself - links in, the way every suite now runs it (no
// QML_DISABLE_DISK_CACHE anywhere). Regression for a startup segfault under
// the pinned Qt 6.9.3: the Loader's sourceComponent binding used to read
// `group.id` off NetworkController.selectedGroup (a QVariantMap), and the
// generated C++ for that lookup handed AOTCompiledContext::
// initGetValueLookup() a null QMetaObject and crashed the moment the page
// was built - ac3hearth crashed on start the same way. Network.qml's own
// comment at that binding says what it reads instead and why.
//
// Each case builds the page in one of the Loader's states - nothing
// selected, a group selected - since each takes a different branch of the
// binding; a crash, not a failed assertion, is what a regression looks like.
TestCase {
    id: testCase
    name: "NetworkPageAot"
    when: windowShown
    width: 1200
    height: 800

    Component { id: networkComponent; Network { width: 1200; height: 800 } }

    function initTestCase() {
        NetworkController.start();
    }

    function test_networkPageBuildsWithNothingSelected() {
        const page = createTemporaryObject(networkComponent, testCase.parent);
        verify(page !== null);
        waitForRendering(page);
        verify(findChild(page, "networkNewGroup") !== null, "the sink list did not build");
    }

    function test_networkPageBuildsWithAGroupSelected() {
        const page = createTemporaryObject(networkComponent, testCase.parent);
        verify(page !== null);
        waitForRendering(page);
        mouseClick(findChild(page, "networkNewGroup"));
        tryVerify(function() { return NetworkController.selectedGroupId.length > 0; }, 5000);
        tryVerify(function() { return findChild(page, "networkGroupName") !== null; }, 5000,
                  "selecting a group did not show its editor");
        mouseClick(findChild(page, "networkGroupDelete"));
        tryVerify(function() { return NetworkController.selectedGroupId.length === 0; }, 5000);
        waitForRendering(page);
    }
}
