import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3ForgeCrucible

// What Crucible is about to do to the sound settings, said once, before it
// does it (docs/crucible/promotion.md, Phase 6). Every sentence that names
// the silent device, or how this platform gets one, comes from
// CrucibleController's seams (nullSinkName, silentDeviceAdvice,
// silentDeviceBlocker, silentDeviceFromPackage, movesDefault), so the same
// file reads right on each platform in that platform's own words. Every way
// out, Escape included, counts as seen; the acknowledgement is versioned
// (firstRun/acknowledgedVersion) so a later change to what is said here can
// show it once more.
Dialog {
    id: root
    modal: true
    anchors.centerIn: parent
    width: Math.min(640, parent ? parent.width - 60 : 640)
    padding: Theme.space6
    title: ""
    closePolicy: Popup.CloseOnEscape
    // CloseOnEscape fires only while the popup has active focus; without this
    // the first thing a keyboard-only person meets is a modal they cannot
    // close. Not now takes the focus because it is the choice that changes
    // nothing.
    focus: true
    onOpened: firstRunLaterButton.forceActiveFocus()
    background: Rectangle {
        color: Theme.bg
        border.color: Theme.text
        border.width: 2
    }

    // Asks the window for the Settings page.
    signal openSettings()

    // Whether the system default output moves on this platform at all.
    // Where it does not, each application is silenced where it is tapped,
    // and the rows about the move and the restore have nothing to say.
    readonly property bool movesDefault: CrucibleController.movesDefault && CrucibleController.silentDeviceNeeded
    // Whether Send can do something now: the device is there, or this
    // application makes it as part of the same press.
    readonly property bool canSend: CrucibleController.nullSinkPresent || CrucibleController.silentDeviceCanCreate

    onClosed: CrucibleController.firstRunAcknowledged = true

    // One numbered row: a rule, the ordinal, a heading and a body, with room
    // for more lines beneath the body (the shape of the GUI's first-run
    // screen).
    component Step: ColumnLayout {
        id: step
        property string ordinal: ""
        property string heading: ""
        property string body: ""
        default property alias more: extra.data
        Layout.fillWidth: true
        spacing: Theme.space2
        Accessible.role: Accessible.Grouping
        Accessible.name: step.heading
        Accessible.description: step.body
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 2; color: Theme.divider }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space3
            Text {
                text: step.ordinal
                font.pixelSize: Theme.fontMono
                font.family: Theme.monoFamily
                color: Theme.accent700
                Layout.alignment: Qt.AlignTop
            }
            ColumnLayout {
                id: extra
                Layout.fillWidth: true
                spacing: 2
                Text { Layout.fillWidth: true; text: step.heading; wrapMode: Text.WordWrap; font.pixelSize: Theme.fontHeading; font.weight: Font.DemiBold; color: Theme.text }
                Text { Layout.fillWidth: true; text: step.body; wrapMode: Text.WordWrap; font.pixelSize: Theme.fontSmall; color: Theme.neutral700 }
            }
        }
    }
    component Detail: Text {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        font.pixelSize: Theme.fontSmall
        color: Theme.neutral700
    }

    contentItem: ColumnLayout {
        spacing: Theme.space3
        // On the content, not the dialog: a Popup is not an Item.
        Accessible.role: Accessible.Dialog
        Accessible.name: qsTr("What Crucible does to your sound settings")

        Text {
            text: qsTr("FIRST RUN")
            font.pixelSize: Theme.fontMicro
            font.letterSpacing: 1.2
            color: Theme.accent700
        }
        Text {
            Layout.fillWidth: true
            text: qsTr("What Crucible does to your sound settings")
            wrapMode: Text.WordWrap
            font.family: Theme.headingFamily
            font.pixelSize: Theme.fontTitle
            font.weight: Font.Bold
            color: Theme.text
        }

        // 01. Where applications play. The device is named by the platform
        // seam; where nothing moves, the row says so instead.
        Step {
            objectName: "firstRunDeviceRow"
            ordinal: "01"
            heading: root.movesDefault ? qsTr("Applications are sent to a silent device") : qsTr("Applications are silenced where they are tapped")
            body: root.movesDefault
                ? qsTr("Your default output becomes \"%1\", a device nothing is heard from. Every application then plays into it and Crucible taps each one there.").arg(CrucibleController.nullSinkName)
                : qsTr("Nothing in your sound settings changes here: each application is silenced where Crucible taps it.")
            Detail {
                objectName: "firstRunDeviceStatus"
                visible: root.movesDefault
                text: CrucibleController.nullSinkPresent
                    ? qsTr("\"%1\" is on this machine.").arg(CrucibleController.nullSinkName)
                    : qsTr("There is no \"%1\" yet: %2.").arg(CrucibleController.nullSinkName).arg(CrucibleController.silentDeviceAdvice)
            }
            // What stands in the way, in the platform's own words, verbatim.
            Detail {
                objectName: "firstRunDeviceBlocker"
                visible: root.movesDefault && !CrucibleController.nullSinkPresent && CrucibleController.silentDeviceBlocker.length > 0
                text: CrucibleController.silentDeviceBlocker
                color: Theme.accent
            }
        }

        // 02. What is heard, and where.
        Step {
            ordinal: "02"
            heading: qsTr("You hear one endpoint")
            body: qsTr("Crucible encodes the room and sends it to the receiver, TV, headphones or speakers it chooses; you can pin either on the Signal path page. A receiver over HDMI is opened exclusively, so nothing else can play to it while Crucible runs.")
        }

        // 03. The way back, only where something moved.
        Step {
            objectName: "firstRunRestoreRow"
            ordinal: "03"
            visible: root.movesDefault
            heading: qsTr("It is put back when Crucible quits")
            body: qsTr("Quitting from the tray restores %1. Closing the window only hides it while \"Keep running in the tray\" is on, so applications stay on the silent device until you quit or press Restore.").arg(CrucibleController.previousDefaultName.length ? CrucibleController.previousDefaultName : qsTr("your previous default output"))
            // Only where the silent device is an installed package that
            // outlives the application.
            Detail {
                visible: CrucibleController.silentDeviceFromPackage && !CrucibleController.nullSinkPresent
                text: qsTr("Installing the silent device is a separate step in Settings that asks for administrator rights, and it stays installed until you remove it.")
            }
        }

        Detail {
            visible: CrucibleController.migratedFromDemo
            Layout.topMargin: Theme.space2
            text: qsTr("Your settings from the earlier demo were carried over.")
            color: Theme.textMuted
        }

        CrucibleCheck {
            objectName: "firstRunMoveOnLaunch"
            Layout.fillWidth: true
            Layout.topMargin: Theme.space2
            visible: root.movesDefault
            text: qsTr("Do this every time Crucible starts")
            note: qsTr("Off: Crucible asks with the button on the Room page each time.")
            checked: CrucibleController.moveDefaultOnLaunch
            onToggled: function(on) { CrucibleController.moveDefaultOnLaunch = on; }
        }

        RowLayout {
            Layout.topMargin: Theme.space3
            spacing: Theme.space2
            Item { Layout.fillWidth: true }
            CrucibleButton {
                objectName: "firstRunSettings"
                text: qsTr("Open Settings")
                onClicked: { root.close(); root.openSettings(); }
            }
            CrucibleButton {
                id: firstRunLaterButton
                objectName: "firstRunLater"
                text: qsTr("Not now")
                onClicked: root.close()
            }
            // Closed before the move, so a refusal's fallback (the platform's
            // own sound settings) does not open behind a modal; the refusal's
            // message shows on the Room rail, where it already does.
            CrucibleButton {
                objectName: "firstRunSend"
                visible: root.movesDefault
                primary: true
                enabled: root.canSend
                text: root.canSend ? qsTr("Send applications to %1").arg(CrucibleController.nullSinkName) : qsTr("No silent device yet")
                onClicked: { root.close(); CrucibleController.moveDefaultToNullSink(); }
            }
        }
    }
}
