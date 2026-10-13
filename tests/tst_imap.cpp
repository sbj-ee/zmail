// "Gmail (app password)" account, part 2: the IMAP read path against an
// in-process Gmail-flavoured fake (tests/mock/FakeImapServer). Made-up
// passwords only; nothing talks to Google.
#include "LogCapture.h"
#include "core/AppPasswordAccount.h"
#include "core/ImapClient.h"
#include "core/ImapSyncEngine.h"
#include "core/MailCache.h"
#include "core/MailSession.h"
#include "core/MessageParser.h"
#include "core/RawMime.h"
#include "core/TokenStore.h"
#include "mock/FakeImapServer.h"

#include <QSettings>
#include <QTimeZone>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>

using namespace zmail;
using zmail::test::FakeImapServer;

namespace {
const QString kFakePw = QStringLiteral("abcdefghijklmnop");

AppPasswordAccount accountFor(const FakeImapServer &s)
{
    AppPasswordAccount a;
    a.email = s.user;
    a.imapHost = QStringLiteral("127.0.0.1");
    a.imapPort = s.port();
    a.smtpHost = QStringLiteral("127.0.0.1");
    a.smtpPort = 1;
    return a;
}

struct Rig
{
    FakeImapServer server;
    MailCache cache;
    std::unique_ptr<ImapSyncEngine> sync;
    QString password = kFakePw;
    int passwordReads = 0;

    explicit Rig(int seed = 12, bool idle = true, bool condstore = true)
    {
        server.idleCapable = idle;
        server.condstoreCapable = condstore;
        server.seedDemo(seed);
        cache.open(QStringLiteral(":memory:"));
        sync = std::make_unique<ImapSyncEngine>(
            accountFor(server),
            [this](std::function<void(bool, const QString &, const QString &)> cb) {
                ++passwordReads;
                cb(true, password, {});
            },
            &cache);
        sync->setPlaintextForTests(true);
        sync->setPollInterval(3600 * 1000);
    }
    bool startAndSettle()
    {
        QSignalSpy idle(sync.get(), &SyncEngine::idle);
        sync->start();
        return idle.wait(10000);
    }
    bool idleWait() { return QTest::qWaitFor([this] { return server.anyIdling(); }, 5000); }
    QString idOf(quint64 msgid) const { return QString::number(msgid, 16); }
    quint64 lastMsgId(const QString &subject)
    {
        // Search the fake by subject.
        quint64 found = 0;
        for (quint64 id = 0x18f3a0c2b4d50001ULL; id < 0x18f3a0c2b4d50000ULL + 500; ++id) {
            if (auto *m = server.find(id); m && m->raw.contains(subject.toUtf8())) {
                found = id;
            }
        }
        return found;
    }
};
} // namespace

class TstImap : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("zmail-tst-imap"));
        QSettings().clear();
    }
    void cleanup() { QSettings().clear(); }

    void parsesResponsesWithLiterals()
    {
        const QByteArray resp = "* 3 FETCH (UID 17 X-GM-MSGID 1278455344230334865 X-GM-LABELS (\"\\\\Important\" Receipts "
                                "\"Caf&AOk-\") FLAGS (\\Seen) BODY[HEADER.FIELDS (SUBJECT)] {21}\r\nSubject: hi (there)\r\n)";
        int pos = 2;
        QList<ImapValue> v;
        QVERIFY(ImapClient::parseValues(resp, pos, v));
        QCOMPARE(v.size(), 3);
        QCOMPARE(v.at(1).text, QByteArray("FETCH"));
        const ImapValue &a = v.at(2);
        QVERIFY(a.isList());
        QCOMPARE(a.get("UID")->num(), 17);
        QCOMPARE(a.get("X-GM-LABELS")->items.size(), 3);
        QCOMPARE(a.get("X-GM-LABELS")->items.at(0).str(), QStringLiteral("\\Important"));
        QCOMPARE(ImapClient::decodeMailboxName(a.get("X-GM-LABELS")->items.at(2).text), QStringLiteral("Caf\u00e9"));
        QCOMPARE(a.get("BODY[HEADER.FIELDS (SUBJECT)]")->text, QByteArray("Subject: hi (there)\r\n"));
        QCOMPARE(ImapClient::encodeMailboxName(QStringLiteral("Caf\u00e9 & co")), QByteArray("Caf&AOk- &- co"));
        QCOMPARE(ImapClient::decodeMailboxName("Caf&AOk- &- co"), QStringLiteral("Caf\u00e9 & co"));
        QCOMPARE(ImapClient::quote(QStringLiteral("a\"b\\c")), QByteArray("\"a\\\"b\\\\c\""));
    }

    void rawMimeMatchesTheApiShape()
    {
        const QByteArray raw =
            "From: =?UTF-8?Q?Ren=C3=A9e?= <renee@example.org>\r\n"
            "Subject: =?UTF-8?B?Q2Fmw6k=?= =?UTF-8?B?IG1lbnU=?=\r\n"
            "Content-Type: multipart/mixed; boundary=\"outer\"\r\n\r\n"
            "--outer\r\nContent-Type: multipart/alternative; boundary=inner\r\n\r\n"
            "--inner\r\nContent-Type: text/plain; charset=iso-8859-1\r\nContent-Transfer-Encoding: quoted-printable\r\n\r\n"
            "Caf=E9 soft=\r\nbreak\r\n"
            "--inner\r\nContent-Type: text/html; charset=UTF-8\r\nContent-Transfer-Encoding: base64\r\n\r\n"
            + QByteArray("<p>Caf\xc3\xa9</p>").toBase64() + "\r\n--inner--\r\n"
            "--outer\r\nContent-Type: application/pdf\r\nContent-Disposition: attachment; filename*=UTF-8''men%C3%BC.pdf\r\n"
            "Content-Transfer-Encoding: base64\r\n\r\nJVBERi0=\r\n--outer--\r\n";
        QJsonObject json;
        json.insert(QStringLiteral("id"), QStringLiteral("abc"));
        json.insert(QStringLiteral("payload"), RawMime::toGmailPayload(raw));
        const CachedMessage m = MessageParser::fromMetadata(json);
        QCOMPARE(m.subject, QStringLiteral("Caf\u00e9 menu"));
        QCOMPARE(m.fromName, QStringLiteral("Ren\u00e9e"));
        const MessageParser::Body b = MessageParser::bodyFromFull(json);
        QCOMPARE(b.text.trimmed(), QStringLiteral("Caf\u00e9 softbreak"));
        QVERIFY(b.html.contains(QStringLiteral("Caf\u00e9")));
        QCOMPARE(b.attachments, QStringList{QStringLiteral("men\u00fc.pdf")});
    }

    void fetchResponsesMapToApiLabels()
    {
        const QByteArray resp = "* 1 FETCH (UID 5 X-GM-MSGID 255 X-GM-THRID 4095 X-GM-LABELS (\"\\\\Important\" \"\\\\Sent\" "
                                "Receipts) FLAGS (\\Flagged) INTERNALDATE \"12-Oct-2026 09:15:00 -0500\" RFC822.SIZE 1234)";
        int pos = 2;
        QList<ImapValue> v;
        QVERIFY(ImapClient::parseValues(resp, pos, v));
        const CachedMessage m = ImapSyncEngine::messageFromFetch(v.at(2), QStringLiteral("INBOX"));
        QCOMPARE(m.id, QStringLiteral("ff"));
        QCOMPARE(m.threadId, QStringLiteral("fff"));
        for (const char *l : {"IMPORTANT", "SENT", "Receipts", "STARRED", "UNREAD", "INBOX"}) {
            QVERIFY2(m.labels.contains(QLatin1String(l)), l);
        }
        QCOMPARE(m.size, 1234);
        QCOMPARE(m.internalDateMs, QDateTime(QDate(2026, 10, 12), QTime(14, 15), QTimeZone::utc()).toMSecsSinceEpoch());
        QCOMPARE(ImapSyncEngine::parseInternalDate(QStringLiteral(" 2-Jan-2026 00:00:00 +0000")),
                 QDateTime(QDate(2026, 1, 2), QTime(0, 0), QTimeZone::utc()).toMSecsSinceEpoch());
        QCOMPARE(ImapSyncEngine::labelIdForAttributes({QStringLiteral("\\HasNoChildren"), QStringLiteral("\\Junk")},
                                                      QStringLiteral("[Gmail]/Spam")),
                 QStringLiteral("SPAM"));
    }

    void fullSyncListsFoldersCountsAndInbox()
    {
        Rig r(12);
        LogCapture log;
        QVERIFY(r.startAndSettle());
        QStringList ids;
        for (const CachedLabel &l : r.cache.labels()) {
            ids.append(l.id);
        }
        for (const char *want : {"INBOX", "SENT", "DRAFT", "TRASH", "SPAM", "STARRED", "IMPORTANT", "Receipts",
                                 "Projects/Substation", "Notifications"}) {
            QVERIFY2(ids.contains(QLatin1String(want)), want);
        }
        QVERIFY(!ids.contains(QStringLiteral("[Gmail]")));
        QVERIFY(!ids.contains(QStringLiteral("ALL")));
        int inboxTotal = -1;
        int inboxUnread = -1;
        for (const CachedLabel &l : r.cache.labels()) {
            if (l.id == QLatin1String("INBOX")) {
                inboxTotal = l.total;
                inboxUnread = l.unread;
            }
        }
        QCOMPARE(inboxTotal, 12);
        QCOMPARE(inboxUnread, 4);
        QCOMPARE(r.cache.mailboxTotal(), 13); // + the Sent message
        QCOMPARE(r.cache.count(QStringLiteral("INBOX")), 12);
        const QList<CachedMessage> inbox = r.cache.messages(QStringLiteral("INBOX"));
        QVERIFY(!inbox.isEmpty());
        QVERIFY(!inbox.first().subject.isEmpty());
        QVERIFY(r.idleWait());
        QVERIFY(!log.all().contains(kFakePw));
        QVERIFY(!r.server.commands.join('\n').contains(kFakePw.toUtf8()));
        QCOMPARE(r.sync->fullSyncs(), 1);
    }

    void idleBringsNewMailWithoutPolling()
    {
        Rig r(6);
        QVERIFY(r.startAndSettle());
        QTRY_VERIFY(r.server.anyIdling());
        QSignalSpy newMail(r.sync.get(), &SyncEngine::newMail);
        const quint64 id = r.server.deliver(FakeImapServer::makeMessage(QStringLiteral("Nia"), QStringLiteral("nia@example.org"),
                                                                         QStringLiteral("Fresh one"), QStringLiteral("hello")));
        QVERIFY(newMail.wait(5000));
        QCOMPARE(newMail.first().first().toStringList(), QStringList{r.idOf(id)});
        const CachedMessage m = r.cache.summary(r.idOf(id));
        QCOMPARE(m.subject, QStringLiteral("Fresh one"));
        QVERIFY(m.labels.contains(QStringLiteral("INBOX")));
        QVERIFY(m.unread());
        QTRY_VERIFY(r.server.anyIdling()); // back to IDLE afterwards
    }

    void condstoreCarriesFlagAndLabelChanges()
    {
        Rig r(6);
        QVERIFY(r.startAndSettle());
        QTRY_VERIFY(r.server.anyIdling());
        const quint64 id = r.lastMsgId(QStringLiteral("Q4 field schedule"));
        QVERIFY(id);
        // Read elsewhere: \Seen arrives through CHANGEDSINCE.
        auto *m = r.server.find(id);
        QSet<QString> flags = m->flags;
        const bool wasUnread = !flags.contains(QStringLiteral("\\Seen"));
        flags.insert(QStringLiteral("\\Seen"));
        flags.insert(QStringLiteral("\\Flagged"));
        QSignalSpy changed(r.sync.get(), &SyncEngine::messagesChanged);
        r.server.setFlags(id, flags);
        QTRY_VERIFY(r.cache.summary(r.idOf(id)).labels.contains(QStringLiteral("STARRED")));
        QVERIFY(!r.cache.summary(r.idOf(id)).unread());
        Q_UNUSED(wasUnread)
        bool sawChangedSince = false;
        for (const QByteArray &c : r.server.commands) {
            sawChangedSince = sawChangedSince || c.contains("CHANGEDSINCE");
        }
        QVERIFY(sawChangedSince);

        // Archived elsewhere: it leaves the Inbox.
        QTRY_VERIFY(r.server.anyIdling());
        r.server.setLabels(id, {QStringLiteral("Receipts")});
        QTRY_VERIFY(!r.cache.summary(r.idOf(id)).labels.contains(QStringLiteral("INBOX")));
    }

    void bodyFetchedWithPeek()
    {
        Rig r(6);
        QVERIFY(r.startAndSettle());
        const quint64 id = r.lastMsgId(QStringLiteral("Q4 field schedule")); // i=0: has HTML
        QVERIFY(id);
        const QSet<QString> before = r.server.find(id)->flags;
        CachedMessage got;
        QString error;
        bool done = false;
        r.sync->fetchBody(r.idOf(id), [&](const CachedMessage &m, const QString &e) {
            got = m;
            error = e;
            done = true;
        });
        QTRY_VERIFY(done);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(got.hasBody);
        QVERIFY(got.bodyHtml.contains(QStringLiteral("draft schedule")));
        QVERIFY(got.bodyText.contains(QStringLiteral("draft schedule")));
        QVERIFY(!got.messageIdHeader.isEmpty());
        QVERIFY(!got.snippet.isEmpty());
        QCOMPARE(r.server.find(id)->flags, before); // BODY.PEEK: still unread on the server
        bool peek = false;
        for (const QByteArray &c : r.server.commands) {
            peek = peek || c.contains("BODY.PEEK[]");
        }
        QVERIFY(peek);
        // Cached now: no second round trip.
        const int commands = int(r.server.commands.size());
        done = false;
        r.sync->fetchBody(r.idOf(id), [&](const CachedMessage &, const QString &) { done = true; });
        QVERIFY(done);
        QCOMPARE(int(r.server.commands.size()), commands);
    }

    void otherFoldersAndPaging()
    {
        Rig r(30);
        r.sync->setInitialCount(10);
        r.sync->setPageSize(10);
        QVERIFY(r.startAndSettle());
        QCOMPARE(r.cache.count(QStringLiteral("INBOX")), 10);
        QVERIFY(r.sync->hasMore(QStringLiteral("INBOX")));
        QSignalSpy changed(r.sync.get(), &SyncEngine::messagesChanged);
        r.sync->fetchMore(QStringLiteral("INBOX"));
        QTRY_COMPARE(r.cache.count(QStringLiteral("INBOX")), 20);
        QSignalSpy finished(r.sync.get(), &SyncEngine::loadAllFinished);
        r.sync->loadAll(QStringLiteral("INBOX"));
        QVERIFY(finished.wait(10000));
        QCOMPARE(finished.first().at(2).toBool(), true);
        QCOMPARE(r.cache.count(QStringLiteral("INBOX")), 30);
        QVERIFY(!r.sync->hasMore(QStringLiteral("INBOX")));

        r.sync->ensureLabel(QStringLiteral("SENT"));
        QTRY_COMPARE(r.cache.count(QStringLiteral("SENT")), 1);
        r.sync->ensureLabel(QStringLiteral("Projects/Substation"));
        QTRY_VERIFY(r.cache.count(QStringLiteral("Projects/Substation")) > 0);
        QTRY_VERIFY(r.server.anyIdling());
    }

    void reconnectsAfterDrop()
    {
        Rig r(4);
        QVERIFY(r.startAndSettle());
        QTRY_VERIFY(r.server.anyIdling());
        QCOMPARE(r.server.logins, 1);
        r.server.dropConnections();
        QTRY_COMPARE_WITH_TIMEOUT(r.server.logins, 2, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(r.server.anyIdling(), 10000);
        QCOMPARE(r.sync->fullSyncs(), 1); // incremental catch-up, not a full resync
        QSignalSpy newMail(r.sync.get(), &SyncEngine::newMail);
        r.server.deliver(FakeImapServer::makeMessage(QStringLiteral("Nia"), QStringLiteral("nia@example.org"),
                                                     QStringLiteral("After the drop"), QStringLiteral("hi")));
        QVERIFY(newMail.wait(5000));
    }

    void pollsWhenThereIsNoIdle()
    {
        Rig r(4, /*idle=*/false, /*condstore=*/false);
        r.sync->setPollInterval(1000);
        QVERIFY(r.startAndSettle());
        QSignalSpy newMail(r.sync.get(), &SyncEngine::newMail);
        r.server.deliver(FakeImapServer::makeMessage(QStringLiteral("Nia"), QStringLiteral("nia@example.org"),
                                                     QStringLiteral("Polled"), QStringLiteral("hi")));
        QVERIFY(newMail.wait(5000));
        QVERIFY(!r.server.commands.join('\n').contains("IDLE"));
    }

    void refusedPasswordIsReported()
    {
        Rig r(2);
        r.password = QStringLiteral("zzzzzzzzzzzzzzzz"); // made up, and wrong
        QSignalSpy err(r.sync.get(), &SyncEngine::syncError);
        r.sync->start();
        QVERIFY(err.wait(5000));
        QVERIFY(err.first().first().toString().contains(QStringLiteral("refused")));
        QVERIFY(!err.first().first().toString().contains(r.password));
    }

    void mutationsSayNotYet()
    {
        Rig r(3);
        QVERIFY(r.startAndSettle());
        QSignalSpy status(r.sync.get(), &SyncEngine::statusChanged);
        r.sync->moveToLabel(QStringLiteral("1"), QStringLiteral("Receipts"));
        QVERIFY(status.last().first().toString().contains(QStringLiteral("next zmail update")));
        QSignalSpy trashFailed(r.sync.get(), &SyncEngine::trashFailed);
        r.sync->trash(QStringLiteral("1"));
        QCOMPARE(trashFailed.count(), 1);
    }

    void sessionResumesAppPasswordAccount()
    {
        FakeImapServer server;
        server.seedDemo(5);
        MemoryTokenStore store;
        store.values.insert(AppPasswordAccount::appPasswordKey(server.user), kFakePw);
        {
            QSettings s;
            accountFor(server).save(s);
        }
        SessionOptions o;
        o.client.status = ClientConfig::LoadStatus::Missing; // no OAuth client at all
        o.store = &store;
        o.cachePathOverride = QStringLiteral(":memory:");
        o.contactsPathOverride = QStringLiteral(":memory:");
        o.imapPlaintextForTests = true;
        o.pollIntervalMs = 3600 * 1000;
        MailSession session(o);
        QCOMPARE(session.state(), MailSession::State::NeedsClient);
        session.restoreSaved();
        QCOMPARE(session.state(), MailSession::State::SignedIn);
        QCOMPARE(session.accountKind(), AccountKind::GmailAppPassword);
        QCOMPARE(session.account(), server.user);
        QVERIFY(qobject_cast<ImapSyncEngine *>(session.sync()));
        QVERIFY(!session.api());
        QVERIFY(!session.sender());
        QSignalSpy idle(session.sync(), &SyncEngine::idle);
        QVERIFY(idle.wait(10000));
        QCOMPARE(session.cache()->count(QStringLiteral("INBOX")), 5);

        session.signOut();
        QCOMPARE(session.state(), MailSession::State::NeedsClient);
        QTRY_VERIFY(!store.values.contains(AppPasswordAccount::appPasswordKey(server.user)));
        QCOMPARE(AppPasswordAccount::savedKind(QSettings()), AccountKind::GmailOAuth);
    }
};

QTEST_MAIN(TstImap)
#include "tst_imap.moc"
