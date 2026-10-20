// Right-click Junk / Suspicious > Empty Junk…: every message in Spam goes to
// Trash (what Delete does everywhere in zmail; nothing is erased), in batches
// of 200, one at a time, cancellable, with a count from the server first.
//  - ImapSyncEngine against the fake IMAP server: count, batching, Trash not
//    EXPUNGE, a throttle waited out / given up on, partial failure, Stop
//    (between batches and during a throttle wait), a lost connection
//  - GmailSyncEngine against the mock Gmail API: count, batching, 429 backoff,
//    partial failure, Stop
//  - the window: the menu item, the confirmation (count, Trash wording,
//    Cancel by default), the progress bar and Stop, the result
// Fake servers and made-up passwords only: no real mail is touched.
#include "MainWindow.hpp"
#include "core/AppPasswordAccount.h"
#include "core/AuthManager.h"
#include "core/GmailClient.h"
#include "core/GmailSyncEngine.h"
#include "core/ImapSyncEngine.h"
#include "core/MailCache.h"
#include "core/MailSession.h"
#include "core/ServerLimits.h"
#include "core/TokenStore.h"
#include "mock/FakeImapServer.h"
#include "mock/MockGoogle.h"
#include "ui/EmptyJunkPrompt.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTest>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>

using namespace zmail;
using zmail::test::FakeImapServer;
using zmail::test::MockGoogle;

namespace {

QByteArray mail(const QString &subject, const char *from = "Crypto Alerts <alerts@coin-boost.example>")
{
    return QByteArray("From: ") + from + "\r\nTo: Alex Morgan <alex.morgan@example.com>\r\nSubject: " + subject.toUtf8()
           + "\r\nDate: Mon, 12 Oct 2026 22:40:00 -0500\r\nMessage-ID: <" + subject.toUtf8().toHex().left(24)
           + "@mail.test>\r\nContent-Type: text/plain; charset=utf-8\r\n\r\nHello.\r\n";
}

AppPasswordAccount accountFor(const FakeImapServer &server)
{
    AppPasswordAccount a;
    a.email = server.user;
    a.imapHost = QStringLiteral("127.0.0.1");
    a.imapPort = server.port();
    a.smtpHost = QStringLiteral("127.0.0.1");
    a.smtpPort = 1;
    return a;
}

// The engine on its own, over IMAP.
struct ImapRig
{
    FakeImapServer server;
    MailCache cache;
    std::unique_ptr<ImapSyncEngine> sync;
    QList<quint64> spam, inbox;

    explicit ImapRig(int spamCount, bool start = true)
    {
        for (int i = 0; i < 3; ++i) {
            inbox << server.deliver(mail(QStringLiteral("Panel schedule %1").arg(i), "Dana Whitfield <dana@lakeshore-electric.example>"));
        }
        for (int i = 0; i < spamCount; ++i) {
            spam << server.deliver(mail(QStringLiteral("Double your balance %1").arg(i)), {QStringLiteral("\\Spam")});
        }
        cache.open(QStringLiteral(":memory:"));
        sync = std::make_unique<ImapSyncEngine>(
            accountFor(server),
            [this](std::function<void(bool, const QString &, const QString &)> cb) { cb(true, server.password, {}); }, &cache);
        sync->setPlaintextForTests(true);
        sync->setPollInterval(3600 * 1000);
        sync->setBackoffScaleForTests(0.0002); // Gmail's 10 minutes become ~0.1 s
        if (start) {
            QSignalSpy idle(sync.get(), &SyncEngine::idle);
            sync->start();
            if (!idle.wait(10000)) {
                qFatal("no initial sync");
            }
        }
    }
    int inSpam() const { return count(QStringLiteral("\\Spam")); }
    int inTrash() const { return count(QStringLiteral("\\Trash")); }
    int count(const QString &label) const
    {
        int n = 0;
        for (const quint64 id : spam) {
            n += const_cast<FakeImapServer &>(server).find(id)->labels.contains(label) ? 1 : 0;
        }
        return n;
    }
    QList<QByteArray> moves() const
    {
        QList<QByteArray> out;
        for (const QByteArray &c : server.commands) {
            if (c.startsWith("UID MOVE")) {
                out << c;
            }
        }
        return out;
    }
    bool settle() { return QTest::qWaitFor([this] { return !sync->isBusy() && server.anyIdling(); }, 15000); }
};

QStringList progressValues(const QSignalSpy &spy)
{
    QStringList out;
    for (const QList<QVariant> &a : spy) {
        out << QStringLiteral("%1/%2").arg(a.at(0).toInt()).arg(a.at(1).toInt());
    }
    return out;
}

// The engine on its own, over the Gmail API (mock).
struct ApiRig
{
    MockGoogle g;
    MemoryTokenStore store;
    QNetworkAccessManager nam;
    std::unique_ptr<AuthManager> auth;
    std::unique_ptr<GmailClient> api;
    MailCache cache;
    std::unique_ptr<GmailSyncEngine> sync;
    QStringList spam;

    explicit ApiRig(int spamCount)
    {
        g.listen();
        store.values.insert(QStringLiteral("refresh-token:") + g.email, QString::fromLatin1(MockGoogle::kRefreshToken));
        auth = std::make_unique<AuthManager>(g.clientConfig(), &store, &nam);
        bool restored = false;
        auth->restore(g.email, [&restored](bool, const QString &) { restored = true; });
        (void)QTest::qWaitFor([&restored] { return restored; }, 5000);
        api = std::make_unique<GmailClient>(auth.get(), &nam, g.apiBase());
        api->setBackoffBaseMs(5);
        cache.open(QStringLiteral(":memory:"));
        sync = std::make_unique<GmailSyncEngine>(api.get(), &cache);
        sync->setInitialCount(50);
        sync->setPollInterval(3600 * 1000);
        g.seedSystemLabels();
        for (int i = 0; i < 3; ++i) {
            g.addMessage(msg(QStringLiteral("Panel schedule %1").arg(i), {QStringLiteral("INBOX")}), false);
        }
        for (int i = 0; i < spamCount; ++i) {
            spam << g.addMessage(msg(QStringLiteral("Double your balance %1").arg(i), {QStringLiteral("SPAM")}), false);
        }
        sync->start();
        (void)QTest::qWaitFor([this] { return !sync->isBusy() && cache.historyId() > 0; }, 30000);
    }
    static MockGoogle::Message msg(const QString &subject, const QStringList &labels)
    {
        MockGoogle::Message m;
        m.from = QStringLiteral("Crypto Alerts <alerts@coin-boost.example>");
        m.to = QStringLiteral("demo.user@example.com");
        m.subject = subject;
        m.text = QStringLiteral("Body of ") + subject;
        m.labels = labels;
        m.date = QDateTime::currentDateTimeUtc();
        return m;
    }
    int inSpam() const { return count(QStringLiteral("SPAM")); }
    int inTrash() const { return count(QStringLiteral("TRASH")); }
    int count(const QString &label) const
    {
        int n = 0;
        for (const QString &id : spam) {
            n += g.messages().value(id).labels.contains(label) ? 1 : 0;
        }
        return n;
    }
    int batchCalls() const
    {
        int n = 0;
        for (const QString &r : g.requests) {
            n += r.contains(QLatin1String("/messages/batchModify")) && r.startsWith(QLatin1String("POST")) ? 1 : 0;
        }
        return n;
    }
};

// The window on an IMAP account.
struct UiRig
{
    FakeImapServer imap;
    MemoryTokenStore store;
    std::unique_ptr<MailSession> session;
    std::unique_ptr<MainWindow> w;
    QTreeWidget *tree = nullptr;
    QList<quint64> spam;

    explicit UiRig(int spamCount)
    {
        imap.deliver(mail(QStringLiteral("Panel schedule for the Oak St job"), "Dana Whitfield <dana.whitfield@lakeshore-electric.example>"));
        imap.deliver(mail(QStringLiteral("Lunch on Thursday?"), "Sam Ortiz <sam.ortiz@example.org>"));
        imap.deliver(mail(QStringLiteral("Your October statement is ready"), "Northside Credit Union <statements@northside-cu.example>"));
        for (int i = 0; i < spamCount; ++i) {
            spam << imap.deliver(mail(i == 0 ? QStringLiteral("Double your balance today")
                                      : i < 4 ? QStringList{QStringLiteral("You have been selected"), QStringLiteral("Final notice: claim your prize"),
                                                            QStringLiteral("Cheap meds, no prescription")}.at(i - 1)
                                              : QStringLiteral("Prize notice %1").arg(i),
                                      i % 2 ? "Prize Desk <winner@prize-desk.example>" : "Crypto Alerts <alerts@coin-boost.example>"),
                                 {QStringLiteral("\\Spam")});
        }
        store.values.insert(AppPasswordAccount::appPasswordKey(imap.user), imap.password);
        QSettings s;
        s.clear();
        accountFor(imap).save(s);
        SessionOptions o;
        o.client.status = ClientConfig::LoadStatus::Missing;
        o.store = &store;
        o.cachePathOverride = QStringLiteral(":memory:");
        o.contactsPathOverride = QStringLiteral(":memory:");
        o.imapPlaintextForTests = true;
        o.pollIntervalMs = 3600 * 1000;
        session = std::make_unique<MailSession>(o);
        w = std::make_unique<MainWindow>();
        w->resize(1100, 700);
        w->setSession(session.get());
        session->restoreSaved();
        w->show();
        tree = w->findChild<QTreeWidget *>(QStringLiteral("mailboxTree"));
    }
    ~UiRig()
    {
        w.reset();
        session.reset();
        QSettings().clear();
    }
    ImapSyncEngine *engine() { return qobject_cast<ImapSyncEngine *>(session->sync()); }
    QTreeWidgetItem *item(const QString &key) const
    {
        for (QTreeWidgetItemIterator it(tree); *it; ++it) {
            if ((*it)->data(0, Qt::UserRole).toString() == key) {
                return *it;
            }
        }
        return nullptr;
    }
    QString total(const QString &key) const
    {
        return item(key) ? item(key)->text(1).section(QLatin1Char('/'), -1).trimmed() : QStringLiteral("<none>");
    }
    bool ready()
    {
        return QTest::qWaitFor([this] { return w->isLive() && item(QStringLiteral("Junk")) && session->sync() && !session->sync()->isBusy(); },
                               20000);
    }
    int inSpam() const
    {
        int n = 0;
        for (const quint64 id : spam) {
            n += const_cast<FakeImapServer &>(imap).find(id)->labels.contains(QStringLiteral("\\Spam")) ? 1 : 0;
        }
        return n;
    }
    int inTrash() const
    {
        int n = 0;
        for (const quint64 id : spam) {
            n += const_cast<FakeImapServer &>(imap).find(id)->labels.contains(QStringLiteral("\\Trash")) ? 1 : 0;
        }
        return n;
    }
    // Right-click a sidebar row: the menu, left open (caller closes it).
    QMenu *menuFor(const QString &key)
    {
        QTreeWidgetItem *row = item(key);
        if (!row) {
            return nullptr;
        }
        tree->scrollToItem(row);
        emit tree->customContextMenuRequested(tree->visualItemRect(row).center());
        QMenu *menu = nullptr;
        (void)QTest::qWaitFor([&] {
            for (QWidget *x : QApplication::topLevelWidgets()) {
                if (auto *m = qobject_cast<QMenu *>(x); m && m->isVisible() && m->objectName() == QLatin1String("mailboxMenu")) {
                    menu = m;
                }
            }
            return menu != nullptr;
        }, 3000);
        return menu;
    }
    static QStringList names(QMenu *m)
    {
        QStringList out;
        for (QAction *a : m->actions()) {
            out << a->objectName();
        }
        return out;
    }
    QMessageBox *dialog(const char *name)
    {
        QMessageBox *box = nullptr;
        (void)QTest::qWaitFor([&] {
            for (QWidget *x : QApplication::topLevelWidgets()) {
                if (auto *m = qobject_cast<QMessageBox *>(x); m && m->isVisible() && m->objectName() == QLatin1String(name)) {
                    box = m;
                }
            }
            return box != nullptr;
        }, 5000);
        return box;
    }
    // Screenshots only when asked for (ZMAIL_SHOT_DIR): the whole screen, so
    // popups and dialogs are in it. Needs a real (xcb) platform to show them.
    void shot(const char *name, QWidget *around = nullptr)
    {
        const QString dir = qEnvironmentVariable("ZMAIL_SHOT_DIR");
        if (dir.isEmpty()) {
            return;
        }
        QTest::qWait(400);
        QPixmap p = QApplication::primaryScreen()->grabWindow(0);
        if (around && QGuiApplication::platformName() == QLatin1String("offscreen")) {
            p = around->grab();
        }
        p.save(dir + QLatin1Char('/') + QLatin1String(name) + QStringLiteral(".png"));
    }
};

} // namespace

class TstEmptyJunk : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    // ---- wording -------------------------------------------------------

    void promptSaysTrashAndTheCount()
    {
        using P = zmail::ui::EmptyJunkPrompt;
        const QString exact = P::question(1234, true, {});
        QVERIFY2(exact.contains(QLocale().toString(1234)), qPrintable(exact));
        QVERIFY(exact.contains(QStringLiteral("Trash")));
        QVERIFY(!exact.contains(QStringLiteral("about")));
        QVERIFY(P::question(1, true, {}).contains(QStringLiteral("the 1 message")));
        const QString approx = P::question(57, false, QStringLiteral("HTTP 503"));
        QVERIFY2(approx.contains(QStringLiteral("about 57")) && approx.contains(QStringLiteral("out of date"))
                     && approx.contains(QStringLiteral("HTTP 503")),
                 qPrintable(approx));
        const QString detail = P::detail(true);
        QVERIFY(detail.contains(QStringLiteral("Trash")) && detail.contains(QStringLiteral("nothing is erased permanently")));
        QVERIFY(detail.contains(QString::number(SyncEngine::kEmptyJunkBatch)));
        QVERIFY(P::detail(true).contains(QStringLiteral("Gmail's count")));
        QVERIFY(!P::detail(false).contains(QStringLiteral("Gmail's count")));
        QVERIFY(P::result(450, 450, false, {}).contains(QStringLiteral("450 messages moved to Trash")));
        const QString partial = P::result(200, 450, false, QStringLiteral("boom"));
        QVERIFY2(partial.contains(QStringLiteral("200 of 450")) && partial.contains(QStringLiteral("250 still in Junk"))
                     && partial.contains(QStringLiteral("boom")),
                 qPrintable(partial));
        QVERIFY(P::result(200, 450, true, {}).contains(QStringLiteral("Stopped: 200 of 450")));
    }

    // ---- IMAP (app-password account) ------------------------------------

    void imapCountComesFromTheServer()
    {
        ImapRig r(7);
        int count = -1;
        bool exact = false;
        QString why;
        r.sync->countJunk([&](int c, bool e, const QString &w) { count = c; exact = e; why = w; });
        QTRY_VERIFY_WITH_TIMEOUT(count >= 0, 5000);
        QCOMPARE(count, 7);
        QVERIFY(exact);
        QVERIFY(why.isEmpty());
        QVERIFY(r.server.commands.contains(QByteArray("STATUS \"[Gmail]/Spam\" (MESSAGES UNSEEN)")));
        // Spam changes behind zmail's back: the count is the server's, not the cache's.
        r.server.deliver(mail(QStringLiteral("One more")), {QStringLiteral("\\Spam")});
        count = -1;
        r.sync->countJunk([&](int c, bool e, const QString &) { count = c; exact = e; });
        QTRY_COMPARE_WITH_TIMEOUT(count, 8, 5000);
        QVERIFY(r.settle());
    }

    void imapCountIsApproximateWhenTheServerCannotBeAsked()
    {
        ImapRig off(5, /*start=*/false); // never connected
        int count = -1;
        bool exact = true;
        QString why;
        off.sync->countJunk([&](int c, bool e, const QString &w) { count = c; exact = e; why = w; });
        QVERIFY(!exact);
        QVERIFY(!why.isEmpty());
        QCOMPARE(count, 0); // nothing cached yet

        ImapRig r(5);
        r.server.throttleCommands = 1; // the STATUS answers NO [THROTTLED]
        count = -1;
        exact = true;
        r.sync->countJunk([&](int c, bool e, const QString &w) { count = c; exact = e; why = w; });
        QTRY_VERIFY_WITH_TIMEOUT(count >= 0, 5000);
        QVERIFY(!exact);
        QVERIFY(!why.isEmpty()); // Gmail hung up on it (the existing throttle response)
        QCOMPARE(count, 5); // the sidebar's last count (read at connect)
        QVERIFY(r.settle());
    }

    // 450 messages: batches of 200, 200, 50, each one UID MOVE into Trash, and
    // never \Deleted / EXPUNGE: Trash, not erased.
    void imapMovesToTrashInBatchesOf200()
    {
        ImapRig r(450);
        r.sync->ensureLabel(QStringLiteral("SPAM")); // some of it is cached, some is not
        QVERIFY(r.settle());
        QVERIFY(r.cache.count(QStringLiteral("SPAM")) > 0);
        QCOMPARE(r.inSpam(), 450);

        QSignalSpy progress(r.sync.get(), &SyncEngine::junkEmptyProgress);
        QSignalSpy emptied(r.sync.get(), &SyncEngine::junkEmptied);
        r.sync->emptyJunk();
        QVERIFY(r.sync->emptyingJunk());
        QVERIFY(emptied.wait(20000));
        QCOMPARE(progressValues(progress), (QStringList{"0/450", "200/450", "400/450", "450/450"}));
        QCOMPARE(emptied.count(), 1);
        QCOMPARE(emptied.first().at(0).toInt(), 450);
        QCOMPARE(emptied.first().at(1).toInt(), 450);
        QCOMPARE(emptied.first().at(2).toBool(), false);
        QVERIFY(emptied.first().at(3).toString().isEmpty());
        QVERIFY(!r.sync->emptyingJunk());

        const QList<QByteArray> moves = r.moves();
        QCOMPARE(moves.size(), 3);
        for (const QByteArray &m : moves) {
            QVERIFY2(m.endsWith("\"[Gmail]/Trash\""), m.constData());
        }
        for (const QByteArray &c : std::as_const(r.server.commands)) {
            QVERIFY2(!c.contains("EXPUNGE") && !c.contains("\\Deleted"), c.constData());
        }
        QCOMPARE(r.inSpam(), 0);
        QCOMPARE(r.inTrash(), 450);
        for (const quint64 id : std::as_const(r.spam)) { // still there, in Trash
            QCOMPARE(r.server.find(id)->labels, QStringList{QStringLiteral("\\Trash")});
            QVERIFY(!r.server.find(id)->gone);
        }
        for (const quint64 id : std::as_const(r.inbox)) { // untouched
            QVERIFY(r.server.find(id)->labels.contains(QStringLiteral("\\Inbox")));
        }
        // The cache follows, and the counts are the server's.
        for (const QString &id : r.cache.messageIds(QStringLiteral("SPAM"))) {
            QFAIL(qPrintable(QStringLiteral("still cached in SPAM: ") + id));
        }
        QVERIFY(r.cache.count(QStringLiteral("TRASH")) > 0);
        QVERIFY(r.settle());
        int spamTotal = -1, trashTotal = -1;
        for (const CachedLabel &l : r.cache.labels()) {
            if (l.id == QLatin1String("SPAM")) spamTotal = l.total;
            if (l.id == QLatin1String("TRASH")) trashTotal = l.total;
        }
        QCOMPARE(spamTotal, 0);
        QCOMPARE(trashTotal, 450);
    }

    void imapEmptySpamIsANoOp()
    {
        ImapRig r(0);
        QSignalSpy emptied(r.sync.get(), &SyncEngine::junkEmptied);
        r.sync->emptyJunk();
        QVERIFY(emptied.wait(10000));
        QCOMPARE(emptied.first().at(0).toInt(), 0);
        QCOMPARE(emptied.first().at(1).toInt(), 0);
        QVERIFY(emptied.first().at(3).toString().isEmpty());
        QVERIFY(r.moves().isEmpty());
        QVERIFY(r.settle());
    }

    void imapSecondRequestWhileRunningIsIgnored()
    {
        ImapRig r(250);
        QSignalSpy emptied(r.sync.get(), &SyncEngine::junkEmptied);
        r.sync->emptyJunk();
        r.sync->emptyJunk(); // ignored
        QVERIFY(emptied.wait(20000));
        QTest::qWait(300);
        QCOMPARE(emptied.count(), 1);
        QCOMPARE(r.moves().size(), 2); // 200 + 50, once
        QVERIFY(r.settle());
    }

    // NO [THROTTLED] on a batch: the existing response, ImapClient::throttled
    // hangs up and the reconnect waits ServerLimits::backoffMs. The run ends
    // there and says so; what already moved stays moved; asking again after the
    // reconnect finishes the rest.
    void imapThrottleHangsUpAndReports()
    {
        ImapRig r(450);
        QSignalSpy emptied(r.sync.get(), &SyncEngine::junkEmptied);
        bool throttled = false;
        connect(r.sync.get(), &SyncEngine::junkEmptyProgress, r.sync.get(), [&r, &throttled](int done, int) {
            if (done == 200 && !throttled) {
                throttled = true;
                r.server.throttleMoves = 1; // the second batch
            }
        });
        r.sync->emptyJunk();
        QVERIFY(emptied.wait(20000));
        QCOMPARE(emptied.count(), 1);
        QCOMPARE(emptied.first().at(0).toInt(), 200);
        QCOMPARE(emptied.first().at(1).toInt(), 450);
        QCOMPARE(emptied.first().at(2).toBool(), false);
        const QString error = emptied.first().at(3).toString();
        QVERIFY2(error.contains(QStringLiteral("limiting this account")), qPrintable(error));
        QCOMPARE(r.sync->lastLimit(), ServerLimits::Kind::Bandwidth);
        QCOMPARE(r.moves().size(), 2); // not hammered: no resend of the refused batch
        QCOMPARE(r.inTrash(), 200);
        QCOMPARE(r.inSpam(), 250);
        QVERIFY(!r.sync->emptyingJunk());
        QTRY_VERIFY_WITH_TIMEOUT(r.sync->retryInMs() >= 0 || r.sync->isRunning(), 5000); // a reconnect is planned

        // After the backoff (shrunk here) it is back and the rest goes.
        QTRY_VERIFY_WITH_TIMEOUT(r.sync->lastLimit() == ServerLimits::Kind::None && r.settle(), 30000);
        emptied.clear();
        r.sync->emptyJunk();
        QVERIFY(emptied.wait(20000));
        QCOMPARE(emptied.first().at(0).toInt(), 250);
        QCOMPARE(emptied.first().at(1).toInt(), 250);
        QVERIFY(emptied.first().at(3).toString().isEmpty());
        QCOMPARE(r.inSpam(), 0);
        QCOMPARE(r.inTrash(), 450);
        QVERIFY(r.settle());
    }

    // Throttled on the very first batch: nothing moved, nothing lost.
    void imapThrottleOnTheFirstBatch()
    {
        ImapRig r(450);
        QSignalSpy emptied(r.sync.get(), &SyncEngine::junkEmptied);
        r.server.throttleMoves = 1;
        r.sync->emptyJunk();
        QVERIFY(emptied.wait(20000));
        QCOMPARE(emptied.first().at(0).toInt(), 0);
        QCOMPARE(emptied.first().at(1).toInt(), 450);
        QVERIFY(!emptied.first().at(3).toString().isEmpty());
        QCOMPARE(r.inSpam(), 450);
        QCOMPARE(r.moves().size(), 1);
    }

    // An ordinary refusal on the second batch: the first stays moved, the run
    // ends there, and the cache agrees with the server.
    void imapPartialFailureReportsHowFarItGot()
    {
        ImapRig r(450);
        r.sync->ensureLabel(QStringLiteral("SPAM"));
        QVERIFY(r.settle());
        QSignalSpy emptied(r.sync.get(), &SyncEngine::junkEmptied);
        r.server.failMovesAfter = 1;
        r.sync->emptyJunk();
        QVERIFY(emptied.wait(20000));
        QCOMPARE(emptied.first().at(0).toInt(), 200);
        QCOMPARE(emptied.first().at(1).toInt(), 450);
        QCOMPARE(emptied.first().at(2).toBool(), false);
        const QString error = emptied.first().at(3).toString();
        QVERIFY2(error.contains(QStringLiteral("refused")), qPrintable(error));
        QCOMPARE(r.moves().size(), 2); // no retry of a plain refusal, no third batch
        QCOMPARE(r.inTrash(), 200);
        QCOMPARE(r.inSpam(), 250);
        QVERIFY(r.settle());
        // What the cache says is in Trash is in Trash on the server.
        for (const QString &id : r.cache.messageIds(QStringLiteral("TRASH"))) {
            QVERIFY(r.server.find(id.toULongLong(nullptr, 16))->labels.contains(QStringLiteral("\\Trash")));
        }
        for (const QString &id : r.cache.messageIds(QStringLiteral("SPAM"))) {
            QVERIFY(r.server.find(id.toULongLong(nullptr, 16))->labels.contains(QStringLiteral("\\Spam")));
        }
        // And a second try finishes the rest.
        r.server.failMovesAfter = -1;
        emptied.clear();
        r.sync->emptyJunk();
        QVERIFY(emptied.wait(20000));
        QCOMPARE(emptied.first().at(0).toInt(), 250);
        QCOMPARE(emptied.first().at(1).toInt(), 250);
        QCOMPARE(r.inSpam(), 0);
        QVERIFY(r.settle());
    }

    // Stop between batches: what moved stays moved, nothing more is sent.
    void imapStopBetweenBatches()
    {
        ImapRig r(1000);
        QSignalSpy emptied(r.sync.get(), &SyncEngine::junkEmptied);
        connect(r.sync.get(), &SyncEngine::junkEmptyProgress, r.sync.get(), [&r](int done, int) {
            if (done == 200) {
                r.sync->cancelEmptyJunk();
            }
        });
        r.sync->emptyJunk();
        QVERIFY(emptied.wait(20000));
        QCOMPARE(emptied.first().at(0).toInt(), 200);
        QCOMPARE(emptied.first().at(1).toInt(), 1000);
        QCOMPARE(emptied.first().at(2).toBool(), true);
        QVERIFY(emptied.first().at(3).toString().isEmpty());
        QCOMPARE(r.moves().size(), 1);
        QCOMPARE(r.inSpam(), 800);
        QCOMPARE(r.inTrash(), 200);
        QVERIFY(!r.sync->emptyingJunk());
        QVERIFY(r.settle());
    }

    void imapLostConnectionEndsTheRunWithAnError()
    {
        ImapRig r(1000);
        QSignalSpy emptied(r.sync.get(), &SyncEngine::junkEmptied);
        bool dropped = false;
        connect(r.sync.get(), &SyncEngine::junkEmptyProgress, r.sync.get(), [&r, &dropped](int done, int) {
            if (done == 200 && !dropped) {
                dropped = true;
                r.server.dropConnections();
            }
        });
        r.sync->emptyJunk();
        QVERIFY(emptied.wait(20000));
        QCOMPARE(emptied.count(), 1);
        QVERIFY(emptied.first().at(0).toInt() >= 200);
        QVERIFY(emptied.first().at(0).toInt() < 1000);
        QVERIFY(!emptied.first().at(3).toString().isEmpty());
        QVERIFY(!r.sync->emptyingJunk());
        QCOMPARE(r.inTrash(), emptied.first().at(0).toInt());
    }

    // ---- Gmail API (OAuth account) --------------------------------------

    void apiCountIsGmailsMessagesTotal()
    {
        ApiRig r(12);
        int count = -1;
        bool exact = false;
        r.sync->countJunk([&](int c, bool e, const QString &) { count = c; exact = e; });
        QTRY_VERIFY_WITH_TIMEOUT(count >= 0, 10000);
        QCOMPARE(count, 12);
        QVERIFY(exact);
        // Failure: approximate, from the cache.
        r.g.addFault({QStringLiteral("/gmail/v1/users/me/labels/SPAM"), 503, 100, -1});
        r.api->setMaxAttempts(1);
        QString why;
        exact = true;
        count = -1;
        r.sync->countJunk([&](int c, bool e, const QString &w) { count = c; exact = e; why = w; });
        QTRY_VERIFY_WITH_TIMEOUT(count >= 0, 10000);
        QVERIFY(!exact);
        QVERIFY(!why.isEmpty());
    }

    void apiMovesToTrashInBatchesOf200()
    {
        ApiRig r(450);
        QCOMPARE(r.inSpam(), 450);
        QSignalSpy progress(r.sync.get(), &SyncEngine::junkEmptyProgress);
        QSignalSpy emptied(r.sync.get(), &SyncEngine::junkEmptied);
        const int before = r.batchCalls();
        r.sync->emptyJunk();
        QVERIFY(r.sync->emptyingJunk());
        QVERIFY(emptied.wait(30000));
        QCOMPARE(progressValues(progress), (QStringList{"0/450", "200/450", "400/450", "450/450"}));
        QCOMPARE(emptied.first().at(0).toInt(), 450);
        QCOMPARE(emptied.first().at(1).toInt(), 450);
        QCOMPARE(emptied.first().at(2).toBool(), false);
        QVERIFY(emptied.first().at(3).toString().isEmpty());
        QCOMPARE(r.batchCalls() - before, 3);
        QCOMPARE(r.inSpam(), 0);
        QCOMPARE(r.inTrash(), 450); // Trash, not deleted
        for (const QString &id : std::as_const(r.spam)) {
            QVERIFY(r.g.messages().contains(id));
        }
        for (const QString &r2 : std::as_const(r.g.requests)) { // never messages.delete / batchDelete
            QVERIFY2(!r2.startsWith(QLatin1String("DELETE")) && !r2.contains(QLatin1String("batchDelete")), qPrintable(r2));
        }
        QCOMPARE(r.cache.messageIds(QStringLiteral("SPAM")).size(), 0);
        QVERIFY(!r.sync->emptyingJunk());
    }

    // 429s on the batches: GmailClient backs off and retries; the run still
    // finishes, and no batch is lost or doubled.
    void apiRidesOutRateLimits()
    {
        ApiRig r(450);
        QSignalSpy emptied(r.sync.get(), &SyncEngine::junkEmptied);
        r.g.addFault({QStringLiteral("/gmail/v1/users/me/messages/batchModify"), 429, 3, -1});
        const int retriesBefore = r.api->retries();
        r.sync->emptyJunk();
        QVERIFY(emptied.wait(60000));
        QCOMPARE(emptied.first().at(0).toInt(), 450);
        QVERIFY(emptied.first().at(3).toString().isEmpty());
        QVERIFY(r.api->retries() - retriesBefore >= 3);
        QCOMPARE(r.inTrash(), 450);
        QCOMPARE(r.inSpam(), 0);
    }

    void apiPartialFailureReportsHowFarItGot()
    {
        ApiRig r(450);
        r.api->setMaxAttempts(2);
        QSignalSpy emptied(r.sync.get(), &SyncEngine::junkEmptied);
        bool faulted = false;
        connect(r.sync.get(), &SyncEngine::junkEmptyProgress, r.sync.get(), [&r, &faulted](int done, int) {
            if (done == 200 && !faulted) {
                faulted = true;
                r.g.addFault({QStringLiteral("/gmail/v1/users/me/messages/batchModify"), 500, 100, -1});
            }
        });
        r.sync->emptyJunk();
        QVERIFY(emptied.wait(60000));
        QCOMPARE(emptied.first().at(0).toInt(), 200);
        QCOMPARE(emptied.first().at(1).toInt(), 450);
        QCOMPARE(emptied.first().at(2).toBool(), false);
        QVERIFY(!emptied.first().at(3).toString().isEmpty());
        QCOMPARE(r.inTrash(), 200);
        QCOMPARE(r.inSpam(), 250);
        QVERIFY(!r.sync->emptyingJunk());
    }

    void apiStopBetweenBatches()
    {
        ApiRig r(1000);
        QSignalSpy emptied(r.sync.get(), &SyncEngine::junkEmptied);
        connect(r.sync.get(), &SyncEngine::junkEmptyProgress, r.sync.get(), [&r](int done, int) {
            if (done == 400) {
                r.sync->cancelEmptyJunk();
            }
        });
        r.sync->emptyJunk();
        QVERIFY(emptied.wait(60000));
        QCOMPARE(emptied.first().at(0).toInt(), 400);
        QCOMPARE(emptied.first().at(1).toInt(), 1000);
        QCOMPARE(emptied.first().at(2).toBool(), true);
        QCOMPARE(r.inTrash(), 400);
        QCOMPARE(r.inSpam(), 600);
    }

    // ---- the window -----------------------------------------------------

    void windowMenuAndConfirmation()
    {
        UiRig r(5);
        QVERIFY(r.ready());
        QTRY_COMPARE_WITH_TIMEOUT(r.total(QStringLiteral("Junk")), QStringLiteral("5"), 10000);

        // Only Junk has it; Trash keeps its own Empty Trash and gets no Empty Junk.
        r.tree->setCurrentItem(r.item(QStringLiteral("Junk"))); // looking at Spam, as one would
        QTest::qWait(800);
        QMenu *junk = r.menuFor(QStringLiteral("Junk"));
        QVERIFY(junk);
        const QStringList junkNames = UiRig::names(junk);
        QVERIFY(junkNames.contains(QStringLiteral("actionEmptyJunk")));
        QVERIFY(!junkNames.contains(QStringLiteral("actionEmptyTrash")));
        QAction *empty = junk->findChild<QAction *>(QStringLiteral("actionEmptyJunk"));
        QCOMPARE(empty->text().remove(QLatin1Char('&')), QStringLiteral("Empty Junk\u2026"));
        r.shot("empty-junk-menu", junk);
        junk->close();
        for (const QString &key : {QStringLiteral("In"), QStringLiteral("Trash")}) {
            QMenu *m = r.menuFor(key);
            QVERIFY(m);
            QVERIFY(!UiRig::names(m).contains(QStringLiteral("actionEmptyJunk")));
            if (key == QLatin1String("Trash")) {
                QVERIFY(UiRig::names(m).contains(QStringLiteral("actionEmptyTrash")));
            }
            m->close();
        }

        // The question: the server's count, Trash (not erased), Cancel by default.
        junk = r.menuFor(QStringLiteral("Junk"));
        junk->findChild<QAction *>(QStringLiteral("actionEmptyJunk"))->trigger();
        junk->close();
        QMessageBox *box = r.dialog("emptyJunkDialog");
        QVERIFY(box);
        QVERIFY2(box->text().contains(QStringLiteral("all 5 messages")) && box->text().contains(QStringLiteral("Trash")),
                 qPrintable(box->text()));
        QVERIFY(box->informativeText().contains(QStringLiteral("Gmail's count")));
        QVERIFY(box->informativeText().contains(QStringLiteral("nothing is erased permanently")));
        QVERIFY(box->defaultButton());
        QCOMPARE(box->defaultButton()->text().remove(QLatin1Char('&')), QStringLiteral("Cancel"));
        QCOMPARE(box->escapeButton(), box->defaultButton());
        r.shot("empty-junk-confirm", box);
        // Cancel: nothing moves.
        box->defaultButton()->click();
        QTest::qWait(500);
        QCOMPARE(r.inSpam(), 5);
        for (const QByteArray &c : std::as_const(r.imap.commands)) {
            QVERIFY2(!c.startsWith("UID MOVE"), c.constData());
        }
        QVERIFY(!r.dialog("emptyJunkDialog") || !r.dialog("emptyJunkDialog")->isVisible());

        // Again, and confirm.
        junk = r.menuFor(QStringLiteral("Junk"));
        junk->findChild<QAction *>(QStringLiteral("actionEmptyJunk"))->trigger();
        junk->close();
        box = r.dialog("emptyJunkDialog");
        QVERIFY(box);
        QAbstractButton *go = box->findChild<QAbstractButton *>(QStringLiteral("emptyJunkConfirm"));
        QVERIFY(go);
        QCOMPARE(go->text(), QStringLiteral("Empty Junk"));
        go->click();
        QTRY_COMPARE_WITH_TIMEOUT(r.inSpam(), 0, 15000);
        QCOMPARE(r.inTrash(), 5);
        QTRY_VERIFY_WITH_TIMEOUT(r.w->statusBar()->currentMessage().contains(QStringLiteral("Junk emptied: 5 messages moved to Trash")), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(r.total(QStringLiteral("Junk")).isEmpty() || r.total(QStringLiteral("Junk")) == QLatin1String("0"), 10000);
        QVERIFY(!r.w->findChild<QWidget *>(QStringLiteral("emptyJunkBar"))->isVisible());
        r.shot("empty-junk-done");
        // Nothing left: asking again says so, no dialog.
        junk = r.menuFor(QStringLiteral("Junk"));
        junk->findChild<QAction *>(QStringLiteral("actionEmptyJunk"))->trigger();
        junk->close();
        QTRY_VERIFY_WITH_TIMEOUT(r.w->statusBar()->currentMessage().contains(QStringLiteral("already empty")), 10000);
        const QMessageBox *none = r.dialog("emptyJunkDialog");
        QVERIFY(!none || !none->isVisible());
    }

    // Gmail can't be asked for the count: the question says "about", why, and
    // that it may be out of date, and still defaults to Cancel.
    void windowApproximateCount()
    {
        UiRig r(4);
        QVERIFY(r.ready());
        QTRY_COMPARE_WITH_TIMEOUT(r.total(QStringLiteral("Junk")), QStringLiteral("4"), 10000);
        r.imap.throttleCommands = 1; // the STATUS is refused
        QMenu *junk = r.menuFor(QStringLiteral("Junk"));
        junk->findChild<QAction *>(QStringLiteral("actionEmptyJunk"))->trigger();
        junk->close();
        QMessageBox *box = r.dialog("emptyJunkDialog");
        QVERIFY(box);
        QVERIFY2(box->text().contains(QStringLiteral("about 4")) && box->text().contains(QStringLiteral("out of date")),
                 qPrintable(box->text()));
        QVERIFY(!box->informativeText().contains(QStringLiteral("Gmail's count")));
        QCOMPARE(box->defaultButton()->text().remove(QLatin1Char('&')), QStringLiteral("Cancel"));
        r.shot("empty-junk-confirm-approximate", box);
        box->defaultButton()->click();
        QCOMPARE(r.inSpam(), 4);
    }

    // While it runs: the "Emptying Junk: N of M [Stop]" bar, and the menu offers
    // Stop. A partial failure says how far it got.
    void windowProgressStopAndFailure()
    {
        UiRig r(450);
        QVERIFY(r.ready());
        QTRY_COMPARE_WITH_TIMEOUT(r.total(QStringLiteral("Junk")), QStringLiteral("450"), 15000);

        // Hold the second batch's answer, so the run can be looked at half way.
        connect(r.session->sync(), &SyncEngine::junkEmptyProgress, this, [&r](int done, int) {
            if (done == 200) {
                r.imap.holdMoves = true;
            }
        });
        r.w->emptyJunk(/*confirm=*/false);
        auto *bar = r.w->findChild<QWidget *>(QStringLiteral("emptyJunkBar"));
        QVERIFY(bar);
        QTRY_VERIFY_WITH_TIMEOUT(bar->isVisible(), 5000);
        auto *barLabel = bar->findChild<QLabel *>(QStringLiteral("emptyJunkLabel"));
        QTRY_COMPARE_WITH_TIMEOUT(barLabel->text(), QStringLiteral("Emptying Junk: 200 of 450"), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(int(r.imap.movesDone), 2, 5000); // the held batch has been sent
        QVERIFY(r.w->findChild<QLabel *>(QStringLiteral("syncLabel"))->text().contains(QStringLiteral("Emptying Junk: ")));
        QMenu *junk = r.menuFor(QStringLiteral("Junk"));
        QVERIFY(junk);
        QVERIFY(UiRig::names(junk).contains(QStringLiteral("actionStopEmptyJunk")));
        QVERIFY(!UiRig::names(junk).contains(QStringLiteral("actionEmptyJunk")));
        junk->close();
        r.shot("empty-junk-progress");
        r.w->findChild<QToolButton *>(QStringLiteral("emptyJunkStop"))->click(); // the bar's Stop
        QCOMPARE(barLabel->text(), QStringLiteral("Stopping\u2026"));
        r.imap.releaseMoves(); // the batch in flight lands; no more are sent
        QTRY_VERIFY_WITH_TIMEOUT(!bar->isVisible(), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(r.w->statusBar()->currentMessage(), QStringLiteral("Stopped: 400 of 450 messages moved to Trash; 50 still in Junk."), 10000);
        QCOMPARE(r.inTrash(), 400);
        QCOMPARE(r.inSpam(), 50);
        QVERIFY(QTest::qWaitFor([&r] { return !r.session->sync()->isBusy(); }, 10000));

        // Part way, then the server refuses: a dialog with the numbers.
        const int already = r.inTrash();
        r.imap.movesDone = 0;
        r.imap.failMovesAfter = 0; // refused from the first batch
        r.w->emptyJunk(/*confirm=*/false);
        QMessageBox *box = r.dialog("emptyJunkResult");
        QVERIFY2(box, qPrintable(r.w->statusBar()->currentMessage()));
        const int total = 450 - already;
        QVERIFY2(box->text().contains(QStringLiteral("0 of %1").arg(total))
                     && box->text().contains(QStringLiteral("%1 still in Junk").arg(total))
                     && box->text().contains(QStringLiteral("refused")),
                 qPrintable(box->text()));
        QVERIFY(r.w->statusBar()->currentMessage().contains(QStringLiteral("0 of %1").arg(total)));
        r.shot("empty-junk-partial", box);
        box->accept();
        QVERIFY(!bar->isVisible());
        QCOMPARE(r.inTrash(), already);
        QCOMPARE(r.inSpam(), total);
        QTRY_COMPARE_WITH_TIMEOUT(r.total(QStringLiteral("Junk")), QString::number(total), 10000);
    }
};

QTEST_MAIN(TstEmptyJunk)
#include "tst_emptyjunk.moc"
