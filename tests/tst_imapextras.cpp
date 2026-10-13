// "Gmail (app password)" account, part 5: snooze, Gmail's connection /
// bandwidth limits with backoff, the From display name, and the vacation
// responder's absence. Against the fake IMAP and SMTP servers; made-up
// passwords only.
#include "MainWindow.hpp"
#include "core/AppPasswordAccount.h"
#include "core/ImapSyncEngine.h"
#include "core/MailCache.h"
#include "core/MailSession.h"
#include "core/ServerLimits.h"
#include "core/SmtpSender.h"
#include "core/TokenStore.h"
#include "mock/FakeImapServer.h"
#include "mock/FakeSmtpServer.h"
#include "ui/AppPasswordDialog.h"

#include <QLineEdit>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTest>

using namespace zmail;
using zmail::test::FakeImapServer;
using zmail::test::FakeSmtpServer;
using Kind = ServerLimits::Kind;

namespace {

AppPasswordAccount accountFor(const FakeImapServer &imap, const FakeSmtpServer *smtp = nullptr)
{
    AppPasswordAccount a;
    a.email = imap.user;
    a.imapHost = QStringLiteral("127.0.0.1");
    a.imapPort = imap.port();
    a.smtpHost = QStringLiteral("127.0.0.1");
    a.smtpPort = smtp ? smtp->port() : 1;
    return a;
}

struct Engine
{
    MailCache cache;
    std::unique_ptr<ImapSyncEngine> sync;
    QString password;
    Engine(FakeImapServer &imap, const QString &pw)
        : password(pw)
    {
        cache.open(QStringLiteral(":memory:"));
        sync = std::make_unique<ImapSyncEngine>(
            accountFor(imap), [this](std::function<void(bool, const QString &, const QString &)> cb) { cb(true, password, {}); },
            &cache);
        sync->setPlaintextForTests(true);
        sync->setPollInterval(3600 * 1000);
        sync->setBackoffScaleForTests(0.001); // 60 s -> 60 ms
    }
};

QByteArray mail(const QByteArray &subject)
{
    return "From: Alex Morgan <alex.morgan@gmail.com>\r\nTo: priya@northwind.example\r\nSubject: " + subject
        + "\r\nMIME-Version: 1.0\r\nContent-Type: text/plain\r\n\r\nHello.\r\n";
}

} // namespace

class TstImapExtras : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    // ---- Gmail's limits ---------------------------------------------------

    void classifiesGmailRefusals()
    {
        QCOMPARE(ServerLimits::classifyImap(QStringLiteral("[AUTHENTICATIONFAILED] Invalid credentials (Failure)")), Kind::AuthFailed);
        QCOMPARE(ServerLimits::classifyImap(QStringLiteral("IMAP sign-in failed: [ALERT] Too many simultaneous connections. (Failure)")),
                 Kind::TooManyConnections);
        QCOMPARE(ServerLimits::classifyImap(QStringLiteral("[ALERT] Account exceeded command or bandwidth limits. (Failure)")),
                 Kind::Bandwidth);
        QCOMPARE(ServerLimits::classifyImap(QStringLiteral("[THROTTLED] slow down")), Kind::Bandwidth);
        QCOMPARE(ServerLimits::classifyImap(QStringLiteral("Unexpected IMAP greeting: * BYE [UNAVAILABLE] Temporary System Error")),
                 Kind::Unavailable);
        QCOMPARE(ServerLimits::classifyImap(QStringLiteral("The IMAP server closed the connection.")), Kind::None);

        QCOMPARE(ServerLimits::classifySmtp(535, QStringLiteral("5.7.8 Username and Password not accepted")), Kind::AuthFailed);
        QCOMPARE(ServerLimits::classifySmtp(454, QStringLiteral("4.7.0 Too many login attempts, please try again later")),
                 Kind::TooManyConnections);
        QCOMPARE(ServerLimits::classifySmtp(421, QStringLiteral("4.7.0 Try again later, closing connection.")), Kind::Unavailable);
        QCOMPARE(ServerLimits::classifySmtp(550, QStringLiteral("5.4.5 Daily user sending limit exceeded.")), Kind::SendingLimit);
        QCOMPARE(ServerLimits::classifySmtp(550, QStringLiteral("5.1.1 The email account does not exist")), Kind::None);
    }

    void backoffGrowsAndCaps()
    {
        QCOMPARE(ServerLimits::backoffMs(Kind::Unavailable, 0, 1.0, false), 2'000);
        QCOMPARE(ServerLimits::backoffMs(Kind::Unavailable, 3, 1.0, false), 16'000);
        QCOMPARE(ServerLimits::backoffMs(Kind::Unavailable, 30, 1.0, false), 5 * 60'000);
        QCOMPARE(ServerLimits::backoffMs(Kind::TooManyConnections, 0, 1.0, false), 60'000);
        QCOMPARE(ServerLimits::backoffMs(Kind::TooManyConnections, 9, 1.0, false), 15 * 60'000);
        QCOMPARE(ServerLimits::backoffMs(Kind::Bandwidth, 0, 1.0, false), 10 * 60'000);
        QCOMPARE(ServerLimits::backoffMs(Kind::Bandwidth, 5, 1.0, false), 60 * 60'000);
        QCOMPARE(ServerLimits::backoffMs(Kind::AuthFailed, 0), -1);
        QCOMPARE(ServerLimits::backoffMs(Kind::SendingLimit, 0), -1);
        for (int i = 0; i < 50; ++i) { // +-20% jitter
            const qint64 j = ServerLimits::backoffMs(Kind::TooManyConnections, 0);
            QVERIFY(j >= 48'000 && j <= 72'000);
        }
        QVERIFY(!ServerLimits::explain(Kind::AuthFailed, QStringLiteral("imap.gmail.com")).contains(QLatin1String("abcd")));
    }

    void tooManyConnectionsBacksOffThenSyncs()
    {
        FakeImapServer imap;
        imap.seedDemo(4);
        imap.loginRefusals = {"NO [ALERT] Too many simultaneous connections. (Failure)"};
        Engine e(imap, imap.password);
        QSignalSpy errors(e.sync.get(), &SyncEngine::syncError);
        QSignalSpy idle(e.sync.get(), &SyncEngine::idle);
        e.sync->start();
        QTRY_COMPARE(e.sync->lastLimit(), Kind::TooManyConnections);
        QVERIFY(!errors.isEmpty());
        QVERIFY(errors.last().first().toString().contains(QLatin1String("too many connections")));
        QVERIFY(e.sync->retryInMs() > 0 && e.sync->retryInMs() <= 72); // 60 s, scaled
        QVERIFY(idle.wait(5000));                                       // then it gets in
        QCOMPARE(imap.refusedLogins, 1);
        QCOMPARE(imap.logins, 1);
        QCOMPARE(e.sync->lastLimit(), Kind::None);
        QCOMPARE(e.cache.count(QStringLiteral("INBOX")), 4);
    }

    void unavailableGreetingRetries()
    {
        FakeImapServer imap;
        imap.seedDemo(3);
        imap.byeGreetings = {"* BYE [UNAVAILABLE] Temporary System Error", "* BYE [UNAVAILABLE] Temporary System Error"};
        Engine e(imap, imap.password);
        QSignalSpy idle(e.sync.get(), &SyncEngine::idle);
        e.sync->start();
        QVERIFY(idle.wait(10000));
        QCOMPARE(imap.connectAttempts, 3);
        QCOMPARE(e.cache.count(QStringLiteral("INBOX")), 3);
    }

    void throttledMidSessionHangsUpAndComesBack()
    {
        FakeImapServer imap;
        imap.seedDemo(3);
        Engine e(imap, imap.password);
        QSignalSpy idle(e.sync.get(), &SyncEngine::idle);
        e.sync->start();
        QVERIFY(idle.wait(10000));
        const int loginsBefore = imap.logins;
        imap.throttleCommands = 1;
        QSignalSpy status(e.sync.get(), &SyncEngine::statusChanged);
        e.sync->pollNow(true);
        QTRY_COMPARE(e.sync->lastLimit(), Kind::Bandwidth);
        bool said = false;
        for (const auto &s : status) {
            said = said || s.first().toString().contains(QLatin1String("limiting"));
        }
        QVERIFY(said);
        // 10 min scaled to ~0.6 s, then a fresh login and sync.
        QTRY_VERIFY_WITH_TIMEOUT(imap.logins > loginsBefore, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(imap.anyIdling(), 5000);
        imap.deliver(FakeImapServer::makeMessage(QStringLiteral("Ops"), QStringLiteral("ops@example.com"),
                                                 QStringLiteral("After the throttle"), QStringLiteral("hi")));
        QTRY_COMPARE_WITH_TIMEOUT(e.cache.count(QStringLiteral("INBOX")), 4, 5000);
    }

    void refusedPasswordIsNotRetried()
    {
        FakeImapServer imap;
        imap.seedDemo(2);
        Engine e(imap, QStringLiteral("zzzzzzzzzzzzzzzz")); // not the fake's made-up one
        QSignalSpy errors(e.sync.get(), &SyncEngine::syncError);
        e.sync->start();
        QTRY_VERIFY(e.sync->authFailed());
        QCOMPARE(e.sync->retryInMs(), -1);
        QVERIFY(errors.last().first().toString().contains(QLatin1String("won't keep retrying")));
        QTest::qWait(400);
        e.sync->pollNow(true);
        QTest::qWait(300);
        QCOMPARE(imap.connectAttempts, 1); // Google locks accounts that keep trying
        // A fresh start (new password from the account dialog) tries again.
        e.sync->stop();
        e.password = imap.password;
        QSignalSpy idle(e.sync.get(), &SyncEngine::idle);
        e.sync->start();
        QVERIFY(idle.wait(10000));
        QVERIFY(!e.sync->authFailed());
    }

    void smtpRetriesTemporaryRefusals()
    {
        FakeImapServer imap;
        FakeSmtpServer smtp(&imap);
        SmtpSender sender(accountFor(imap, &smtp),
                          [&](std::function<void(bool, const QString &, const QString &)> cb) { cb(true, imap.password, {}); },
                          nullptr);
        sender.setPlaintextForTests(true);
        sender.setBackoffScaleForTests(0.01);
        auto send = [&](const QByteArray &m) {
            Sender::Result out;
            bool done = false;
            sender.send(m, {}, [&](const Sender::Result &r) {
                out = r;
                done = true;
            });
            QTest::qWaitFor([&] { return done; }, 15000);
            return out;
        };
        smtp.greetings = {"421 4.7.0 Try again later, closing connection.", "421 4.3.0 Temporary System Problem."};
        QSignalSpy retrying(&sender, &SmtpSender::retrying);
        Sender::Result r = send(mail("Gets through on the third try"));
        QVERIFY2(r.ok, qPrintable(r.err.message));
        QCOMPARE(retrying.count(), 2);
        QCOMPARE(smtp.connections, 3);
        QCOMPARE(smtp.delivered.size(), 1);

        // Too many logins, three times: two retries, then it says so.
        smtp.authReplies = {"454 4.7.0 Too many login attempts, please try again later.",
                            "454 4.7.0 Too many login attempts, please try again later.",
                            "454 4.7.0 Too many login attempts, please try again later."};
        r = send(mail("Gives up after two retries"));
        QVERIFY(!r.ok);
        QVERIFY(r.err.message.contains(QLatin1String("too many connections")));
        QCOMPARE(r.err.httpStatus, 454);
        QCOMPARE(smtp.delivered.size(), 1);

        // The daily limit, and a refused password: no retry.
        const int before = smtp.connections;
        smtp.dataReply = "550 5.4.5 Daily user sending limit exceeded.";
        r = send(mail("Over the daily limit"));
        QVERIFY(!r.ok);
        QVERIFY(r.err.message.contains(QLatin1String("daily sending limit")));
        QCOMPARE(smtp.connections, before + 1);
        smtp.dataReply.clear();
        smtp.password = QStringLiteral("zzzzzzzzzzzzzzzz");
        r = send(mail("Refused password"));
        QVERIFY(!r.ok);
        QVERIFY(r.err.message.contains(QLatin1String("app password")));
        QCOMPARE(smtp.connections, before + 2);
    }

    // ---- Snooze -----------------------------------------------------------

    void snoozeLeavesInboxAndWakes()
    {
        FakeImapServer imap;
        imap.seedDemo(5);
        Engine e(imap, imap.password);
        QSignalSpy idle(e.sync.get(), &SyncEngine::idle);
        e.sync->start();
        QVERIFY(idle.wait(10000));
        const QString id = e.cache.messages(QStringLiteral("INBOX")).first().id;
        const quint64 msgid = id.toULongLong(nullptr, 16);
        const qint64 wake = QDateTime::currentMSecsSinceEpoch() + 3600 * 1000;
        e.sync->snooze(id, wake);
        QCOMPARE(e.cache.snooze(id).wakeMs, wake);
        QVERIFY(!e.cache.summary(id).labels.contains(QStringLiteral("INBOX")));
        QTRY_VERIFY(!imap.find(msgid)->labels.contains(QStringLiteral("\\Inbox")));
        QCOMPARE(e.sync->wakeDue(wake - 1), 0);

        QSignalSpy woke(e.sync.get(), &SyncEngine::snoozesWoke);
        QCOMPARE(e.sync->wakeDue(wake + 1), 1);
        QCOMPARE(woke.count(), 1);
        QVERIFY(e.cache.summary(id).labels.contains(QStringLiteral("INBOX")));
        QTRY_VERIFY(imap.find(msgid)->labels.contains(QStringLiteral("\\Inbox")));
        QVERIFY(e.cache.snooze(id).badge); // "was snoozed" until opened

        // Unsnooze early puts it back too.
        e.sync->snooze(id, wake);
        QTRY_VERIFY(!imap.find(msgid)->labels.contains(QStringLiteral("\\Inbox")));
        e.sync->unsnooze(id);
        QTRY_VERIFY(imap.find(msgid)->labels.contains(QStringLiteral("\\Inbox")));
        QVERIFY(e.cache.snooze(id).messageId.isEmpty());

        // Refused by the server: the snooze is dropped and it stays in INBOX.
        imap.refuseChanges = true;
        e.sync->snooze(id, wake);
        QTRY_VERIFY(e.cache.snooze(id).messageId.isEmpty());
        QTRY_VERIFY(e.cache.summary(id).labels.contains(QStringLiteral("INBOX")));
    }

    void snoozeSurvivesTrash()
    {
        FakeImapServer imap;
        imap.seedDemo(5);
        Engine e(imap, imap.password);
        QSignalSpy idle(e.sync.get(), &SyncEngine::idle);
        e.sync->start();
        QVERIFY(idle.wait(10000));
        const QString id = e.cache.messages(QStringLiteral("INBOX")).first().id;
        const qint64 wake = QDateTime::currentMSecsSinceEpoch() + 1000;
        e.sync->snooze(id, wake);
        e.sync->trash(id);
        QTRY_VERIFY(imap.find(id.toULongLong(nullptr, 16))->labels == QStringList{QStringLiteral("\\Trash")});
        e.sync->wakeDue(wake + 1); // deleted while snoozed: stays in Trash
        QVERIFY(e.cache.snooze(id).messageId.isEmpty());
        QTest::qWait(300);
        QCOMPARE(imap.find(id.toULongLong(nullptr, 16))->labels, QStringList{QStringLiteral("\\Trash")});
        QVERIFY(!e.cache.summary(id).labels.contains(QStringLiteral("INBOX")));
    }

    // ---- Display name, vacation -------------------------------------------

    void displayNameGoesInFrom()
    {
        ui::AppPasswordDialog d(nullptr);
        auto *name = d.findChild<QLineEdit *>(QStringLiteral("displayNameEdit"));
        QVERIFY(name);
        name->setText(QStringLiteral("  Alex Morgan "));
        d.findChild<QLineEdit *>(QStringLiteral("emailEdit"))->setText(QStringLiteral("alex.morgan@gmail.com"));
        QCOMPARE(d.account().displayName, QStringLiteral("Alex Morgan"));

        FakeImapServer imap;
        imap.seedDemo(2);
        FakeSmtpServer smtp(&imap);
        MemoryTokenStore store;
        store.values.insert(AppPasswordAccount::appPasswordKey(imap.user), imap.password);
        AppPasswordAccount a = accountFor(imap, &smtp);
        a.displayName = QStringLiteral("Alex Morgan");
        {
            QSettings s;
            s.clear();
            a.save(s);
            QCOMPARE(AppPasswordAccount::load(s).displayName, QStringLiteral("Alex Morgan"));
        }
        SessionOptions o;
        o.client.status = ClientConfig::LoadStatus::Missing;
        o.store = &store;
        o.cachePathOverride = QStringLiteral(":memory:");
        o.contactsPathOverride = QStringLiteral(":memory:");
        o.imapPlaintextForTests = true;
        o.pollIntervalMs = 3600 * 1000;
        MailSession session(o);
        MainWindow w;
        w.setSession(&session);
        session.restoreSaved();
        QCOMPARE(session.fromHeader(), QStringLiteral("Alex Morgan <alex.morgan@gmail.com>"));

        QSignalSpy identity(&session, &MailSession::identityChanged);
        const QString answer = QStringLiteral("Morgan, Alex");
        QVERIFY(w.editDisplayName(&answer));
        QCOMPARE(identity.count(), 1);
        QCOMPARE(session.fromHeader(), QStringLiteral("\"Morgan, Alex\" <alex.morgan@gmail.com>"));
        QCOMPARE(AppPasswordAccount::load(QSettings()).displayName, answer);
        const QString blank;
        QVERIFY(w.editDisplayName(&blank));
        QCOMPARE(session.fromHeader(), QStringLiteral("alex.morgan@gmail.com"));

        // No vacation responder over IMAP/SMTP: say where to set it instead.
        QVERIFY(!w.showVacationDialog());
        QVERIFY(w.statusBar()->currentMessage().contains(QLatin1String("Gmail's web settings")));
        QSettings().clear();
    }
};

QTEST_MAIN(TstImapExtras)
#include "tst_imapextras.moc"
