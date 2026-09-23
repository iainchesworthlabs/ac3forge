import QtQuick
import QtTest

import Ac3ForgeHearth

// PlayPage.qml's queue interactions (issue #886): HearthController is a
// QML_SINGLETON shared by every test_* function in this file's one process,
// and Qt Quick Test runs test_* functions in ALPHABETICAL order, not
// declaration order - so every test here reads its OWN starting queue
// length before asserting, rather than assuming the queue starts empty, and
// removes whatever it added before returning, the same discipline
// apps/gui/tests/qml/tst_guided_wizard.qml's own header comment (see
// [[gui-qml-test-function-alphabetical-order]]) had to add after the fact.
//
// addFiles()/addFolder() do not need the paths they are given to resolve to
// real, decodable audio: queue_row() (hearth_controller.cpp) reflects a
// freshly-added QueueItem's path/title immediately, before any probe has
// run - probing is what would later mark an item unplayable, and this file
// does not depend on that timing either way. Every assertion after a write
// still goes through tryVerify(), never a bare compare() - HearthController.queue
// is only republished by poll(), the same asynchronous shape
// tst_decoder_settings.qml's own header note describes.
TestCase {
    id: testCase
    name: "PlayQueue"
    when: windowShown
    width: 960
    height: 620

    Component { id: playPageComponent; PlayPage { width: 960; height: 620 } }

    function init() {
        HearthController.start();
    }

    // Removes every queue row whose path is in `paths`, by PATH rather than
    // a remembered index: another test's own additions may still be sitting
    // earlier in the queue (alphabetical order, not declaration order - see
    // this file's header), so an index captured at add time can be stale by
    // the time cleanup runs. Takes the WHOLE list in one call rather than
    // one path at a time: HearthController.removeAt() is asynchronous
    // (posted to the engine, only visible once poll() republishes queue -
    // this file's own header note), so two separate calls each reading
    // HearthController.queue back to back both see the SAME pre-removal
    // snapshot - the second call's own index is then wrong once the first
    // removal actually lands, and removes whatever ends up sitting there
    // instead (or nothing, if that leaves the index out of range). Reading
    // one snapshot and removing every match from it, highest index first,
    // is what keeps each removeAt() call's own index valid against every
    // removal still ahead of it in the same pass.
    // The exact bug PlayPage.qml's Add files/Add folder dialogs and drag-drop
    // hit (issue tracked in the PR this test landed with): a FileDialog/
    // FolderDialog's own `url` value (selectedFiles/selectedFolder) or a
    // DropArea drop.urls entry has no toLocalFile() of its own once QML
    // hands it to JavaScript - only a real C++ QUrl does, which is what
    // marshalling it through urlToLocalFile()'s own QUrl parameter produces.
    // This is deliberately a unit test of that conversion, not the real
    // dialogs themselves: Qt Quick Dialogs' native FileDialog/FolderDialog
    // populate selectedFiles/selectedFolder from the OS's own picker, which
    // cannot be driven headlessly under -platform offscreen - exactly the
    // gap that let the original bug ship untested. Qt.resolvedUrl() gives a
    // real `url`-typed value here (resolved against this file's own file://
    // location), the same shape QML hands the invokable from a real dialog.
    function test_urlToLocalFileConvertsAUrlWithoutThrowing() {
        const fileName = testCase.name + "-urlToLocalFile.ac3";
        const url = Qt.resolvedUrl(fileName);
        const path = HearthController.urlToLocalFile(url);
        verify(path.length > 0, "urlToLocalFile() returned an empty path for " + url);
        verify(path.indexOf(fileName) >= 0,
               "urlToLocalFile() did not return the expected file name: " + path);
    }

    function removeAllWithPaths(paths) {
        for (let i = HearthController.queue.length - 1; i >= 0; --i) {
            if (paths.indexOf(HearthController.queue[i].path) >= 0) {
                HearthController.removeAt(i);
            }
        }
    }

    function test_addFilesAppendsRowsWithTitleFromTheFileName() {
        const startCount = HearthController.queue.length;
        const path1 = testCase.name + "-addFiles-1.ec3";
        const path2 = testCase.name + "-addFiles-2.ac3";

        HearthController.addFiles([path1, path2]);
        tryVerify(function() { return HearthController.queue.length === startCount + 2; }, 15000,
                  "addFiles() never grew the queue by 2");

        const row1 = HearthController.queue.find(function(item) { return item.path === path1; });
        const row2 = HearthController.queue.find(function(item) { return item.path === path2; });
        verify(row1 !== undefined, "no queue row for " + path1);
        verify(row2 !== undefined, "no queue row for " + path2);
        compare(row1.title, path1);
        compare(row2.title, path2);
        // Freshly added, not yet probed: playable() reads
        // facts.unplayable_because.empty(), true until a probe says
        // otherwise (queue_row()'s own comment).
        compare(row1.playable, true);
        compare(row2.playable, true);
        // current is deliberately NOT asserted here: Queue::add() (apps/hearth/engine/queue.cpp)
        // auto-selects the first item added to a previously-empty queue as current - real,
        // correct engine behaviour, not something this file's own title-from-filename test needs
        // to characterise (tests/hearth/test_queue.cpp already covers Queue::add()'s own
        // selection rule), and this test does not control whether the queue was empty before it
        // ran (another test's own leftover state, alphabetical order - see this file's header).

        removeAllWithPaths([path1, path2]);
        tryVerify(function() { return HearthController.queue.length === startCount; }, 15000);
    }

    // tryCompare()'s expected-value argument is read ONCE, when tryCompare
    // itself is called - not a live expression it re-evaluates every poll.
    // Right after addFiles()/removeAt(), HearthController.queue.length has
    // not caught up yet either (the same asynchronous poll() shape this
    // file's header describes), so passing it directly as the expected
    // value captures the STALE pre-change count and the call is satisfied
    // immediately, before queueList has actually caught up.
    //
    // queueList.count and HearthController.queue.length are bound together
    // (ListView model: HearthController.queue) and so always agree with
    // EACH OTHER on every poll tick, whether or not either has caught up
    // with a pending add/remove yet - confirmed the hard way: comparing
    // them to each other alone is trivially, immediately true regardless of
    // whether the change actually landed, which let a still-pending
    // removeAt() leak into the next test's own starting count. Checking
    // both against a baseline CAPTURED BEFORE the mutation (targetCount)
    // is what actually waits for the real change, the same "capture a
    // fixed target, then poll for it" shape every other tryVerify() in
    // this file already uses.
    function queueListMatches(queueList, targetCount) {
        return queueList.count === targetCount && HearthController.queue.length === targetCount;
    }

    function test_addFilesReflectedInThePageQueueList() {
        const page = createTemporaryObject(playPageComponent, testCase.parent);
        verify(page !== null);
        waitForRendering(page);
        const queueList = findChild(page, "queueList");
        verify(queueList !== null, "PlayPage carries no queueList child");
        const startCount = HearthController.queue.length;
        tryVerify(function() { return queueListMatches(queueList, startCount); }, 15000);

        const path = testCase.name + "-queueList.ec3";
        HearthController.addFiles([path]);
        tryVerify(function() { return queueListMatches(queueList, startCount + 1); }, 15000,
                  "queueList never reflected the added file");

        removeAllWithPaths([path]);
        tryVerify(function() { return queueListMatches(queueList, startCount); }, 15000,
                  "queueList never reflected the removal");
    }

    function test_playItemChangesCurrentIndex() {
        const startCount = HearthController.queue.length;
        const path = testCase.name + "-playItem.ec3";
        HearthController.addFiles([path]);
        tryVerify(function() { return HearthController.queue.length === startCount + 1; }, 15000);
        const index = HearthController.queue.findIndex(function(item) { return item.path === path; });
        verify(index >= 0);

        HearthController.playItem(index);
        tryVerify(function() { return HearthController.currentIndex === index; }, 15000,
                  "playItem() never moved currentIndex to " + index);
        tryVerify(function() { return HearthController.queue[index] !== undefined &&
                                       HearthController.queue[index].current === true; }, 15000);

        // Waited out, unlike an earlier draft of this test: a cleanup that
        // returns before its own removeAt() has actually landed leaves the
        // NEXT test's startCount (captured before this one's pending
        // removal resolves) too high by one, and whichever count that
        // removal actually lands against next reads as a mismatch there
        // instead - found the hard way as an apparently-flaky failure one
        // test further down the file, reliable under ctest's slower
        // back-to-back process startup and absent running this file alone.
        removeAllWithPaths([path]);
        tryVerify(function() { return HearthController.queue.length === startCount; }, 15000);
    }

    function test_removeAtShrinksTheQueue() {
        const startCount = HearthController.queue.length;
        const path1 = testCase.name + "-removeAt-1.ec3";
        const path2 = testCase.name + "-removeAt-2.ec3";
        HearthController.addFiles([path1, path2]);
        tryVerify(function() { return HearthController.queue.length === startCount + 2; }, 15000);

        const index1 = HearthController.queue.findIndex(function(item) { return item.path === path1; });
        verify(index1 >= 0);
        HearthController.removeAt(index1);
        tryVerify(function() { return HearthController.queue.length === startCount + 1; }, 15000);
        verify(HearthController.queue.find(function(item) { return item.path === path1; }) === undefined,
               "removeAt() left the removed row in the queue");
        verify(HearthController.queue.find(function(item) { return item.path === path2; }) !== undefined,
               "removeAt() removed the wrong row");

        removeAllWithPaths([path2]);
        tryVerify(function() { return HearthController.queue.length === startCount; }, 15000);
    }
}
