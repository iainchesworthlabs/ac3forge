import QtQuick
import QtQuick.Effects

import Ac3ForgeCrucible

// An application's icon: the platform's own picture for it, through the
// appicon image provider (the shell's icon for its executable on Windows,
// the icon theme's on Linux), with the monogram underneath until it loads
// and instead of it when the platform has nothing better.
//
// One URL on every platform: the executable path, then the platform's icon
// name, application id and the display name as a query. Each provider reads
// what its platform can use and ignores the rest.
Item {
    id: root
    property string name: ""
    property string imagePath: ""
    // The platform's icon-theme name and application id for it, when the
    // platform gives them; empty otherwise.
    property string iconName: ""
    property string appId: ""
    property color fill: Theme.neutral700
    property int size: 28
    // Greyed: the application has nothing to tap right now.
    property bool dimmed: false
    // Whether the provider found an icon. For tests: `visible` reads the
    // effective value and is always false under a TestCase.
    readonly property bool hasIcon: icon.status === Image.Ready
    width: size
    height: size
    Monogram {
        objectName: "monogram"
        anchors.fill: parent
        name: root.name
        fill: root.dimmed ? Theme.neutral500 : root.fill
        size: root.size
        opacity: root.dimmed ? 0.55 : 1.0
        visible: icon.status !== Image.Ready
    }
    Image {
        id: icon
        anchors.fill: parent
        // encodeURIComponent turns ?, &, + and % into %XX, so the first raw
        // ? is the separator and every value decodes on the other side.
        source: (root.imagePath.length || root.iconName.length || root.appId.length)
                ? "image://appicon/" + encodeURIComponent(root.imagePath)
                  + "?icon=" + encodeURIComponent(root.iconName)
                  + "&app=" + encodeURIComponent(root.appId)
                  + "&name=" + encodeURIComponent(root.name)
                : ""
        sourceSize: Qt.size(root.size, root.size)
        fillMode: Image.PreserveAspectFit
        smooth: true
        asynchronous: true
        cache: true
        visible: status === Image.Ready && !root.dimmed
    }
    MultiEffect {
        anchors.fill: icon
        source: icon
        visible: icon.status === Image.Ready && root.dimmed
        saturation: -1.0
        brightness: -0.1
        opacity: 0.6
    }
}
