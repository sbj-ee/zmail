// 1.1.0: large messages are parsed, sanitized and prepared off the UI thread.
// offthread::run itself, and MessageView's async body with its stale-result
// guard. (The sync engines' async parse is covered by running tst_imap and
// tst_sync again with ZMAIL_OFFTHREAD_BYTES=0; see CMakeLists.)
#include "core/OffThread.h"
#include "ui/MessageView.h"
#include "ui/SafeHtmlView.h"

#include <QApplication>
#include <QSignalSpy>
#include <QThread>
#include <QtTest>

#include <memory>

using namespace zmail;

namespace {
ui::ViewMessage bigMessage(const QString &id, const QString &marker)
{
    ui::ViewMessage m;
    m.id = id;
    m.from = QStringLiteral("Ada <ada@example.com>");
    m.subject = QStringLiteral("Big %1").arg(marker);
    QString html = QStringLiteral("<html><body><p>%1</p>").arg(marker);
    for (int i = 0; i < 400; ++i) {
        html += QStringLiteral("<table><tr><td style='color:#333'>row %1 of filler text</td></tr></table>").arg(i);
    }
    m.bodyHtml = html + QStringLiteral("</body></html>");
    return m;
}
} // namespace

class TestOffThread : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        // Anything over 4 KB counts as large here (the real threshold is 256 KB).
        qputenv("ZMAIL_OFFTHREAD_BYTES", "4096");
        QCOMPARE(offthread::thresholdBytes(), qsizetype(4096));
    }

    void smallRunsInline()
    {
        const int before = offthread::jobsStarted();
        QObject ctx;
        QThread *ran = nullptr;
        int got = 0;
        offthread::run(&ctx, false, [&ran] { ran = QThread::currentThread(); return 7; }, [&got](int v) { got = v; });
        QCOMPARE(got, 7); // synchronous
        QCOMPARE(ran, QThread::currentThread());
        QCOMPARE(int(offthread::jobsStarted()), before);
    }

    void largeRunsOnPoolAndReturnsOnUiThread()
    {
        const int before = offthread::jobsStarted();
        QObject ctx;
        std::atomic<QThread *> ran{nullptr};
        QThread *doneOn = nullptr;
        int got = 0;
        offthread::run(&ctx, true, [&ran] { ran = QThread::currentThread(); return 42; },
                       [&](int v) { got = v; doneOn = QThread::currentThread(); });
        QCOMPARE(got, 0); // not yet: it comes back through the event loop
        QTRY_COMPARE(got, 42);
        QVERIFY(ran.load() != nullptr);
        QVERIFY(ran.load() != QThread::currentThread());
        QCOMPARE(doneOn, QThread::currentThread());
        QCOMPARE(int(offthread::jobsStarted()), before + 1);
    }

    void destroyedContextGetsNothing()
    {
        bool called = false;
        std::atomic<bool> release{false};
        {
            auto ctx = std::make_unique<QObject>();
            offthread::run(ctx.get(), true, [&release] {
                while (!release) {
                    QThread::msleep(1);
                }
                return 1;
            }, [&called](int) { called = true; });
        } // context gone while the job still runs
        release = true;
        QThreadPool::globalInstance()->waitForDone();
        QTest::qWait(50);
        QVERIFY(!called);
    }

    void largeBodyPreparedOffThread()
    {
        ui::MessageView v;
        v.resize(800, 600);
        v.show();
        const int before = offthread::jobsStarted();
        QSignalSpy prepared(&v, &ui::MessageView::bodyPrepared);
        v.setMessage(bigMessage(QStringLiteral("m1"), QStringLiteral("FIRSTMARKER")));
        QVERIFY(v.preparingBody());
        QVERIFY(v.body()->toPlainText().contains(QStringLiteral("Preparing message")));
        QVERIFY(prepared.wait(10000));
        QVERIFY(!v.preparingBody());
        QVERIFY(v.body()->toPlainText().contains(QStringLiteral("FIRSTMARKER")));
        QCOMPARE(int(offthread::jobsStarted()), before + 1);
        // A re-render (zoom) reuses the prepared body: no second job.
        v.zoomIn();
        QVERIFY(!v.preparingBody());
        QVERIFY(v.body()->toPlainText().contains(QStringLiteral("FIRSTMARKER")));
        QCOMPARE(int(offthread::jobsStarted()), before + 1);
    }

    void staleResultIsDropped()
    {
        ui::MessageView v;
        v.resize(800, 600);
        v.show();
        QSignalSpy prepared(&v, &ui::MessageView::bodyPrepared);
        v.setMessage(bigMessage(QStringLiteral("a"), QStringLiteral("STALEMARKER")));
        v.setMessage(bigMessage(QStringLiteral("b"), QStringLiteral("FRESHMARKER"))); // selection moved on
        QVERIFY(prepared.wait(10000));
        QThreadPool::globalInstance()->waitForDone(); // the first job has finished too
        QTest::qWait(100);
        QCOMPARE(prepared.count(), 1); // only b's result was shown
        QCOMPARE(v.message().id, QStringLiteral("b"));
        QVERIFY(v.body()->toPlainText().contains(QStringLiteral("FRESHMARKER")));
        QVERIFY(!v.body()->toPlainText().contains(QStringLiteral("STALEMARKER")));

        // Cleared while preparing: nothing comes back to an empty view.
        v.setMessage(bigMessage(QStringLiteral("c"), QStringLiteral("GONEMARKER")));
        v.clear();
        QThreadPool::globalInstance()->waitForDone();
        QTest::qWait(100);
        QCOMPARE(prepared.count(), 1);
        QVERIFY(!v.body()->toPlainText().contains(QStringLiteral("GONEMARKER")));
    }

    void smallBodyStaysSynchronous()
    {
        ui::MessageView v;
        v.resize(800, 600);
        const int before = offthread::jobsStarted();
        ui::ViewMessage m;
        m.id = QStringLiteral("s");
        m.bodyHtml = QStringLiteral("<p>SMALLMARKER</p>");
        v.setMessage(m);
        QVERIFY(!v.preparingBody());
        QVERIFY(v.body()->toPlainText().contains(QStringLiteral("SMALLMARKER")));
        QCOMPARE(int(offthread::jobsStarted()), before);
    }
};

QTEST_MAIN(TestOffThread)
#include "tst_offthread.moc"
