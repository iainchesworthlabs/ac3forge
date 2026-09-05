import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3Forge

// About — reachable from Main.qml's control row, beside Preferences.
// appVersionDetails is a context property main.cpp sets once at startup
// from ac3::version_details() (build-time-immutable text: version, git
// commit/branch/dirty state, build target) - no C++ round trip needed here
// for something that never changes at runtime. Modelled directly on
// PreferencesDialog.qml's shape (modal, centered, squared border) rather
// than introducing a second dialog style.
Dialog {
    id: root

    modal: true
    anchors.centerIn: parent
    width: Math.min(560, parent ? parent.width - 60 : 560)
    padding: Theme.space6
    title: ""

    // The version as version_details() headlines it: the first line of
    // appVersionDetails reads "ac3forge <version>", the same text the
    // VERSION block below prints in full, and the version is everything
    // after that line's first space - so the subtitle and the block can
    // never disagree. typeof guards the context property's absence (the
    // Qt Quick Test harness registers none), where a bare reference would
    // raise a second ReferenceError beside the VERSION block's own.
    readonly property string headlineVersion: {
        if (typeof appVersionDetails !== "string")
            return "";
        const first = appVersionDetails.split("\n")[0];
        const space = first.indexOf(" ");
        return space < 0 ? first : first.substring(space + 1);
    }

    background: Rectangle {
        color: Theme.bg
        border.color: Theme.text
        border.width: 2
    }

    component AboutKicker: Text {
        Layout.topMargin: Theme.space2
        font.pixelSize: 10
        font.letterSpacing: 1.2
        color: Theme.textMuted
    }
    component AboutBody: Text {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        font.pixelSize: 11
        color: Theme.textMuted
        onLinkActivated: (link) => Qt.openUrlExternally(link)
    }

    contentItem: ColumnLayout {
        spacing: Theme.space4

        // A Popup/Dialog is not itself an Item ("Accessible must be
        // attached to an Item or an Action" at runtime otherwise) - its
        // contentItem is. title is "" (a styled Text below draws the
        // visible "Forge" heading instead), so Dialog's own
        // title-derived accessible name has nothing to read without this.
        Accessible.role: Accessible.Dialog
        Accessible.name: qsTr("About Forge")

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space4

            Image {
                source: "qrc:/icons/ac3forge-256.png"
                Layout.preferredWidth: 64
                Layout.preferredHeight: 64
                smooth: true
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.space1

                Text {
                    text: qsTr("Forge")
                    font.pixelSize: 22
                    font.family: Theme.headingFamily
                    font.weight: Font.ExtraBold
                    color: Theme.text
                }
                Text {
                    // The member's name above, the family and the version
                    // beneath it (docs/family/recasting.md, "The name"):
                    // "AC3Forge" is the family in prose and "Forge" is the
                    // ac3cli + ac3gui pair; "AC3Forge Forge" is never written.
                    Layout.fillWidth: true
                    text: qsTr("the AC3Forge encoder tools, %1").arg(root.headlineVersion)
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                    color: Theme.neutral700
                }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Clean-room AC-3 / E-AC-3 encoder — ATSC A/52, ETSI TS 103 420")
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                    color: Theme.neutral700
                }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 2; color: Theme.divider }

        AboutKicker { text: qsTr("VERSION") }
        Text {
            objectName: "aboutVersionText"
            Layout.fillWidth: true
            text: appVersionDetails
            font.family: Theme.monoFamily
            font.pixelSize: 12
            color: Theme.text
            wrapMode: Text.WordWrap
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 2; color: Theme.divider }

        AboutKicker { text: qsTr("LICENSE") }
        AboutBody {
            textFormat: Text.RichText
            text: qsTr("AC3Forge is free software: you can redistribute it and/or modify it "
                        + "under the terms of the GNU General Public License as published by "
                        + "the Free Software Foundation, either version 3 of the License, or "
                        + "(at your option) any later version. It is distributed WITHOUT ANY "
                        + "WARRANTY; see the <a href=\"https://www.gnu.org/licenses/gpl-3.0.html\">"
                        + "GNU General Public License</a> for details.")
        }
        AboutBody {
            // The Archivo font this dialog (and the whole app) renders in is
            // bundled under a separate license - apps/gui/fonts/OFL.txt ships
            // beside the faces because SIL OFL 1.1 requires attribution to
            // travel with the font; this is that attribution.
            text: qsTr("Includes the Archivo typeface, licensed under the SIL Open Font License 1.1.")
        }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            Button {
                objectName: "aboutCloseButton"
                text: qsTr("Close")
                highlighted: true
                onClicked: root.close()
            }
        }
    }
}
