import QtQuick
import QtQuick.Layouts

import Ac3ForgeCrucible

// The sound's path, as the three stations it passes through, so the two
// devices this app depends on read as two stages of one thing rather than
// two settings called "output":
//
//   applications play to  →  Crucible  →  you hear it on
//   (the Windows default,     (this app: taps,    (the endpoint the pin and
//    the silent device)        room, encoder)      the hardware chose)
//
// Each station says what it is set to, warns when it is not what the path
// needs, and carries the one action that fixes it. The Room rail shows it
// stacked; the Output page shows it side by side when there is room.
Item {
    id: root
    property bool wide: false
    // Whether the hearing station offers "Choose…" (the Room rail does; the
    // Output page is where it leads).
    property bool showChoose: true
    signal openOutput()
    implicitWidth: grid.implicitWidth
    implicitHeight: grid.implicitHeight

    readonly property bool appsOnSilent: CrucibleController.defaultIsNullSink
    readonly property bool silentPresent: CrucibleController.nullSinkPresent

    component Warning: RowLayout {
        property alias text: body.text
        Layout.fillWidth: true
        spacing: Theme.space2
        Text { text: "⚠"; color: Theme.accent; font.pixelSize: 13; Layout.alignment: Qt.AlignTop }
        Text { id: body; Layout.fillWidth: true; color: Theme.text; font.pixelSize: Theme.fontSmall; wrapMode: Text.WordWrap }
    }
    component Station: Rectangle {
        id: station
        property string kicker: ""
        property string title: ""
        property bool warn: false
        default property alias content: body.data
        Layout.fillWidth: true
        Layout.fillHeight: root.wide
        Layout.alignment: Qt.AlignTop
        implicitHeight: body.implicitHeight + Theme.space3 * 2
        implicitWidth: 200
        color: Theme.surface
        border.color: warn ? Theme.accent : Theme.divider
        border.width: 1
        ColumnLayout {
            id: body
            anchors.fill: parent
            anchors.margins: Theme.space3
            spacing: Theme.space2
            Text { text: station.kicker; color: Theme.textMuted; font.pixelSize: 10; font.letterSpacing: 1.2 }
            Text { Layout.fillWidth: true; text: station.title; color: Theme.text; font.family: Theme.headingFamily; font.pixelSize: 15; font.weight: Font.DemiBold; elide: Text.ElideRight }
        }
    }
    component Arrow: Text {
        text: root.wide ? "→" : "↓"
        color: Theme.textMuted
        font.pixelSize: 18
        Layout.alignment: Qt.AlignCenter
        Layout.leftMargin: root.wide ? Theme.space2 : 0
        Layout.rightMargin: root.wide ? Theme.space2 : 0
    }

    GridLayout {
        id: grid
        width: root.width
        columns: root.wide ? 5 : 1
        columnSpacing: 0
        rowSpacing: Theme.space1

        // 1. Where applications play: the Windows default output.
        Station {
            kicker: qsTr("1 · APPLICATIONS PLAY TO")
            title: CrucibleController.defaultOutputName.length ? CrucibleController.defaultOutputName : qsTr("no default output")
            warn: !root.appsOnSilent || !root.silentPresent
            Text {
                Layout.fillWidth: true
                visible: root.appsOnSilent
                text: qsTr("The silent device: nothing is heard from here. This app taps each application as it plays into it.")
                      + (CrucibleController.previousDefaultName.length ? " " + qsTr("%1 is restored on quit.").arg(CrucibleController.previousDefaultName) : "")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }
            Warning {
                visible: !root.appsOnSilent
                text: qsTr("A real device: you hear every application directly, on top of what this app sends out. Send applications to the silent device instead.")
            }
            Warning {
                visible: !root.silentPresent
                text: qsTr("There is no silent device to send them to: install the Desktop Atmos driver (Settings).")
            }
            CrucibleButton {
                Layout.fillWidth: true
                text: root.appsOnSilent
                    ? qsTr("Restore %1").arg(CrucibleController.previousDefaultName.length ? CrucibleController.previousDefaultName : qsTr("the previous output"))
                    : qsTr("Send applications to %1").arg(CrucibleController.nullSinkName)
                enabled: root.appsOnSilent ? CrucibleController.previousDefaultName.length > 0 : root.silentPresent
                primary: !root.appsOnSilent && root.silentPresent
                onClicked: root.appsOnSilent ? CrucibleController.restoreDefault() : CrucibleController.moveDefaultToNullSink()
            }
            Text { Layout.fillWidth: true; visible: CrucibleController.defaultMessage.length > 0; text: CrucibleController.defaultMessage; color: Theme.accent; font.pixelSize: Theme.fontSmall; wrapMode: Text.WordWrap }
        }
        Arrow {}

        // 2. This app.
        Station {
            kicker: qsTr("2 · DESKTOP ATMOS")
            title: CrucibleController.soundingCount + qsTr(" with sound · ") + CrucibleController.placedCount + qsTr(" placed")
            warn: !CrucibleController.running
            Text {
                Layout.fillWidth: true
                text: (CrucibleController.objectsEnabled ? qsTr("Placed applications are objects; the rest mix to the 5.1 bed.") : qsTr("No signing key: everything mixes to the 5.1 bed, placement pans within it."))
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }
            Warning {
                visible: !CrucibleController.running
                text: qsTr("The engine is stopped: nothing is tapped or sent out. Start it from the status strip.")
            }
        }
        Arrow {}

        // 3. Where it is heard: the endpoint the pin and the hardware chose.
        Station {
            kicker: qsTr("3 · YOU HEAR IT ON")
            title: CrucibleController.endpointName.length ? CrucibleController.endpointName : qsTr("no endpoint")
            warn: CrucibleController.modeKey === "none"
            Text { Layout.fillWidth: true; text: CrucibleController.modeName; color: Theme.text; font.pixelSize: 13; elide: Text.ElideRight }
            Text { Layout.fillWidth: true; text: CrucibleController.outputReason; color: Theme.textMuted; font.pixelSize: Theme.fontSmall; wrapMode: Text.WordWrap }
            RowLayout {
                Layout.fillWidth: true
                Text { Layout.fillWidth: true; text: (CrucibleController.preferredEndpoint.length ? qsTr("endpoint: your choice") : qsTr("endpoint: automatic")) + qsTr(" · pin: ") + CrucibleController.pinned; color: Theme.textMuted; font.family: Theme.monoFamily; font.pixelSize: 11; elide: Text.ElideRight }
                CrucibleButton { visible: root.showChoose; text: qsTr("Choose…"); onClicked: root.openOutput() }
            }
        }
    }
}
