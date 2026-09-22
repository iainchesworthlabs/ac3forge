import QtQuick

import Ac3ForgeHearth

// Stands in for a page this slice has not built yet (planning/hearth-design.md
// has the approved artboard). Kept as its own file, rather than inlined six
// times in Main.qml, so each page becomes a one-line swap for the real thing
// when its own slice lands.
Item {
    property string pageName: ""

    Text {
        anchors.centerIn: parent
        text: qsTr("%1 is not built yet").arg(pageName)
        color: Theme.textMuted
        font.pixelSize: Theme.fontNormal
    }
}
