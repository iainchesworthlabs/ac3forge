import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3ForgeHearth

// The pairing view (planning/hearth-design.md, "Network - discovery and
// pairing"): a sink not paired with this computer. Pairing is asked for with
// the button here, never by selecting the row - selecting one only shows what
// it is, and asking takes the sink from whichever server holds it
// (network_sinks.hpp's own header comment). Once asked, the sink shows a
// six-digit code on its own page and serial console, and the person copies
// it into the boxes. NetworkController.selectedSink.pairing says which of the
// three steps to show: "none" (the button), "requested" (reaching the sink)
// and "code"/"active" (the boxes).
RowLayout {
    id: root
    anchors.fill: parent
    spacing: Theme.gap * 2

    readonly property var sink: NetworkController.selectedSink
    readonly property string step: root.sink.pairing ?? "none"
    readonly property bool typing: root.step === "code" || root.step === "active"
    // A code that did not match leaves the boxes for the next one, empty.
    readonly property string error: NetworkController.pairingError
    onErrorChanged: {
        if (root.error.length > 0) {
            root.clearCode();
        }
    }

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
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                visible: (root.sink.notice ?? "").length > 0 && !root.typing
                text: qsTr("%1 Pairing takes it from the other server.").arg(root.sink.notice ?? "")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }

            // --- 1: ask ----------------------------------------------------
            RowLayout {
                visible: root.step === "none"
                spacing: Theme.gap

                AppButton {
                    objectName: "networkPairingStart"
                    text: qsTr("Pair with this computer")
                    enabled: root.sink.canPair === true
                    onClicked: NetworkController.pairSink(root.sink.id)
                }
            }

            // --- 2: reaching the sink -------------------------------------
            RowLayout {
                visible: root.step === "requested"
                spacing: Theme.gap

                BusyIndicator {
                    implicitWidth: 24
                    implicitHeight: 24
                    running: visible
                }
                Text {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    objectName: "networkPairingWaiting"
                    text: qsTr("Asking %1 to pair…").arg(root.sink.name ?? "")
                    color: Theme.textMuted
                    wrapMode: Text.WordWrap
                }
                AppButton {
                    text: qsTr("Cancel")
                    onClicked: NetworkController.cancelPairing(root.sink.id)
                }
            }

            // --- 3: the code -----------------------------------------------
            Text {
                visible: root.typing
                text: qsTr("THE CODE THE SINK SHOWS")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMicro
                font.bold: true
            }

            RowLayout {
                visible: root.typing
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
                text: (root.sink.pageUrl ?? "").length > 0
                      ? qsTr("The sink shows a new six-digit code for each attempt, on its own page "
                            + "(%1) and on its serial console.").arg(root.sink.pageUrl)
                      : qsTr("The sink shows a new six-digit code for each attempt, on its own page "
                            + "and on its serial console.")
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
                visible: root.typing
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
            const field = digitFields.itemAt(i);
            code += field ? field.text : "";
        }
        return code;
    }
    function codeComplete() {
        return root.currentCode().length === NetworkController.pairingDigitCount;
    }
    function clearCode() {
        for (let i = digitFields.count - 1; i >= 0; --i) {
            const field = digitFields.itemAt(i);
            if (field) {
                field.text = "";
            }
        }
    }
    function updateCode() {
        // Nothing is submitted per keystroke - pairing.md's dynamic code
        // holds back after 20 failed rounds, so a partial guess sent early
        // would burn one for nothing. "Pair" enables once every box is full.
    }
}
