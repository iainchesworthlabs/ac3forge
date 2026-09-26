import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

import Ac3ForgeHearth

// A Hearth sink's firmware (planning/esp32-ota.md, O5): what it runs, what it
// would go back to, how its last update and its last crash went, and what the
// board's own page offers - an update from a file, a rollback and a restart,
// each asked about first. NetworkController.sinkFirmware carries every value
// as the text shown here (sink_firmware_view.hpp, which words them as
// tools/hearth/ota.py does); the sink is asked about its firmware only while
// this tab is open (watchSinkFirmware). None of it goes through Sendspin, so
// the tab follows an update through the restart that takes the sink off it.
ScrollView {
    id: root
    clip: true
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

    readonly property var firmware: NetworkController.sinkFirmware
    readonly property var candidate: NetworkController.sinkFirmwareCandidate
    // Scaled, like every other page's label column.
    readonly property int labelWidth: Math.round(90 * Theme.fontScale)
    // What the dialog asks about: "update", "rollback" or "restart".
    property string asking: ""

    Component.onCompleted: NetworkController.watchSinkFirmware(true)
    Component.onDestruction: NetworkController.watchSinkFirmware(false)

    // Only the rows the sink has something to say in.
    readonly property var rows: [
        { label: qsTr("Running"), value: root.firmware.runningText ?? "" },
        { label: qsTr("Other slot"), value: root.firmware.otherText ?? "" },
        { label: qsTr("This app"), value: root.firmware.buildText ?? "" },
        { label: qsTr("Mode"), value: root.firmware.modeText ?? "" },
        { label: qsTr("Trial"), value: root.firmware.trialText ?? "" },
        { label: qsTr("Update"), value: root.firmware.uploadText ?? "" },
        { label: qsTr("Last update"), value: root.firmware.lastUpdateText ?? "" },
        { label: qsTr("Last crash"), value: root.firmware.crashText ?? "" }
    ].filter(function(row) { return row.value.length > 0; })

    function ask(what) {
        root.asking = what;
        askDialog.open();
    }
    function act() {
        if (root.asking === "update") {
            NetworkController.updateSinkFirmware();
        } else if (root.asking === "rollback") {
            NetworkController.rollbackSinkFirmware();
        } else if (root.asking === "restart") {
            NetworkController.restartSink();
        }
    }

    ColumnLayout {
        width: root.availableWidth
        spacing: Theme.gap * 2

        // No ordinals, as on the other two tabs: the page numbers its
        // columns, not the cards inside the middle one.
        Card {
            rule: false
            title: qsTr("Firmware")

            Text {
                objectName: "sinkFirmwareStatus"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                visible: text.length > 0
                text: root.firmware.statusText ?? qsTr("Asking the sink about its firmware…")
                color: root.firmware.answering === false && root.firmware.reported === true ? Theme.bad
                                                                                           : Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }

            Repeater {
                model: root.rows

                delegate: RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: Theme.gap

                    Text {
                        Layout.preferredWidth: root.labelWidth
                        Layout.alignment: Qt.AlignTop
                        text: modelData.label
                        color: Theme.textMuted
                        elide: Text.ElideRight
                    }
                    Text {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        text: modelData.value
                        color: Theme.text
                        wrapMode: Text.Wrap
                    }
                }
            }
        }

        Card {
            rule: false
            title: qsTr("Update")

            // This app's own update while it runs: the bytes sent, then what
            // the board is doing through its restart and its trial.
            ColumnLayout {
                objectName: "sinkFirmwareProgress"
                Layout.fillWidth: true
                visible: root.firmware.updating === true
                spacing: Theme.gap / 2

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 6
                    visible: (root.firmware.progress ?? -1) >= 0
                    color: Theme.border
                    Rectangle {
                        width: parent.width * Math.max(0, Math.min(1, root.firmware.progress ?? 0))
                        height: parent.height
                        color: Theme.accent
                    }
                    Accessible.role: Accessible.ProgressBar
                    Accessible.name: root.firmware.progressText ?? ""
                }
                Text {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    text: root.firmware.progressText ?? ""
                    color: Theme.text
                    wrapMode: Text.WordWrap
                }
            }

            // How it ended, in the tool's words: updated, rolled back and
            // why, refused and why, or not come back and what to try.
            Text {
                objectName: "sinkFirmwareOutcome"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                visible: text.length > 0 && root.firmware.updating !== true
                text: root.firmware.outcomeText ?? ""
                color: root.firmware.outcome === "updated" ? Theme.text : Theme.bad
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                visible: text.length > 0
                text: root.firmware.actionText ?? ""
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }

            Flow {
                Layout.fillWidth: true
                spacing: Theme.gap

                AppButton {
                    objectName: "sinkFirmwareUpdate"
                    primary: true
                    enabled: root.firmware.canUpdate === true
                    text: qsTr("Update from a file…")
                    onClicked: imageDialog.open()
                }
                AppButton {
                    objectName: "sinkFirmwareRollback"
                    enabled: root.firmware.canRollback === true
                    text: qsTr("Roll back")
                    onClicked: root.ask("rollback")
                }
                AppButton {
                    objectName: "sinkFirmwareRestart"
                    enabled: root.firmware.canRestart === true
                    text: qsTr("Restart")
                    onClicked: root.ask("restart")
                }
            }
            Text {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                text: qsTr("An update stops what the sink plays. The sink checks the image before it writes "
                          + "it, restarts on it, and keeps it only once it has held healthy through its trial; "
                          + "otherwise it goes back to the image before by itself.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }
        }

        Card {
            rule: false
            title: qsTr("Without a cable")

            Text {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                text: qsTr("The sink keeps its console's recent output, and the core dump of its last crash. "
                          + "Each opens in the browser.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }
            Flow {
                Layout.fillWidth: true
                spacing: Theme.gap

                AppButton {
                    objectName: "sinkFirmwareLog"
                    enabled: (root.firmware.logUrl ?? "").length > 0
                    text: qsTr("Recent console output")
                    onClicked: Qt.openUrlExternally(root.firmware.logUrl)
                }
                AppButton {
                    objectName: "sinkFirmwareCoredump"
                    visible: (root.firmware.coredumpUrl ?? "").length > 0
                    text: qsTr("Save the core dump")
                    onClicked: Qt.openUrlExternally(root.firmware.coredumpUrl)
                }
            }
        }
    }

    FileDialog {
        id: imageDialog
        title: qsTr("Choose a firmware image")
        nameFilters: [qsTr("Firmware images (*.bin)"), qsTr("All files (*)")]
        onAccepted: {
            NetworkController.chooseSinkFirmwareFile(selectedFile);
            root.ask("update");
        }
    }

    // Asks before anything that cannot be taken back, in AboutDialog.qml's
    // shape; for an image the sink would not take it says why instead.
    Dialog {
        id: askDialog
        objectName: "sinkFirmwareAsk"
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        focus: true
        width: Math.min(520, parent ? parent.width - 60 : 520)
        padding: Theme.space6
        title: ""
        background: Rectangle {
            color: Theme.bg
            border.color: Theme.text
            border.width: 2
        }

        readonly property bool refused: root.asking === "update" && (root.candidate.refusal ?? "").length > 0
        readonly property string sinkName: NetworkController.selectedSink.name ?? ""

        onOpened: askCancel.forceActiveFocus()
        onClosed: NetworkController.clearSinkFirmwareFile()

        contentItem: ColumnLayout {
            spacing: Theme.gap

            Text {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                text: {
                    if (root.asking === "update") {
                        return askDialog.refused ? qsTr("This image cannot go on %1").arg(askDialog.sinkName)
                                                 : qsTr("Update %1?").arg(askDialog.sinkName);
                    }
                    return root.asking === "rollback" ? qsTr("Roll %1 back?").arg(askDialog.sinkName)
                                                      : qsTr("Restart %1?").arg(askDialog.sinkName);
                }
                color: Theme.text
                font.pixelSize: Theme.fontHeading
                font.bold: true
                wrapMode: Text.WordWrap
                Accessible.role: Accessible.Heading
            }
            Text {
                objectName: "sinkFirmwareAskText"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                text: {
                    if (root.asking === "update") {
                        const what = (root.candidate.text ?? "").length > 0 ? root.candidate.text
                                                                             : (root.candidate.name ?? "");
                        return askDialog.refused
                                ? qsTr("%1: %2.").arg(what).arg(root.candidate.refusal)
                                : qsTr("%1 goes onto the sink in place of %2. It stops playing while it takes "
                                       + "the image.").arg(what).arg(root.firmware.runningVersion ?? "");
                    }
                    if (root.asking === "rollback") {
                        return (root.firmware.trialText ?? "").length > 0
                                ? qsTr("The sink gives up the trial of the image it runs, and restarts into "
                                       + "the image before it.")
                                : qsTr("The sink restarts into %1, the image in its other slot, which then has "
                                       + "a trial of its own.").arg(root.firmware.otherVersion ?? "");
                    }
                    return qsTr("The sink stops playing and starts again on the image it runs.");
                }
                color: Theme.textMuted
                wrapMode: Text.Wrap
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.gap
                spacing: Theme.gap
                Item { Layout.fillWidth: true }
                AppButton {
                    id: askCancel
                    objectName: "sinkFirmwareAskCancel"
                    text: askDialog.refused ? qsTr("Close") : qsTr("Cancel")
                    onClicked: askDialog.close()
                }
                AppButton {
                    objectName: "sinkFirmwareAskConfirm"
                    visible: !askDialog.refused
                    primary: true
                    text: root.asking === "update" ? qsTr("Update")
                                                   : root.asking === "rollback" ? qsTr("Roll back") : qsTr("Restart")
                    onClicked: {
                        root.act();
                        askDialog.close();
                    }
                }
            }
        }
    }
}
