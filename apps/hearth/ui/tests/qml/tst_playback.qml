import QtQuick
import QtTest

import Ac3ForgeHearth
import Ac3ForgeHearthTest

import "HearthTestHelpers.js" as H

// The Play page and the transport bar driven end to end (FEATURE_COVERAGE.md
// rows 9-32): files go in through the page's own Add files... dialog and its
// drop area, the transport bar's own buttons and scrubber play, pause, seek,
// skip and stop them, and the REAL engine plays each item into a fake device
// (TestServices.useFakeRoom(), test_room.hpp) with a clock of its own - so
// "it plays" is asserted on what the device was actually handed (frames
// heard, a non-zero peak), not only on a state string.
//
// The streams are the repository's own small golden files, copied into the
// suite's scratch folder under readable names (TestServices.stageFixture()):
// a 2.5 s AC-3 5.1 stream and a 30 s E-AC-3 stereo one, real encodes that
// the file item loader probes and decodes like any other.
//
// HearthController is one singleton for the whole file and test_* functions
// run in alphabetical order, so init() stops the transport and empties the
// queue before every case (tst_play_queue.qml's own header explains why
// nothing here assumes a starting state), and every wait is a
// tryVerify()/tryCompare() on the poll-driven properties.
TestCase {
    id: testCase
    name: "Playback"
    when: windowShown
    width: 1200
    height: 700

    readonly property string repo: Qt.resolvedUrl("../../../../../").toString()
    property string shortAc3: ""
    property string longEac3: ""
    property string notAudio: ""

    Component {
        id: playerComponent
        Item {
            width: 1200
            height: 700
            property alias page: page
            property alias transport: transport
            PlayPage { id: page; width: 1200; height: 652 }
            TransportBar { id: transport; y: 652; width: 1200; height: 48 }
        }
    }

    function initTestCase() {
        verify(TestServices.useFakeRoom(), "the fake device room could not be installed");
        HearthController.firstRunSeen = true;
        HearthController.start();
        shortAc3 = TestServices.stageFixture(repo + "tests/golden/external-baseline/ac3-51-448/dee.ac3",
                                             "Short 5.1.ac3");
        longEac3 = TestServices.stageFixture(repo + "tests/golden/external-baseline/eac3-music-stereo-96/dee.ec3",
                                             "Long stereo.ec3");
        // Plain text under an audio file's name: probing it is what marks it
        // unplayable, and the queue row has to say so.
        notAudio = TestServices.stageFixture(repo + "README.md", "Not audio.ec3");
        verify(shortAc3.length > 0 && longEac3.length > 0 && notAudio.length > 0, "fixtures were not staged");
    }

    function init() {
        TestServices.setClockSpeed(1.0);
        HearthController.stop();
        for (let i = HearthController.queue.length - 1; i >= 0; --i) {
            HearthController.removeAt(i);
        }
        tryVerify(function() { return HearthController.queue.length === 0 && HearthController.state === "stopped"; },
                  10000, "the queue never emptied before the case");
        if (HearthController.volumeDb !== 0) {
            HearthController.setVolumeDb(0);
            tryCompare(HearthController, "volumeDb", 0, 5000);
        }
    }

    function makePlayer() {
        const player = createTemporaryObject(playerComponent, testCase.parent);
        verify(player !== null);
        waitForRendering(player);
        return player;
    }

    function control(player, name) {
        const found = findChild(player, name);
        verify(found !== null, "no control named " + name);
        return found;
    }

    // Opens the page's own "Add files" dialog with its own button and
    // accepts it on `path`. Under -platform offscreen there is no native
    // picker, so QtQuick.Dialogs shows its own non-native one in a window of
    // its own; the file is chosen the one way that survives that dialog's
    // accept() - as its selection when it opens (the non-native
    // implementation drops a selection made after it has opened, since its
    // own list view never saw it) - and the dialog is then accepted, which
    // is what runs PlayPage's own onAccepted with selectedFiles.
    function addThroughDialog(player, path) {
        const dialog = H.dialog(player.page, "Add files", TestServices);
        verify(dialog !== null, "PlayPage declares no \"Add files\" dialog");
        dialog.currentFolder = H.fileUrl(H.folderOf(path));
        dialog.selectedFile = H.fileUrl(path);
        mouseClick(control(player.page, "addFiles"));
        tryVerify(function() { return dialog.visible; }, 5000, "Add files... did not open the dialog");
        dialog.accept();
        tryVerify(function() { return !dialog.visible; }, 5000);
    }

    function queued(path) {
        return HearthController.queue.findIndex(function(item) { return item.path === path; });
    }

    // One short and one long item, the short first: through the dialog for
    // the first, through the drop area for the second - both of the page's
    // ways in.
    function queueBoth(player) {
        addThroughDialog(player, shortAc3);
        tryVerify(function() { return queued(shortAc3) === 0; }, 10000, "the dialog's file never reached the queue");
        verify(TestServices.dropFiles(player.page, [longEac3]), "the drop area refused the drop");
        tryVerify(function() { return queued(longEac3) === 1; }, 10000, "the dropped file never reached the queue");
    }

    // --- adding -------------------------------------------------------------

    function test_addFilesDialogQueuesTheChosenFileWithItsProbedFacts() {
        const player = makePlayer();
        const queueList = control(player.page, "queueList");
        addThroughDialog(player, shortAc3);
        tryCompare(queueList, "count", 1, 10000);
        compare(HearthController.queue[0].title, "Short 5.1.ac3");
        // The first item added to an empty queue is the current one, and the
        // Now playing card names it.
        tryCompare(HearthController, "currentIndex", 0, 5000);
        verify(H.textItem(player.page, "Short 5.1.ac3") !== null, "the queue row does not show the title");
        // An item's facts are read when the engine opens it (Player's own
        // session open, not at add time), so play it: stream kind, channels
        // and rate are then what the file says about itself.
        mouseClick(control(player.transport, "transportPlayPause"));
        tryVerify(function() { return HearthController.queue[0].streamKind === "AC-3"; }, 10000,
                  "the queued item was never probed as AC-3");
        compare(HearthController.queue[0].channels, 6);
        compare(HearthController.queue[0].sampleRate, 48000);
        compare(HearthController.queue[0].codecBadge, "A3");
        verify(HearthController.queue[0].durationMs > 2000 && HearthController.queue[0].durationMs < 3000,
               "durationMs " + HearthController.queue[0].durationMs);
        // The row's metadata line shows them.
        tryVerify(function() { return H.textContaining(queueList, "AC-3 · 6 ch · 48 kHz") !== null; }, 5000,
                  "the queue row does not show the probed facts");
    }

    function test_addFolderDialogQueuesEveryStreamInTheFolder() {
        const a = TestServices.stageFixture(shortAc3, "One.ac3", "album");
        const b = TestServices.stageFixture(longEac3, "Two.ec3", "album");
        verify(a.length > 0 && b.length > 0);
        const player = makePlayer();
        const dialog = H.dialog(player.page, "Add folder", TestServices);
        verify(dialog !== null, "PlayPage declares no \"Add folder\" dialog");
        dialog.currentFolder = H.fileUrl(H.folderOf(H.folderOf(a)));
        dialog.selectedFolder = H.fileUrl(H.folderOf(a));
        mouseClick(control(player.page, "addFolder"));
        tryVerify(function() { return dialog.visible; }, 5000, "Add folder... did not open the dialog");
        dialog.accept();
        tryVerify(function() { return queued(a) >= 0 && queued(b) >= 0; }, 10000,
                  "Add folder never queued the folder's two streams");
        compare(HearthController.queue.length, 2);
    }

    // Dropped files are queued in drop order. A file that is not a stream
    // is found out when the engine tries to open it: with the default
    // "skip" policy the row turns into a "not playable" pill with the reason
    // under the title, and playback moves on to the next item.
    function test_droppedFilesAreQueuedAndAnUnplayableOneIsSkipped() {
        const player = makePlayer();
        compare(HearthController.onFailure, "skip");
        verify(TestServices.dropFiles(player.page, [notAudio, longEac3]), "the drop area refused the drop");
        tryVerify(function() { return queued(notAudio) === 0 && queued(longEac3) === 1; }, 10000,
                  "the dropped files never reached the queue");
        mouseClick(control(player.transport, "transportPlayPause"));
        tryVerify(function() { return HearthController.queue[0].playable === false; }, 10000,
                  "a text file was never marked unplayable");
        verify(HearthController.queue[0].note.length > 0, "an unplayable row carries no reason");
        tryVerify(function() { return H.textItem(player.page, "not playable") !== null; }, 5000,
                  "the row shows no \"not playable\" pill");
        tryVerify(function() { return H.textItem(player.page, HearthController.queue[0].note) !== null; }, 5000,
                  "the row does not show why");
        tryCompare(HearthController, "currentIndex", 1, 10000);
        tryCompare(HearthController, "state", "playing", 10000);
        // The transport bar's note/error line says what happened.
        const status = control(player.transport, "transportState");
        tryVerify(function() { return status.visible && status.text.length > 0
                                      && (status.text === HearthController.errorText
                                          || status.text === HearthController.noteText); }, 10000,
                  "the transport bar says nothing about the skipped item");
        tryVerify(function() { return HearthController.queue[1].streamKind === "E-AC-3"; }, 10000);
        compare(HearthController.queue[1].codecBadge, "E3");
    }

    // --- the transport --------------------------------------------------------

    function test_playButtonPlaysIntoTheDeviceAndTheMonitorFollows() {
        const player = makePlayer();
        queueBoth(player);
        const play = control(player.transport, "transportPlayPause");
        compare(play.accessibleName, "Play");
        TestServices.resetPeak();
        const heardBefore = TestServices.device().framesHeard;

        mouseClick(play);
        tryCompare(HearthController, "state", "playing", 10000);
        compare(play.accessibleName, "Pause");
        // The engine opened the fake room's default endpoint at the item's
        // own rate and width, and is feeding it real decoded audio.
        tryVerify(function() { return TestServices.device().open; }, 10000, "the engine never opened the device");
        compare(TestServices.device().endpoint, "fake-speakers");
        compare(TestServices.device().sampleRate, 48000);
        tryVerify(function() { return TestServices.device().framesHeard > heardBefore + 24000; }, 10000,
                  "half a second of audio never reached the device");
        verify(TestServices.device().peak > 0.01, "the device was only ever handed silence");

        // The position advances, and the transport bar's own readout with it.
        tryVerify(function() { return HearthController.positionMs >= 1000; }, 10000, "positionMs never advanced");
        tryVerify(function() { return control(player.transport, "transportElapsed").text !== "00:00"; }, 5000);
        compare(control(player.transport, "transportDuration").text, "00:02");

        // The monitor: one meter per rendered slot of the 2.0 layout, moving
        // off the floor, and a loudness and this-frame reading.
        tryVerify(function() {
            return HearthController.levels.length === HearthController.speakerLabels.length
                   && HearthController.levels.some(function(level) { return level.peakDb > -60; });
        }, 10000, "the meters never moved");
        tryVerify(function() { return HearthController.loudness.momentary !== undefined; }, 10000,
                  "no momentary loudness while playing");
        tryVerify(function() { return HearthController.thisFrame.dialnorm !== undefined; }, 10000,
                  "no this-frame report while playing");
        verify(H.textContaining(player.page, "access unit") !== null, "the This frame card shows no access unit");
        // Now playing names the item, and its metadata line the next one.
        verify(H.textContaining(player.page, "next: Long stereo.ec3") !== null,
               "the Now playing line does not name the next item");
        // The signal path's "you hear it on" stage names the device.
        tryVerify(function() { return H.textItem(player.page, "Test speakers") !== null; }, 5000,
                  "the signal path does not name the device playing");
    }

    function test_pauseButtonPausesTheDeviceAndPlayResumesIt() {
        const player = makePlayer();
        queueBoth(player);
        const play = control(player.transport, "transportPlayPause");
        mouseClick(play);
        tryCompare(HearthController, "state", "playing", 10000);
        tryVerify(function() { return HearthController.positionMs > 200; }, 10000);

        mouseClick(play);
        tryCompare(HearthController, "state", "paused", 10000);
        tryVerify(function() { return TestServices.device().paused; }, 5000, "the device was never paused");
        compare(play.accessibleName, "Play");
        const pausedAt = HearthController.positionMs;

        mouseClick(play);
        tryCompare(HearthController, "state", "playing", 10000);
        tryVerify(function() { return !TestServices.device().paused; }, 5000, "the device was never resumed");
        tryVerify(function() { return HearthController.positionMs > pausedAt + 200; }, 10000,
                  "the position did not move on after resuming");
    }

    // A press on the scrubber pauses, a release there seeks to that point
    // and resumes (TransportBar.qml's own onPressedChanged).
    function test_scrubberSeeksToWhereItIsReleased() {
        const player = makePlayer();
        const b = TestServices.stageFixture(longEac3, "Seek.ec3");
        verify(TestServices.dropFiles(player.page, [b]));
        tryVerify(function() { return HearthController.queue.length === 1; }, 10000);
        mouseClick(control(player.transport, "transportPlayPause"));
        tryCompare(HearthController, "state", "playing", 10000);
        tryVerify(function() { return HearthController.durationMs > 20000; }, 10000, "no duration for the item");

        const scrubber = control(player.transport, "transportPosition");
        tryVerify(function() { return scrubber.enabled; }, 5000);
        const x = scrubber.leftPadding + scrubber.availableWidth * 0.75;
        mousePress(scrubber, x, scrubber.height / 2);
        tryCompare(HearthController, "state", "paused", 5000);
        mouseRelease(scrubber, x, scrubber.height / 2);
        const target = HearthController.durationMs * 0.75;
        tryVerify(function() { return Math.abs(HearthController.positionMs - target) < 2500; }, 10000,
                  "the position " + HearthController.positionMs + " never reached ~" + target);
        tryCompare(HearthController, "state", "playing", 5000);
    }

    function test_nextPreviousAndStopButtons() {
        const player = makePlayer();
        queueBoth(player);
        const next = control(player.transport, "transportNext");
        const previous = control(player.transport, "transportPrevious");
        const stop = control(player.transport, "transportStop");
        compare(previous.enabled, false);
        compare(stop.enabled, false);
        mouseClick(control(player.transport, "transportPlayPause"));
        tryCompare(HearthController, "state", "playing", 10000);
        tryVerify(function() { return next.enabled && stop.enabled; }, 5000);

        mouseClick(next);
        tryCompare(HearthController, "currentIndex", 1, 10000);
        tryVerify(function() { return HearthController.queue[1].current === true; }, 5000);
        tryCompare(HearthController, "state", "playing", 10000);
        tryVerify(function() { return previous.enabled && !next.enabled; }, 5000,
                  "Previous/Next enabled states did not follow the last item");

        mouseClick(previous);
        tryCompare(HearthController, "currentIndex", 0, 10000);

        mouseClick(stop);
        tryCompare(HearthController, "state", "stopped", 10000);
        tryVerify(function() { return !stop.enabled; }, 5000);
    }

    function test_clickingAQueueRowPlaysThatItem() {
        const player = makePlayer();
        queueBoth(player);
        const queueList = control(player.page, "queueList");
        tryCompare(queueList, "count", 2, 5000);
        // Row 1's own MouseArea (the second delegate), found by its index.
        // The ListView rebuilds its delegates whenever the queue snapshot
        // changes, so a click landing while a row is being replaced is
        // simply made again, as a person would.
        let playing = false;
        tryVerify(function() {
            playing = playing || HearthController.currentIndex === 1;
            if (!playing) {
                const row = H.find(queueList, function(item) { return item.index === 1 && item.modelData !== undefined; });
                if (row !== null) {
                    mouseClick(row);
                }
            }
            return playing;
        }, 10000, "clicking queue row 1 never made it current");
        tryCompare(HearthController, "state", "playing", 10000);
        // The ListView rebuilds its delegates whenever the queue snapshot
        // changes, so the row is looked up afresh rather than held.
        tryVerify(function() {
            const current = H.find(queueList, function(item) { return item.index === 1 && item.modelData !== undefined; });
            return current !== null && H.textItem(current, "playing") !== null;
        }, 5000, "the playing row shows no \"playing\" pill");
    }

    // The end of one item runs straight into the next with the output left
    // open (both are 48 kHz; gapless is on by default) - played at eight
    // times real time so the 2.5 s item ends without waiting it out.
    function test_theQueueAdvancesByItselfAtTheEndOfAnItem() {
        const player = makePlayer();
        queueBoth(player);
        compare(HearthController.gapless, true);
        TestServices.setClockSpeed(8.0);
        const opensBefore = TestServices.device().opens;
        mouseClick(control(player.transport, "transportPlayPause"));
        tryCompare(HearthController, "state", "playing", 10000);
        tryCompare(HearthController, "currentIndex", 1, 15000);
        tryCompare(HearthController, "state", "playing", 10000);
        // One output open across the join: gapless, not a reopen.
        compare(TestServices.device().opens, opensBefore + 1);
    }

    function test_gaplessToggleInTheTransportBar() {
        const player = makePlayer();
        const gapless = control(player.transport, "transportGapless");
        const before = HearthController.gapless;
        mouseClick(gapless);
        tryCompare(HearthController, "gapless", !before, 5000);
        compare(gapless.Accessible.checked, !before);
        // Keyboard too: it takes focus and Space flips it back. (An earlier
        // case's file dialog window may have been the focus window; key
        // events only reach an item in the active one.)
        testCase.parent.Window.window.requestActivate();
        tryVerify(function() { return testCase.parent.Window.window.active; }, 5000);
        gapless.forceActiveFocus();
        tryVerify(function() { return gapless.activeFocus; }, 5000);
        keyClick(Qt.Key_Space);
        tryCompare(HearthController, "gapless", before, 5000);
    }

    // The volume slider is -60..0 dB; dragging it to the far left sets the
    // engine's master volume, the readout follows, and what reaches the
    // device is that much quieter.
    function test_volumeSliderTurnsTheDeviceDown() {
        const player = makePlayer();
        const b = TestServices.stageFixture(longEac3, "Volume.ec3");
        verify(TestServices.dropFiles(player.page, [b]));
        tryVerify(function() { return HearthController.queue.length === 1; }, 10000);
        mouseClick(control(player.transport, "transportPlayPause"));
        tryCompare(HearthController, "state", "playing", 10000);
        TestServices.resetPeak();
        tryVerify(function() { return TestServices.device().peak > 0.01; }, 10000);
        const loud = TestServices.device().peak;

        const volume = control(player.transport, "transportVolume");
        mouseDrag(volume, volume.width - 2, volume.height / 2, -volume.width, 0);
        tryVerify(function() { return HearthController.volumeDb <= -50; }, 5000,
                  "volumeDb " + HearthController.volumeDb + " after dragging the slider left");
        tryVerify(function() {
            return control(player.transport, "transportVolumeReadout").text
                   === HearthController.volumeDb.toFixed(1) + " dB";
        }, 5000);
        // Blocks rendered before the change may still be on their way; keep
        // resetting until one full reading comes back quiet. (Latched:
        // tryVerify() calls the function once more after it first holds, and
        // that call must not see a peak it has just reset.)
        let quiet = false;
        tryVerify(function() {
            const peak = TestServices.device().peak;
            TestServices.resetPeak();
            quiet = quiet || (peak > 0 && peak < loud * 0.05);
            return quiet;
        }, 10000, "the device was not handed quieter audio (loud " + loud + ")");
    }
}
