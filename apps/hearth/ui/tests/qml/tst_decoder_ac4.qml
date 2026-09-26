import QtQuick
import QtTest

import Ac3ForgeHearth
import Ac3ForgeHearthTest

import "HearthTestHelpers.js" as H

// The AC-4 decoder page (DecoderAc4.qml; planning/ac4.md, phase I2): every
// control, driven by a real click, press or key, writes its setting, and what
// the engine then plays changes as the setting's formula says - measured
// tone by tone at the fake device (TestServices.toneLevelDb()) on streams
// ac4::Encoder writes here (TestServices.writeAc4Stream(), test_room.hpp):
//
//   tones          5.1, a tone a channel (L 440, R 620, C 800, LFE 90, Ls
//                  1030, Rs 1270 Hz), dialnorm -24 dBFS, Lo/Ro centre -1.5
//                  and surround -4.5 dB, Lt/Rt -3 and -6, the LFE -4.5, and
//                  dialogue enhancement on C up to 9 dB
//   presentations  stereo music and effects (L 331, R 457 Hz), English
//                  dialogue at 1 117 Hz that may be raised 6 dB, and audio
//                  description at 1 531 Hz: presentation 1 without the
//                  description, 2 with it
//
// A level is read over the last third of a second the engine handed the
// device, so each check waits until that window is all of the setting's
// making. tests/hearth/test_ac4_engine.cpp holds the same formulas sample
// for sample through the engine alone; these hold that the page's controls
// reach them.
TestCase {
    id: testCase
    name: "DecoderAc4"
    when: windowShown
    width: 900
    // Every card on the page without scrolling (tst_decoder_settings.qml's
    // own height note).
    height: 2400

    property string tones: ""
    property string presentations: ""
    // How close a measured level holds its formula's, in dB.
    readonly property real tolerance: 0.1

    Component { id: pageComponent; DecoderPage { width: 900; height: 2400 } }

    function initTestCase() {
        verify(TestServices.useFakeRoom(), "the fake device room could not be installed");
        HearthController.start();
        tryVerify(function() { return Object.keys(HearthController.decoderSettings).length > 0; }, 15000,
                  "decoderSettings was never populated");
        tones = TestServices.writeAc4Stream("tones.ac4", "tones");
        presentations = TestServices.writeAc4Stream("presentations.ac4", "presentations");
        verify(tones.length > 0 && presentations.length > 0, "the AC-4 streams could not be written");
    }

    function cleanup() {
        HearthController.stop();
        for (let i = HearthController.queue.length - 1; i >= 0; --i) {
            HearthController.removeAt(i);
        }
        tryVerify(function() { return HearthController.queue.length === 0; }, 10000);
    }

    function setting(key) {
        return HearthController.decoderSettings[key];
    }
    // Several settings at once, through the controller, for a starting point.
    function settle(values) {
        const next = Object.assign({}, HearthController.decoderSettings);
        for (const key in values) {
            next[key] = values[key];
        }
        HearthController.setDecoderSettings(next);
        tryVerify(function() {
            for (const key in values) {
                if (setting(key) !== values[key]) {
                    return false;
                }
            }
            return true;
        }, 10000, "the settings never reached the engine");
    }

    // On the AC-4 sub-page, by its switch, unless `onItsOwn`: then it opens
    // on AC-3 and E-AC-3 and turns to AC-4 when an AC-4 item plays.
    function makePage(onItsOwn) {
        const page = createTemporaryObject(pageComponent, testCase.parent);
        verify(page !== null);
        waitForRendering(page);
        if (onItsOwn === true) {
            compare(page.format, "eac3");
        } else {
            mouseClick(findChild(page, "seg-ac4"));
            compare(page.format, "ac4");
            // The sub-page is laid out once it shows; a press before then
            // lands where its controls are not yet.
            waitForRendering(page);
        }
        return page;
    }

    // The rendered slot `label` (HearthController.speakerLabels) is on.
    function slot(label) {
        const index = HearthController.speakerLabels.indexOf(label);
        verify(index >= 0, "no " + label + " slot in " + HearthController.speakerLabels);
        return index;
    }
    function level(label, hz) {
        return TestServices.toneLevelDb(slot(label), hz);
    }
    // The tone's level once it has held within 0.02 dB for longer than the
    // window it is read over - a reading taken while the device has had
    // nothing new can repeat itself, so one repeat proves nothing.
    function steady(label, hz) {
        const readings = [];
        let settled = NaN;
        tryVerify(function() {
            const now = level(label, hz);
            const at = Date.now();
            if (!isFinite(now)) {
                readings.length = 0;
                return false;
            }
            readings.push({ at: at, db: now });
            while (readings.length > 1 && at - readings[1].at >= 500) {
                readings.shift();
            }
            if (at - readings[0].at < 500) {
                return false;
            }
            let low = Infinity;
            let high = -Infinity;
            for (const reading of readings) {
                low = Math.min(low, reading.db);
                high = Math.max(high, reading.db);
            }
            if (high - low < 0.02) {
                settled = now;
                return true;
            }
            return false;
        }, 15000, "the " + hz + " Hz tone in " + label + " never settled: " + level(label, hz) + " dB");
        return settled;
    }
    // Waits until the tone's level is `expected` dB, within the tolerance.
    function heardAt(label, hz, expected, what) {
        tryVerify(function() { return Math.abs(level(label, hz) - expected) < tolerance; }, 15000,
                  what + ": the " + hz + " Hz tone in " + label + " reads " + level(label, hz)
                  + " dB, not " + expected);
    }
    function heardOff(label, hz, reference, what) {
        tryVerify(function() { return level(label, hz) < reference - 60; }, 15000,
                  what + ": the " + hz + " Hz tone in " + label + " is still there at " + level(label, hz) + " dB");
    }
    // 20 log10 of Part 1 clause 5.7.9.3.3's 2^((Lout - dialnorm) / 6), the
    // gain dialogue normalisation takes the stream's -24 dBFS dialogue by.
    function normalised(outputLevel) {
        return 20 * Math.log10(Math.pow(2, (outputLevel + 24) / 6));
    }

    function useLayout(layout) {
        HearthController.setLayoutText(layout);
        tryVerify(function() { return HearthController.speakerLabels.length === (layout === "2.0" ? 2 : 6); },
                  10000, "the layout never became " + layout);
    }
    function play(path, layout) {
        useLayout(layout);
        HearthController.addFiles([path]);
        tryVerify(function() { return HearthController.queue.length === 1; }, 10000);
        HearthController.play();
        tryCompare(HearthController, "state", "playing", 10000);
    }

    // A press along a slider at `fraction` of its travel moves it there
    // (QQC Slider moves on press), and onMoved writes it.
    function pressSlider(slider, fraction) {
        verify(slider !== null, "no slider");
        tryVerify(function() { return slider.enabled; }, 5000, "the slider is disabled");
        tryVerify(function() { return slider.visible && slider.availableWidth > 0; }, 5000,
                  "the slider is not laid out");
        mouseClick(slider, slider.leftPadding + (slider.availableWidth * fraction), slider.height / 2);
    }

    // Keys reach an item only in the active window.
    function keysTo(item) {
        verify(item !== null);
        testCase.parent.Window.window.requestActivate();
        tryVerify(function() { return testCase.parent.Window.window.active; }, 5000);
        item.forceActiveFocus();
        tryVerify(function() { return item.activeFocus; }, 5000);
    }

    function test_aTheBannerIsGoneAndEveryCardIsLive() {
        const page = makePage();
        verify(H.textContaining(page, "Not in this build") === null, "the banner is still there");
        for (const name of ["Dialogue enhancement, dB", "Dialogue level, dB", "Output level, dBFS"]) {
            const control = H.byAccessibleName(page, name);
            verify(control !== null, "no " + name + " control");
            verify(control.enabled, name + " is disabled");
        }
        verify(findChild(page, "ac4PresentationCombo") !== null);
        verify(findChild(page, "ac4DeviceCombo") !== null);
        verify(H.checkBox(page, "Mix in audio description") !== null);
        verify(H.checkBox(page, "Dialogue normalisation") !== null);
        verify(H.checkBox(page, "Follow the stream's preferred downmix") !== null);
        // AC-4's dynamic range is its own (planning/ac4.md, decision 12): no
        // operating mode here.
        compare(H.segment(page, "Mode", "line"), null);
        verify(H.textItem(page, "Nothing AC-4 is playing.") !== null);
    }

    // Each control writes its own setting, and puts it back.
    function test_eachControlWritesItsSetting() {
        settle({ ac4DialogueEnhancementDb: 0, ac4DialogueDb: 0, ac4AssociatedDb: 0, ac4OutputLevelDbfs: -31,
                 ac4AudioDescription: false, ac4Normalise: true, ac4Drc: "auto", ac4PreferredDownmix: false,
                 stereoFold: "loro", concealment: "repeatFade" });
        const page = makePage();

        pressSlider(H.byAccessibleName(page, "Dialogue enhancement, dB"), 0.5);
        tryVerify(function() { return setting("ac4DialogueEnhancementDb") === 6; }, 10000,
                  "enhancement: " + setting("ac4DialogueEnhancementDb"));
        pressSlider(H.byAccessibleName(page, "Dialogue level, dB"), 0.25);
        tryVerify(function() { return setting("ac4DialogueDb") === -6; }, 10000,
                  "dialogue level: " + setting("ac4DialogueDb"));
        pressSlider(H.byAccessibleName(page, "Output level, dBFS"), 1.0);
        tryVerify(function() { return setting("ac4OutputLevelDbfs") === 0; }, 10000,
                  "output level: " + setting("ac4OutputLevelDbfs"));
        // The readouts follow.
        verify(H.textItem(page, "6 dB") !== null, "no enhancement readout");
        verify(H.textItem(page, "−6 dB") !== null, "no dialogue level readout");
        verify(H.textItem(page, "0 dBFS") !== null, "no output level readout");

        const description = H.checkBox(page, "Mix in audio description");
        const itsLevel = H.byAccessibleName(page, "Audio description level, dB");
        verify(!itsLevel.enabled, "its level is live with audio description off");
        mouseClick(description);
        tryVerify(function() { return setting("ac4AudioDescription") === true; }, 10000);
        pressSlider(itsLevel, 0.5);
        tryVerify(function() { return setting("ac4AssociatedDb") === -6; }, 10000,
                  "its level: " + setting("ac4AssociatedDb"));
        mouseClick(description);
        tryVerify(function() { return setting("ac4AudioDescription") === false; }, 10000);

        // The device and the level have nothing to act on without
        // normalisation.
        const normalise = H.checkBox(page, "Dialogue normalisation");
        mouseClick(normalise);
        tryVerify(function() { return setting("ac4Normalise") === false; }, 10000);
        tryVerify(function() {
            return !findChild(page, "ac4DeviceCombo").enabled && !H.byAccessibleName(page, "Output level, dBFS").enabled;
        }, 5000);
        mouseClick(normalise);
        tryVerify(function() { return setting("ac4Normalise") === true; }, 10000);

        // Down on the focused device list: Automatic, then Home theatre.
        const device = findChild(page, "ac4DeviceCombo");
        keysTo(device);
        keyClick(Qt.Key_Down);
        tryVerify(function() { return setting("ac4Drc") === "homeTheatre"; }, 10000, "device: " + setting("ac4Drc"));
        for (const next of ["flatPanelTv", "portableSpeakers", "portableHeadphones", "off"]) {
            keyClick(Qt.Key_Down);
            tryVerify(function() { return setting("ac4Drc") === next; }, 10000, "device: " + setting("ac4Drc"));
        }
        for (let i = 0; i < 5; ++i) {
            keyClick(Qt.Key_Up);
        }
        tryVerify(function() { return setting("ac4Drc") === "auto"; }, 10000, "device: " + setting("ac4Drc"));

        const preferred = H.checkBox(page, "Follow the stream's preferred downmix");
        mouseClick(preferred);
        tryVerify(function() { return setting("ac4PreferredDownmix") === true; }, 10000);
        mouseClick(preferred);
        tryVerify(function() { return setting("ac4PreferredDownmix") === false; }, 10000);

        // The controls shared with AC-3 and E-AC-3 are the same settings.
        mouseClick(H.segment(page, "Downmix", "ltrt"));
        tryVerify(function() { return setting("stereoFold") === "ltrt"; }, 10000);
        mouseClick(H.segment(page, "Downmix", "loro"));
        tryVerify(function() { return setting("stereoFold") === "loro"; }, 10000);
        mouseClick(H.segment(page, "Bad frame", "mute"));
        tryVerify(function() { return setting("concealment") === "mute"; }, 10000);
        mouseClick(H.segment(page, "Bad frame", "repeatFade"));
        tryVerify(function() { return setting("concealment") === "repeatFade"; }, 10000);
        // The LFE in a fold: on for AC-4 until the listener sets it.
        const lfe = H.checkBox(page, "Mix the LFE in");
        verify(lfe !== null);
        const was = setting("mixLfe") ?? true;
        compare(lfe.checked, was);
        mouseClick(lfe);
        tryVerify(function() { return setting("mixLfe") === !was; }, 10000);
        mouseClick(lfe);
        tryVerify(function() { return setting("mixLfe") === was; }, 10000);
    }

    // The output level and dialogue enhancement, heard.
    function test_levelAndEnhancementAreHeardAsTheirFormulasSay() {
        settle({ ac4Normalise: false, ac4Drc: "off", ac4DialogueEnhancementDb: 0, ac4OutputLevelDbfs: -31 });
        const page = makePage();
        play(tones, "5.1");
        const left = steady("L", 440);
        const centre = steady("C", 800);
        verify(left > -30 && left < -10, "the L tone reads " + left + " dB");

        // Dialogue normalisation on, from the page, at -31 and then -17 dBFS.
        mouseClick(H.checkBox(page, "Dialogue normalisation"));
        tryVerify(function() { return setting("ac4Normalise") === true; }, 10000);
        heardAt("L", 440, left + normalised(-31), "at -31 dBFS");
        pressSlider(H.byAccessibleName(page, "Output level, dBFS"), (-17 + 31) / 31);
        tryVerify(function() { return setting("ac4OutputLevelDbfs") === -17; }, 10000,
                  "output level: " + setting("ac4OutputLevelDbfs"));
        heardAt("L", 440, left + normalised(-17), "at -17 dBFS");
        heardAt("C", 800, centre + normalised(-17), "at -17 dBFS");

        // Dialogue enhancement (Part 1 clause 5.7.8): C, whose parameters
        // are 1 in every band, up by the gain to the stream's cap of 9 dB; L
        // as it was.
        pressSlider(H.byAccessibleName(page, "Dialogue enhancement, dB"), 0.5);
        tryVerify(function() { return setting("ac4DialogueEnhancementDb") === 6; }, 10000);
        heardAt("C", 800, centre + normalised(-17) + 6, "enhancement 6 dB");
        heardAt("L", 440, left + normalised(-17), "enhancement 6 dB");
        pressSlider(H.byAccessibleName(page, "Dialogue enhancement, dB"), 1.0);
        tryVerify(function() { return setting("ac4DialogueEnhancementDb") === 12; }, 10000);
        heardAt("C", 800, centre + normalised(-17) + 9, "enhancement capped at 9 dB");
    }

    // The downmix a stereo layout takes, and the LFE in it, heard: Part 1
    // clause 6.2.17's Tables 217 and 218 with the stream's own gains.
    function test_downmixIsHeardWithTheStreamsGains() {
        settle({ ac4Normalise: false, ac4Drc: "off", ac4DialogueEnhancementDb: 0, ac4PreferredDownmix: false,
                 stereoFold: "loro", mixLfe: true });
        const page = makePage();
        // Each tone as coded, from its own speaker.
        play(tones, "5.1");
        const coded = { left: steady("L", 440), centre: steady("C", 800), lfe: steady("LFE", 90),
                        leftSurround: steady("Ls", 1030), rightSurround: steady("Rs", 1270) };
        HearthController.stop();
        tryVerify(function() { return HearthController.state !== "playing"; }, 10000);
        useLayout("2.0");
        HearthController.play();
        tryCompare(HearthController, "state", "playing", 10000);

        // Lo/Ro: Lo = L + c C + s Ls + lfe LFE, and no Rs.
        heardAt("L", 440, coded.left, "Lo/Ro");
        heardAt("L", 800, coded.centre - 1.5, "Lo/Ro");
        heardAt("L", 1030, coded.leftSurround - 4.5, "Lo/Ro");
        heardAt("L", 90, coded.lfe - 4.5, "Lo/Ro");
        heardOff("L", 1270, coded.rightSurround, "Lo/Ro");
        // Lt/Rt, from the page: both surrounds in each output at -6 dB.
        mouseClick(H.segment(page, "Downmix", "ltrt"));
        tryVerify(function() { return setting("stereoFold") === "ltrt"; }, 10000);
        heardAt("L", 1270, coded.rightSurround - 6, "Lt/Rt");
        heardAt("R", 1030, coded.leftSurround - 6, "Lt/Rt");
        heardAt("L", 800, coded.centre - 3, "Lt/Rt");
        // The LFE out, and back, from the page.
        mouseClick(H.checkBox(page, "Mix the LFE in"));
        tryVerify(function() { return setting("mixLfe") === false; }, 10000);
        heardOff("L", 90, coded.lfe, "no LFE");
        mouseClick(H.checkBox(page, "Mix the LFE in"));
        tryVerify(function() { return setting("mixLfe") === true; }, 10000);
        heardAt("L", 90, coded.lfe - 4.5, "the LFE back");
        // Lo/Ro again, then the stream's own preference, which is Lt/Rt.
        mouseClick(H.segment(page, "Downmix", "loro"));
        tryVerify(function() { return setting("stereoFold") === "loro"; }, 10000);
        heardOff("L", 1270, coded.rightSurround, "Lo/Ro again");
        mouseClick(H.checkBox(page, "Follow the stream's preferred downmix"));
        tryVerify(function() { return setting("ac4PreferredDownmix") === true; }, 10000);
        heardAt("L", 1270, coded.rightSurround - 6, "the stream's preferred Lt/Rt");
        mouseClick(H.checkBox(page, "Follow the stream's preferred downmix"));
        tryVerify(function() { return setting("ac4PreferredDownmix") === false; }, 10000);
    }

    // The presentation picker, the dialogue level and audio description,
    // heard.
    function test_presentationDialogueAndDescriptionAreHeard() {
        settle({ ac4Normalise: false, ac4Drc: "off", ac4DialogueDb: 0, ac4AssociatedDb: 0,
                 ac4AudioDescription: false, ac4PresentationId: -1, ac4PresentationIndex: -1 });
        const page = makePage(true);
        play(presentations, "2.0");
        tryCompare(page, "format", "ac4", 10000);
        // The table lists what the decoder reads: two presentations.
        tryVerify(function() { return (HearthController.currentMedia.ac4?.presentations ?? []).length === 2; },
                  15000, "the media information never listed the presentations");
        tryVerify(function() { return H.textContaining(page, "music and effects + dialogue + audio description")
                                      !== null; }, 5000, "the presentation table shows no content");
        // With no audio description wanted, none is heard.
        const music = steady("L", 331);
        const dialogue = steady("L", 1117);
        heardOff("L", 1531, dialogue, "no audio description");

        // The dialogue level, from the page (Part 1 clause 6.2.16.1): -6 dB
        // on the dialogue alone, then up to the stream's maximum of 6 dB
        // however far the slider goes.
        pressSlider(H.byAccessibleName(page, "Dialogue level, dB"), 0.25);
        tryVerify(function() { return setting("ac4DialogueDb") === -6; }, 10000);
        heardAt("L", 1117, dialogue - 6, "dialogue at -6 dB");
        heardAt("L", 331, music, "dialogue at -6 dB");
        pressSlider(H.byAccessibleName(page, "Dialogue level, dB"), 1.0);
        tryVerify(function() { return setting("ac4DialogueDb") === 12; }, 10000);
        heardAt("L", 1117, dialogue + 6, "dialogue capped at +6 dB");

        // Audio description, from the page: the presentation that carries
        // it plays, and it is heard at its level (clause 6.2.16.2).
        mouseClick(H.checkBox(page, "Mix in audio description"));
        tryVerify(function() { return setting("ac4AudioDescription") === true; }, 10000);
        tryVerify(function() { return HearthController.thisFrame.ac4PresentationId === 2; }, 15000,
                  "the presentation with audio description never played");
        const described = steady("L", 1531);
        verify(described > music - 20, "the description reads " + described + " dB");
        pressSlider(H.byAccessibleName(page, "Audio description level, dB"), 0.5);
        tryVerify(function() { return setting("ac4AssociatedDb") === -6; }, 10000);
        heardAt("L", 1531, described - 6, "audio description at -6 dB");
        heardAt("L", 1117, dialogue + 6, "audio description at -6 dB");

        // The picker, from the keyboard: presentation 1, which carries no
        // description however it is set.
        // The card around it is named "Presentation" too.
        const picker = findChild(page, "ac4PresentationCombo");
        keysTo(picker);
        keyClick(Qt.Key_Down);
        tryVerify(function() { return setting("ac4PresentationId") === 1; }, 10000,
                  "the picker chose " + setting("ac4PresentationId"));
        tryVerify(function() { return HearthController.thisFrame.ac4PresentationId === 1; }, 15000);
        heardOff("L", 1531, described, "presentation 1");
        // Back to Automatic.
        keyClick(Qt.Key_Up);
        tryVerify(function() { return setting("ac4PresentationId") === -1; }, 10000);
    }
}
