import QtQuick
import QtQuick.Layouts

import Ac3ForgeHearth

// "06 Objects" (main-play.png / play-minimum-size.png), pulled out of
// PlayPage.qml so it can be placed in either of two spots depending on
// window width - PlayPage.qml's own dedicated sidebar column when there is
// room, folded into the monitor column's scroll when there is not
// (PlayPage.qml's `narrow`, matching play-minimum-size.png's own collapse).
Card {
    title: qsTr("06 Objects")
    flat: true
    summary: HearthController.hasObjectMetadata
             ? qsTr("%1 placed").arg(HearthController.objectsPlaced)
             : (HearthController.playing ? qsTr("No object metadata in this stream")
                                          : qsTr("Nothing playing"))

    Item {
        id: room
        Layout.fillWidth: true
        Layout.preferredHeight: width
        visible: HearthController.hasObjectMetadata

        Rectangle {
            anchors.fill: parent
            color: Theme.neutral100
            border.color: Theme.border
            border.width: 1
        }
        // A stand-in for the design's dashed room circle - a plain ring, since
        // a Rectangle border has no dash pattern without pulling in
        // QtQuick.Shapes for one decorative line.
        Rectangle {
            anchors.centerIn: parent
            width: Math.min(room.width, room.height) * 0.85
            height: width
            radius: width / 2
            color: "transparent"
            border.color: Theme.neutral400
            border.width: 1
        }

        Repeater {
            model: HearthController.objects

            // oba::Position's own room cuboid (x: 0 left wall to 1 right
            // wall, y: 0 front to 1 back) - the same frame apps/gui's
            // ObjectInspectorDialog plan view places its markers in
            // (object_decode_controller.cpp), placed here directly with no
            // reprojection. label distinguishes a bed/speaker entry (named)
            // from a dynamic object (unnamed); raised is position.z above
            // the bed plane.
            delegate: Rectangle {
                id: marker
                required property var modelData
                readonly property bool isSpeaker: marker.modelData.label.length > 0
                readonly property color markerColor:
                    marker.isSpeaker ? Theme.neutral800 : Theme.bad
                width: 10
                height: 10
                color: marker.modelData.raised ? "transparent" : marker.markerColor
                border.width: marker.modelData.raised ? 2 : 0
                border.color: marker.markerColor
                x: marker.modelData.x * room.width - width / 2
                y: marker.modelData.y * room.height - height / 2

                Text {
                    visible: marker.isSpeaker
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.top
                    text: marker.modelData.label
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontFine
                }
            }
        }
    }

    Flow {
        Layout.fillWidth: true
        visible: HearthController.hasObjectMetadata
        spacing: Theme.gap

        Row {
            spacing: 4
            Rectangle { width: 8; height: 8; anchors.verticalCenter: parent.verticalCenter; color: Theme.bad }
            Text { text: qsTr("object"); color: Theme.textMuted; font.pixelSize: Theme.fontFine }
        }
        Row {
            spacing: 4
            Rectangle {
                width: 8; height: 8; anchors.verticalCenter: parent.verticalCenter
                color: "transparent"; border.color: Theme.bad; border.width: 1
            }
            Text { text: qsTr("raised"); color: Theme.textMuted; font.pixelSize: Theme.fontFine }
        }
        Row {
            spacing: 4
            Rectangle {
                width: 8; height: 8; anchors.verticalCenter: parent.verticalCenter
                color: Theme.neutral800
            }
            Text { text: qsTr("speaker"); color: Theme.textMuted; font.pixelSize: Theme.fontFine }
        }
        Row {
            spacing: 4
            Rectangle {
                width: 8; height: 8; anchors.verticalCenter: parent.verticalCenter
                color: "transparent"; border.color: Theme.neutral800; border.width: 1
            }
            Text { text: qsTr("height"); color: Theme.textMuted; font.pixelSize: Theme.fontFine }
        }
    }
}
