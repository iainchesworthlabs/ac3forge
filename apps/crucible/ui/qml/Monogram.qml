import QtQuick

import Ac3ForgeCrucible

// The application's icon stand-in: the first letters of its name in a
// square. AppIcon shows it until the platform's own icon has loaded, and
// instead of one where the platform has none; the shape and size are what
// the mockups fix.
Rectangle {
    id: root
    property string name: ""
    property color fill: Theme.neutral700
    property int size: 28
    // The letters shown, for tests.
    readonly property string text: label.text

    width: size
    height: size
    color: fill
    Text {
        id: label
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
