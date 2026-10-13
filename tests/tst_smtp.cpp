// "Gmail (app password)" account, part 4: sending over SMTP, Send Queued,
// and drafts kept in [Gmail]/Drafts over IMAP. Against the fake IMAP and
// SMTP servers, with made-up passwords only.
#include "MainWindow.hpp"
#include "core/AppPasswordAccount.h"
#include "core/ImapSyncEngine.h"
#include "core/MailCache.h"
#include "core/MailSession.h"
#include "core/SmtpSender.h"
#include "core/TokenStore.h"
#include "mock/FakeImapServer.h"
#include "mock/FakeSmtpServer.h"

#include <QAction>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>

using namespace zmail;
using zmail::test::FakeImapServer;
using zmail::test::FakeSmtpServer;

namespace {

QByteArray mail(const QByteArray &subject, const QByteArray &extraHeaders = {}, const QByteArray &body = "Hello.\r\n")
{
    return "From: Alex Morgan <alex.morgan@gmail.com>\r\n"
           "To: \"Raman, Priya\" <priya@northwind.example>, marcus.lee@example.org\r\n"
           "Cc: Dana <dana@whitfield.example>\r\n"
           + extraHeaders + "Subject: " + subject + "\r\nMessage-ID: <" + subject.toHex().left(16)
           + "@zmail>\r\nMIME-Version: 1.0\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n" + body;
}

struct Rig
{
    FakeImapServer imap;
    FakeSmtpServer smtp{&imap};
    MailCache cache;
    std::unique_ptr<ImapSyncEngine> sync;
    std::unique_ptr<SmtpSender> sender;
    AppPasswordAccount account;

    Rig()
    {
        imap.seedDemo(6);
        cache.open(QStringLiteral(":memory:"));
        account.email = imap.user;
        account.imapHost = QStringLiteral("127.0.0.1");
        account.imapPort = imap.port();
        account.smtpHost = QStringLiteral("127.0.0.1");
        account.smtpPort = smtp.port();
        auto pw = [this](std::function<void(bool, const QString &, const QString &)> cb) { cb(true, imap.password, {}); };
        sync = std::make_unique<ImapSyncEngine>(account, pw, &cache);
        sync->setPlaintextForTests(true);
        sync->setPollInterval(3600 * 1000);
        sender = std::make_unique<SmtpSender>(account, pw, sync.get());
        sender->setPlaintextForTests(true);
        sender->setTimeoutMs(5000);
        QSignalSpy idle(sync.get(), &SyncEngine::idle);
        sync->start();
        if (!idle.wait(10000)) {
            qFatal("no initial sync");
        }
    }
    Sender::Result send(const QByteArray &mime)
    {
        Sender::Result out;
        bool done = false;
        sender->send(mime, {}, [&](const Sender::Result &r) {
            out = r;
            done = true;
        });
        if (!QTest::qWaitFor([&] { return done; }, 10000)) {
            out.err.message = QStringLiteral("timed out");
        }
        return out;
    }
    Sender::Result saveDraft(const QByteArray &mime, const QString &replace)
    {
        Sender::Result out;
        bool done = false;
        sender->saveDraft(mime, {}, replace, [&](const Sender::Result &r) {
            out = r;
            done = true;
        });
        QTest::qWaitFor([&] { return done; }, 10000);
        return out;
    }
    bool onServer(const QString &hexId)
    {
        auto *m = imap.find(hexId.toULongLong(nullptr, 16));
        return m && !m->gone;
    }
};

} // namespace

class TstSmtp : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void envelopeAndPayload()
    {
        const QByteArray m = mail("Envelope", "Bcc: boss@northwind.example,\r\n hidden@example.com\r\n",
                                  "first\n.leading dot\r\n..two dots\r\nlast\r\n");
        QCOMPARE(SmtpSender::envelopeRecipients(m),
                 (QStringList{QStringLiteral("priya@northwind.example"), QStringLiteral("marcus.lee@example.org"),
                              QStringLiteral("dana@whitfield.example"), QStringLiteral("boss@northwind.example"),
                              QStringLiteral("hidden@example.com")}));
        const QByteArray d = SmtpSender::dataPayload(m);
        QVERIFY(!d.contains("Bcc:"));
        QVERIFY(!d.contains("hidden@example.com"));
        QVERIFY(d.contains("\r\nfirst\r\n..leading dot\r\n...two dots\r\nlast\r\n.\r\n"));
        QVERIFY(d.endsWith("last\r\n.\r\n"));
        QVERIFY(!d.contains("\r\r"));
        // Body lines that merely look like headers are left alone.
        QVERIFY(SmtpSender::dataPayload(mail("x", {}, "Bcc: not a header\r\n")).contains("Bcc: not a header"));
    }

    void sendsOverSmtpAndShowsInSent()
    {
        Rig r;
        QSignalSpy progress(r.sender.get(), &Sender::progress);
        const Sender::Result res = r.send(mail("Crew roster for Friday", "Bcc: boss@northwind.example\r\n"));
        QVERIFY2(res.ok, qPrintable(res.err.message));
        QCOMPARE(r.smtp.delivered.size(), 1);
        const auto &d = r.smtp.delivered.first();
        QCOMPARE(d.from, QByteArray("alex.morgan@gmail.com"));
        QCOMPARE(d.rcpts.size(), 4);
        QVERIFY(d.rcpts.contains("boss@northwind.example"));
        QVERIFY(!d.data.contains("boss@northwind.example")); // Bcc stays out of the message
        QVERIFY(d.data.contains("Subject: Crew roster for Friday"));
        QVERIFY(!progress.isEmpty());
        // The password never shows up in what was said, only AUTH (masked here).
        for (const QByteArray &c : r.smtp.commands) {
            QVERIFY(!c.contains(r.imap.password.toUtf8()));
        }
        QVERIFY(r.smtp.commands.contains("AUTH PLAIN ****"));
        // Gmail files it in Sent Mail; the engine picks it up.
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                for (const CachedMessage &m : r.cache.messages(QStringLiteral("SENT"))) {
                    if (m.subject == QLatin1String("Crew roster for Friday")) {
                        return true;
                    }
                }
                return false;
            }(),
            10000);
    }

    void wrongPasswordSaysSo()
    {
        Rig r;
        r.smtp.password = QStringLiteral("zzzzzzzzzzzzzzzz"); // the server expects another made-up one
        const Sender::Result res = r.send(mail("Nope"));
        QVERIFY(!res.ok);
        QVERIFY(res.err.message.contains(QLatin1String("app password")));
        QVERIFY(!res.err.message.contains(r.imap.password));
        QVERIFY(r.smtp.delivered.isEmpty());
    }

    void refusedRecipientNamesIt()
    {
        Rig r;
        r.smtp.rejectRecipient = "marcus.lee@example.org";
        const Sender::Result res = r.send(mail("To someone gone"));
        QVERIFY(!res.ok);
        QVERIFY(res.err.message.contains(QLatin1String("marcus.lee@example.org")));
        QVERIFY(r.smtp.delivered.isEmpty());
    }

    void refusedOrDroppedDataFails()
    {
        Rig r;
        r.smtp.rejectData = true;
        Sender::Result res = r.send(mail("Too big for the fake"));
        QVERIFY(!res.ok);
        QVERIFY(res.err.message.contains(QLatin1String("552")) || res.err.message.contains(QLatin1String("too big")));
        r.smtp.rejectData = false;
        r.smtp.hangUpAfterData = true;
        res = r.send(mail("Hung up on"));
        QVERIFY(!res.ok);
        QVERIFY(!res.err.message.isEmpty());
        r.smtp.hangUpAfterData = false;
        QVERIFY(r.send(mail("Works again")).ok);
    }

    void overTheLimitNeverConnects()
    {
        Rig r;
        const QByteArray big = mail("Huge", {}, QByteArray(26'000'000, 'a'));
        const Sender::Result res = r.send(big);
        QVERIFY(!res.ok);
        QVERIFY(res.blocked);
        QCOMPARE(r.smtp.connections, 0);
        QVERIFY(!r.send("Subject: nobody\r\n\r\nhi\r\n").ok); // no recipients
        QCOMPARE(r.smtp.connections, 0);
    }

    void draftsLiveInGmailDrafts()
    {
        Rig r;
        const int draftsBefore = r.cache.count(QStringLiteral("DRAFT"));
        Sender::Result first = r.saveDraft(mail("Draft: budget notes", {}, "v1\r\n"), {});
        QVERIFY2(first.ok, qPrintable(first.err.message));
        QVERIFY(!first.draftId.isEmpty());
        QVERIFY(r.onServer(first.draftId));
        auto *m = r.imap.find(first.draftId.toULongLong(nullptr, 16));
        QVERIFY(m->labels.contains(QStringLiteral("\\Draft")));
        QVERIFY(m->flags.contains(QStringLiteral("\\Draft")));
        QVERIFY(m->raw.contains("v1"));
        QVERIFY(r.cache.summary(first.draftId).labels.contains(QStringLiteral("DRAFT")));
        QCOMPARE(r.cache.count(QStringLiteral("DRAFT")), draftsBefore + 1);

        // Saving again replaces it: a new message, the old one expunged.
        Sender::Result second = r.saveDraft(mail("Draft: budget notes", {}, "v2\r\n"), first.draftId);
        QVERIFY(second.ok);
        QVERIFY(second.draftId != first.draftId);
        QVERIFY(!r.onServer(first.draftId));
        QVERIFY(r.onServer(second.draftId));
        QVERIFY(!r.cache.contains(first.draftId));
        QCOMPARE(r.cache.count(QStringLiteral("DRAFT")), draftsBefore + 1);
        QVERIFY(r.imap.commands.join('\n').contains("UID EXPUNGE"));

        bool done = false;
        Sender::Result del;
        r.sender->deleteDraft(second.draftId, [&](const Sender::Result &res) {
            del = res;
            done = true;
        });
        QTRY_VERIFY(done);
        QVERIFY(del.ok);
        QVERIFY(!r.onServer(second.draftId));
        QVERIFY(!r.cache.contains(second.draftId));
        QCOMPARE(r.cache.count(QStringLiteral("DRAFT")), draftsBefore);
        // Deleting one already gone is fine.
        done = false;
        r.sender->deleteDraft(second.draftId, [&](const Sender::Result &res) {
            del = res;
            done = true;
        });
        QTRY_VERIFY(done);
        QVERIFY(del.ok);
    }

    void largeDraftUsesSynchronizingLiteral()
    {
        Rig r;
        const QByteArray body = QByteArray(300'000, 'x') + "\r\n";
        const Sender::Result res = r.saveDraft(mail("Big draft", {}, body), {});
        QVERIFY(res.ok);
        QCOMPARE(r.imap.appended.size(), 1);
        QVERIFY(r.imap.appended.first().endsWith(body));
    }

    void sendQueuedFromMainWindow()
    {
        FakeImapServer imap;
        imap.seedDemo(4);
        FakeSmtpServer smtp(&imap);
        MemoryTokenStore store;
        store.values.insert(AppPasswordAccount::appPasswordKey(imap.user), imap.password);
        AppPasswordAccount a;
        a.email = imap.user;
        a.imapHost = QStringLiteral("127.0.0.1");
        a.imapPort = imap.port();
        a.smtpHost = QStringLiteral("127.0.0.1");
        a.smtpPort = smtp.port();
        {
            QSettings s;
            a.save(s);
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
        QVERIFY(qobject_cast<SmtpSender *>(session.sender()));
        QSignalSpy idle(session.sync(), &SyncEngine::idle);
        QVERIFY(idle.wait(10000));

        // A draft that was queued: once sent, the draft goes too.
        Sender::Result draft;
        bool saved = false;
        session.sender()->saveDraft(mail("Queued: invoice follow-up"), {}, {}, [&](const Sender::Result &r) {
            draft = r;
            saved = true;
        });
        QTRY_VERIFY(saved);
        QVERIFY(draft.ok);
        for (const QByteArray &subject : {QByteArray("Queued: invoice follow-up"), QByteArray("Queued: site visit")}) {
            MailCache::QueuedMessage q;
            q.mime = mail(subject);
            q.subject = QString::fromUtf8(subject);
            q.to = QStringLiteral("priya@northwind.example");
            q.draftId = subject.contains("invoice") ? draft.draftId : QString();
            session.cache()->addQueued(q);
        }
        QCOMPARE(session.cache()->queued().size(), 2);
        auto *sendQueued = w.findChild<QAction *>(QStringLiteral("actionSendQueued"));
        QVERIFY(sendQueued);
        sendQueued->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(smtp.delivered.size(), 2, 10000);
        QTRY_VERIFY(session.cache()->queued().isEmpty());
        QTRY_VERIFY(imap.find(draft.draftId.toULongLong(nullptr, 16))->gone);
        QSettings().clear();
    }
};

QTEST_MAIN(TstSmtp)
#include "tst_smtp.moc"
