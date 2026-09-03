import QtQuick

import Ac3ForgeDesk

// An application's icon: the one Windows shows for its executable (through
// the appicon image provider), with the monogram underneath until it loads
// and instead of it when the shell has nothing better than a generic sheet.
Item {
    id: root
    property string name: ""
    property string imagePath: ""
    property color fill: Theme.neutral700
    property int size: 28
    width: size
    height: size
    Monogram {
        anchors.fill: parent
        name: root.name
        fill: root.fill
        size: root.size
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
        visible: status === Image.Ready
    }
}
