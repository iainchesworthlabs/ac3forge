#pragma once

#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>

#include <tuple>
#include <utility>

// Where a GUI controller's background work runs, and the promise that makes it
// safe to start that work with a captured `this`: no job outlives the
// controller that started it.
//
// QcController, StreamPlayerController, ObjectDecodeController and
// EncoderController each hand a file to a worker so the window stays
// responsive, and each worker ends the same way, with a
// QMetaObject::invokeMethod(this, ...) back to the GUI thread carrying what it
// found. A controller that has already been destroyed cannot take that call.
// invokeMethod on a dead QObject reads freed memory to find the object's
// thread, and where it gets as far as posting it leaves a queued call
// addressed to the freed object in the GUI thread's event list, which
// ~QCoreApplication then walks and dereferences
// (QCoreApplicationPrivate::cleanupThreadData). In the QML suites that is an
// access violation while the process tears down, after every test case has
// passed. In the shipped window it is the same crash on quitting while a file
// is still being decoded. The workers used to run on the global thread pool
// with their futures thrown away, so there was nothing to wait on.
//
// A BackgroundJobs owns its pool, so wait() covers this controller's jobs and
// no one else's. The controller's destructor asks each job that would keep
// running to stop, through a flag the job already polls, and then calls wait()
// before any member is destroyed. From that point every job has returned, so
// none can read a destroyed member or post to a destroyed object; a call a
// finished job queued to the still-living controller is discarded by ~QObject
// with everything else addressed to it. This class's own destructor waits as
// well, but only as a backstop: a member declared after the BackgroundJobs one
// is already gone by then.
//
// wait() returns when the jobs do, so a job with no way to stop holds it up.
// The loops that run for as long as the audio does or as long as an encode
// does each poll a flag for that reason. A job that is one call into a library
// has none; see EncoderController::encodeAc4, which quitting mid-encode waits
// for, as ~QCoreApplication's wait on the global pool always did.
namespace forge_gui {

class BackgroundJobs {
   public:
    ~BackgroundJobs() { wait(); }

    // Runs `job` on this object's pool. The result is discarded: a job reports
    // through a queued call to its controller, not through a future.
    template <typename Job>
    void run(Job&& job) {
        std::ignore = QtConcurrent::run(&pool_, std::forward<Job>(job));
    }

    // Blocks until every job started so far, running or queued, has returned.
    void wait() { pool_.waitForDone(); }

   private:
    QThreadPool pool_;
};

}  // namespace forge_gui
