import QtQuick

import Ac3ForgeDesk

// The application's icon stand-in: the first letters of its name in a
// square. Real Windows icons come later; the shape and size are what the
// mockups fix.
Rectangle {
    id: root
    property string name: ""
    property color fill: Theme.neutral700
    property int size: 28

    width: size
    height: size
    color: fill
    Text {
        anchors.centerIn: parent
        text: {
            const n = root.name.trim();
            if (n.length === 0) return "?";
            const words = n.split(/[\s_-]+/);
            if (words.length >= 2) return (words[0][0] + words[1][0]).toUpperCase();
            return n.length > 1 ? n[0].toUpperCase() + n[1].toLowerCase() : n[0].toUpperCase();
        }
        color: Theme.bg
        font.family: Theme.headingFamily
        font.pixelSize: Math.round(root.size * 0.43)
        font.weight: Font.DemiBold
    }
}
