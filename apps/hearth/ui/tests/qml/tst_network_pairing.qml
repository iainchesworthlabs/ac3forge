import QtQuick
import QtTest

import Ac3ForgeHearth
import Ac3ForgeHearthTest

import "HearthTestHelpers.js" as H

// The Network page against a real Hearth test sink (FEATURE_COVERAGE.md
// rows 67-77, 83): apps/hearth/testsink's own Sink runs in this process on
// loopback (TestServices.startTestSink(), test_room.hpp) and is handed to
// NetworkController's own NetworkSinks as a found service - the one step
// mDNS would otherwise do, and the one a CI container cannot (no multicast).
// Everything after that is the real thing: NetworkSinks dials it over
// WebSocket, the page lists it, selecting it starts a real dynamic-code
// pairing attempt, the sink prints its six digits, and the digits are typed
// into the page's own code boxes. Once paired, the sink's own settings view
// shows what the sink itself reports (its outputs, its crossover range, the
// decoder settings it lists - none, for this sink - and its playback
// report), a group made on the page takes it as a member, and a member
// volume set from the page's slider is heard by the sink as a player
// command. Editing the sink's speaker settings is offered but cannot reach
// this sink (it does not list the Settings command); that case is an
// expectFail recorded as a UI bug in FEATURE_COVERAGE.md.
//
// Runs with QML_DISABLE_DISK_CACHE=1 (tests/CMakeLists.txt): Network.qml's
// AOT-compiled bindings crash under Qt 6.9.3, see tst_network_page_aot.qml.
//
// NetworkController is one singleton for the whole file and test_* run in
// alphabetical order, so every case calls ensureSink()/ensurePaired() for
// the state it needs instead of relying on an earlier case.
TestCase {
    id: testCase
    name: "NetworkPairing"
    when: windowShown
    width: 1400
    height: 1400

    readonly property string sinkName: "Loopback sink"
    property bool sinkStarted: false

    Component { id: networkComponent; Network { width: 1400; height: 1400 } }
    Component { id: settingsComponent; Settings { width: 1200; height: 1400 } }

    function makePage() {
        const page = createTemporaryObject(networkComponent, testCase.parent);
        verify(page !== null);
        waitForRendering(page);
        return page;
    }

    function sinkRow() {
        return NetworkController.sinks.find(function(row) { return row.name === sinkName; });
    }

    function ensureSink() {
        if (!sinkStarted) {
            compare(TestServices.startTestSink(sinkName), "", "the test sink did not start");
            sinkStarted = true;
        }
        tryVerify(function() { return sinkRow() !== undefined; }, 15000, "the test sink never appeared in the list");
    }

    // The sink's row in NetworkSinkList: a delegate whose modelData is the row.
    function rowItem(page) {
        let found = null;
        tryVerify(function() {
            found = H.find(page, function(item) {
                return item.modelData !== undefined && item.modelData !== null && item.modelData.name === sinkName
                       && item.current !== undefined;
            });
            return found !== null;
        }, 10000, "the sink has no row on the page");
        return found;
    }

    // Selects the sink, waits for the code the sink prints, types it into
    // the six boxes and presses Pair - the whole pairing ceremony from the
    // page's side.
    function codesPrinted() {
        return TestServices.testSinkLog().filter(function(line) { return line.indexOf("PAIRING CODE") >= 0; }).length;
    }

    function pairFromThePage(page) {
        // A code from an earlier, cancelled attempt is still the last one in
        // the sink's log until this attempt prints its own.
        const printedBefore = codesPrinted();
        // Selecting the row is what asks the sink to pair - even when it is
        // already the selected one (a cancelled attempt leaves it selected).
        mouseClick(rowItem(page));
        clickUntil(function() { return rowItem(page); },
                   function() { return NetworkController.selectedId === sinkRow().id; }, "the sink row never selected");
        let first = null;
        tryVerify(function() { first = findChild(page, "networkPairingDigit-0"); return first !== null; }, 10000,
                  "selecting an unpaired sink did not show the pairing view");
        tryVerify(function() { return codesPrinted() > printedBefore && TestServices.testSinkCode().length === 6; },
                  15000, "the sink never printed a six-digit code: " + TestServices.testSinkLog().join(" | "));
        const code = TestServices.testSinkCode();
        const pair = findChild(page, "networkPairingPair");
        compare(pair.enabled, false);
        mouseClick(first);
        // Each digit moves focus to the next box by itself.
        for (let i = 0; i < code.length; ++i) {
            keyClick(code.charAt(i));
        }
        tryVerify(function() { return pair.enabled; }, 5000, "Pair stayed disabled with every box filled");
        mouseClick(pair);
        tryVerify(function() { return sinkRow().badge === "paired"; }, 15000,
                  "the sink never paired: " + NetworkController.pairingError + " / " + TestServices.testSinkLog().join(" | "));
    }

    function ensurePaired(page) {
        ensureSink();
        if (sinkRow().badge !== "paired") {
            pairFromThePage(page);
        } else if (NetworkController.selectedId !== sinkRow().id) {
            clickUntil(function() { return rowItem(page); },
                       function() { return NetworkController.selectedId === sinkRow().id; }, "the sink row never selected");
        }
        tryCompare(NetworkController, "selectedSinkSettable", true, 15000);
        tryVerify(function() { return findChild(page, "networkSinkSettingsTab") !== null; }, 10000,
                  "a paired Hearth sink did not show its settings view");
    }

    function typeInto(field, text) {
        verify(field !== null, "no text field");
        mouseClick(field);
        field.selectAll();
        for (let i = 0; i < text.length; ++i) {
            keyClick(text.charAt(i));
        }
        keyClick(Qt.Key_Return);
    }

    // Clicks what `find()` returns until `done()` holds. For a control in a
    // Repeater delegate that the page rebuilds whenever the controller
    // republishes (a queue row, a speaker row, a group member): a click that
    // lands while its delegate is being replaced is lost, as it would be for
    // a person clicking at that instant, so it is simply made again.
    function clickUntil(find, done, message) {
        let ok = false;
        tryVerify(function() {
            ok = ok || done();
            if (!ok) {
                const target = find();
                if (target !== null) {
                    mouseClick(target);
                }
            }
            return ok;
        }, 10000, message);
    }

    function test_cancelEndsThePairingAttempt() {
        ensureSink();
        if (sinkRow().badge === "paired") {
            skip("the sink is already paired in this process");
        }
        const page = makePage();
        mouseClick(rowItem(page));
        tryVerify(function() { return findChild(page, "networkPairingCancel") !== null; }, 10000);
        tryVerify(function() { return TestServices.testSinkCode().length === 6; }, 15000,
                  "the sink never printed a code to cancel");
        mouseClick(findChild(page, "networkPairingCancel"));
        tryVerify(function() {
            return TestServices.testSinkLog().some(function(line) { return line.indexOf("pairing ") >= 0
                                                                          && line.indexOf("PAIRING CODE") < 0; });
        }, 10000, "the sink never heard the attempt end: " + TestServices.testSinkLog().join(" | "));
        compare(sinkRow().badge, "notPaired");
    }

    function test_discoveredSinkIsListedAndPairsWithTheCodeItShows() {
        const page = makePage();
        ensureSink();
        compare(NetworkController.discoveredCount >= 1, true);
        const row = rowItem(page);
        verify(H.textItem(row, sinkName) !== null, "the row does not show the sink's name");
        // Look again asks mDNS again; the loopback sink stays listed.
        mouseClick(findChild(page, "networkRescan"));
        tryVerify(function() { return sinkRow() !== undefined; }, 5000);
        if (sinkRow().badge !== "paired") {
            verify(H.textItem(row, "not paired") !== null, "an unpaired row does not say so");
            pairFromThePage(page);
        }
        tryVerify(function() { return H.textItem(rowItem(page), "paired") !== null; }, 5000,
                  "the row's badge did not turn to paired");
        tryCompare(NetworkController, "selectedSinkSettable", true, 15000);
        verify(TestServices.testSinkLog().some(function(line) { return line.indexOf("paired with server") >= 0; }),
               "the sink does not say it paired");
    }

    function test_groupsCreateRenameAddMemberAndDelete() {
        const page = makePage();
        ensurePaired(page);
        const groupsBefore = NetworkController.groupCount;
        mouseClick(findChild(page, "networkNewGroup"));
        tryCompare(NetworkController, "groupCount", groupsBefore + 1, 10000);
        tryVerify(function() { return NetworkController.selectedGroup.id !== undefined; }, 10000);
        const groupId = NetworkController.selectedGroup.id;
        let name = null;
        tryVerify(function() { name = findChild(page, "networkGroupName"); return name !== null; }, 10000,
                  "a new group did not show its editor");
        typeInto(name, "Living room");
        tryVerify(function() { return NetworkController.selectedGroup.name === "Living room"; }, 10000);

        const add = findChild(page, "networkGroupAddMember");
        tryVerify(function() { return add.enabled; }, 10000, "the connected sink is not offered as a member");
        mouseClick(add);
        tryVerify(function() { return (NetworkController.selectedGroup.members ?? []).length === 1; }, 10000,
                  "Add to the group added no member");
        const member = NetworkController.selectedGroup.members[0];
        compare(member.name, sinkName);

        // The member's own volume, set from its slider, reaches the sink as
        // a player command.
        // The member rows are rebuilt on every republish, so the press is
        // repeated until one lands on a live row (clickUntil()'s comment).
        let pressed = false;
        tryVerify(function() {
            const m = NetworkController.selectedGroup.members[0];
            pressed = pressed || (m !== undefined && Math.abs(m.volume - 30) <= 2);
            if (!pressed) {
                const volume = findChild(page, "networkGroupMemberVolume-" + member.sinkId);
                if (volume !== null && volume.enabled) {
                    mouseClick(volume, volume.leftPadding + volume.availableWidth * 0.3, volume.height / 2);
                }
            }
            return pressed;
        }, 10000, "the member volume never moved");
        tryVerify(function() {
            return TestServices.testSinkLog().some(function(line) { return /volume (2[89]|3[012])$/.test(line); });
        }, 10000, "the sink never heard the volume: " + TestServices.testSinkLog().join(" | "));

        mouseClick(findChild(page, "networkGroupRemoveMember-" + member.sinkId));
        tryVerify(function() { return (NetworkController.selectedGroup.members ?? []).length === 0; }, 10000);
        mouseClick(findChild(page, "networkGroupDelete"));
        tryCompare(NetworkController, "groupCount", groupsBefore, 10000);
        verify(NetworkController.groups.every(function(g) { return g.id !== groupId; }));
    }

    // The Decoder tab offers exactly the settings the sink lists
    // (NetworkSinkDecoder.qml's accepts()): this test sink lists none, so
    // every control is shown and disabled, and the report panel beside it
    // is the sink's own report.
    function test_pairedSinkDecoderTabFollowsWhatTheSinkAccepts() {
        const page = makePage();
        ensurePaired(page);
        mouseClick(H.segment(page, "Speakers or decoder", "decoder"));
        let mode = null;
        tryVerify(function() { mode = H.segment(page, "Mode", "rf"); return mode !== null; }, 10000,
                  "the Decoder tab shows no Mode control");
        compare(NetworkController.sinkDecoderSettings.acceptedKeys.length, 0);
        compare(mode.parent.enabled, false, "a setting the sink does not list is editable");
        compare(H.segment(page, "Bad frame", "mute").parent.enabled, false);
        const before = NetworkController.sinkDecoderSettings.mode;
        mouseClick(mode);
        compare(NetworkController.sinkDecoderSettings.mode, before);
        // The report panel: the sink's own words, nothing played yet.
        tryVerify(function() { return H.textItem(page, "Nothing playing.") !== null; }, 5000,
                  "the report panel does not show the sink's report");
        tryVerify(function() { return H.textItem(page, "0 bursts") !== null; }, 5000);
    }

    // The Speakers tab reads the sink's own facts: its 5.1 test layout's six
    // outputs, its crossover range. What it cannot do against this sink is
    // change anything - see the expectFail below.
    function test_pairedSinkSpeakersTabShowsTheSinkAndEditsReachIt() {
        const page = makePage();
        ensurePaired(page);
        tryVerify(function() { return NetworkController.sinkSpeakerSettings.outputs === 6; }, 10000,
                  "the Speakers tab does not show the sink's six outputs");
        compare(NetworkController.sinkSpeakerSettings.management.crossoverMinHz, 40);
        compare(NetworkController.sinkSpeakerSettings.management.crossoverMaxHz, 250);
        verify(findChild(page, "networkSinkTrim-0") !== null, "no trim field for the sink's first speaker");
        const layout = NetworkController.sinkSpeakerSettings.layoutText === "7.1" ? "5.1" : "7.1";
        const preset = H.segment(page, "Speaker layout", layout);
        verify(preset !== null);
        // KNOWN BUG (FEATURE_COVERAGE.md, "UI bugs found"): the tab's
        // controls are enabled and clickable, but the edit is silently
        // dropped. NetworkSinks::push_sink_settings() is refused by
        // ServerSession::ac3forge_command() because the sink's state does
        // not list the Settings command (apps/hearth/testsink lists only
        // volume and mute), and nothing on the page says so - the report
        // stays "Nothing sent yet." and the layout does not change. Marked
        // expectFail so the suite stays green and turns red (XPASS) the day
        // either the page honours supported_commands or the edit lands.
        mouseClick(preset);
        expectFail("", "the sink does not list the Settings command and the page offers the edit anyway");
        tryVerify(function() { return NetworkController.sinkSpeakerSettings.layoutText === layout; }, 1500,
                  "the sink layout never changed to " + layout + " (" + NetworkController.sinkReport.settingsText + ")");
    }

    // The pairing Hearth made is a record in its settings: the Settings
    // page lists it, and Forget removes it.
    function test_pairingRecordShowsInSettingsAndForgetRemovesIt() {
        const network = makePage();
        ensurePaired(network);
        const settings = createTemporaryObject(settingsComponent, testCase.parent);
        verify(settings !== null);
        waitForRendering(settings);
        tryVerify(function() { return HearthController.pairingRecords.length >= 1; }, 5000,
                  "no pairing record after pairing");
        const forget = findChild(settings, "pairingForget-0");
        verify(forget !== null, "the Settings page lists no pairing record");
        const count = HearthController.pairingRecords.length;
        mouseClick(forget);
        tryVerify(function() { return HearthController.pairingRecords.length === count - 1; }, 5000,
                  "Forget did not remove the record");
    }
}
