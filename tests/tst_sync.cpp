// Gmail sync against the mock API: labels, initial INBOX metadata, paging,
// history paging, 404 -> full resync, 429/5xx backoff, read + mark-as-read,
// new-mail signal, FTS5 cache. No live Google calls.
#include "LogCapture.h"
#include "core/AuthManager.h"
#include "core/GmailClient.h"
#include "core/MailCache.h"
#include "core/MessageParser.h"
#include "core/SyncEngine.h"
#include "core/TokenStore.h"
#include "mock/MockGoogle.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <sys/stat.h>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <algorithm>
#include <memory>

using namespace zmail;
using zmail::test::MockGoogle;

namespace {
struct Rig
{
    MockGoogle g;
    MemoryTokenStore store;
    QNetworkAccessManager nam;
    std::unique_ptr<AuthManager> auth;
    std::unique_ptr<GmailClient> api;
    MailCache cache;
    std::unique_ptr<SyncEngine> sync;

    explicit Rig(int initial = 500)
    {
        g.listen();
        store.values.insert(QStringLiteral("refresh-token:") + g.email, QString::fromLatin1(MockGoogle::kRefreshToken));
        auth = std::make_unique<AuthManager>(g.clientConfig(), &store, &nam);
        bool restored = false;
        auth->restore(g.email, [&restored](bool, const QString &) { restored = true; }); // keyring reads are async
        (void)QTest::qWaitFor([&restored] { return restored; }, 5000);
        api = std::make_unique<GmailClient>(auth.get(), &nam, g.apiBase());
        api->setBackoffBaseMs(5);
        cache.open(QStringLiteral(":memory:"));
        sync = std::make_unique<SyncEngine>(api.get(), &cache);
        sync->setInitialCount(initial);
        sync->setPollInterval(3600 * 1000); // tests poll by hand
    }
    MockGoogle::Message msg(const QString &subject, QStringList labels = {QStringLiteral("INBOX"), QStringLiteral("UNREAD")},
                            int minutesAgo = 0)
    {
        MockGoogle::Message m;
        m.from = QStringLiteral("Ada Lovelace <ada@example.org>");
        m.to = QStringLiteral("demo.user@example.com");
        m.subject = subject;
        m.text = QStringLiteral("Body of ") + subject;
        m.labels = labels;
        m.date = QDateTime::currentDateTimeUtc().addSecs(-60 * minutesAgo);
        return m;
    }
    bool waitIdle(int ms = 15000)
    {
        QSignalSpy idle(sync.get(), &SyncEngine::idle);
        return idle.wait(ms) || !sync->isBusy();
    }
};
} // namespace

class TstSync : public QObject
{
    Q_OBJECT

private slots:
    void fts5IsAvailable()
    {
        MailCache c;
        QVERIFY(c.open(QStringLiteral(":memory:")));
        QVERIFY2(c.hasFts5(), "SQLite was built without FTS5");
        CachedMessage m;
        m.id = QStringLiteral("a1");
        m.subject = QStringLiteral("Fishing trip in November");
        m.fromName = QStringLiteral("Marcus");
        m.labels = {QStringLiteral("INBOX")};
        c.upsert(m);
        m.id = QStringLiteral("a2");
        m.subject = QStringLiteral("Budget review");
        c.upsert(m);
        QCOMPARE(c.search(QStringLiteral("fishing")), QStringList{QStringLiteral("a1")});
        c.setBody(QStringLiteral("a2"), QStringLiteral("the contractor estimate"), {}, {});
        QCOMPARE(c.search(QStringLiteral("contractor")), QStringList{QStringLiteral("a2")});
        c.remove(QStringLiteral("a1"));
        QVERIFY(c.search(QStringLiteral("fishing")).isEmpty());
        QCOMPARE(c.count(QStringLiteral("INBOX")), 1);
    }

    void cacheFileIsPrivate()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("zmail/demo@example.com/zmail.db"));
        {
            MailCache c;
            QVERIFY(c.open(path));
            c.setHistoryId(42);
            QCOMPARE(c.historyId(), 42);
        }
        struct stat st{};
        QCOMPARE(::stat(QFile::encodeName(path).constData(), &st), 0);
        QCOMPARE(int(st.st_mode & 0777), 0600);
        QCOMPARE(::stat(QFile::encodeName(QFileInfo(path).absolutePath()).constData(), &st), 0);
        QCOMPARE(int(st.st_mode & 0777), 0700);
        MailCache again;
        QVERIFY(again.open(path));
        QCOMPARE(again.historyId(), 42); // persisted
    }

    void labelsAndInitialInboxSync()
    {
        Rig r(25);
        r.g.seedSystemLabels();
        r.g.addLabel({QStringLiteral("Label_9"), QStringLiteral("Receipts"), QStringLiteral("user"), QStringLiteral("#e08e0b")});
        for (int i = 0; i < 60; ++i) {
            r.g.addMessage(r.msg(QStringLiteral("Message %1").arg(i), {QStringLiteral("INBOX")}, i), false);
        }
        r.g.addMessage(r.msg(QStringLiteral("Sent one"), {QStringLiteral("SENT")}), false);
        r.sync->start();
        QTRY_VERIFY_WITH_TIMEOUT(r.cache.historyId() > 0 && !r.sync->isBusy(), 20000);

        QList<CachedLabel> labels = r.cache.labels();
        QVERIFY(std::any_of(labels.begin(), labels.end(), [](const CachedLabel &l) {
            return l.id == QLatin1String("Label_9") && l.name == QLatin1String("Receipts") && l.color == QLatin1String("#e08e0b");
        }));
        QCOMPARE(r.cache.count(QStringLiteral("INBOX")), 25); // only the newest N
        const QList<CachedMessage> inbox = r.cache.messages(QStringLiteral("INBOX"));
        QCOMPARE(inbox.first().subject, QStringLiteral("Message 0"));
        QCOMPARE(inbox.first().fromName, QStringLiteral("Ada Lovelace"));
        QCOMPARE(inbox.first().fromAddr, QStringLiteral("ada@example.org"));
        QVERIFY(inbox.first().internalDateMs > 0);
        QVERIFY(inbox.first().size > 0);
        QVERIFY(!inbox.first().snippet.isEmpty());
        QCOMPARE(r.g.count(QStringLiteral("GET /gmail/v1/users/me/messages/")), 25);

        // Scrolling: the next page.
        QVERIFY(r.sync->hasMore(QStringLiteral("INBOX")));
        r.sync->fetchMore(QStringLiteral("INBOX"));
        QTRY_COMPARE_WITH_TIMEOUT(r.cache.count(QStringLiteral("INBOX")), 60, 20000);
        QVERIFY(!r.sync->hasMore(QStringLiteral("INBOX")));

        // Another mailbox is fetched when selected.
        r.sync->ensureLabel(QStringLiteral("SENT"));
        QTRY_COMPARE_WITH_TIMEOUT(r.cache.count(QStringLiteral("SENT")), 1, 10000);
    }

    void historyPagingAndNewMail()
    {
        Rig r;
        r.g.seedSystemLabels();
        r.g.historyPageSize = 2; // force several history pages
        r.g.addMessage(r.msg(QStringLiteral("old"), {QStringLiteral("INBOX")}, 100), false);
        const QString gone = r.g.addMessage(r.msg(QStringLiteral("to be deleted"), {QStringLiteral("INBOX")}, 90), false);
        const QString readMe = r.g.addMessage(r.msg(QStringLiteral("read elsewhere"), {QStringLiteral("INBOX"), QStringLiteral("UNREAD")}, 80), false);
        r.sync->start();
        QTRY_VERIFY_WITH_TIMEOUT(r.cache.count(QStringLiteral("INBOX")) == 3 && !r.sync->isBusy(), 20000);
        const qint64 before = r.cache.historyId();

        QSignalSpy newMail(r.sync.get(), &SyncEngine::newMail);
        const QString n1 = r.g.addMessage(r.msg(QStringLiteral("new 1")));
        const QString n2 = r.g.addMessage(r.msg(QStringLiteral("new 2")));
        r.g.addMessage(r.msg(QStringLiteral("newsletter, already read"), {QStringLiteral("INBOX")}));
        r.g.addMessage(r.msg(QStringLiteral("my reply"), {QStringLiteral("SENT")}));
        r.g.deleteMessage(gone);
        r.g.setMessageLabels(readMe, {}, {QStringLiteral("UNREAD")});
        r.sync->pollNow(true);
        QTRY_COMPARE_WITH_TIMEOUT(newMail.count(), 1, 20000);
        QStringList ids = newMail.first().first().toStringList();
        ids.sort();
        QStringList want{n1, n2};
        want.sort();
        QCOMPARE(ids, want); // unread INBOX only; not SENT, not already-read
        QVERIFY(r.g.count(QStringLiteral("GET /gmail/v1/users/me/history")) >= 3); // paged
        QVERIFY(!r.cache.contains(gone));
        QVERIFY(!r.cache.message(readMe).unread());
        QCOMPARE(r.cache.historyId(), r.g.historyId());
        QVERIFY(r.cache.historyId() > before);
        QCOMPARE(r.sync->fullSyncs(), 1);

        // Nothing new: no signal, no re-fetch.
        const int gets = r.g.count(QStringLiteral("GET /gmail/v1/users/me/messages/"));
        r.sync->pollNow(true);
        QTRY_VERIFY(!r.sync->isBusy());
        QTest::qWait(50);
        QCOMPARE(newMail.count(), 1);
        QCOMPARE(r.g.count(QStringLiteral("GET /gmail/v1/users/me/messages/")), gets);
    }

    void historyTooOldTriggersFullResync()
    {
        Rig r;
        r.g.seedSystemLabels();
        r.g.addMessage(r.msg(QStringLiteral("a"), {QStringLiteral("INBOX")}, 10), false);
        r.sync->start();
        QTRY_VERIFY_WITH_TIMEOUT(r.cache.count(QStringLiteral("INBOX")) == 1 && !r.sync->isBusy(), 20000);
        r.g.addMessage(r.msg(QStringLiteral("b"), {QStringLiteral("INBOX")}, 5), false);
        r.g.expireHistory(); // cached historyId is now older than Gmail keeps
        QSignalSpy resync(r.sync.get(), &SyncEngine::fullResyncStarted);
        r.sync->pollNow(true);
        QTRY_COMPARE_WITH_TIMEOUT(resync.count(), 1, 20000);
        QTRY_VERIFY_WITH_TIMEOUT(r.cache.count(QStringLiteral("INBOX")) == 2 && !r.sync->isBusy(), 20000);
        QCOMPARE(r.sync->fullSyncs(), 2);
        QCOMPARE(r.cache.historyId(), r.g.historyId());
    }

    void backoffOn429And5xx()
    {
        Rig r;
        r.g.seedSystemLabels();
        r.g.addFault({QStringLiteral("/gmail/v1/users/me/labels"), 429, 2, 0});
        r.g.addFault({QStringLiteral("/gmail/v1/users/me/profile"), 503, 1, -1});
        r.g.addMessage(r.msg(QStringLiteral("x"), {QStringLiteral("INBOX")}), false);
        r.sync->start();
        QTRY_VERIFY_WITH_TIMEOUT(r.cache.count(QStringLiteral("INBOX")) == 1 && !r.sync->isBusy(), 20000);
        QVERIFY(r.g.count(QStringLiteral("GET /gmail/v1/users/me/labels")) >= 3);
        QCOMPARE(r.g.count(QStringLiteral("GET /gmail/v1/users/me/profile")), 2);
        QVERIFY(r.api->retries() >= 3);
    }

    void giveUpAfterMaxAttempts()
    {
        Rig r;
        r.api->setMaxAttempts(3);
        r.g.addFault({QStringLiteral("/gmail/v1/users/me/profile"), 500, 100, -1});
        ApiError got;
        bool done = false;
        r.api->getProfile([&](const QJsonObject &, const ApiError &e) { got = e; done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
        QVERIFY(got.isError);
        QCOMPARE(got.httpStatus, 500);
        QCOMPARE(r.g.count(QStringLiteral("GET /gmail/v1/users/me/profile")), 3);
    }

    void expiredAccessTokenIsRefreshedOn401()
    {
        Rig r;
        bool done = false;
        r.api->getProfile([&](const QJsonObject &, const ApiError &) { done = true; });
        QTRY_VERIFY(done);
        QCOMPARE(r.g.tokensIssued, 1);
        // Google invalidated the access token early; the client should
        // refresh once and retry instead of failing.
        r.g.addFault({QStringLiteral("/gmail/v1/users/me/profile"), 401, 1, -1});
        ApiError err;
        done = false;
        r.api->getProfile([&](const QJsonObject &, const ApiError &e) { err = e; done = true; });
        QTRY_VERIFY(done);
        QVERIFY(!err.isError);
        QCOMPARE(r.g.tokensIssued, 2);
    }

    void readFullMessageAndMarkRead()
    {
        Rig r;
        r.g.seedSystemLabels();
        auto m = r.msg(QStringLiteral("HTML one"));
        m.html = QStringLiteral("<p>Hello <b>there</b> \u00e9</p><img src='https://tracker.example/p.gif'>");
        m.text = QStringLiteral("Hello there \u00e9");
        m.attachments = {QStringLiteral("report.pdf")};
        const QString id = r.g.addMessage(m, false);
        r.sync->start();
        QTRY_VERIFY_WITH_TIMEOUT(r.cache.contains(id) && !r.sync->isBusy(), 20000);
        QVERIFY(r.cache.message(id).unread());

        CachedMessage full;
        r.sync->fetchBody(id, [&](const CachedMessage &c, const QString &) { full = c; });
        QTRY_VERIFY(full.hasBody);
        QCOMPARE(full.bodyText, QStringLiteral("Hello there \u00e9"));
        QVERIFY(full.bodyHtml.contains(QStringLiteral("<b>there</b>")));
        QCOMPARE(full.attachments, QStringList{QStringLiteral("report.pdf")});
        QCOMPARE(r.g.count(QStringLiteral("GET /gmail/v1/users/me/messages/") + id), 2); // metadata + full

        // Cached: no second fetch.
        bool again = false;
        r.sync->fetchBody(id, [&](const CachedMessage &c, const QString &) { again = c.hasBody; });
        QVERIFY(again);

        r.sync->markRead(id);
        QVERIFY(!r.cache.message(id).unread()); // optimistic
        QTRY_COMPARE(r.g.modifyCalls, QStringList{id + QStringLiteral(":-UNREAD")});
        QVERIFY(!r.g.messages().value(id).labels.contains(QStringLiteral("UNREAD")));
    }

    void trashIsOptimisticAndCcIsCached()
    {
        Rig r;
        r.g.seedSystemLabels();
        auto m = r.msg(QStringLiteral("Copy me"));
        m.cc = QStringLiteral("Grace Hopper <grace@example.org>");
        const QString id = r.g.addMessage(m, false);
        r.sync->start();
        QTRY_VERIFY_WITH_TIMEOUT(r.cache.contains(id) && !r.sync->isBusy(), 20000);
        QCOMPARE(r.cache.message(id).cc, m.cc);

        QSignalSpy changed(r.sync.get(), &SyncEngine::messagesChanged);
        r.sync->trash(id);
        QVERIFY(r.cache.message(id).labels.contains(QStringLiteral("TRASH"))); // optimistic
        QVERIFY(!r.cache.message(id).labels.contains(QStringLiteral("INBOX")));
        QVERIFY(changed.count() >= 1);
        QTRY_COMPARE(r.g.trashCalls, QStringList{id});
        QTRY_VERIFY(r.g.messages().value(id).labels.contains(QStringLiteral("TRASH")));

        // Gmail refuses (404): the optimistic move is rolled back.
        CachedMessage local;
        local.id = QStringLiteral("gone-at-google");
        local.subject = QStringLiteral("Deleted elsewhere");
        local.labels = {QStringLiteral("INBOX")};
        r.cache.upsert(local);
        r.sync->trash(local.id);
        QVERIFY(r.cache.message(local.id).labels.contains(QStringLiteral("TRASH")));
        QTRY_COMPARE(r.cache.message(local.id).labels, QStringList{QStringLiteral("INBOX")});
    }

    void parserHandlesCharsetsAndAddresses()
    {
        QCOMPARE(MessageParser::splitAddress(QStringLiteral("\"Raman, Priya\" <priya@example.com>")),
                 qMakePair(QStringLiteral("Raman, Priya"), QStringLiteral("priya@example.com")));
        QCOMPARE(MessageParser::splitAddress(QStringLiteral("bare@example.com")),
                 qMakePair(QStringLiteral("bare@example.com"), QStringLiteral("bare@example.com")));
        const QByteArray latin1 = QByteArray("Caf\xe9");
        QJsonObject part{{QStringLiteral("mimeType"), QStringLiteral("text/plain")},
                         {QStringLiteral("headers"), QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("Content-Type")},
                                                                            {QStringLiteral("value"), QStringLiteral("text/plain; charset=\"ISO-8859-1\"")}}}},
                         {QStringLiteral("body"), QJsonObject{{QStringLiteral("data"), QString::fromLatin1(latin1.toBase64(QByteArray::Base64UrlEncoding))}}}};
        const auto body = MessageParser::bodyFromFull({{QStringLiteral("payload"), part}});
        QCOMPARE(body.text, QStringLiteral("Caf\u00e9"));
    }

    void rateLimiterSpacesRequests()
    {
        Rig r;
        r.api->setQuota(60 * 1000 / 10 * 5, 10); // burst 10 units, then 1 unit / 2 ms
        int done = 0;
        QElapsedTimer t;
        t.start();
        for (int i = 0; i < 6; ++i) { // 6 x 5 units = 30 units; 10 burst + 20 at ~0.42/ms
            r.api->getMessageMetadata(QStringLiteral("missing"), [&](const QJsonObject &, const ApiError &) { ++done; });
        }
        QTRY_COMPARE_WITH_TIMEOUT(done, 6, 10000);
        QVERIFY2(t.elapsed() >= 30, qPrintable(QString::number(t.elapsed())));
    }

    void inFlightCapPreventsParallelFlood()
    {
        Rig r;
        r.api->setMaxInFlight(2);
        r.api->setMaxSendsPerPump(1);
        r.g.seedSystemLabels();
        for (int i = 0; i < 20; ++i) {
            r.g.addLabel({QStringLiteral("Label_%1").arg(100 + i), QStringLiteral("L%1").arg(i),
                          QStringLiteral("user"), {}});
        }
        int done = 0;
        int peak = 0;
        QTimer sampler;
        QObject::connect(&sampler, &QTimer::timeout, &sampler, [&] {
            peak = std::max(peak, r.api->inFlight());
        });
        sampler.start(0);
        // Fire many 1-unit GETs the way refreshLabels used to (all at once).
        for (int i = 0; i < 20; ++i) {
            r.api->getLabel(QStringLiteral("Label_%1").arg(100 + i),
                            [&](const QJsonObject &, const ApiError &) { ++done; });
        }
        QTRY_COMPARE_WITH_TIMEOUT(done, 20, 20000);
        sampler.stop();
        peak = std::max(peak, r.api->inFlight());
        QVERIFY2(peak <= 2, qPrintable(QStringLiteral("peak inFlight=%1").arg(peak)));
        QVERIFY(peak >= 1);
        QCOMPARE(r.api->inFlight(), 0);
    }

    void labelsGet429RetriesWithCooldown()
    {
        Rig r;
        r.api->setMaxInFlight(2);
        r.api->setBackoffBaseMs(20);
        r.g.seedSystemLabels();
        // First three GETs under /labels/ fail with 429 (+ Retry-After); then succeed.
        r.g.addFault({QStringLiteral("/gmail/v1/users/me/labels/"), 429, 3, 0});
        int done = 0;
        int errors = 0;
        for (int i = 0; i < 3; ++i) {
            r.api->getLabel(QStringLiteral("INBOX"), [&](const QJsonObject &, const ApiError &e) {
                ++done;
                if (e.isError) {
                    ++errors;
                }
            });
        }
        QTRY_COMPARE_WITH_TIMEOUT(done, 3, 20000);
        QCOMPARE(errors, 0);
        QVERIFY(r.api->retries() >= 3);
        // list + gets: at least the three successes plus three 429s
        QVERIFY(r.g.count(QStringLiteral("GET /gmail/v1/users/me/labels/INBOX")) >= 6);
    }
};

QTEST_MAIN(TstSync)
#include "tst_sync.moc"
