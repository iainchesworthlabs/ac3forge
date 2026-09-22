import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3ForgeHearth

// About: what this is, the version and build it came from, the licences it
// carries. Reached from ShortcutsDialog's own About… button ("? -> Shortcuts
// -> About -> Licences" - Main.qml's own comment says why there is no
// second header control for this). The same shape as AC3Forge Crucible's
// About (apps/crucible/ui/qml/AboutDialog.qml), on Hearth's own theme, for
// Hearth's own notices.
Dialog {
    id: root
    objectName: "aboutDialog"
    // Licences...: Main.qml opens LicencesDialog over this one.
    signal showLicences()
    modal: true
    // A Popup honours CloseOnEscape only while it has active focus, so the
    // dialog takes it as it opens; and Close is where a keyboard user wants
    // to start, so it is what has it.
    focus: true
    onOpened: aboutCloseButton.forceActiveFocus()
    anchors.centerIn: parent
    width: Math.min(560, parent ? parent.width - 60 : 560)
    padding: Theme.space6
    title: ""
    background: Rectangle {
        color: Theme.bg
        border.color: Theme.text
        border.width: 2
    }

    component Kicker: Text {
        Layout.topMargin: Theme.space2
        font.pixelSize: Theme.fontMicro
        font.letterSpacing: 1.2
        color: Theme.textMuted
    }
    component Body: Text {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        font.pixelSize: Theme.fontSmall
        color: Theme.textMuted
        onLinkActivated: function(link) { Qt.openUrlExternally(link); }
    }

    contentItem: ColumnLayout {
        spacing: Theme.space2
        // On the content, not the dialog: a Popup is not an Item.
        Accessible.role: Accessible.Dialog
        Accessible.name: qsTr("About Hearth")

        RowLayout {
            spacing: Theme.space3
            Image {
                source: "qrc:/icons/ac3forge-256.png"
                sourceSize: Qt.size(40, 40)
                Layout.preferredWidth: 40
                Layout.preferredHeight: 40
            }
            Text {
                text: qsTr("AC3Forge Hearth")
                color: Theme.text
                font.family: Theme.headingFamily
                font.pixelSize: Theme.fontTitle
                font.weight: Font.Bold
            }
        }

        Kicker { text: qsTr("WHAT IT DOES") }
        Body {
            text: qsTr("Plays AC-3, E-AC-3 and E-AC-3 JOC to a device on this computer, as a "
                        + "bitstream to a receiver, or to Hearth sinks and Sendspin players on the "
                        + "network, and shows what the decoder did with each stream. Part of the "
                        + "ac3forge project: the decoder, the renderer and the Sendspin "
                        + "implementation are the library's.")
        }

        Kicker { text: qsTr("VERSION") }
        Body {
            font.family: Theme.monoFamily
            text: HearthController.versionDetails
        }

        Kicker { text: qsTr("LICENCES") }
        Body {
            text: qsTr("AC3Forge Hearth and the ac3forge library are free software under the GNU "
                        + "General Public License, version 3 or later; the full text is LICENSE.txt "
                        + "in the package. Dolby, Dolby Atmos and Dolby Digital Plus are trademarks "
                        + "of Dolby Laboratories; this is a clean-room implementation of published "
                        + "standards and is not affiliated with Dolby.")
        }
        // No component is named here: what this build carries from others
        // differs by platform and by build, and the notices file generated
        // for it (apps/hearth/notices/) is the one place that says.
        Body {
            text: qsTr("Licences lists the third-party code in this build with each one's licence. "
                        + "The package carries the same text as NOTICES.txt.")
        }
        Body {
            Layout.topMargin: Theme.space2
            text: "<a href=\"https://github.com/iainchesworthlabs/ac3forge\">github.com/iainchesworthlabs/ac3forge</a>"
            textFormat: Text.RichText
            linkColor: Theme.accent
        }

        RowLayout {
            Layout.topMargin: Theme.space3
            Item { Layout.fillWidth: true }
            Button { objectName: "aboutLicencesButton"; text: qsTr("Licences…"); onClicked: root.showLicences() }
            Button { id: aboutCloseButton; objectName: "aboutCloseButton"; text: qsTr("Close"); highlighted: true; onClicked: root.close() }
        }
    }
}
