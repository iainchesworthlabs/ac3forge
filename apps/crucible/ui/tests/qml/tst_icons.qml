import QtQuick
import QtTest

import Ac3ForgeCrucible

// The application icon, through the appicon image provider the harness
// registers (qml_test_main.cpp). On Linux, each of the provider's lookup
// rungs against the fixture theme and .desktop entries under
// ui/tests/fixtures/xdg: the ctest entry points XDG_DATA_DIRS there, and
// the provider seeds Qt's icon theme from it under the offscreen platform.
// On every platform, the monogram where nothing matches. The rung cases
// skip off Linux: the Windows provider reads only the path and asks the
// shell, which knows nothing of a fixture theme. hasIcon rather than
// visible: a TestCase item is invisible by design, so nothing under it is
// ever effectively visible (tst_settings.qml).
TestCase {
    id: testCase
    name: "Icons"
    when: windowShown
    width: 400
    height: 300

    Component { id: appIcon; AppIcon {} }

    readonly property bool isLinux: Qt.platform.os === "linux"

    function makeIcon(props) {
        const item = createTemporaryObject(appIcon, testCase, props);
        verify(item, "an AppIcon was created");
        return item;
    }

    function expectIcon(item, rung) {
        tryVerify(function() { return item.hasIcon; }, 5000, rung + " did not reach Image.Ready");
    }

    function test_iconNameResolvesThroughTheTheme() {
        if (!isLinux) skip("the icon theme is the Linux provider's");
        expectIcon(makeIcon({ name: "Fixture", iconName: "crucible-fixture-icon" }), "the icon-name rung");
    }

    function test_desktopEntryByExec() {
        if (!isLinux) skip("the .desktop entries are the Linux provider's");
        // No icon name and no application id: the entry whose Exec names
        // this binary carries the icon. A NoDisplay entry with the same Exec
        // sorts before it and names an icon the fixture theme lacks, so a
        // wrong pick would be a monogram.
        expectIcon(makeIcon({ name: "Whatever", imagePath: "/nowhere/bin/crucible-fixture-byexec" }), "the Exec rung");
    }

    function test_desktopEntryByWmClass() {
        if (!isLinux) skip("the .desktop entries are the Linux provider's");
        // A binary no entry names; the StartupWMClass equals the name.
        expectIcon(makeIcon({ name: "Crucible Fixture WM", imagePath: "/nowhere/launcher-x" }), "the StartupWMClass rung");
    }

    function test_desktopEntryByAppId() {
        if (!isLinux) skip("the .desktop entries are the Linux provider's");
        // A sandboxed application: no path at all, only its application id,
        // which is the .desktop file's name.
        expectIcon(makeIcon({ name: "Sandboxed", appId: "org.ac3forge.CrucibleFixture" }), "the application-id rung");
    }

    function test_themeByBinaryName() {
        if (!isLinux) skip("the icon theme is the Linux provider's");
        // No entry names it; the theme has an icon under the binary's name.
        expectIcon(makeIcon({ name: "Bare", imagePath: "/nowhere/crucible-fixture-bare" }), "the theme-by-binary rung");
    }

    function test_unknownApplicationKeepsTheMonogram() {
        const item = makeIcon({ name: "Nothing", imagePath: "/nowhere/nothing-here" });
        wait(1000);
        compare(item.hasIcon, false);
        const monogram = findChild(item, "monogram");
        verify(monogram, "the monogram carries objectName monogram");
        compare(monogram.text, "No");
    }

    function test_sizesFollowTheRequest() {
        if (!isLinux) skip("the icon theme is the Linux provider's");
        // The rail's size and the 3D room's, from one identity: the fixture
        // theme carries 32 and 256, and each request is scaled to its own.
        const small = makeIcon({ name: "Fixture", iconName: "crucible-fixture-icon", size: 28 });
        const large = makeIcon({ name: "Fixture", iconName: "crucible-fixture-icon", size: 160 });
        expectIcon(small, "the 28-pixel request");
        expectIcon(large, "the 160-pixel request");
        compare(small.width, 28);
        compare(large.width, 160);
    }
}
