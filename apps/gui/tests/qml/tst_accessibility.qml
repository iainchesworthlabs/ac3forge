import QtQuick
import QtTest

import Ac3Forge

// Roadmap UX3's accessibility half. The discipline this suite enforces is
// the QC preset regression's own lesson (tst_qc_panel.qml's
// test_presetControlOffersEveryPresetAndSelectsWhatItNames): that control's
// model silently drifted from its backing data for two releases because
// nothing checked the control against it. An Accessible.name/description
// that is a hardcoded copy of a label can drift exactly the same way a
// hand-written option list did - every case here changes the BACKING DATA
// between two instances (or two states of one instance) and asserts the
// accessible text changes with it, never comparing against a literal
// string typed into the test.
TestCase {
    id: testCase
    name: "Accessibility"

    // ---- ChannelMeter -------------------------------------------------
    Component {
        id: channelMeterComponent
        ChannelMeter {}
    }

    function test_channelMeterAccessibleNameTracksChannelName() {
        const left = createTemporaryObject(channelMeterComponent, testCase, {
            channelName: "L", fed: true, level: ({ peakDb: -20 })
        });
        const right = createTemporaryObject(channelMeterComponent, testCase, {
            channelName: "R", fed: true, level: ({ peakDb: -20 })
        });
        verify(left !== null && right !== null);
        compare(left.Accessible.name, "L");
        compare(right.Accessible.name, "R");
        verify(left.Accessible.name !== right.Accessible.name);
    }

    function test_channelMeterAccessibleDescriptionTracksLiveState() {
        const meter = createTemporaryObject(channelMeterComponent, testCase, {
            channelName: "C", fed: true, level: ({ peakDb: -40, clipped: false })
        });
        verify(meter !== null);
        const quiet = meter.Accessible.description;

        meter.level = ({ peakDb: -1, clipped: true });
        const clipped = meter.Accessible.description;
        verify(quiet !== clipped, "description did not change with the meter's own state");
        verify(clipped.indexOf("clip") >= 0, "clipped description should say so: " + clipped);

        meter.fed = false;
        verify(meter.Accessible.description !== clipped,
               "an unfed channel must not still report the fed reading");
    }

    // The CLIP box is its own Accessible.Button, checkable independently of
    // the row's own indicator - test_channelMeterAccessible* above already
    // covers the row; this covers the control INSIDE it a screen reader
    // would actually activate. (Whether it is currently actionable is left
    // to the item's own real `enabled` state - QQuickAccessibleAttached has
    // no separate "disabled" property to assert against; Qt's own
    // accessibility bridge reads Item.enabled directly.)
    function test_channelMeterClipBoxChecksWithLiveState() {
        const meter = createTemporaryObject(channelMeterComponent, testCase, {
            channelName: "Ls", fed: true, level: ({ peakDb: -1, clipped: true })
        });
        verify(meter !== null);
        const clipBox = findChild(meter, "clipBox");
        verify(clipBox !== null);
        compare(clipBox.Accessible.checked, true);

        meter.level = ({ peakDb: -20, clipped: false });
        compare(clipBox.Accessible.checked, false);
    }

    // ---- QcGateMeter ----------------------------------------------------
    Component {
        id: gateMeterComponent
        QcGateMeter {}
    }

    function test_qcGateMeterAccessibleDescriptionTracksPassFailAndValue() {
        const meter = createTemporaryObject(gateMeterComponent, testCase, {
            label: "Integrated loudness", unit: "LKFS", hasValue: true,
            value: -23.0, minValue: -40, maxValue: 0,
            bandLow: -24.0, bandHigh: -22.0, pass: true
        });
        verify(meter !== null);
        compare(meter.Accessible.name, "Integrated loudness");
        const passing = meter.Accessible.description;
        verify(passing.indexOf("within limit") >= 0, passing);

        meter.value = -10.0;
        meter.pass = false;
        const failing = meter.Accessible.description;
        verify(failing.indexOf("outside limit") >= 0, failing);
        verify(passing !== failing);
    }

    function test_qcGateMeterAccessibleDescriptionReportsNoValue() {
        const meter = createTemporaryObject(gateMeterComponent, testCase, {
            label: "True peak", hasValue: false
        });
        verify(meter !== null);
        compare(meter.Accessible.description, "n/a");
    }

    // ---- SegmentedControl -------------------------------------------------
    Component {
        id: segmentedComponent
        SegmentedControl {}
    }

    function test_segmentedControlSegmentCheckedTracksCurrentValue() {
        const seg = createTemporaryObject(segmentedComponent, testCase, {
            accessibleName: "Theme",
            model: [{ value: "light", label: "Light" }, { value: "dark", label: "Dark" }],
            currentValue: "light"
        });
        verify(seg !== null);
        compare(seg.Accessible.name, "Theme");

        const light = findChild(seg, "seg-light");
        const dark = findChild(seg, "seg-dark");
        verify(light !== null && dark !== null);
        compare(light.Accessible.checked, true);
        compare(dark.Accessible.checked, false);

        seg.currentValue = "dark";
        compare(light.Accessible.checked, false);
        compare(dark.Accessible.checked, true);
    }

    // ---- Card -------------------------------------------------------------
    Component {
        id: cardComponent
        Card {}
    }

    function test_cardAccessibleNameTracksTitleAndIsEmptyWhenUntitled() {
        const card = createTemporaryObject(cardComponent, testCase, {});
        verify(card !== null);
        compare(card.Accessible.name, "");

        // Set after creation, not via createTemporaryObject's initial
        // properties: `title` is `property alias title: heading.text`, and
        // Qt 6's initial-properties fast path does not reliably forward an
        // aliased value to its target before the object's own bindings
        // settle - a plain, non-aliased property (every other control this
        // suite constructs) does not have this quirk.
        card.title = "Loudness";
        compare(card.Accessible.name, "Loudness");

        card.title = "";
        compare(card.Accessible.name, "");
    }
}
