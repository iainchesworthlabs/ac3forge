import QtQuick
import QtTest

import Ac3ForgeCrucible
import Ac3ForgeCrucibleTest

// The Signal path page, driven from its own controls over a scripted machine
// (ui/tests/qml_test_main.cpp): an HDMI receiver, a stereo default and the
// silent device. Because the default output here is the FAKE default device,
// "Send applications here", Restore and the refusal path can all be pressed -
// on a real machine they would move the developer's own sound settings,
// which is why tst_output.qml and tst_firstrun.qml never press them.
//
// Every case waits for the engine's answer (the mode it settled on, the
// endpoint table it probed, the default it now reports) rather than reading
// back the setting it just wrote.
TestCase {
    id: testCase
    name: "SignalPath"
    when: windowShown
    width: 1480
    height: 820

    Component { id: outputPage; OutputPage { width: 1480; height: 780 } }
    Component { id: roomPage; RoomPage { width: 1480; height: 780 } }

    function init() {
        CrucibleController.stop();
        CrucibleController.pinned = "auto";
        CrucibleController.preferredEndpoint = "";
        CrucibleController.bypassCodec = false;
        CrucibleController.nullSinkName = "Desktop Atmos";
    }

    function cleanup() {
        CrucibleController.stop();
        CrucibleController.pinned = "auto";
        CrucibleController.preferredEndpoint = "";
        TestServices.clear();
    }

    function machine(endpoints) {
        if (!TestServices.scriptMachine([{ app: 900, name: "chrome", active: true }], endpoints)) {
            skip("the scripted machine is not available in this harness");
        }
        CrucibleController.start();
        if (!CrucibleController.running) {
            skip("the engine did not run over the scripted machine: " + CrucibleController.lastError);
        }
        tryVerify(function() { return CrucibleController.modeKey.length > 0 && CrucibleController.modeKey !== "none"; }, 5000,
                  "the engine chose an output: " + CrucibleController.modeKey + " " + CrucibleController.outputReason);
        tryVerify(function() { return CrucibleController.endpoints.length === endpoints.length; }, 5000, "the probe reached the table");
    }

    function makePage(component) {
        const page = createTemporaryObject(component || outputPage, testCase.parent);
        verify(page);
        waitForRendering(page);
        return page;
    }

    function reveal(item) {
        // Layouts place their children at the next polish, so a control that
        // has just appeared (the card, on a selection) is where it will be
        // only once every layout above it has been polished.
        for (let up = item; up; up = up.parent) waitForItemPolished(up);
        let flick = item.parent;
        while (flick && flick.contentY === undefined) flick = flick.parent;
        if (!flick) return;
        const top = item.mapToItem(flick.contentItem, 0, 0).y;
        const wanted = Math.max(0, Math.min(flick.contentHeight - flick.height, top - flick.height / 3));
        // Only a scroll draws a new frame, and waitForRendering waits for one.
        if (Math.abs(flick.contentY - wanted) > 0.5) {
            flick.contentY = wanted;
            waitForRendering(item);
        }
    }

    function click(item) {
        reveal(item);
        verify(item.enabled, (item.objectName || item.text) + " is enabled");
        mouseClick(item);
    }

    // A button by its label, anywhere under root (the page's own Flow rows
    // carry no object names; their labels are what a person reads).
    function button(root, text) {
        if (root.text === text && root.clicked !== undefined) return root;
        const kids = root.children || [];
        for (let i = 0; i < kids.length; ++i) {
            const found = button(kids[i], text);
            if (found) return found;
        }
        if (root.contentItem && root.contentItem !== root) return button(root.contentItem, text);
        return null;
    }

    // Chooses a pin the way a person does: open the box, click the entry.
    function choosePin(page, value) {
        const pin = findChild(page, "pinBox");
        reveal(pin);
        mouseClick(pin);
        tryCompare(pin.popup, "opened", true);
        const index = pin.indexOfValue(value);
        verify(index >= 0, "the pin list offers " + value);
        const list = pin.popup.contentItem;
        list.positionViewAtIndex(index, ListView.Contain);
        let delegate = null;
        tryVerify(function() { delegate = list.itemAtIndex(index); return delegate !== null; }, 2000);
        waitForRendering(list);
        mouseClick(delegate);
        tryCompare(pin.popup, "visible", false);
        compare(pin.currentValue, value);
    }

    // --- the endpoint table ------------------------------------------------------

    function test_theTableListsWhatTheProbeFound() {
        machine(["avr", "realtek", "null"]);
        const page = makePage();
        for (const id of ["avr", "realtek", "null"]) {
            tryVerify(function() { return findChild(page, "endpointRow-" + id) !== null; }, 3000, "a row for " + id);
        }
        // The silent device is never heard, so it cannot be chosen to hear
        // on; the default is where applications play already.
        compare(findChild(page, "hear-null").enabled, false);
        compare(findChild(page, "send-realtek").enabled, false);
        compare(findChild(page, "send-realtek").text, "Applications play here");
        const avr = findChild(page, "endpointRow-avr");
        verify(avr.note.indexOf("you hear it here") === 0, "the receiver is where the engine went: " + avr.note);
        verify(avr.Accessible.description.indexOf("E-AC-3") === 0, avr.Accessible.description);
        compare(CrucibleController.endpointName, "AVR (HDMI)");
    }

    // --- the pin -------------------------------------------------------------------

    function test_aPinChosenFromTheBoxChangesWhatIsHeard() {
        machine(["avr", "realtek", "null"]);
        const page = makePage();
        const automatic = CrucibleController.modeKey;
        verify(automatic === "ddplus" || automatic === "atmos", "a receiver takes E-AC-3 automatically: " + automatic);

        choosePin(page, "dd");
        compare(CrucibleController.pinned, "dd");
        compare(String(TestServices.storedSetting("output/pinned")), String("dd"), "the pin is in the store");
        tryCompare(CrucibleController, "modeKey", "dd", 5000);
        compare(CrucibleController.endpointName, "AVR (HDMI)");
        // The station that says what is heard reads the engine's answer.
        const hear = findChild(page, "station-hear");
        tryVerify(function() { return hear.detail.indexOf(CrucibleController.modeName) === 0; }, 3000, hear.detail);
        verify(hear.detail.indexOf("pin: dd") >= 0, hear.detail);

        choosePin(page, "pcm");
        tryCompare(CrucibleController, "modeKey", "pcm", 5000);

        choosePin(page, "auto");
        compare(String(TestServices.storedSetting("output/pinned")), String("auto"));
        tryCompare(CrucibleController, "modeKey", automatic, 5000);
    }

    function test_aPinNoEndpointCanCarryFallsBackAndSaysWhy() {
        // Stereo and the silent device only: nothing takes a bitstream.
        machine(["realtek", "null"]);
        const page = makePage();
        choosePin(page, "atmos");
        compare(CrucibleController.pinned, "atmos");
        wait(300);  // a few polls: the engine keeps what it can carry
        verify(CrucibleController.modeKey !== "atmos", "no endpoint here carries Atmos: " + CrucibleController.modeKey);
        verify(CrucibleController.outputReason.length > 0, "the fallback is explained");
    }

    // --- hear it here ---------------------------------------------------------------

    function test_hearItHereChoosesAnEndpointAndAutomaticHandsItBack() {
        machine(["avr", "headphones", "realtek", "null"]);
        const page = makePage();
        compare(CrucibleController.endpointName, "AVR (HDMI)");
        // The table is rebuilt whenever the probe's answer changes, so each
        // button is found afresh rather than held across a change.
        const hear = function() { return findChild(page, "hear-hp"); };
        verify(hear());
        compare(hear().text, "Hear it here");
        click(hear());
        compare(CrucibleController.preferredEndpoint, "hp");
        compare(String(TestServices.storedSetting("output/endpoint")), "hp");
        tryCompare(CrucibleController, "endpointName", "Headphones (USB)", 5000);
        tryVerify(function() { return hear() && hear().text === "Automatic"; }, 5000, "the chosen row offers Automatic");
        verify(findChild(page, "endpointRow-hp").note.indexOf("your choice") >= 0, findChild(page, "endpointRow-hp").note);
        const station = findChild(page, "station-hear");
        tryVerify(function() { return station.title === "Headphones (USB)"; }, 3000, station.title);
        verify(station.detail.indexOf("endpoint: your choice") >= 0, station.detail);

        click(hear());
        compare(CrucibleController.preferredEndpoint, "");
        tryCompare(CrucibleController, "endpointName", "AVR (HDMI)", 5000);
        tryVerify(function() { return hear() && hear().text === "Hear it here"; }, 5000);
    }

    // --- where applications play ------------------------------------------------------

    function test_sendApplicationsToTheSilentDeviceAndRestoreFromTheStation() {
        machine(["avr", "realtek", "null"]);
        const page = makePage();
        compare(CrucibleController.defaultIsNullSink, false);
        compare(CrucibleController.defaultOutputName, "Speakers (Realtek)");
        const station = findChild(page, "station-apps");
        verify(station.warn, "a real default is a warning");
        const send = button(station, "Send applications to Desktop Atmos");
        verify(send, "the station offers the move");
        click(send);
        tryCompare(CrucibleController, "defaultIsNullSink", true, 3000);
        compare(CrucibleController.defaultOutputName, "Speakers (Desktop Atmos)");
        compare(CrucibleController.previousDefaultName, "Speakers (Realtek)");
        compare(CrucibleController.defaultMessage, "");
        tryCompare(station, "warn", false);
        verify(station.detail.indexOf("Speakers (Realtek) is restored on quit.") >= 0, station.detail);

        const restore = button(station, "Restore Speakers (Realtek)");
        verify(restore, "the same button now restores");
        click(restore);
        tryCompare(CrucibleController, "defaultIsNullSink", false, 3000);
        compare(CrucibleController.defaultOutputName, "Speakers (Realtek)");
    }

    function test_sendApplicationsHereMovesTheDefaultToAnyRow() {
        machine(["avr", "realtek", "null"]);
        const page = makePage();
        click(findChild(page, "send-null"));
        tryCompare(CrucibleController, "defaultIsNullSink", true, 3000);
        // The table is the engine's probe, which a real machine repeats on
        // the device change a move causes. The scripted machine announces no
        // device changes, so the page's own Re-probe stands in for one; the
        // rows then follow: the silent device is where applications play.
        click(button(page, "Re-probe"));
        tryVerify(function() { return findChild(page, "send-null").text === "Applications play here"; }, 5000);
        tryVerify(function() { return findChild(page, "send-realtek").enabled; }, 5000);
        click(findChild(page, "send-realtek"));
        tryCompare(CrucibleController, "defaultIsNullSink", false, 3000);
        compare(CrucibleController.defaultOutputName, "Speakers (Realtek)");
    }

    function test_aRefusedMoveSaysWhyAndOpensTheSoundSettings() {
        machine(["avr", "realtek", "null"]);
        const page = makePage();
        verify(TestServices.refuseDefaultMoves("the policy said no"));
        const opened = TestServices.soundSettingsOpened();
        click(button(findChild(page, "station-apps"), "Send applications to Desktop Atmos"));
        tryCompare(CrucibleController, "defaultMessage", "the policy said no", 3000);
        compare(CrucibleController.defaultIsNullSink, false);
        compare(TestServices.soundSettingsOpened(), opened + 1, "the refusal opens the platform's sound settings");
        const station = findChild(page, "station-apps");
        verify(station.detail.indexOf("the policy said no") >= 0, station.detail);
    }

    function test_aRefusedRestoreSaysWhyAndLeavesTheSilentDevice() {
        machine(["avr", "realtek", "null"]);
        const page = makePage();
        const station = findChild(page, "station-apps");
        click(button(station, "Send applications to Desktop Atmos"));
        tryCompare(CrucibleController, "defaultIsNullSink", true, 3000);
        verify(TestServices.refuseDefaultMoves("the policy will not put it back"));
        const opened = TestServices.soundSettingsOpened();
        click(button(station, "Restore Speakers (Realtek)"));
        tryCompare(CrucibleController, "defaultMessage", "the policy will not put it back", 3000);
        compare(CrucibleController.defaultIsNullSink, true, "applications stay on the silent device");
        compare(TestServices.soundSettingsOpened(), opened + 1);
    }

    function test_openSoundSettingsAndReprobeButtons() {
        machine(["avr", "realtek", "null"]);
        const page = makePage();
        const before = TestServices.soundSettingsOpened();
        click(button(page, "Open Sound settings"));
        compare(TestServices.soundSettingsOpened(), before + 1);
        click(button(page, "Re-probe"));
        // A re-probe re-reads the same machine: the same table, the same answer.
        tryVerify(function() { return CrucibleController.endpoints.length === 3; }, 5000);
        compare(CrucibleController.endpointName, "AVR (HDMI)");
        compare(CrucibleController.running, true);
    }

    function test_aMissingSilentDeviceGreysTheMoveAndSaysSo() {
        machine(["avr", "realtek"]);
        const page = makePage();
        compare(CrucibleController.nullSinkPresent, false);
        const station = findChild(page, "station-apps");
        const send = button(station, "Send applications to Desktop Atmos");
        verify(send);
        compare(send.enabled, false, "nothing to send them to");
        verify(station.detail.indexOf("There is no silent device to send them to") >= 0, station.detail);
    }

    function test_sendCreatesTheSilentDeviceFirstWhereTheApplicationMakesIt() {
        if (!TestServices.scriptMachineThatMakesItsSilentDevice([{ app: 900, name: "chrome", active: true }])) {
            skip("the scripted machine is not available in this harness");
        }
        CrucibleController.start();
        if (!CrucibleController.running) {
            skip("the engine did not run over the scripted machine: " + CrucibleController.lastError);
        }
        const page = makePage();
        compare(CrucibleController.nullSinkPresent, false);
        const station = findChild(page, "station-apps");
        const send = button(station, "Send applications to Desktop Atmos");
        verify(send.enabled, "offered: the press makes the device first");
        click(send);
        tryCompare(CrucibleController, "defaultIsNullSink", true, 3000);
        compare(CrucibleController.nullSinkPresent, true);
        compare(CrucibleController.defaultOutputName, "Speakers (Desktop Atmos)");
        // The probe sees the new device too.
        tryVerify(function() { return findChild(page, "endpointRow-null") !== null; }, 5000, "the silent device joined the table");
    }

    // --- the codec path -----------------------------------------------------------

    function test_theBypassCheckIsClickedThroughToTheEngine() {
        machine(["avr", "realtek", "null"]);
        const page = makePage();
        choosePin(page, "pcm");
        tryCompare(CrucibleController, "modeKey", "pcm", 5000);
        const check = findChild(page, "bypassCheck");
        click(check);
        compare(CrucibleController.bypassCodec, true);
        compare(String(TestServices.storedSetting("codec/bypass")), String(true));
        tryCompare(CrucibleController, "codecBypassed", true, 5000);
        click(check);
        tryCompare(CrucibleController, "codecBypassed", false, 5000);
    }
}
