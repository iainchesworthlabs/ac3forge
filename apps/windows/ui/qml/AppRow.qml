import QtQuick
import QtQuick.Layouts

import Ac3ForgeDesk

// One application in the left rail: its icon, name, the tag, the mono
// detail line, and a level bar. `app` is a live AppEntry; its properties
// update in place.
Rectangle {
    id: root
    required property var app
    property bool selected: false
    signal clicked()
    readonly property bool placed: app.slot >= 0
    readonly property string detail: placed
        ? (app.width === 2 ? qsTr("slots ") + (app.slot + 1) + "+" + (app.slot + 2) : qsTr("slot ") + (app.slot + 1)) + " · " + app.x.toFixed(2) + ", " + app.y.toFixed(2) + ", " + (app.z >= 0 ? "+" : "") + app.z.toFixed(2)
        : qsTr("bed") + (app.fullscreen ? qsTr(" · full-screen") : "") + (app.active ? "" : qsTr(" · idle")) + (app.tapped ? "" : qsTr(" · no tap")) + (app.background ? qsTr(" · background") : "")
    implicitHeight: row.implicitHeight + Theme.space4
    color: selected ? Theme.neutral100 : "transparent"
    border.color: selected ? Theme.accent : "transparent"
    border.width: 1
    Accessible.role: Accessible.ListItem
    Accessible.name: app.name + ", " + detail
    RowLayout {
        id: row
        anchors.fill: parent
        anchors.margins: Theme.space2
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        spacing: 10
        AppIcon {
            name: root.app.name
            imagePath: root.app.imagePath
            size: 28
            fill: root.placed ? (root.selected ? Theme.accent600 : Theme.neutral700) : (root.app.active ? Theme.neutral600 : Theme.neutral500)
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.space1
            RowLayout {
                spacing: Theme.space2
                Text { text: root.app.name; color: Theme.text; font.pixelSize: Theme.fontNormal; elide: Text.ElideRight; Layout.fillWidth: true }
                Rectangle {
                    visible: root.placed || root.app.fullscreen
                    implicitWidth: tag.implicitWidth + 12
                    implicitHeight: tag.implicitHeight + 4
                    color: "transparent"
                    border.color: Theme.divider
                    border.width: 1
                    Text {
                        id: tag
                        anchors.centerIn: parent
                        text: root.app.fullscreen ? qsTr("full-screen") : qsTr("placed")
                        color: Theme.textMuted
                        font.family: Theme.monoFamily
                        font.pixelSize: 10
                        font.letterSpacing: 0.5
                    }
                }
            }
            Text {
                text: root.detail
                color: Theme.textMuted
                font.family: Theme.monoFamily
                font.pixelSize: 11
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            Rectangle {
                Layout.fillWidth: true
                height: 4
                color: Theme.neutral300
                Rectangle {
                    height: 4
                    // -60 dBFS .. 0 dBFS across the bar.
                    width: parent.width * Math.max(0, Math.min(1, (root.app.level + 60) / 60))
                    color: Theme.accent700
                    Behavior on width { NumberAnimation { duration: 80 } }
                }
            }
        }
    }
    MouseArea { anchors.fill: parent; onClicked: root.clicked() }
}
