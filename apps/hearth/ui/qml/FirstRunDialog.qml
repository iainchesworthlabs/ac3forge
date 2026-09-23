import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3ForgeHearth

// "Before you play anything" (planning/hearth-design.md; docs/hearth/design/
// screenshots/first-run.png): shown once, over the Play page, before the
// queue has played anything - what output Hearth uses, where per-output
// setup lives, and that a network sink needs pairing first. Closing it,
// either way, marks HearthController.firstRunSeen so it does not return
// (Main.qml's Component.onCompleted). Crucible's own FirstRunDialog.qml
// carries the identical shape for the identical reason - one explanation,
// said once, with a way straight to the page it is about.
Dialog {
    id: root
    modal: true
    anchors.centerIn: parent
    width: Math.min(640, parent ? parent.width - 60 : 640)
    padding: Theme.pad * 1.5
    title: ""
    closePolicy: Popup.CloseOnEscape
    // CloseOnEscape only fires while the popup itself has active focus;
    // without this the first thing a keyboard-only person meets is a modal
    // they cannot close.
    focus: true
    onOpened: notNowButton.forceActiveFocus()
    background: Rectangle {
        color: Theme.bg
        border.color: Theme.text
        border.width: 2
    }

    // Asks the window for the Speakers page.
    signal openSpeakers()

    onClosed: HearthController.firstRunSeen = true

    // One numbered row: a rule, the ordinal, a heading and a body.
    component Step: ColumnLayout {
        id: step
        property string ordinal: ""
        property string heading: ""
        property string body: ""
        Layout.fillWidth: true
        spacing: Theme.gap / 2
        Accessible.role: Accessible.Grouping
        Accessible.name: step.heading
        Accessible.description: step.body
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.gap
            Text {
                text: step.ordinal
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fontBody
                color: Theme.accentInk
                Layout.alignment: Qt.AlignTop
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Text {
                    Layout.fillWidth: true
                    text: step.heading
                    wrapMode: Text.WordWrap
                    font.pixelSize: Theme.fontHeading
                    font.bold: true
                    color: Theme.text
                }
                Text {
                    Layout.fillWidth: true
                    text: step.body
                    wrapMode: Text.WordWrap
                    font.pixelSize: Theme.fontSmall
                    color: Theme.textMuted
                }
            }
        }
    }

    contentItem: ColumnLayout {
        spacing: Theme.gap
        // On the content, not the dialog: a Popup is not an Item.
        Accessible.role: Accessible.Dialog
        Accessible.name: qsTr("Before you play anything")

        Text {
            text: qsTr("FIRST RUN")
            font.pixelSize: Theme.fontMicro
            font.bold: true
            font.letterSpacing: 1.2
            color: Theme.accentInk
        }
        Text {
            Layout.fillWidth: true
            text: qsTr("Before you play anything")
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.fontTitle
            font.bold: true
            color: Theme.text
        }

        // 01. Where it plays. deviceName is the same fact the Speakers page
        // heads itself with, so both agree on what "the default output" is.
        Step {
            objectName: "firstRunOutputRow"
            ordinal: "01"
            heading: HearthController.deviceName.length > 0
                ? qsTr("It plays to %1").arg(HearthController.deviceName)
                : qsTr("It plays to your default output")
            body: qsTr("That is this computer's default output. The output name at the top "
                      + "right opens the list of receivers, passthrough devices and network "
                      + "sinks.")
        }

        // 02. Where per-output setup lives - real, on the Speakers page
        // already (routing, trim, delay, crossover).
        Step {
            ordinal: "02"
            heading: qsTr("Your speakers are set per output")
            body: qsTr("The Speakers page holds each output's routing, levels, delays and bass "
                      + "management. Nothing in the system's own sound settings changes.")
        }

        // 03. Network sinks need pairing before they play.
        Step {
            ordinal: "03"
            heading: qsTr("Network sinks play once they're paired")
            body: qsTr("Hearth lists the Sendspin players it finds on the network. To play to "
                      + "one, pair it with the code the sink shows.")
        }

        RowLayout {
            Layout.topMargin: Theme.gap
            spacing: Theme.gap
            Item { Layout.fillWidth: true }
            Button {
                id: notNowButton
                objectName: "firstRunNotNow"
                text: qsTr("Not now")
                padding: Theme.pad
                onClicked: root.close()
                background: Rectangle {
                    color: Theme.bg
                    border.color: Theme.accentInk
                    border.width: 1
                    radius: Theme.radius
                }
                contentItem: Text {
                    text: notNowButton.text
                    color: Theme.accentInk
                    font.pixelSize: Theme.fontBody
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }
            Button {
                id: openSpeakersButton
                objectName: "firstRunOpenSpeakers"
                text: qsTr("Open Speakers")
                padding: Theme.pad
                onClicked: { root.close(); root.openSpeakers(); }
                background: Rectangle {
                    color: Theme.accent
                    radius: Theme.radius
                }
                contentItem: Text {
                    text: openSpeakersButton.text
                    color: Theme.accentText
                    font.pixelSize: Theme.fontBody
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }
    }
}
