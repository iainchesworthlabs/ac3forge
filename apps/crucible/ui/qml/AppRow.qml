import QtQuick
import QtQuick.Layouts

import Ac3ForgeCrucible

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
        ? RoomWords.placement(app) + " · " + RoomWords.numbers(app.x, app.y, app.z)
        : qsTr("bed") + (app.fullscreen ? qsTr(" · full-screen") : "") + (app.silent ? qsTr(" · no audio") : (app.active ? "" : qsTr(" · idle"))) + (app.tapped || app.silent ? "" : qsTr(" · no tap")) + (app.background ? qsTr(" · background") : "")
    implicitHeight: row.implicitHeight + Theme.space4
    color: selected ? Theme.neutral100 : "transparent"
    border.color: selected ? Theme.accent : "transparent"
    border.width: 1
    // The name is what the row IS; where it sits is what a reader wants
    // next, and whether this is the chosen one. The level is deliberately
    // not here: it changes every 60 ms and would fire a name-changed event
    // at that rate for every row in the list.
    Accessible.role: Accessible.ListItem
    Accessible.name: root.app.name
    Accessible.description: root.detail
    Accessible.selected: root.selected
    Accessible.focusable: true
    Accessible.onPressAction: root.clicked()
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
            iconName: root.app.iconName
            appId: root.app.appId
            size: 28
            dimmed: root.app.silent
            fill: root.placed ? (root.selected ? Theme.accent600 : Theme.neutral700) : (root.app.active ? Theme.neutral600 : Theme.neutral500)
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.space1
            RowLayout {
                spacing: Theme.space2
                Text { text: root.app.name; color: root.app.silent ? Theme.textMuted : Theme.text; font.pixelSize: Theme.fontNormal; elide: Text.ElideRight; Layout.fillWidth: true }
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
                        font.pixelSize: Theme.fontMicro
                        font.letterSpacing: 0.5
                    }
                }
            }
            Text {
                text: root.detail
                color: Theme.textMuted
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fontMono
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
                    // Linear over one poll, so successive readings join into
                    // one motion rather than easing to a stop at each.
                    Behavior on width { NumberAnimation { duration: 60; easing.type: Easing.Linear } }
                }
            }
        }
    }
    MouseArea { anchors.fill: parent; onClicked: root.clicked() }
}
