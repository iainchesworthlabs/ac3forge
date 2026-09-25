import QtQuick
import QtTest

import Ac3ForgeHearth

// A Hearth sink's own settings pages (issue #875, network-sink-{speakers,
// decoder}.png): NetworkSinkSpeakers.qml/NetworkSinkDecoder.qml/
// NetworkSinkReport.qml/NetworkSinkOnlyOnSink.qml, all read from
// NetworkController's own sink*/selectedSinkSettable properties.
//
// No sink is ever found here: qml_test_main.cpp turns NetworkController's
// mDNS discovery off, so NetworkController.start() runs a real, dial-out-only
// NetworkSinks that lists only the sinks a suite hands it, and this suite
// hands it none - selectedSinkSettable stays false throughout. That is exactly the
// state worth testing directly: every page below must render safely, and
// every setter must be a safe no-op, with EVERY sink* property at its empty
// default (a fresh app launch, before anything is ever selected, looks
// identical) - the same reasoning tst_decoder_settings.qml's own
// test_emptyMapChangesNothing() applies to a single key, generalised here to
// the whole surface never having a sink to act on at all.
TestCase {
    id: testCase
    name: "NetworkSinkSettings"
    when: windowShown
    width: 1200
    height: 900

    Component { id: speakersComponent; NetworkSinkSpeakers { width: 1200; height: 900 } }
    Component { id: decoderComponent; NetworkSinkDecoder { width: 1200; height: 900 } }
    Component { id: reportComponent; NetworkSinkReport { width: 1200; height: 900 } }
    Component { id: onlyOnSinkComponent; NetworkSinkOnlyOnSink { width: 1200; height: 900 } }

    function init() {
        NetworkController.start();
    }

    function test_nothingSelectedLeavesEveryPropertyAtItsEmptyDefault() {
        compare(NetworkController.selectedSinkSettable, false);
        compare(Object.keys(NetworkController.sinkSpeakerSettings).length, 0);
        compare(Object.keys(NetworkController.sinkDecoderSettings).length, 0);
        compare(Object.keys(NetworkController.sinkReport).length, 0);
        compare(Object.keys(NetworkController.sinkOnlyOnSink).length, 0);
    }

    // Every setter reads the selected sink fresh (network_controller.cpp's
    // own selected_sink_facts()) and bails out when there is none - proven
    // here by calling every one of them with nothing selected and nothing
    // crashing or throwing a QML error.
    function test_everySetterIsASafeNoOpWithNoSinkSelected() {
        NetworkController.setSinkLayoutText("5.1");
        NetworkController.setSinkHeights("ceiling");
        NetworkController.setSinkSpeakerSmall(0, true);
        NetworkController.setSinkTrimDb(0, 3.0);
        NetworkController.setSinkDelayMs(0, 4.4);
        NetworkController.setSinkCrossoverHz(100);
        NetworkController.setSinkRoutingAssignment(0, 1);
        NetworkController.setSinkDecoderSettings({ "mode": "rf" });
        NetworkController.startSinkIdentify(0);
        NetworkController.stopSinkIdentify();
        wait(150);
        compare(NetworkController.selectedSinkSettable, false);
    }

    // Each page's own "?? []"/"?? {}" fallbacks (every property they read
    // off NetworkController's own empty-by-default maps) must hold up
    // against genuinely empty backing data, not just a hand-filled example -
    // this is the state every one of these pages is actually in until A6's
    // group/streaming follow-up (or a real sink on the test network) gives
    // this controller something to select. waitForRendering() failing (a
    // QML binding error, a null model, an unhandled undefined) is the
    // regression this guards against.
    function test_everyPageRendersSafelyWithNoSinkSelected() {
        const speakers = createTemporaryObject(speakersComponent, testCase.parent);
        verify(speakers !== null);
        waitForRendering(speakers);

        const decoder = createTemporaryObject(decoderComponent, testCase.parent);
        verify(decoder !== null);
        waitForRendering(decoder);

        const report = createTemporaryObject(reportComponent, testCase.parent);
        verify(report !== null);
        waitForRendering(report);

        const onlyOnSink = createTemporaryObject(onlyOnSinkComponent, testCase.parent);
        verify(onlyOnSink !== null);
        waitForRendering(onlyOnSink);
    }
}
