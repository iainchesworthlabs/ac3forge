import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3ForgeHearth

// The output picker (issue #828): a modal listing this computer's PCM
// devices, what they can carry over passthrough, and the Sendspin groups
// made on the Network page (planning/hearth-design.md's own artboard,
// docs/hearth/design/screenshots/output-picker.png). Modelled on
// AboutDialog.qml's shape (modal, centred, squared border) rather than
// introducing a second dialog style.
//
// "This computer · PCM" and "Network · Sendspin" are actionable here:
// HearthController.outputDevices comes straight from
// ac3::audio::enumerate_render_devices(), the same enumeration `ac3cli
// outputs` prints, and "Play here" pins the engine to whichever row is
// selected (HearthController.selectOutputDevice); a group row pins it to the
// group instead (HearthController.selectOutputGroup), whose members each get
// the stream to decode for themselves. The passthrough rows read that same
// enumeration's own live probe of each endpoint (RenderDeviceInfo::
// supports_ac3_passthrough/eac3 - "checked by opening it", not guessed) but
// stay read-only here. A single sink plays as a group of one: groups are
// where the Network page puts sinks together.
Dialog {
    id: root

    // Main.qml's own page switch handles the actual navigation; this dialog
    // only asks for it, the same shape TransportBar's buttons use for the
    // engine's commands.
    signal networkPageRequested()

    modal: true
    anchors.centerIn: parent
    width: Math.min(720, parent ? parent.width - 80 : 720)
    height: Math.min(640, parent ? parent.height - 80 : 640)
    padding: Theme.space6
    // A styled Text below draws the visible "Where Hearth plays" heading
    // instead (AboutDialog.qml's own comment says why title stays "" with
    // the accessible name given to contentItem explicitly).
    title: ""

    // The row about to be confirmed - not necessarily the one actually
    // playing (currentDeviceId), so that clicking a row is a proposal the
    // user still has to confirm with "Play here" rather than an immediate
    // switch. At most one of the two is set: a device of this computer, or
    // a network group (NetworkController.groups's own id).
    property string selectedDeviceId: ""
    property string selectedGroupId: ""

    background: Rectangle {
        color: Theme.bg
        border.color: Theme.text
        border.width: 2
    }

    onOpened: {
        HearthController.refreshOutputDevices();
        if (HearthController.outputGroupName.length > 0) {
            selectedGroupId = HearthController.outputGroupName;
            selectedDeviceId = "";
        } else {
            selectedGroupId = "";
            selectedDeviceId = root.defaultSelection();
        }
    }

    function selectDevice(id) {
        root.selectedDeviceId = id;
        root.selectedGroupId = "";
    }
    function selectGroup(id) {
        root.selectedGroupId = id;
        root.selectedDeviceId = "";
    }

    readonly property var passthroughDevices: HearthController.outputDevices.filter(
        function(d) { return d.supportsAc3 || d.supportsEac3; })

    // What to preselect when the dialog opens: the device actually playing,
    // else this machine's own default, else simply the first row - always
    // something, when there is anything to choose at all, matching the
    // mockup's own "the current output starts selected" state.
    function defaultSelection() {
        if (HearthController.currentDeviceId.length > 0) {
            return HearthController.currentDeviceId;
        }
        const devices = HearthController.outputDevices;
        for (let i = 0; i < devices.length; ++i) {
            if (devices[i].isDefault) {
                return devices[i].id;
            }
        }
        return devices.length > 0 ? devices[0].id : "";
    }

    function formatRateRange(rates) {
        if (!rates || rates.length === 0) {
            return qsTr("rates not reported");
        }
        let lo = rates[0] / 1000;
        let hi = lo;
        for (let i = 1; i < rates.length; ++i) {
            const khz = rates[i] / 1000;
            lo = Math.min(lo, khz);
            hi = Math.max(hi, khz);
        }
        const fmt = function(v) { return (Math.round(v * 10) / 10).toString(); };
        return lo === hi ? qsTr("%1 kHz").arg(fmt(lo)) : qsTr("%1–%2 kHz").arg(fmt(lo)).arg(fmt(hi));
    }

    function deviceSummary(device) {
        const parts = [device.channels > 0 ? qsTr("%1 ch").arg(device.channels)
                                            : qsTr("channels not reported")];
        if (device.speakers.length > 0) {
            parts.push(device.speakers);
        }
        parts.push(root.formatRateRange(device.sampleRates));
        return parts.join(qsTr(" · "));
    }

    function passthroughSummary(device) {
        return qsTr("AC-3 %1 · E-AC-3 %2").arg(device.supportsAc3 ? "✓" : "–")
                                          .arg(device.supportsEac3 ? "✓" : "–");
    }

    component SectionHeading: RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: Theme.space2
        spacing: Theme.space2
        property alias text: label.text
        Text {
            id: label
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
            font.bold: true
            font.capitalization: Font.AllUppercase
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }
    }

    // A borderless-when-empty badge - "playing here" today, and shaped for
    // the network section's "paired" / "in use elsewhere" / "not paired" /
    // "group" once that section has real rows to put them on.
    component StatusPill: Rectangle {
        property alias text: label.text
        visible: text.length > 0
        color: "transparent"
        border.color: Theme.border
        border.width: 1
        radius: Theme.radius
        implicitWidth: visible ? label.implicitWidth + Theme.gap : 0
        implicitHeight: label.implicitHeight + Theme.space1
        Text {
            id: label
            anchors.centerIn: parent
            color: Theme.textMuted
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fontMicro
        }
    }

    contentItem: ColumnLayout {
        spacing: Theme.space4

        Accessible.role: Accessible.Dialog
        Accessible.name: qsTr("Where Hearth plays")

        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.space1
            Text {
                text: qsTr("OUTPUT")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMicro
                font.letterSpacing: 1.2
            }
            Text {
                text: qsTr("Where Hearth plays")
                color: Theme.text
                font.pixelSize: Theme.fontTitle
                font.family: Theme.headingFamily
                font.weight: Font.ExtraBold
            }
        }

        ScrollView {
            id: deviceScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                width: deviceScroll.availableWidth
                spacing: Theme.space4

                // --- this computer - PCM (the actionable section) --------
                SectionHeading { text: qsTr("This computer · PCM") }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space1

                    Repeater {
                        model: HearthController.outputDevices
                        delegate: Rectangle {
                            id: deviceRow
                            required property var modelData
                            required property int index
                            objectName: "outputDeviceRow-" + index
                            Layout.fillWidth: true
                            readonly property bool selected: modelData.id === root.selectedDeviceId
                            readonly property bool playingHere:
                                modelData.id.length > 0
                                && modelData.id === HearthController.currentDeviceId
                            implicitHeight: pcmRow.implicitHeight + Theme.gap
                            color: selected ? Theme.accent100 : Theme.surface
                            border.color: selected ? Theme.accent : Theme.border
                            border.width: selected ? 2 : 1
                            radius: Theme.radius

                            Accessible.role: Accessible.RadioButton
                            Accessible.name: deviceRow.modelData.name
                            Accessible.checkable: true
                            Accessible.checked: deviceRow.selected
                            Accessible.onPressAction: root.selectDevice(deviceRow.modelData.id)

                            activeFocusOnTab: true
                            Keys.onSpacePressed: root.selectDevice(deviceRow.modelData.id)
                            Keys.onReturnPressed: root.selectDevice(deviceRow.modelData.id)

                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: -Theme.focusRingOffset
                                visible: deviceRow.activeFocus
                                color: "transparent"
                                border.color: Theme.focusRing
                                border.width: Theme.focusRingWidth
                                z: 100
                            }

                            RowLayout {
                                id: pcmRow
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.margins: Theme.gap
                                spacing: Theme.gap

                                Text {
                                    Layout.preferredWidth: 280
                                    Layout.maximumWidth: 280
                                    text: deviceRow.modelData.isDefault
                                          ? qsTr("%1 · default").arg(deviceRow.modelData.name)
                                          : deviceRow.modelData.name
                                    color: Theme.text
                                    font.bold: true
                                    elide: Text.ElideRight
                                }
                                Text {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 120
                                    text: root.deviceSummary(deviceRow.modelData)
                                    color: Theme.textMuted
                                    font.family: Theme.monoFamily
                                    font.pixelSize: Theme.fontSmall
                                    elide: Text.ElideRight
                                }
                                StatusPill { text: deviceRow.playingHere ? qsTr("playing here") : "" }
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.selectDevice(deviceRow.modelData.id)
                            }
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: HearthController.outputDevices.length === 0
                        text: qsTr("No output devices were found on this computer.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                    }
                }

                // --- passthrough - informational only ---------------------
                SectionHeading { text: qsTr("Passthrough · the receiver decodes") }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space1

                    Repeater {
                        model: root.passthroughDevices
                        delegate: Rectangle {
                            required property var modelData
                            Layout.fillWidth: true
                            implicitHeight: passRow.implicitHeight + Theme.gap
                            color: Theme.surface
                            border.color: Theme.border
                            border.width: 1
                            radius: Theme.radius
                            opacity: 0.85

                            RowLayout {
                                id: passRow
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.margins: Theme.gap
                                spacing: Theme.gap
                                Text {
                                    Layout.preferredWidth: 280
                                    Layout.maximumWidth: 280
                                    text: modelData.name
                                    color: Theme.text
                                    elide: Text.ElideRight
                                }
                                Text {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 120
                                    text: root.passthroughSummary(modelData)
                                    color: Theme.textMuted
                                    font.family: Theme.monoFamily
                                    font.pixelSize: Theme.fontSmall
                                }
                            }
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: root.passthroughDevices.length === 0
                        text: qsTr("No passthrough-capable outputs were found.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                    }
                }

                Text {
                    Layout.fillWidth: true
                    text: qsTr("Read by asking each endpoint whether it accepts AC-3 and E-AC-3 over "
                              + "IEC 61937 in exclusive mode. Hearth does not bitstream yet in this "
                              + "build, so picking a row here has no effect.")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                    wrapMode: Text.WordWrap
                }

                // --- network - the groups made on the Network page ---------
                SectionHeading { text: qsTr("Network · Sendspin") }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space1

                    Repeater {
                        model: NetworkController.groups
                        delegate: Rectangle {
                            id: groupRow
                            required property var modelData
                            required property int index
                            objectName: "outputGroupRow-" + index
                            Layout.fillWidth: true
                            readonly property bool selected: modelData.id === root.selectedGroupId
                            readonly property bool playingHere: modelData.id === HearthController.outputGroupName
                            implicitHeight: groupLine.implicitHeight + Theme.gap
                            color: selected ? Theme.accent100 : Theme.surface
                            border.color: selected ? Theme.accent : Theme.border
                            border.width: selected ? 2 : 1
                            radius: Theme.radius

                            Accessible.role: Accessible.RadioButton
                            Accessible.name: qsTr("%1, group").arg(groupRow.modelData.name)
                            Accessible.checkable: true
                            Accessible.checked: groupRow.selected
                            Accessible.onPressAction: root.selectGroup(groupRow.modelData.id)

                            activeFocusOnTab: true
                            Keys.onSpacePressed: root.selectGroup(groupRow.modelData.id)
                            Keys.onReturnPressed: root.selectGroup(groupRow.modelData.id)

                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: -Theme.focusRingOffset
                                visible: groupRow.activeFocus
                                color: "transparent"
                                border.color: Theme.focusRing
                                border.width: Theme.focusRingWidth
                                z: 100
                            }

                            RowLayout {
                                id: groupLine
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.margins: Theme.gap
                                spacing: Theme.gap

                                Text {
                                    Layout.preferredWidth: 280
                                    Layout.maximumWidth: 280
                                    text: groupRow.modelData.name
                                    color: Theme.text
                                    font.bold: true
                                    elide: Text.ElideRight
                                }
                                Text {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 120
                                    text: (groupRow.modelData.membersText ?? "").length > 0
                                          ? qsTr("%1 · %2").arg(groupRow.modelData.subtitle)
                                                           .arg(groupRow.modelData.membersText)
                                          : groupRow.modelData.subtitle
                                    color: Theme.textMuted
                                    font.family: Theme.monoFamily
                                    font.pixelSize: Theme.fontSmall
                                    elide: Text.ElideRight
                                }
                                StatusPill { text: groupRow.playingHere ? qsTr("playing here") : "" }
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.selectGroup(groupRow.modelData.id)
                            }
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: NetworkController.groups.length === 0
                        text: qsTr("No groups yet. On the Network page, pair a sink and add it to a group - "
                                  + "a group of one plays to a single sink.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }
                }

                Text {
                    Layout.fillWidth: true
                    visible: NetworkController.groups.length > 0
                    text: qsTr("Each sink in a group decodes the stream for itself, to its own speakers. "
                              + "A group plays to the members connected when it starts, and to others as "
                              + "they connect.")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                    wrapMode: Text.WordWrap
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.gap

            Button {
                objectName: "outputPickerNetworkPage"
                text: qsTr("Network page…")
                onClicked: { root.networkPageRequested(); root.close(); }
            }
            Item { Layout.fillWidth: true }
            Button {
                objectName: "outputPickerCancel"
                text: qsTr("Cancel")
                onClicked: root.close()
            }
            Button {
                objectName: "outputPickerPlayHere"
                text: qsTr("Play here")
                highlighted: true
                enabled: root.selectedDeviceId.length > 0 || root.selectedGroupId.length > 0
                onClicked: {
                    if (root.selectedGroupId.length > 0) {
                        HearthController.selectOutputGroup(root.selectedGroupId);
                    } else {
                        HearthController.selectOutputDevice(root.selectedDeviceId);
                    }
                    root.close();
                }
            }
        }
    }
}
