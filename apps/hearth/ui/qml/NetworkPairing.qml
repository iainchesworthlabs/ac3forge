import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3ForgeHearth

// The pairing view (planning/hearth-design.md, "Network - discovery and
// pairing"): a sink not yet paired with this computer shows the dynamic
// six-digit code it is printing on its own serial console or page, and the
// person copies it in here. The engine (NetworkSinks::select_sink()) starts
// the attempt as soon as the sink is selected - there is nothing else useful
// to do with an unpaired sink - so by the time this loads a code attempt is
// already running and the sink is already showing a code.
RowLayout {
    id: root
    anchors.fill: parent
    spacing: Theme.gap * 2

    readonly property var sink: NetworkController.selectedSink

    ColumnLayout {
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.preferredWidth: 2
        spacing: Theme.gap

        Card {
            ordinal: "02"
            title: qsTr("Pair %1").arg(root.sink.name ?? "")

            Text {
                Layout.fillWidth: true
                // Every wrapped or elided Text in a fillWidth Layout needs
                // this, or its default minimum (its full, single-line width)
                // can force the whole column wider than intended - found the
                // hard way when one sentence in NetworkSinkList.qml's own
                // footer swallowed most of the page (its own comment there
                // has the full story).
                Layout.minimumWidth: 0
                text: qsTr("%1 has not been paired with this computer. Pairing makes a key that "
                          + "both keep; after it, the sink takes streams from Hearth without a code.")
                          .arg(root.sink.name ?? "")
                color: Theme.text
                wrapMode: Text.WordWrap
            }

            Text {
                text: qsTr("THE CODE THE SINK SHOWS")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMicro
                font.bold: true
            }

            RowLayout {
                spacing: Theme.gap / 2

                Repeater {
                    id: digitFields
                    model: NetworkController.pairingDigitCount

                    property var digits: []

                    delegate: TextField {
                        id: digitField
                        required property int index
                        objectName: "networkPairingDigit-" + index
                        implicitWidth: 48
                        implicitHeight: 48
                        horizontalAlignment: Text.AlignHCenter
                        font.family: Theme.monoFamily
                        font.pixelSize: Theme.fontTitle
                        maximumLength: 1
                        validator: RegularExpressionValidator { regularExpression: /[0-9]/ }
                        Accessible.name: qsTr("Digit %1 of %2").arg(index + 1).arg(digitFields.model)

                        onTextChanged: {
                            if (text.length === 1 && index + 1 < digitFields.count) {
                                digitFields.itemAt(index + 1).forceActiveFocus();
                            }
                            root.updateCode();
                        }
                        Keys.onPressed: function(event) {
                            if (event.key === Qt.Key_Backspace && text.length === 0 && index > 0) {
                                digitFields.itemAt(index - 1).forceActiveFocus();
                            }
                        }
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                text: qsTr("The sink prints a new six-digit code on its serial console for each "
                          + "attempt. A sink with a fixed code has eight digits, printed on the "
                          + "device.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }

            Text {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                visible: text.length > 0
                text: NetworkController.pairingError
                color: Theme.bad
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }

            RowLayout {
                spacing: Theme.gap

                AppButton {
                    objectName: "networkPairingPair"
                    text: qsTr("Pair")
                    enabled: root.codeComplete()
                    onClicked: NetworkController.submitPairingCode(root.sink.id, root.currentCode())
                }
                AppButton {
                    objectName: "networkPairingCancel"
                    text: qsTr("Cancel")
                    onClicked: NetworkController.cancelPairing(root.sink.id)
                }
            }
        }

        Text {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            text: qsTr("✓ The connection to the sink is encrypted before any code is typed.")
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.WordWrap
        }

        Item { Layout.fillHeight: true }
    }

    NetworkSinkInfo {
        // fillWidth, not just preferredWidth: a Layout item without it can
        // still grow past its preferredWidth ratio to its own content's
        // implicit size when there's room, which broke NetworkSinkList's
        // 300px column the same way (that file's own comment has the story)
        // - explicit fillWidth is what makes the 2:1 split with the
        // ColumnLayout on the left actually hold.
        Layout.fillWidth: true
        Layout.preferredWidth: 1
        Layout.fillHeight: true
        sink: root.sink
    }

    function currentCode() {
        let code = "";
        for (let i = 0; i < digitFields.count; ++i) {
            code += digitFields.itemAt(i).text;
        }
        return code;
    }
    function codeComplete() {
        return root.currentCode().length === NetworkController.pairingDigitCount;
    }
    function updateCode() {
        // Nothing is submitted per keystroke - pairing.md's dynamic code
        // holds back after 20 failed rounds, so a partial guess sent early
        // would burn one for nothing. "Pair" enables once every box is full.
    }
}
