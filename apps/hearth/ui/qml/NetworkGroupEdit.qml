import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3ForgeHearth

// The group editor (planning/hearth-design.md, "Network - editing a group",
// network-group.png): a group's own name, its members with per-member
// volume/mute and a way to remove one, adding a paired sink to it, the
// group's own volume, and making it where Hearth plays. Backed by a real
// ac3::sendspin::Group (NetworkController.selectedGroup), which the engine
// plays to once it is the output (HearthController.selectOutputGroup(),
// network_group_sink.hpp); "Late chunks" still says nothing, since no member
// reports one to this page yet.
RowLayout {
    id: root
    anchors.fill: parent
    spacing: Theme.gap * 2

    readonly property var group: NetworkController.selectedGroup ?? ({})
    readonly property var members: root.group.members ?? []
    // This group is where Hearth plays: pinned as the output, and the engine
    // playing to it now.
    readonly property bool isOutput: (root.group.id ?? "") !== "" && HearthController.outputGroupName === root.group.id
    readonly property bool playingHere: root.isOutput && (HearthController.outputFormat.mode ?? "") === "networkGroup"
                                        && HearthController.playing
    readonly property bool anyConnected: root.members.some((m) => m.connected)

    // Paired sinks not already in this group - "Add to the group"'s own model.
    // A paired sink that is not connected right now can still join: it plays
    // once it is (network_sinks.hpp's own comment).
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
            ordinal: "02"
            title: qsTr("Group · %1").arg(root.group.name ?? "")

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gap

                Text {
                    text: qsTr("Name")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                }
                AppTextField {
                    id: nameField
                    objectName: "networkGroupName"
                    Layout.fillWidth: true
                    text: root.group.name ?? ""
                    onEditingFinished: NetworkController.renameGroup(root.group.id, text)
                }
                AppButton {
                    objectName: "networkGroupDelete"
                    text: qsTr("Delete group")
                    onClicked: {
                        if (root.isOutput) {
                            HearthController.selectOutputGroup("");
                        }
                        NetworkController.deleteGroup(root.group.id);
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gap

                AppButton {
                    objectName: "networkGroupPlayHere"
                    text: root.isOutput ? qsTr("Hearth plays here") : qsTr("Play to this group")
                    primary: !root.isOutput
                    enabled: !root.isOutput && root.members.length > 0
                    onClicked: HearthController.selectOutputGroup(root.group.id)
                }
                Text {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    text: root.members.length === 0
                          ? qsTr("Add a paired sink to play to this group.")
                          : root.isOutput
                            ? qsTr("Everything the queue plays goes to this group. Pick another output from the "
                                  + "output picker to stop.")
                            : qsTr("Makes this group where the queue plays, in place of this computer's own "
                                  + "output.")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                    wrapMode: Text.WordWrap
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
                    AppSlider {
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
                    AppCheckBox {
                        objectName: "networkGroupMemberMute-" + memberRow.modelData.sinkId
                        Layout.preferredWidth: 48
                        enabled: memberRow.modelData.connected && memberRow.modelData.muteSupported
                        checked: memberRow.modelData.muted
                        onToggled: NetworkController.setMemberMuted(root.group.id, memberRow.modelData.sinkId, checked)
                    }
                    AppButton {
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

                AppComboBox {
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
                AppButton {
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
                AppSlider {
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
            ordinal: "03"
            title: qsTr("Group")

            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: Theme.gap
                rowSpacing: Theme.gap / 2

                Text { text: qsTr("State"); color: Theme.textMuted; font.pixelSize: Theme.fontSmall }
                Text {
                    objectName: "networkGroupState"
                    Layout.fillWidth: true
                    text: root.playingHere
                          ? qsTr("Playing")
                          : root.isOutput
                            ? (root.anyConnected ? qsTr("The output - ready to play")
                                                 : qsTr("The output - waiting for a member to connect"))
                            : qsTr("Not the output")
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
