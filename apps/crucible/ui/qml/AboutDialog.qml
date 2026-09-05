import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3ForgeCrucible

// About: what this is, the version and build it came from, the licences it
// carries. Reached from the header's "?" and the tray menu. The same shape
// as the ac3forge GUI's About, on this app's own theme.
Dialog {
    id: root
    objectName: "aboutDialog"
    // Licences...: Main.qml opens the LicencesDialog over this one.
    signal showLicences()
    modal: true
    // A Popup honours CloseOnEscape only while it has active focus, and a
    // hand-drawn CrucibleButton is not a tab stop, so without these two lines
    // the dialog cannot be dismissed from the keyboard at all.
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
        Accessible.name: qsTr("About Crucible")
        RowLayout {
            spacing: Theme.space3
            Image {
                source: "qrc:/icons/ac3forge-256.png"
                sourceSize: Qt.size(40, 40)
                Layout.preferredWidth: 40
                Layout.preferredHeight: 40
            }
            ColumnLayout {
                spacing: 2
                Text { text: qsTr("AC3Forge Crucible"); color: Theme.text; font.family: Theme.headingFamily; font.pixelSize: Theme.fontTitle; font.weight: Font.Bold }
                Text { text: qsTr("your applications, placed in the room"); color: Theme.textMuted; font.pixelSize: Theme.fontBody }
            }
        }
        Kicker { text: qsTr("WHAT IT DOES") }
        Body {
            text: qsTr("Every application with sound becomes an object in a Dolby Atmos scene. Place it in the room; the result goes to your receiver as E-AC-3 JOC over HDMI, to a PCM or spatial endpoint, or to headphones. An ac3forge demonstration: the encoder, the object layer and the taps are the library's; this window is the room.")
        }
        Kicker { text: qsTr("VERSION") }
        Body {
            font.family: Theme.monoFamily
            text: CrucibleController.versionDetails
        }
        Kicker { text: qsTr("LICENCES") }
        Body {
            text: qsTr("AC3Forge Crucible and the ac3forge library are free software under the GNU General Public License, version 3 or later; the full text is LICENSE.txt in the package. Dolby, Dolby Atmos and Dolby Digital Plus are trademarks of Dolby Laboratories; this is a clean-room implementation of published standards and is not affiliated with Dolby.")
        }
        // No component is named here: what this build carries from others
        // differs by platform and by build, and the notices file generated
        // for it (apps/crucible/notices/) is the one place that says.
        Body {
            text: qsTr("What this build carries from others - Qt, the {fmt} library, the Archivo and Noto Sans typefaces, and what the silent device needs on this platform - is listed with each licence under Licences; the same text ships in the package as NOTICES.txt.")
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
            CrucibleButton { objectName: "aboutLicencesButton"; text: qsTr("Licences…"); onClicked: root.showLicences() }
            CrucibleButton { id: aboutCloseButton; objectName: "aboutCloseButton"; text: qsTr("Close"); primary: true; onClicked: root.close() }
        }
    }
}
