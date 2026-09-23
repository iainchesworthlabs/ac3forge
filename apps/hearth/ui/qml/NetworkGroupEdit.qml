import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3ForgeHearth

// The group editor (planning/hearth-design.md, "Network - editing a group",
// network-group.png): a group's own name, its members with per-member
// volume/mute and a way to remove one, adding a paired sink to it, and the
// group's own volume. Backed by a real ac3::sendspin::Group
// (NetworkController.selectedGroup) - membership and volume/mute are real,
// but there is no live programme yet (Player has no network-group output
// seam - issue #874's own follow-up), so "State" and "Late chunks" below say
// so rather than showing a number this slice cannot make true.
RowLayout {
    id: root
    anchors.fill: parent
    spacing: Theme.gap * 2

    readonly property var group: NetworkController.selectedGroup ?? ({})
    readonly property var members: root.group.members ?? []

    // Paired sinks not already in this group - "Add to the group"'s own model.
    readonly property var candidateSinks: {
        const memberIds = root.members.map((m) => m.sinkId);
        return (NetworkController.sinks ?? []).filter((sink) => {
            return sink.badge === "paired" && memberIds.indexOf(sink.id) === -1;
        });
    }

    ColumnLayout {
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.preferredWidth: 2
        spacing: Theme.gap

        Card {
            title: qsTr("02 GROUP · %1").arg(root.group.name ?? "").toUpperCase()

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gap

                Text {
                    text: qsTr("Name")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                }
                TextField {
                    id: nameField
                    objectName: "networkGroupName"
                    Layout.fillWidth: true
                    text: root.group.name ?? ""
                    onEditingFinished: NetworkController.renameGroup(root.group.id, text)
                }
                Button {
                    objectName: "networkGroupDelete"
                    text: qsTr("Delete group")
                    onClicked: NetworkController.deleteGroup(root.group.id)
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gap
                Text { Layout.preferredWidth: 160; text: qsTr("MEMBER"); color: Theme.textMuted; font.pixelSize: Theme.fontMicro; font.bold: true }
                Text { Layout.preferredWidth: 140; text: qsTr("GETS"); color: Theme.textMuted; font.pixelSize: Theme.fontMicro; font.bold: true }
                Text { Layout.fillWidth: true; text: qsTr("VOLUME"); color: Theme.textMuted; font.pixelSize: Theme.fontMicro; font.bold: true }
                Item { Layout.preferredWidth: 28 }
                Text { Layout.preferredWidth: 48; text: qsTr("MUTE"); color: Theme.textMuted; font.pixelSize: Theme.fontMicro; font.bold: true }
                Item { Layout.preferredWidth: 32 }
            }

            Repeater {
                objectName: "networkGroupMemberRows"
                model: root.members

                delegate: RowLayout {
                    id: memberRow
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: Theme.gap

                    ColumnLayout {
                        // Layout.preferredWidth alone does not stop this
                        // column growing past 160px to its content's
                        // implicit width when the row has room - the same
                        // trap NetworkSinkList.qml's own comment documents -
                        // so it needs maximumWidth too, not just the inner
                        // Text's minimumWidth: 0.
                        Layout.preferredWidth: 160
                        Layout.maximumWidth: 160
                        spacing: 0
                        Text {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            text: memberRow.modelData.name
                            color: Theme.text
                            elide: Text.ElideRight
                        }
                        Text {
                            visible: !memberRow.modelData.connected
                            text: qsTr("not connected")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontMicro
                        }
                    }
                    Text {
                        Layout.preferredWidth: 140
                        Layout.maximumWidth: 140
                        Layout.minimumWidth: 0
                        text: memberRow.modelData.getsText
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        elide: Text.ElideRight
                    }
                    Slider {
                        objectName: "networkGroupMemberVolume-" + memberRow.modelData.sinkId
                        Layout.fillWidth: true
                        from: 0; to: 100
                        stepSize: 1
                        enabled: memberRow.modelData.connected && memberRow.modelData.volumeSupported
                        value: memberRow.modelData.volume
                        onMoved: NetworkController.setMemberVolume(root.group.id, memberRow.modelData.sinkId, Math.round(value))
                    }
                    Text {
                        Layout.preferredWidth: 28
                        text: memberRow.modelData.volume
                        color: Theme.textMuted
                        horizontalAlignment: Text.AlignRight
                    }
                    CheckBox {
                        objectName: "networkGroupMemberMute-" + memberRow.modelData.sinkId
                        Layout.preferredWidth: 48
                        enabled: memberRow.modelData.connected && memberRow.modelData.muteSupported
                        checked: memberRow.modelData.muted
                        onToggled: NetworkController.setMemberMuted(root.group.id, memberRow.modelData.sinkId, checked)
                    }
                    Button {
                        objectName: "networkGroupRemoveMember-" + memberRow.modelData.sinkId
                        Layout.preferredWidth: 32
                        text: "×"
                        Accessible.name: qsTr("Remove %1 from the group").arg(memberRow.modelData.name)
                        onClicked: NetworkController.removeGroupMember(root.group.id, memberRow.modelData.sinkId)
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gap

                ComboBox {
                    id: addMemberBox
                    objectName: "networkGroupAddMemberChoice"
                    Layout.fillWidth: true
                    textRole: "text"
                    valueRole: "id"
                    model: root.candidateSinks.map((sink) => ({
                        id: sink.id,
                        text: sink.name + " · " + (sink.subtitle.split(" · ")[0] ?? "")
                    }))
                    enabled: count > 0
                }
                Button {
                    objectName: "networkGroupAddMember"
                    text: qsTr("Add to the group")
                    enabled: addMemberBox.count > 0
                    onClicked: NetworkController.addGroupMember(root.group.id, addMemberBox.currentValue)
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gap

                Text {
                    text: qsTr("Group volume")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                }
                Slider {
                    objectName: "networkGroupVolume"
                    Layout.fillWidth: true
                    from: 0; to: 100
                    stepSize: 1
                    enabled: root.members.length > 0
                    value: root.group.groupVolume ?? 100
                    onMoved: NetworkController.setGroupVolume(root.group.id, Math.round(value))
                }
                Text {
                    text: qsTr("%1 · the mean of the members").arg(root.group.groupVolume ?? 100)
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                }
            }

            Text {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                text: qsTr("✓ Every member plays the same item at the same moment. A Hearth sink renders "
                          + "it to its own speakers; a Sendspin player gets stereo.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                text: qsTr("✓ Every member has synchronised its clock with this computer. A member "
                          + "reports itself ready only once it has.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                text: qsTr("This computer's own outputs are not group members in this version. A receiver "
                          + "fed a bitstream cannot be one, because it reports no decode latency.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }
        }

        Item { Layout.fillHeight: true }
    }

    ColumnLayout {
        Layout.fillWidth: true
        Layout.preferredWidth: 1
        Layout.fillHeight: true
        spacing: Theme.gap

        Card {
            Layout.fillWidth: true
            title: qsTr("03 GROUP")

            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: Theme.gap
                rowSpacing: Theme.gap / 2

                Text { text: qsTr("State"); color: Theme.textMuted; font.pixelSize: Theme.fontSmall }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Not playing yet - this version does not stream to groups.")
                    color: Theme.text
                    font.pixelSize: Theme.fontSmall
                    wrapMode: Text.WordWrap
                }
                Text { text: qsTr("Members"); color: Theme.textMuted; font.pixelSize: Theme.fontSmall }
                Text {
                    Layout.fillWidth: true
                    text: root.group.membersConnectedText ?? ""
                    color: Theme.text
                    font.pixelSize: Theme.fontSmall
                }
                Text { text: qsTr("Lead time"); color: Theme.textMuted; font.pixelSize: Theme.fontSmall }
                Text {
                    Layout.fillWidth: true
                    text: (root.group.leadTimeText ?? "").length > 0 ? root.group.leadTimeText : qsTr("—")
                    color: Theme.text
                    font.pixelSize: Theme.fontSmall
                    wrapMode: Text.WordWrap
                }
                Text { text: qsTr("Late chunks"); color: Theme.textMuted; font.pixelSize: Theme.fontSmall }
                Text { text: qsTr("—"); color: Theme.text; font.pixelSize: Theme.fontSmall }
            }
        }

        Item { Layout.fillHeight: true }
    }
}
