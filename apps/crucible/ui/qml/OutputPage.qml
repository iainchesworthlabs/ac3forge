import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3ForgeCrucible

// The signal path: the three stations sound passes through, side by side,
// then each stage's detail: the pin (what you hear it as), every endpoint
// with what the probe found, where applications play, the codec path.
//
// Sizing: one column that scrolls; the two-up rows become one-up below
// about 900 px, the endpoint table drops its note column below about 760,
// and every button row wraps rather than overflows.
Flickable {
    id: page
    contentHeight: column.implicitHeight + Theme.space6 * 2
    clip: true
    readonly property real innerWidth: width - Theme.space6 * 2
    readonly property bool twoUp: innerWidth >= 900
    readonly property bool wideTable: innerWidth >= 760
    // The endpoint table's fixed columns, each as wide as its own heading
    // where the English width is not enough, so a translated head widens
    // its column instead of eliding and the cells under it follow the
    // same number. The heading is the widest thing in each of these
    // columns: a tick, a dash or a channel count sits below it.
    readonly property real eac3Column: Math.max(56, eac3Head.implicitWidth + 8)
    readonly property real ac3Column: Math.max(48, ac3Head.implicitWidth + 8)
    readonly property real pcmColumn: Math.max(56, pcmHead.implicitWidth + 8)
    readonly property real spatialColumn: Math.max(60, spatialHead.implicitWidth + 8)
    readonly property real noteColumn: Math.max(220, noteHead.implicitWidth + 8)

    ColumnLayout {
        id: column
        x: Theme.space6
        y: Theme.space6
        width: page.innerWidth
        spacing: Theme.space6

        RailBlock {
            ordinal: "01"
            label: qsTr("SIGNAL PATH")
            Layout.fillWidth: true
            Layout.fillHeight: false
            Text { text: qsTr("two devices, two stages: applications play into one, you hear the result on the other"); color: Theme.textMuted; font.family: Theme.monoFamily; font.pixelSize: Theme.fontMono; elide: Text.ElideRight; Layout.fillWidth: true }
        }
        Text {
            Layout.fillWidth: true
            text: qsTr("Every application plays into the system default output. With the silent \"%1\" device as that default, nothing is heard from it; this app taps each application there, places it in the room, encodes the scene, and sends the result to the endpoint the pin and the hardware choose. That endpoint is the only thing you hear.").arg(CrucibleController.nullSinkName)
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.WordWrap
        }
        SignalPath {
            Layout.fillWidth: true
            wide: page.twoUp
            showChoose: false
        }

        RailBlock { ordinal: "02"; label: qsTr("WHAT YOU HEAR IT AS"); Layout.fillWidth: true; Layout.fillHeight: false }
        Card {
            GridLayout {
                Layout.fillWidth: true
                columns: page.twoUp ? 2 : 1
                columnSpacing: Theme.space6
                rowSpacing: Theme.space4
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignTop
                    spacing: Theme.space2
                    Text { Layout.fillWidth: true; text: CrucibleController.modeKey === "atmos" ? qsTr("%1 · E-AC-3 JOC over HDMI").arg(CrucibleController.modeName) : CrucibleController.modeName; color: Theme.text; font.family: Theme.headingFamily; font.pixelSize: Theme.fontTitle; font.weight: Font.Bold; wrapMode: Text.WordWrap }
                    Text { Layout.fillWidth: true; text: qsTr("%1 The endpoint follows the hardware: pull HDMI and it moves to the next best one below.").arg(CrucibleController.outputReason); color: Theme.textMuted; font.pixelSize: Theme.fontBody; wrapMode: Text.WordWrap }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.maximumWidth: page.twoUp ? 420 : -1
                    Layout.alignment: Qt.AlignTop
                    spacing: Theme.space2
                    //: Kicker over the pin control (the verb: pin the stream to this mode)
                    Text { text: qsTr("PIN"); color: Theme.textMuted; font.pixelSize: Theme.fontMono; font.letterSpacing: 1 }
                    ComboBox {
                        id: pinBox
                        objectName: "pinBox"
                        // The "PIN" kicker above is a separate Text; a
                        // reader has no way to tie it to this control.
                        Accessible.name: qsTr("Pin")
                        Layout.fillWidth: true
                        implicitHeight: Math.max(30, pinText.implicitHeight + 10)
                        // Every mode the policy can choose, less any this build
                        // cannot reach. Headphones hands decoded objects to the
                        // platform's own object renderer, and
                        // CrucibleController.spatialAvailable says whether this
                        // build has one; where it does not, the policy refuses
                        // that mode on every endpoint, so offering it would be
                        // offering a choice that always falls back. Dropped
                        // rather than shown greyed, because a dropdown lists
                        // what can be chosen and the window already drops a
                        // control a build cannot carry (RoomPage.qml's 3D
                        // switch, on CrucibleController.has3D); sync() below is
                        // already written for a stored pin with no entry, and
                        // the reason is printed under the box.
                        model: [
                            { label: qsTr("Automatic · best the hardware can carry"), value: "auto" },
                            { label: qsTr("Atmos"), value: "atmos" },
                            { label: qsTr("Dolby Digital Plus 5.1"), value: "ddplus" },
                            { label: qsTr("Dolby Digital 5.1"), value: "dd" },
                            { label: qsTr("PCM surround"), value: "pcm" },
                            { label: qsTr("Headphones · spatial sound"), value: "headphones" },
                            { label: qsTr("Stereo"), value: "stereo" }]
                            .filter(function(mode) { return mode.value !== "headphones" || CrucibleController.spatialAvailable; })
                        textRole: "label"
                        valueRole: "value"
                        // Never blank: an unknown or empty pin reads as
                        // automatic, which is also what a pin whose entry this
                        // platform does not carry reads as.
                        function sync() { const i = indexOfValue(CrucibleController.pinned); currentIndex = i < 0 ? 0 : i; }
                        Component.onCompleted: sync()
                        onModelChanged: sync()
                        onActivated: CrucibleController.pinned = currentValue
                        Connections { target: CrucibleController; function onSettingsChanged() { pinBox.sync(); } }
                        font.pixelSize: Theme.fontBody
                        background: Rectangle { color: Theme.neutral100; border.color: pinBox.activeFocus ? Theme.focusRing : Theme.divider; border.width: 1 }
                        // Anchored rather than placed at an x, and padded by
                        // the side the control is on, so both follow the
                        // window's mirroring under a right-to-left language.
                        contentItem: Text { id: pinText; leftPadding: pinBox.mirrored ? 26 : 10; rightPadding: pinBox.mirrored ? 10 : 26; text: pinBox.displayText; color: Theme.text; font.pixelSize: Theme.fontBody; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight }
                        indicator: Text { anchors.right: parent.right; anchors.rightMargin: 12; anchors.verticalCenter: parent.verticalCenter; text: "⌄"; color: Theme.textMuted; font.pixelSize: Theme.fontNormal }
                        popup.background: Rectangle { color: Theme.surface; border.color: Theme.divider; border.width: 1 }
                        delegate: ItemDelegate {
                            required property var modelData
                            required property int index
                            width: pinBox.width
                            contentItem: Text { text: modelData.label; color: Theme.text; font.pixelSize: Theme.fontBody }
                            background: Rectangle { color: highlighted ? Theme.neutral200 : "transparent" }
                            highlighted: pinBox.highlightedIndex === index
                        }
                    }
                    Text { Layout.fillWidth: true; text: qsTr("A pin holds as long as some endpoint can carry it, then falls back and says why."); color: Theme.textMuted; font.pixelSize: Theme.fontSmall; wrapMode: Text.WordWrap }
                    // Why the list is one shorter here. The backend's own
                    // sentence, printed as it is written - the way the tray's
                    // absent reason and the silent device's blocker are - so
                    // that no line in this file has to name the operating
                    // system that does have an object renderer.
                    Text {
                        objectName: "spatialAbsentNote"
                        Layout.fillWidth: true
                        visible: CrucibleController.spatialAbsentReason.length > 0
                        text: qsTr("No headphones entry here — %1.").arg(CrucibleController.spatialAbsentReason)
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }

        RailBlock {
            ordinal: "03"
            label: qsTr("ENDPOINTS · WHERE YOU CAN HEAR IT")
            Layout.fillWidth: true
            Text { text: qsTr("re-probed on every device change"); color: Theme.textMuted; font.family: Theme.monoFamily; font.pixelSize: Theme.fontMono }
        }
        Text {
            Layout.fillWidth: true
            text: qsTr("What the probe found on each render endpoint. \"Hear it here\" chooses one: it gets the best mode it can carry, the pin when it can, and \"Automatic\" hands the choice back (the best endpoint for the best mode, a receiver first). \"Send applications here\" is the other stage, the system default: on a real device you would hear every application directly, so the silent device is the one to send them to.")
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.WordWrap
        }
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: table.implicitHeight
            color: Theme.surface
            border.color: Theme.divider
            border.width: 1
            Accessible.role: Accessible.Grouping
            Accessible.name: qsTr("Endpoints")
            ColumnLayout {
                id: table
                width: parent.width
                spacing: 0
                RowLayout {
                    Layout.fillWidth: true
                    Layout.margins: Theme.space2
                    Layout.leftMargin: Theme.space3
                    Layout.rightMargin: Theme.space3
                    spacing: Theme.space2
                    Text { Layout.fillWidth: true; Layout.minimumWidth: 140; text: qsTr("ENDPOINT"); color: Theme.textMuted; font.pixelSize: Theme.fontMono; font.letterSpacing: 1 }
                    Text { id: eac3Head; Layout.preferredWidth: page.eac3Column; text: qsTr("E-AC-3"); color: Theme.textMuted; font.pixelSize: Theme.fontMono; font.letterSpacing: 1; horizontalAlignment: Text.AlignHCenter }
                    Text { id: ac3Head; Layout.preferredWidth: page.ac3Column; text: qsTr("AC-3"); color: Theme.textMuted; font.pixelSize: Theme.fontMono; font.letterSpacing: 1; horizontalAlignment: Text.AlignHCenter }
                    //: Column head over the endpoint's PCM channel count
                    Text { id: pcmHead; Layout.preferredWidth: page.pcmColumn; text: qsTr("PCM CH"); color: Theme.textMuted; font.pixelSize: Theme.fontMono; font.letterSpacing: 1; horizontalAlignment: Text.AlignHCenter }
                    //: Column head: whether the endpoint carries spatial sound
                    Text { id: spatialHead; Layout.preferredWidth: page.spatialColumn; text: qsTr("SPATIAL"); color: Theme.textMuted; font.pixelSize: Theme.fontMono; font.letterSpacing: 1; horizontalAlignment: Text.AlignHCenter }
                    Text { id: noteHead; visible: page.wideTable; Layout.preferredWidth: page.noteColumn; text: qsTr("NOTE"); color: Theme.textMuted; font.pixelSize: Theme.fontMono; font.letterSpacing: 1 }
                    Item { Layout.preferredWidth: 150 + 100 + Theme.space2 }
                }
                Repeater {
                    model: CrucibleController.endpoints
                    delegate: ColumnLayout {
                        id: endpointRow
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 0
                        objectName: "endpointRow-" + endpointRow.modelData.id
                        // The row is ticks and dashes in five columns, which
                        // a reader gets as five stray characters; the same
                        // facts in words, from the same model entry.
                        Accessible.role: Accessible.ListItem
                        Accessible.name: endpointRow.modelData.name
                        Accessible.description: [
                            endpointRow.modelData.eac3 ? qsTr("E-AC-3") : qsTr("no E-AC-3"),
                            endpointRow.modelData.ac3 ? qsTr("AC-3") : qsTr("no AC-3"),
                            endpointRow.modelData.pcmChannels > 0 ? qsTr("%1 PCM channels").arg(endpointRow.modelData.pcmChannels) : qsTr("no PCM"),
                            endpointRow.modelData.spatial ? qsTr("spatial") : "",
                            endpointRow.note
                        ].filter(function(part) { return part.length > 0; }).join(", ")
                        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.divider }
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.margins: 8
                            Layout.leftMargin: Theme.space3
                            Layout.rightMargin: Theme.space3
                            spacing: Theme.space2
                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 140
                                spacing: 1
                                Text { Layout.fillWidth: true; text: endpointRow.modelData.name; color: Theme.text; font.pixelSize: Theme.fontBody; elide: Text.ElideRight }
                                Text { visible: !page.wideTable; Layout.fillWidth: true; text: endpointRow.note; color: endpointRow.modelData.chosen ? Theme.accentInk : Theme.textMuted; font.pixelSize: Theme.fontSmall; elide: Text.ElideRight }
                            }
                            Text { Layout.preferredWidth: page.eac3Column; text: endpointRow.modelData.eac3 ? "✓" : "—"; color: endpointRow.modelData.eac3 ? Theme.text : Theme.textMuted; horizontalAlignment: Text.AlignHCenter; font.pixelSize: Theme.fontBody }
                            Text { Layout.preferredWidth: page.ac3Column; text: endpointRow.modelData.ac3 ? "✓" : "—"; color: endpointRow.modelData.ac3 ? Theme.text : Theme.textMuted; horizontalAlignment: Text.AlignHCenter; font.pixelSize: Theme.fontBody }
                            Text { Layout.preferredWidth: page.pcmColumn; text: endpointRow.modelData.pcmChannels > 0 ? endpointRow.modelData.pcmChannels : "—"; color: endpointRow.modelData.pcmChannels > 0 ? Theme.text : Theme.textMuted; horizontalAlignment: Text.AlignHCenter; font.family: Theme.monoFamily; font.pixelSize: Theme.fontSmall }
                            Text { Layout.preferredWidth: page.spatialColumn; text: endpointRow.modelData.spatial ? "✓" : "—"; color: endpointRow.modelData.spatial ? Theme.text : Theme.textMuted; horizontalAlignment: Text.AlignHCenter; font.pixelSize: Theme.fontBody }
                            Text {
                                visible: page.wideTable
                                Layout.preferredWidth: page.noteColumn
                                text: endpointRow.note
                                color: endpointRow.modelData.chosen ? Theme.accentInk : Theme.textMuted
                                font.pixelSize: Theme.fontSmall
                                elide: Text.ElideRight
                            }
                            // Every row has these two, and their labels
                            // are the same on all of them: without the
                            // endpoint's name a reader walking the table
                            // hears "Hear it here" a dozen times.
                            CrucibleButton {
                                id: hearButton
                                objectName: "hear-" + endpointRow.modelData.id
                                Layout.preferredWidth: 100
                                text: endpointRow.modelData.preferred ? qsTr("Automatic") : qsTr("Hear it here")
                                enabled: !endpointRow.modelData.isNullSink
                                primary: endpointRow.modelData.preferred
                                Accessible.name: hearButton.text + ": " + endpointRow.modelData.name
                                onClicked: CrucibleController.preferredEndpoint = endpointRow.modelData.preferred ? "" : endpointRow.modelData.id
                            }
                            CrucibleButton {
                                id: sendButton
                                objectName: "send-" + endpointRow.modelData.id
                                Layout.preferredWidth: 150
                                text: endpointRow.modelData.isDefault ? qsTr("Applications play here") : qsTr("Send applications here")
                                enabled: !endpointRow.modelData.isDefault
                                Accessible.name: sendButton.text + ": " + endpointRow.modelData.name
                                onClicked: CrucibleController.setDefaultOutput(endpointRow.modelData.id)
                            }
                        }
                        // Every state of the note is a sentence of its own,
                        // and the chosen endpoint's four readings are spelled
                        // out in chosenNote() rather than joined from three
                        // pieces, so no language has to take the English
                        // order of them.
                        readonly property string note: modelData.chosen ? endpointRow.chosenNote()
                            : modelData.preferred ? qsTr("your choice, but it cannot be used: see the reason above")
                            : modelData.isNullSink ? (modelData.isDefault
                                ? qsTr("the silent device · applications play here · never heard")
                                : qsTr("the silent device · never heard"))
                            : modelData.isDefault ? qsTr("applications play here · a real device, so heard directly")
                            : modelData.spatial ? qsTr("spatial sound on · headphones fallback")
                            : modelData.pcmChannels >= 6 ? qsTr("surround PCM fallback") : qsTr("stereo fallback")
                        function chosenNote() {
                            const exclusive = CrucibleController.modeKey === "atmos"
                                || CrucibleController.modeKey === "ddplus"
                                || CrucibleController.modeKey === "dd";
                            if (endpointRow.modelData.preferred) {
                                return exclusive ? qsTr("you hear it here · your choice · exclusive mode")
                                                 : qsTr("you hear it here · your choice");
                            }
                            return exclusive ? qsTr("you hear it here · automatic · exclusive mode")
                                             : qsTr("you hear it here · automatic");
                        }
                    }
                }
                Text {
                    visible: CrucibleController.endpoints.length === 0
                    Layout.margins: Theme.space3
                    text: qsTr("no render endpoints probed yet")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                }
            }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: page.twoUp ? 2 : 1
            columnSpacing: Theme.space6
            rowSpacing: Theme.space6
            ColumnLayout {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.alignment: Qt.AlignTop
                spacing: Theme.space3
                RailBlock { ordinal: "04"; label: qsTr("WHERE APPLICATIONS PLAY"); Layout.fillWidth: true; Layout.fillHeight: false }
                Card {
                    ColumnLayout {
                        spacing: Theme.space2
                        Text {
                            Layout.fillWidth: true
                            textFormat: Text.StyledText
                            text: CrucibleController.defaultIsNullSink
                                ? qsTr("Applications play to <b>%1</b>, the system default output and the silent device: nothing is heard from it, and this app taps each application there.").arg(CrucibleController.defaultOutputName)
                                : qsTr("Applications play to <b>%1</b>, the system default output, which is a real device: you hear each application directly as well as through this app, and a receiver on it cannot be opened exclusively while they do.").arg(CrucibleController.defaultOutputName.length ? CrucibleController.defaultOutputName : qsTr("nothing"))
                            color: Theme.text
                            font.pixelSize: Theme.fontBody
                            wrapMode: Text.WordWrap
                        }
                        Flow {
                            Layout.fillWidth: true
                            spacing: Theme.space2
                            CrucibleButton {
                                text: CrucibleController.defaultIsNullSink ? qsTr("Restore %1").arg(CrucibleController.previousDefaultName.length ? CrucibleController.previousDefaultName : qsTr("the previous output")) : qsTr("Send applications to %1").arg(CrucibleController.nullSinkName)
                                enabled: CrucibleController.defaultIsNullSink ? CrucibleController.previousDefaultName.length > 0 : (CrucibleController.nullSinkPresent || CrucibleController.silentDeviceCanCreate)
                                onClicked: CrucibleController.defaultIsNullSink ? CrucibleController.restoreDefault() : CrucibleController.moveDefaultToNullSink()
                            }
                            CrucibleButton { text: qsTr("Open Sound settings"); onClicked: CrucibleController.openSoundSettings() }
                            CrucibleButton { text: qsTr("Re-probe"); onClicked: CrucibleController.reprobe() }
                        }
                        Text { Layout.fillWidth: true; visible: CrucibleController.defaultMessage.length > 0; text: CrucibleController.defaultMessage; color: Theme.accentInk; font.pixelSize: Theme.fontSmall; wrapMode: Text.WordWrap }
                    }
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.alignment: Qt.AlignTop
                spacing: Theme.space3
                RailBlock { ordinal: "05"; label: qsTr("CODEC PATH"); Layout.fillWidth: true; Layout.fillHeight: false }
                Card {
                    CrucibleCheck {
                        objectName: "bypassCheck"
                        Layout.fillWidth: true
                        text: qsTr("Bypass the codec on headphones and PCM")
                        note: qsTr("Off: headphones, PCM and stereo play a decode of the E-AC-3 stream, so what you hear went through the codec. On: headphones render the engine's own objects and PCM and stereo take its 5.1 bed, codec out of the loop.")
                        checked: CrucibleController.bypassCodec
                        onToggled: function(on) { CrucibleController.bypassCodec = on; }
                    }
                }
            }
        }
    }
}
