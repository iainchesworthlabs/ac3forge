import QtQuick
import QtQuick.Effects

import Ac3ForgeCrucible

// An application's icon: the one Windows shows for its executable (through
// the appicon image provider), with the monogram underneath until it loads
// and instead of it when the shell has nothing better than a generic sheet.
Item {
    id: root
    property string name: ""
    property string imagePath: ""
    property color fill: Theme.neutral700
    property int size: 28
    // Greyed: the application has nothing to tap right now.
    property bool dimmed: false
    width: size
    height: size
    Monogram {
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
        source: root.imagePath.length ? "image://appicon/" + encodeURIComponent(root.imagePath) : ""
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
