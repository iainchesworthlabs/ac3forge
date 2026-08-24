import QtQuick
import QtTest

import Ac3Forge

// Pins codedChannelCount/renderedChannelCount for every surviving Format-tab
// preset against the real EncoderController/chanmap::allocate() path - see
// tst_format_channels.qml for why a real singleton, not a mock. "5.2"
// deliberately has no row here: a bare second LFE with no other extra is
// AllocationError::kOrphanLfe2 (LFE2 needs a full-bandwidth channel sharing
// its dependent substream, and a bed's own LFE/LFE2 never count as one), so
// it was dropped from the preset list entirely rather than left to silently
// resolve to a stereo fallback plan.
TestCase {
    id: testCase
    name: "ChannelCounts"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    function test_presetsProduceTheExpectedChannelCounts() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = false;

        // coded/rendered/dependents measured directly off
        // ac3::eac3::chanmap::allocate() for each preset's bed+extras mask.
        const cases = [
            { preset: "stereo", coded: 2, rendered: 2 },
            { preset: "5.1", coded: 6, rendered: 6 },
            { preset: "7.1", coded: 8, rendered: 8 },
            { preset: "5.1.4", coded: 10, rendered: 10 },
            { preset: "7.1.4", coded: 12, rendered: 12 },
            { preset: "7.2.4", coded: 13, rendered: 13 },
        ];
        for (let i = 0; i < cases.length; i++) {
            const c = cases[i];
            EncoderController.applyChannelPreset(c.preset);
            compare(EncoderController.codedChannelCount, c.coded, c.preset + " coded");
            compare(EncoderController.renderedChannelCount, c.rendered, c.preset + " rendered");
            compare(EncoderController.channelShapeName, c.preset === "stereo" ? "2.0" : c.preset,
                    c.preset + " shape name");
        }
    }

    function test_fivePointTwoIsNotAnOfferedPresetAndApplyingItIsANoOp() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = false;

        // The Format tab's preset row itself no longer offers the button -
        // see Main.qml's PRESETS Repeater model. Confirm the Repeater's
        // delegates have actually loaded (findChild on an unrealised tree
        // would pass this vacuously - see waitForHeaderLayout's own comment
        // in tst_format_channels.qml on this exact Qt Quick Test gotcha)
        // before trusting that "preset-5.2" being absent means anything.
        let fiveOne = null;
        tryVerify(() => {
            fiveOne = findChild(win.contentItem, "preset-5.1");
            return fiveOne !== null;
        });
        compare(findChild(win.contentItem, "preset-5.2"), null);

        // applyChannelPreset() takes a free-form string (also reachable via
        // the live-session layout switcher), so it has to stay harmless for
        // a name that matches nothing in its own preset table - not silently
        // resolve through effectiveChannelPlan()'s allocate(...).value_or()
        // fallback to a stereo plan the picker never asked for.
        EncoderController.applyChannelPreset("5.1");
        compare(EncoderController.codedChannelCount, 6);
        EncoderController.applyChannelPreset("5.2");
        compare(EncoderController.codedChannelCount, 6);
        compare(EncoderController.renderedChannelCount, 6);
        compare(EncoderController.channelShapeName, "5.1");
    }

    function extrasRow(id) {
        const extras = EncoderController.extrasModel;
        for (let i = 0; i < extras.length; i++) {
            if (extras[i].id === id) return extras[i];
        }
        return null;
    }

    function test_lfe2AloneIsRefusedButReachableAlongsideAnotherExtra() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = false;
        EncoderController.applyChannelPreset("5.1");

        // The general case the removed "5.2" preset used to hit blind:
        // orphaning LFE2 is refused, not silently mis-applied, and
        // extrasModel says why - the same reason chanmap::describe()
        // gives the CLI for the identical channel selection.
        let lfe2 = extrasRow("lfe2");
        verify(lfe2 !== null);
        compare(lfe2.checked, false);
        compare(lfe2.enabled, false);
        compare(lfe2.reason, "LFE2 needs a full-bandwidth channel sharing its substream, "
                              + "and none is left");

        EncoderController.toggleExtra("lfe2");
        compare(EncoderController.codedChannelCount, 6);
        lfe2 = extrasRow("lfe2");
        compare(lfe2.checked, false);

        // Tick a real full-bandwidth companion first ("rear") - now LFE2 has
        // a dependent substream to share, so the same toggle succeeds.
        EncoderController.toggleExtra("rear");
        lfe2 = extrasRow("lfe2");
        compare(lfe2.enabled, true);

        EncoderController.toggleExtra("lfe2");
        lfe2 = extrasRow("lfe2");
        compare(lfe2.checked, true);
        compare(EncoderController.codedChannelCount, 9);
        compare(EncoderController.renderedChannelCount, 9);
        compare(EncoderController.channelShapeName, "7.2");
    }
}
