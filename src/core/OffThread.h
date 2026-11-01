#pragma once

// Heavy, pure work (MIME parsing, HTML sanitizing, HtmlFit's string passes)
// for large messages runs on Qt's global thread pool; its result comes back
// on the thread of a context object (the UI thread), never after that object
// is gone. Small messages run inline, exactly as before, so the common case
// stays synchronous. SQLite and widgets are only ever touched in `done`.

#include <QFutureWatcher>
#include <QObject>
#include <QtConcurrent/QtConcurrentRun>

#include <atomic>
#include <type_traits>
#include <utility>

namespace zmail::offthread {

// Messages at or above this many bytes are processed off the UI thread.
inline constexpr qsizetype kDefaultThresholdBytes = 256 * 1024;

// kDefaultThresholdBytes, or ZMAIL_OFFTHREAD_BYTES (tests) when set.
qsizetype thresholdBytes();
inline bool isLarge(qsizetype bytes)
{
    return bytes >= thresholdBytes();
}

// Jobs sent to the pool so far (tests).
std::atomic<int> &jobsStarted();

// Run `work()` (which must not touch widgets, SQLite or `context`) on the pool
// when `offThread`, else right here; then `done(result)` on `context`'s
// thread. If `context` is destroyed first, `done` is never called.
template <typename Work, typename Done>
void run(QObject *context, bool offThread, Work work, Done done)
{
    using T = std::invoke_result_t<Work>;
    if (!offThread) {
        done(work());
        return;
    }
    ++jobsStarted();
    auto *watcher = new QFutureWatcher<T>(context);
    QObject::connect(watcher, &QFutureWatcherBase::finished, context,
                     [watcher, done = std::move(done)]() mutable {
                         watcher->deleteLater();
                         done(watcher->result());
                     });
    watcher->setFuture(QtConcurrent::run(std::move(work)));
}

} // namespace zmail::offthread
