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

    // A station's spoken detail: the text of whichever of its lines are
    // showing, in the order they are drawn. Composed from the items
    // themselves rather than from a second copy of their wording, so a
    // reader is told exactly what is on screen and nothing that is not.
    function sentences(items) {
        const said = [];
        for (let i = 0; i < items.length; ++i) {
            if (items[i] && items[i].visible && items[i].text.length > 0) {
                said.push(items[i].text);
            }
        }
        return said.join(" ");
    }

    component Warning: RowLayout {
        property alias text: body.text
        Layout.fillWidth: true
        spacing: Theme.space2
        // The sign is decoration for a reader - the sentence beside it says
        // "Warning" in words instead, so the two are not read as two things.
        Text { text: "⚠"; color: Theme.accentInk; font.pixelSize: Theme.fontBody; Layout.alignment: Qt.AlignTop; Accessible.ignored: true }
        Text {
            id: body
            Layout.fillWidth: true
            color: Theme.text
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.WordWrap
            Accessible.role: Accessible.StaticText
            Accessible.name: qsTr("Warning: %1").arg(body.text)
        }
    }
    component Station: Rectangle {
        id: station
        property string kicker: ""
        property string title: ""
        property bool warn: false
        // What the station says under its title, as one string, so a reader
        // hears the stage and its state together instead of four unrelated
        // texts it cannot tie to a heading. Each station composes it from
        // the same visible Texts it draws.
        property string detail: ""
        default property alias content: body.data
        Layout.fillWidth: true
        Layout.fillHeight: root.wide
        Layout.alignment: Qt.AlignTop
        implicitHeight: body.implicitHeight + Theme.space3 * 2
        implicitWidth: 200
        color: Theme.surface
        border.color: warn ? Theme.accentInk : Theme.divider
        border.width: 1
        Accessible.role: Accessible.Grouping
        Accessible.name: station.kicker + ", " + station.title
        Accessible.description: (station.warn ? qsTr("Warning. ") : "") + station.detail
        ColumnLayout {
            id: body
            anchors.fill: parent
            anchors.margins: Theme.space3
            spacing: Theme.space2
            Text { text: station.kicker; color: Theme.textMuted; font.pixelSize: Theme.fontMicro; font.letterSpacing: 1.2 }
            Text { Layout.fillWidth: true; text: station.title; color: Theme.text; font.family: Theme.headingFamily; font.pixelSize: Theme.fontHeading; font.weight: Font.DemiBold; elide: Text.ElideRight }
        }
    }
    component Arrow: Text {
        text: root.wide ? "→" : "↓"
        color: Theme.textMuted
        font.pixelSize: Theme.fontArrow
        // The order of the stations is the path; an arrow read aloud between
        // them adds nothing.
        Accessible.ignored: true
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

        // 1. Where applications play: the system default output.
        Station {
            objectName: "station-apps"
            kicker: qsTr("1 · APPLICATIONS PLAY TO")
            title: CrucibleController.defaultOutputName.length ? CrucibleController.defaultOutputName : qsTr("no default output")
            warn: !root.appsOnSilent || !root.silentPresent
            detail: root.sentences([playsToNote, playsToDirect, playsToNoDevice, playsToMessage])
            Text {
                id: playsToNote
                Layout.fillWidth: true
                visible: root.appsOnSilent
                text: qsTr("The silent device: nothing is heard from here. This app taps each application as it plays into it.")
                      + (CrucibleController.previousDefaultName.length ? " " + qsTr("%1 is restored on quit.").arg(CrucibleController.previousDefaultName) : "")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }
            Warning {
                id: playsToDirect
                visible: !root.appsOnSilent
                text: qsTr("A real device: you hear every application directly, on top of what this app sends out. Send applications to the silent device instead.")
            }
            Warning {
                id: playsToNoDevice
                visible: !root.silentPresent
                text: qsTr("There is no silent device to send them to: %1.").arg(CrucibleController.silentDeviceAdvice)
            }
            CrucibleButton {
                Layout.fillWidth: true
                text: root.appsOnSilent
                    ? qsTr("Restore %1").arg(CrucibleController.previousDefaultName.length ? CrucibleController.previousDefaultName : qsTr("the previous output"))
                    : qsTr("Send applications to %1").arg(CrucibleController.nullSinkName)
                enabled: root.appsOnSilent ? CrucibleController.previousDefaultName.length > 0 : (root.silentPresent || CrucibleController.silentDeviceCanCreate)
                primary: !root.appsOnSilent && (root.silentPresent || CrucibleController.silentDeviceCanCreate)
                onClicked: root.appsOnSilent ? CrucibleController.restoreDefault() : CrucibleController.moveDefaultToNullSink()
            }
            Text { id: playsToMessage; Layout.fillWidth: true; visible: CrucibleController.defaultMessage.length > 0; text: CrucibleController.defaultMessage; color: Theme.accentInk; font.pixelSize: Theme.fontSmall; wrapMode: Text.WordWrap }
        }
        Arrow {}

        // 2. This app.
        Station {
            objectName: "station-crucible"
            kicker: qsTr("2 · CRUCIBLE")
            title: CrucibleController.soundingCount + qsTr(" with sound · ") + CrucibleController.placedCount + qsTr(" placed")
            warn: !CrucibleController.running
            detail: root.sentences([crucibleNote, crucibleStopped])
            Text {
                id: crucibleNote
                Layout.fillWidth: true
                text: (CrucibleController.objectsEnabled ? qsTr("Placed applications are objects; the rest mix to the 5.1 bed.") : qsTr("No signing key: everything mixes to the 5.1 bed, placement pans within it."))
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }
            Warning {
                id: crucibleStopped
                visible: !CrucibleController.running
                text: qsTr("The engine is stopped: nothing is tapped or sent out. Start it from the status strip.")
            }
        }
        Arrow {}

        // 3. Where it is heard: the endpoint the pin and the hardware chose.
        Station {
            objectName: "station-hear"
            kicker: qsTr("3 · YOU HEAR IT ON")
            title: CrucibleController.endpointName.length ? CrucibleController.endpointName : qsTr("no endpoint")
            warn: CrucibleController.modeKey === "none"
            detail: root.sentences([hearMode, hearReason, hearChoice])
            Text { id: hearMode; Layout.fillWidth: true; text: CrucibleController.modeName; color: Theme.text; font.pixelSize: Theme.fontBody; elide: Text.ElideRight }
            Text { id: hearReason; Layout.fillWidth: true; text: CrucibleController.outputReason; color: Theme.textMuted; font.pixelSize: Theme.fontSmall; wrapMode: Text.WordWrap }
            RowLayout {
                Layout.fillWidth: true
                Text { id: hearChoice; Layout.fillWidth: true; text: (CrucibleController.preferredEndpoint.length ? qsTr("endpoint: your choice") : qsTr("endpoint: automatic")) + qsTr(" · pin: ") + CrucibleController.pinned; color: Theme.textMuted; font.family: Theme.monoFamily; font.pixelSize: Theme.fontMono; elide: Text.ElideRight }
                CrucibleButton { objectName: "chooseEndpointButton"; visible: root.showChoose; text: qsTr("Choose…"); onClicked: root.openOutput() }
            }
        }
    }
}
