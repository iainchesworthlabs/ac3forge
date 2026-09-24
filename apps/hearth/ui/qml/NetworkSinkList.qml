import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3ForgeHearth

// The Network page's left column (planning/hearth-design.md, "Network"): every
// Sendspin player this computer has found, plus the groups it has made from
// them, shown above the sinks (network-group.png). Shared by every Network
// sub-view (pairing, the group editor, and later a sink's own settings) -
// every one of the design's artboards shows the same list, with only the
// selection changing what the rest of the page shows.
//
// "+ New group..." makes one named "New group" and selects it - the group
// editor's own "Name" field (NetworkGroupEdit.qml) is where it gets a real
// name, the same way a new item elsewhere in this application is renamed
// after creation rather than through an extra dialog first.
ColumnLayout {
    id: root
    spacing: Theme.gap

    readonly property var sinks: NetworkController.sinks
    readonly property string selectedId: NetworkController.selectedId
    readonly property var groups: NetworkController.groups
    readonly property string selectedGroupId: NetworkController.selectedGroupId

    // The column's own heading, drawn like every other numbered section in
    // the app rather than as one uppercase sentence: the ordinal is its own
    // accent-ink run, a rule fills the middle, and the counts sit right-
    // aligned as the summary (network-sink-*.png). That also takes the bare
    // "01" and the em dash out of a translated string.
    SectionHeader {
        // Elided text still reports its full, unwrapped width as this
        // layout's PREFERRED (not just minimum) width unless told otherwise
        // - Layout.minimumWidth: 0 lets it actually shrink, but Network.qml's
        // own Layout.maximumWidth: 300 on this whole component is what
        // stopped one long wrapped sentence at the bottom of this file
        // (found the hard way: it alone pushed this column's own implicit
        // width past 300px and squeezed the rest of the page down to a few
        // pixels) from growing this column past its intended size again.
        // Every elided or wrapped Text in this file and NetworkPairing.qml
        // needs this same Layout.minimumWidth: 0.
        Layout.minimumWidth: 0
        ordinal: "01"
        label: qsTr("On this network")
        summary: qsTr("%1 found · %2 group%3")
                 .arg(NetworkController.discoveredCount)
                 .arg(NetworkController.groupCount)
                 .arg(NetworkController.groupCount === 1 ? "" : "s")
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.gap

        AppButton {
            objectName: "networkNewGroup"
            text: qsTr("+ New group…")
            enabled: NetworkController.canCreateGroups
            onClicked: NetworkController.createGroup(qsTr("New group"))
        }
        AppButton {
            objectName: "networkRescan"
            text: qsTr("↻ Look again")
            onClicked: NetworkController.rescan()
        }
        Item { Layout.fillWidth: true }
    }

    ScrollView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        ColumnLayout {
            width: root.width
            spacing: Theme.gap / 2

            Repeater {
                objectName: "networkGroupRows"
                model: root.groups

                delegate: Rectangle {
                    id: groupRow
                    required property var modelData
                    readonly property bool current: modelData.id === root.selectedGroupId

                    Layout.fillWidth: true
                    implicitHeight: groupRowLayout.implicitHeight + Theme.pad
                    color: current ? Theme.accent100 : Theme.surface
                    border.color: current ? Theme.accent : Theme.border
                    border.width: current ? 2 : 1

                    Accessible.role: Accessible.RadioButton
                    Accessible.name: qsTr("%1, group").arg(modelData.name)
                    Accessible.checkable: true
                    Accessible.checked: groupRow.current
                    Accessible.onPressAction: NetworkController.selectGroup(modelData.id)

                    activeFocusOnTab: true
                    Keys.onSpacePressed: NetworkController.selectGroup(modelData.id)
                    Keys.onReturnPressed: NetworkController.selectGroup(modelData.id)

                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -Theme.focusRingOffset
                        visible: groupRow.activeFocus
                        color: "transparent"
                        border.color: Theme.focusRing
                        border.width: Theme.focusRingWidth
                        z: 100
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: NetworkController.selectGroup(groupRow.modelData.id)
                    }

                    RowLayout {
                        id: groupRowLayout
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.margins: Theme.gap
                        spacing: Theme.gap

                        Rectangle {
                            implicitWidth: 32
                            implicitHeight: 32
                            color: groupRow.current ? Theme.accent : Theme.neutral700
                            Text {
                                anchors.centerIn: parent
                                text: groupRow.modelData.icon
                                color: Theme.bg
                                font.pixelSize: Theme.fontMicro
                                font.bold: true
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2

                            Text {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                text: groupRow.modelData.name
                                color: Theme.text
                                font.pixelSize: Theme.fontBody
                                font.bold: true
                                elide: Text.ElideRight
                            }
                            Text {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                text: groupRow.modelData.subtitle
                                color: Theme.textMuted
                                font.pixelSize: Theme.fontSmall
                                elide: Text.ElideRight
                            }
                        }

                        Rectangle {
                            implicitWidth: groupBadgeText.implicitWidth + Theme.gap
                            implicitHeight: groupBadgeText.implicitHeight + 4
                            color: "transparent"
                            border.color: Theme.divider
                            border.width: 1
                            Text {
                                id: groupBadgeText
                                anchors.centerIn: parent
                                text: groupRow.modelData.badgeText
                                color: Theme.textMuted
                                font.pixelSize: Theme.fontMicro
                            }
                        }
                    }
                }
            }

            Repeater {
                model: root.sinks

                delegate: Rectangle {
                    id: row
                    required property var modelData
                    readonly property bool current: modelData.id === root.selectedId

                    Layout.fillWidth: true
                    implicitHeight: rowLayout.implicitHeight + Theme.pad
                    color: current ? Theme.accent100 : Theme.surface
                    border.color: current ? Theme.accent : Theme.border
                    border.width: current ? 2 : 1

                    Accessible.role: Accessible.RadioButton
                    Accessible.name: qsTr("%1, %2").arg(modelData.name).arg(modelData.badgeText)
                    Accessible.checkable: true
                    Accessible.checked: row.current
                    Accessible.onPressAction: NetworkController.selectSink(modelData.id)

                    activeFocusOnTab: true
                    Keys.onSpacePressed: NetworkController.selectSink(modelData.id)
                    Keys.onReturnPressed: NetworkController.selectSink(modelData.id)

                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -Theme.focusRingOffset
                        visible: row.activeFocus
                        color: "transparent"
                        border.color: Theme.focusRing
                        border.width: Theme.focusRingWidth
                        z: 100
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: NetworkController.selectSink(row.modelData.id)
                    }

                    RowLayout {
                        id: rowLayout
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.margins: Theme.gap
                        spacing: Theme.gap

                        Rectangle {
                            implicitWidth: 32
                            implicitHeight: 32
                            color: row.current ? Theme.accent : Theme.neutral700
                            Text {
                                anchors.centerIn: parent
                                text: row.modelData.icon
                                color: Theme.bg
                                font.pixelSize: Theme.fontMicro
                                font.bold: true
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2

                            Text {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                text: row.modelData.name
                                color: Theme.text
                                font.pixelSize: Theme.fontBody
                                font.bold: true
                                elide: Text.ElideRight
                            }
                            Text {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                text: row.modelData.subtitle
                                color: Theme.textMuted
                                font.pixelSize: Theme.fontSmall
                                elide: Text.ElideRight
                            }
                            Text {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                visible: row.modelData.notice.length > 0
                                text: row.modelData.notice
                                color: Theme.bad
                                font.pixelSize: Theme.fontSmall
                                elide: Text.ElideRight
                            }
                        }

                        Rectangle {
                            visible: row.modelData.badgeText.length > 0
                            implicitWidth: badgeText.implicitWidth + Theme.gap
                            implicitHeight: badgeText.implicitHeight + 4
                            color: "transparent"
                            border.color: Theme.divider
                            border.width: 1
                            Text {
                                id: badgeText
                                anchors.centerIn: parent
                                text: row.modelData.badgeText
                                color: Theme.textMuted
                                font.pixelSize: Theme.fontMicro
                            }
                        }
                    }
                }
            }
        }
    }

    Text {
        Layout.fillWidth: true
        // A wrapped Text's default Layout.minimumWidth is its full,
        // UNWRAPPED width too - this one sentence is wide enough on its own
        // to have been the whole reason this column ignored its 300px
        // Layout.preferredWidth (Network.qml) and swallowed the page.
        Layout.minimumWidth: 0
        text: qsTr("Sendspin players announce themselves on this network. A Hearth sink takes the "
                  + "E-AC-3 stream itself; any other Sendspin player takes stereo.")
        color: Theme.textMuted
        font.pixelSize: Theme.fontSmall
        wrapMode: Text.WordWrap
    }
}
