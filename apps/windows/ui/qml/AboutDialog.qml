import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3ForgeDesk

// About: what this is, the version and build it came from, the licences it
// carries. Reached from the header's "?" and the tray menu. The same shape
// as the ac3forge GUI's About, on this app's own theme.
Dialog {
    id: root
    modal: true
    anchors.centerIn: parent
    width: Math.min(560, parent ? parent.width - 60 : 560)
    padding: Theme.space6
    title: ""
    background: Rectangle {
        color: Theme.bg
        border.color: Theme.text
        border.width: 2
    }
    Accessible.name: qsTr("About Desktop Atmos")

    component Kicker: Text {
        Layout.topMargin: Theme.space2
        font.pixelSize: 10
        font.letterSpacing: 1.2
        color: Theme.textMuted
    }
    component Body: Text {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        font.pixelSize: 12
        color: Theme.textMuted
        onLinkActivated: function(link) { Qt.openUrlExternally(link); }
    }

    contentItem: ColumnLayout {
        spacing: Theme.space2
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
                Text { text: qsTr("Desktop Atmos"); color: Theme.text; font.family: Theme.headingFamily; font.pixelSize: Theme.fontTitle; font.weight: Font.Bold }
                Text { text: qsTr("your applications, placed in the room"); color: Theme.textMuted; font.pixelSize: 13 }
            }
        }
        Kicker { text: qsTr("WHAT IT DOES") }
        Body {
            text: qsTr("Every application with sound becomes an object in a Dolby Atmos scene. Place it in the room; the result goes to your receiver as E-AC-3 JOC over HDMI, to a PCM or spatial endpoint, or to headphones. An ac3forge demonstration: the encoder, the object layer and the taps are the library's; this window is the room.")
        }
        Kicker { text: qsTr("VERSION") }
        Body {
            font.family: Theme.monoFamily
            text: DeskController.versionDetails
        }
        Kicker { text: qsTr("LICENCES") }
        Body {
            text: qsTr("ac3forge and this application: see the LICENSE file in the repository. The Desktop Atmos null-sink driver is a separately licensed derivative of Microsoft's Simple Audio Sample (MS-PL); see apps/windows/driver/README.md. Dolby, Dolby Atmos and Dolby Digital Plus are trademarks of Dolby Laboratories; this is a clean-room implementation of published standards and is not affiliated with Dolby.")
        }
        Body {
            Layout.topMargin: Theme.space2
            text: "<a href=\"https://github.com/iainchesworthlabs/dolbydigitalconverter\">github.com/iainchesworthlabs/dolbydigitalconverter</a>"
            textFormat: Text.RichText
            linkColor: Theme.accent
        }
        RowLayout {
            Layout.topMargin: Theme.space3
            Item { Layout.fillWidth: true }
            DeskButton { text: qsTr("Close"); primary: true; onClicked: root.close() }
        }
    }
}
