import QtQuick
import QtTest

import Ac3Forge

// AC-4 objects from the page, end to end (planning/ac4.md, I5b's exit criteria). The
// Objects tab and the AC-4 tab author objects, the Encode button writes them, and the
// command line the run recorded - the one the command bar echoed - is run through this
// build's ac3cli in a folder holding copies of what it names: the sources, and the scene
// file an AC-4 object encode writes beside its output. The two streams must be the same
// bytes, for a raw stream and an MP4 file, with A-JOC and direct coding, one source and
// several with an assignment, a trim, a fold, a speaker, an LFE and an offset. The page's
// file is then decoded by the object page, each object's place and gain read back.
//
// The last case is I5's round trip held to the page: an ADM BWF master (the fixture
// tests/cli/test_cli_atmos_adm_ac4.cpp writes: two bed channels and one dynamic object that
// jumps position) has its scene authored on the page as AC-4 - the page reads audio, not ADM,
// so the scene is the one the master states - and decodes with its objects, positions within
// 0.06 in each axis and gains within 2 dB.
//
// The pickers are the only seam, as in tst_e2e_encode.qml; running ac3cli is the second,
// through qml_test_main.cpp's cliRunner.
TestCase {
    id: testCase
    name: "E2eAc4Objects"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    readonly property url stereoUrl:
        Qt.resolvedUrl("../../../../fuzz/seeds/fuzz_wav_read/roundtrip-stereo.wav")
    readonly property url surroundUrl:
        Qt.resolvedUrl("../../../../fuzz/seeds/fuzz_wav_read/roundtrip-51.wav")
    readonly property url admUrl: Qt.resolvedUrl("../fixtures/adm-two-beds-one-object.wav")
    readonly property url rawOutUrl: Qt.resolvedUrl("_test_output/tst_e2e_ac4_objects.ac4")
    readonly property url mp4OutUrl: Qt.resolvedUrl("_test_output/tst_e2e_ac4_objects.mp4")
    readonly property url admOutUrl: Qt.resolvedUrl("_test_output/tst_e2e_ac4_objects_adm.ac4")
    readonly property url cliFolderUrl: Qt.resolvedUrl("_test_output/tst_e2e_ac4_objects_cli")
    readonly property url stereoPathsUrl:
        Qt.resolvedUrl("_test_output/roundtrip-stereo-paths.json")

    function cleanup() {
        StreamPlayerController.pause();
        EncoderController.atmosEnabled = false;
        while (EncoderController.sourceModel.length > 0) {
            EncoderController.removeSource(0);
        }
        EncoderController.codecIndex = 0;
        EncoderController.containerIndex = 0;
        EncoderController.bitrateKbps = 192;
        EncoderController.ac4ObjectCodingIndex = 0;
        EncoderController.ac4Dialnorm = 31;
        EncoderController.ac4Crc = true;
    }

    // Every encode and inspection a case starts is waited for before it ends, so no job of
    // this suite is left running at teardown (tst_teardown.qml and tst_ac4_decode.qml are the
    // suites that leave one on purpose).
    function cleanupTestCase() {
        tryCompare(EncoderController, "busy", false, 15000);
        tryCompare(ObjectDecodeController, "busy", false, 15000);
    }

    // ---- helpers (tst_e2e_ac4.qml's) --------------------------------------

    function findByName(root, name) {
        let found = null;
        tryVerify(() => {
            found = findChild(root, name);
            return found !== null;
        }, 5000, "no object called " + name);
        return found;
    }

    function findAccessible(item, name) {
        if (item.Accessible && item.Accessible.name === name) {
            return item;
        }
        const kids = item.children;
        for (let i = 0; i < kids.length; ++i) {
            const hit = findAccessible(kids[i], name);
            if (hit) {
                return hit;
            }
        }
        return null;
    }

    function focusIn(item) {
        const w = item.Window.window;
        w.requestActivate();
        tryVerify(() => w.active, 5000, "window never became active");
        item.forceActiveFocus();
        tryVerify(() => item.activeFocus, 5000, "control never took focus");
    }

    function settle(item) {
        waitForRendering(item);
        let last = "";
        tryVerify(() => {
            const p = item.mapToItem(null, 0, 0);
            const key = p.x + "," + p.y + "," + item.width + "," + item.height;
            const stable = item.width > 0 && key === last;
            last = key;
            return stable;
        }, 5000, "item never settled");
    }

    function click(item) {
        settle(item);
        mouseClick(item);
    }

    function keys(item, key, times) {
        focusIn(item);
        for (let i = 0; i < times; ++i) {
            keyClick(key);
        }
    }

    function pickFile(root, dialogName, url) {
        const dialog = findByName(root, dialogName);
        tryCompare(dialog, "visible", true, 5000);
        dialog.selectedFile = url;
        dialog.accepted();
        dialog.close();
        tryCompare(dialog, "visible", false, 5000);
        return dialog;
    }

    function openTab(win, key) {
        click(findByName(win.contentItem, "tab-" + key));
        compare(win.currentTab, key);
    }

    function pressEncodeAndSaveTo(win, url) {
        const encodeButton = findByName(win.contentItem, "encodeButton");
        tryCompare(encodeButton, "enabled", true);
        click(encodeButton);
        pickFile(win, "saveDialog", url);
        tryCompare(EncoderController, "busy", false, 60000);
    }

    // The source through the first-run card, the Expert tier, AC-4 from the codec picker and
    // object mode from the Objects tab's switch: what tst_ac4_objects.qml opens.
    function openAc4Objects(url) {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        click(findByName(win.contentItem, "firstRun-file"));
        pickFile(win, "openDialog", url);
        tryCompare(EncoderController, "sourceReady", true, 10000);
        click(findByName(win.contentItem, "seg-expert"));
        openTab(win, "format");
        keys(findAccessible(win.contentItem, "Codec"), Qt.Key_Down, 2);
        compare(EncoderController.codecIndex, 2);
        openTab(win, "objects");
        focusIn(findByName(win.contentItem, "atmosSwitch"));
        keyClick(Qt.Key_Space);
        compare(EncoderController.atmosEnabled, true);
        return win;
    }

    // The recorded line, run through ac3cli in a folder holding the files it names, and what
    // it wrote compared with what the page wrote.
    function runEchoedLineAndCompare(line, outName, pageUrl, sources) {
        if (!cliRunner.available()) {
            skip("this build has no ac3cli to run the echoed line through");
        }
        verify(cliRunner.prepare(cliFolderUrl, sources[0]));
        for (let i = 1; i < sources.length; ++i) {
            verify(cliRunner.copyInto(cliFolderUrl, sources[i]), "no " + sources[i]);
        }
        compare(cliRunner.run(line, cliFolderUrl), 0, line);
        const cliUrl = Qt.resolvedUrl("_test_output/tst_e2e_ac4_objects_cli/" + outName);
        verify(cliRunner.size(pageUrl) > 0);
        compare(cliRunner.size(cliUrl), cliRunner.size(pageUrl), line);
        verify(cliRunner.sameBytes(cliUrl, pageUrl), line);
    }

    // The object page's read of a file the page wrote: every decoded frame's objects.
    function inspect(url) {
        ObjectDecodeController.inspectFile(url);
        tryCompare(ObjectDecodeController, "busy", false, 15000);
        compare(ObjectDecodeController.error, "");
        compare(ObjectDecodeController.isAc4, true);
        verify(ObjectDecodeController.frameCount > 2);
        return ObjectDecodeController.frames;
    }

    // ---- cases ----------------------------------------------------------

    function test_rawStreamOfOneSourceEqualsTheEchoedLineAndDecodesToItsObjects() {
        const win = openAc4Objects(stereoUrl);
        EncoderController.bitrateKbps = 256;
        // The first object moves: a key at the start and one 0.4 s in.
        EncoderController.setObjectPathKeyframes(0, [
            { time: 0.0, x: 0.2, y: 0.3, z: 0.0, gain: 0.5 },
            { time: 0.4, x: 0.8, y: 0.7, z: 0.5, gain: 0.5 }]);

        const echoed = "ac3cli atmos-encode roundtrip-stereo.wav out.ac4 256 2 "
                       + "roundtrip-stereo-paths.json codec=ac4";
        compare(win.cliLine, echoed);
        pressEncodeAndSaveTo(win, rawOutUrl);
        const run = EncoderController.runs[0];
        compare(run.status, "done", run.detail);
        compare(run.cliLine, echoed);
        compare(run.eac3, false);
        verify(EncoderController.status.indexOf("2 objects, A-JOC") > 0, EncoderController.status);
        verify(EncoderController.status.indexOf("lags the input by 4385 samples") > 0,
               EncoderController.status);

        runEchoedLineAndCompare(run.cliLine, "out.ac4", rawOutUrl, [stereoUrl, stereoPathsUrl]);

        // The object page reads two dynamic objects, the first moving as it was told.
        const frames = inspect(rawOutUrl);
        compare(ObjectDecodeController.ac4DynamicObjects, 2);
        compare(ObjectDecodeController.ac4BedObjects, 0);
        const first = frames[1].objects[0];
        const last = frames[frames.length - 1].objects[0];
        verify(Math.abs(first.x - last.x) > 0.1 || Math.abs(first.y - last.y) > 0.1,
               "the moving object did not move: " + first.x + "," + first.y + " then "
               + last.x + "," + last.y);
        // The second holds the default place, and the stream's gain is the inverse-root law's.
        verify(Math.abs(frames[1].objects[1].gainDb - 20 * Math.log10(0.7 / Math.SQRT2)) <= 1.0);
    }

    function test_mp4OfSeveralSourcesWithAnAssignmentEqualsTheEchoedLine() {
        const win = openAc4Objects(stereoUrl);
        EncoderController.addSourceFile(surroundUrl);
        compare(EncoderController.sourceModel.length, 2);
        EncoderController.setSourceOffset(1, 0.02);
        // Stereo: two objects, the second 3 dB down. 5.1: the front pair folded to one object,
        // the centre held at the left speaker, the LFE channel the stream's LFE object, and the
        // surrounds silent.
        EncoderController.setAssignment(0, 0, "obj");
        EncoderController.setAssignment(0, 1, "obj@-3");
        EncoderController.setAssignment(1, 0, "objm");
        EncoderController.setAssignment(1, 1, "objm");
        EncoderController.setAssignment(1, 2, "L");
        EncoderController.setAssignment(1, 3, "LFE");
        EncoderController.setAssignment(1, 4, "none");
        EncoderController.setAssignment(1, 5, "none");
        compare(EncoderController.objectCount, 3);
        EncoderController.setObjectPathKeyframes(2, [
            { time: 0.0, x: 0.9, y: 0.1, z: 0.0, gain: 0.5 },
            { time: 0.5, x: 0.1, y: 0.9, z: 0.0, gain: 0.25 }]);
        EncoderController.setObjectPosition(1, 0.3, 0.6, 0.2);
        EncoderController.bitrateKbps = 320;
        EncoderController.containerIndex = 3;                         // MP4
        EncoderController.ac4ObjectCodingIndex = 1;                   // direct-coded
        EncoderController.ac4Dialnorm = 27;
        compare(EncoderController.ac4ObjectsRefusal, "");

        const line = win.cliLine;
        verify(line.indexOf("ac3cli atmos-encode roundtrip-stereo.wav out.mp4 320 0 "
                            + "roundtrip-stereo-paths.json src=roundtrip-51.wav map=") === 0, line);
        verify(line.indexOf(EncoderController.mapToken) > 0, line);
        verify(line.endsWith(" offset=1:0.02 codec=ac4 coding=direct dialnorm=27"), line);

        pressEncodeAndSaveTo(win, mp4OutUrl);
        const run = EncoderController.runs[0];
        compare(run.status, "done", run.detail);
        compare(run.cliLine, line);
        verify(EncoderController.status.indexOf("5 objects, direct-coded") > 0,
               EncoderController.status);

        runEchoedLineAndCompare(run.cliLine, "out.mp4", mp4OutUrl,
                                [stereoUrl, surroundUrl, stereoPathsUrl]);

        // The object page reads the MP4's track: the three dynamic objects, the one held at the
        // speaker and the LFE.
        const frames = inspect(mp4OutUrl);
        compare(frames[1].objects.length, 5);
    }

    // ---- I5's round trip, on the page ---------------------------------------

    // The ring positions ADM's polar coordinates give (tests/oba/test_atmos_motion.cpp's
    // kL and kSR, which ac3::admbridge is checked against), and dead ahead.
    readonly property var admLeft: ({ x: 0.25, y: 0.066987, z: 0.0 })
    readonly property var admRight: ({ x: 0.75, y: 0.066987, z: 0.0 })
    readonly property var admRearRight: ({ x: 0.969846, y: 0.671010, z: 0.0 })
    readonly property var admAhead: ({ x: 0.5, y: 0.0, z: 0.0 })

    function gainDbOf(linear) {
        return linear > 0 ? 20 * Math.log10(linear) : -Infinity;
    }

    // Which decoded object sits at `place` in `objects`: the nearest, and near enough.
    function objectAt(objects, place, used) {
        let best = -1;
        let bestDistance = 1e9;
        for (let i = 0; i < objects.length; ++i) {
            if (used.indexOf(i) >= 0) {
                continue;
            }
            const d = Math.max(Math.abs(objects[i].x - place.x), Math.abs(objects[i].y - place.y),
                               Math.abs(objects[i].z - place.z));
            if (d < bestDistance) {
                bestDistance = d;
                best = i;
            }
        }
        return best;
    }

    function test_anAdmMasterAuthoredOnThePageDecodesWithItsObjectsInTolerance() {
        // The master's audio is the page's source: its three channels, the two bed channels
        // first and the moving object third.
        EncoderController.loadSourceFile(admUrl);
        tryCompare(EncoderController, "sourceReady", true, 10000);
        compare(EncoderController.sourceModel[0].channels, 3);
        EncoderController.codecIndex = 2;
        EncoderController.atmosEnabled = true;
        compare(EncoderController.ac4Objects, true);
        // Its scene as the master states it: the two bed channels held at the speakers they
        // name (M+030 and M-030), the third channel an object at azimuth -110 for 0.096 s
        // and then dead ahead, gain 1 throughout.
        EncoderController.setAssignment(0, 0, "L");
        EncoderController.setAssignment(0, 1, "R");
        EncoderController.setAssignment(0, 2, "obj");
        compare(EncoderController.objectCount, 1);
        EncoderController.setObjectPathKeyframes(0, [
            { time: 0.0, x: admRearRight.x, y: admRearRight.y, z: 0.0, gain: 1.0 },
            { time: 0.095, x: admRearRight.x, y: admRearRight.y, z: 0.0, gain: 1.0 },
            { time: 0.096, x: admAhead.x, y: admAhead.y, z: 0.0, gain: 1.0 }]);
        EncoderController.bitrateKbps = 256;
        compare(EncoderController.ac4ObjectsRefusal, "");

        EncoderController.encodeTo(admOutUrl);
        tryCompare(EncoderController, "busy", false, 60000);
        const run = EncoderController.runs[0];
        compare(run.status, "done", run.detail);
        const lag = /lags the input by (\d+) samples/.exec(EncoderController.status);
        verify(lag !== null, EncoderController.status);

        // The decoded stream, in the object page's terms: every frame's objects.
        const frames = inspect(admOutUrl);
        compare(ObjectDecodeController.ac4DynamicObjects, 3);
        // Well inside each hold, as I5's test picks them, clear of the frame around the jump
        // (input 0.05 s and 0.13 s, at the decoder's lag): the frames those samples come out in.
        const lagSeconds = Number(lag[1]) / 48000;
        const frameOf = (inputSeconds) => {
            for (let k = 0; k < frames.length; ++k) {
                if (inputSeconds + lagSeconds < frames[k].time) {
                    return k;
                }
            }
            return frames.length - 1;
        };
        const before = frames[frameOf(0.05)].objects;
        const after = frames[frameOf(0.13)].objects;

        // Each expected object found among the decoded ones at the first of the two moments,
        // then followed by index to the second.
        const used = [];
        const moving = objectAt(before, admRearRight, used);
        used.push(moving);
        const left = objectAt(before, admLeft, used);
        used.push(left);
        const right = objectAt(before, admRight, used);
        const positionTolerance = 0.06;
        const gainToleranceDb = 2.0;
        // The largest error over the six readings, logged so a run says how far inside the
        // tolerances the stream is.
        let worstPosition = 0.0;
        let worstGainDb = 0.0;
        const check = (decoded, want, what) => {
            verify(Math.abs(decoded.x - want.x) <= positionTolerance, what + " x "
                   + decoded.x + " vs " + want.x);
            verify(Math.abs(decoded.y - want.y) <= positionTolerance, what + " y "
                   + decoded.y + " vs " + want.y);
            verify(Math.abs(decoded.z - want.z) <= positionTolerance, what + " z "
                   + decoded.z + " vs " + want.z);
            verify(Math.abs(decoded.gainDb - gainDbOf(1.0)) <= gainToleranceDb, what + " gain "
                   + decoded.gainDb);
            worstPosition = Math.max(worstPosition, Math.abs(decoded.x - want.x),
                                     Math.abs(decoded.y - want.y), Math.abs(decoded.z - want.z));
            worstGainDb = Math.max(worstGainDb, Math.abs(decoded.gainDb - gainDbOf(1.0)));
        };
        check(before[moving], admRearRight, "the object before the jump");
        check(before[left], admLeft, "the left bed channel");
        check(before[right], admRight, "the right bed channel");
        check(after[moving], admAhead, "the object after the jump");
        check(after[left], admLeft, "the left bed channel later");
        check(after[right], admRight, "the right bed channel later");
        console.info("ADM master authored on the page: worst position error "
                     + worstPosition.toFixed(4) + " of " + positionTolerance + ", worst gain error "
                     + worstGainDb.toFixed(3) + " dB of " + gainToleranceDb + " dB");
        // The jump is in the stream, not a flat reading loose enough to pass.
        const jump = Math.abs(before[moving].x - after[moving].x)
                     + Math.abs(before[moving].y - after[moving].y)
                     + Math.abs(before[moving].z - after[moving].z);
        verify(jump > 0.1, "the object did not move: " + jump);
    }
}
