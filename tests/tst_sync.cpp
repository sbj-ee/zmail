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
#include <QSqlDatabase>
#include <QSqlQuery>
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
    // Idle, including the follow-up history poll a full sync queues.
    bool settle()
    {
        for (int i = 0; i < 3; ++i) {
            if (!QTest::qWaitFor([this] { return !sync->isBusy(); }, 20000)) {
                return false;
            }
            QTest::qWait(30);
        }
        return !sync->isBusy();
    }
    bool waitIdle(int ms = 15000)
    {
        QSignalSpy idle(sync.get(), &SyncEngine::idle);
        return idle.wait(ms) || !sync->isBusy();
    }
};

// One value read through a second connection to a cache file: what is
// committed on disk, as another process would see it.
QVariant onDisk(const QString &path, const QString &sql)
{
    QVariant v;
    const QString name = QStringLiteral("tst-sync-peek");
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(path);
        if (db.open()) {
            QSqlQuery q(db);
            if (q.exec(sql) && q.next()) {
                v = q.value(0);
            }
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(name);
    return v;
}

bool runsOnDisk(const QString &path, const QString &sql)
{
    bool ok = false;
    const QString name = QStringLiteral("tst-sync-run");
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(path);
        if (db.open()) {
            QSqlQuery q(db);
            ok = q.exec(sql);
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(name);
    return ok;
}

CachedMessage cached(const QString &id, const QString &subject, qint64 dateMs)
{
    CachedMessage m;
    m.id = id;
    m.subject = subject;
    m.fromName = QStringLiteral("Ada Lovelace");
    m.fromAddr = QStringLiteral("ada@example.org");
    m.snippet = QStringLiteral("snippet of ") + subject;
    m.internalDateMs = dateMs;
    m.labels = {QStringLiteral("INBOX"), QStringLiteral("UNREAD")};
    return m;
}
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

    // The message list reads every row on each refresh: no bodies, and the
    // snooze state in the same query.
    void listingHasNoBodiesAndJoinsSnoozes()
    {
        MailCache c;
        QVERIFY(c.open(QStringLiteral(":memory:")));
        c.upsert(cached(QStringLiteral("old"), QStringLiteral("Oldest"), 1000));
        c.upsert(cached(QStringLiteral("mid"), QStringLiteral("Middle"), 2000));
        c.upsert(cached(QStringLiteral("new"), QStringLiteral("Newest"), 3000));
        c.setBody(QStringLiteral("mid"), QString(50'000, QLatin1Char('x')), QStringLiteral("<p>big</p>"),
                  {QStringLiteral("report.pdf")});
        c.setSnooze(QStringLiteral("old"), 9'999'999, true); // snoozed
        c.setSnooze(QStringLiteral("new"), 5, true);
        c.markSnoozeWoke(QStringLiteral("new")); // woke: badge only

        const QList<MailCache::Listed> rows = c.listing();
        QCOMPARE(rows.size(), 3);
        QCOMPARE(rows[0].message.id, QStringLiteral("new")); // newest first
        QCOMPARE(rows[1].message.id, QStringLiteral("mid"));
        QCOMPARE(rows[2].message.id, QStringLiteral("old"));
        QCOMPARE(rows[1].message.subject, QStringLiteral("Middle"));
        QCOMPARE(rows[1].message.snippet, QStringLiteral("snippet of Middle"));
        QCOMPARE(rows[1].message.labels, (QStringList{QStringLiteral("INBOX"), QStringLiteral("UNREAD")}));
        QVERIFY(rows[1].message.hasBody);
        QVERIFY(rows[1].message.hasAttachment);
        QCOMPARE(rows[1].message.attachments, QStringList{QStringLiteral("report.pdf")});
        QVERIFY(rows[1].message.bodyText.isEmpty()); // not loaded
        QVERIFY(rows[1].message.bodyHtml.isEmpty());
        QCOMPARE(c.message(QStringLiteral("mid")).bodyText.size(), 50'000); // still there on demand
        QCOMPARE(rows[1].snoozeWakeMs, 0);
        QVERIFY(!rows[1].snoozeBadge);
        QCOMPARE(rows[2].snoozeWakeMs, 9'999'999);
        QVERIFY(!rows[2].snoozeBadge);
        QCOMPARE(rows[0].snoozeWakeMs, 0);
        QVERIFY(rows[0].snoozeBadge);
        QCOMPARE(c.listing(2).size(), 2);
    }

    // Label edits and the engine's label checks read the row without its
    // bodies; behaviour is unchanged.
    void summaryAndLabelEditsSkipBodies()
    {
        MailCache c;
        QVERIFY(c.open(QStringLiteral(":memory:")));
        CachedMessage m = cached(QStringLiteral("a"), QStringLiteral("Fishing trip"), 1);
        m.messageIdHeader = QStringLiteral("<a@mock.example>");
        c.upsert(m);
        c.setBody(QStringLiteral("a"), QString(50'000, QLatin1Char('x')), QStringLiteral("<p>big</p>"), {});
        c.upsert(cached(QStringLiteral("b"), QStringLiteral("Budget"), 2));

        const CachedMessage s = c.summary(QStringLiteral("a"));
        QCOMPARE(s.id, QStringLiteral("a"));
        QCOMPARE(s.subject, QStringLiteral("Fishing trip"));
        QCOMPARE(s.messageIdHeader, QStringLiteral("<a@mock.example>"));
        QCOMPARE(s.labels, (QStringList{QStringLiteral("INBOX"), QStringLiteral("UNREAD")}));
        QVERIFY(s.unread());
        QVERIFY(s.hasBody);
        QVERIFY(s.bodyText.isEmpty());
        QVERIFY(s.bodyHtml.isEmpty());
        QVERIFY(c.summary(QStringLiteral("missing")).id.isEmpty());

        c.modifyLabels(QStringLiteral("a"), {QStringLiteral("Label_1"), QStringLiteral("INBOX")}, {QStringLiteral("UNREAD")});
        QCOMPARE(c.summary(QStringLiteral("a")).labels, (QStringList{QStringLiteral("INBOX"), QStringLiteral("Label_1")}));
        QCOMPARE(c.count(QStringLiteral("UNREAD")), 1); // only "b" now
        QCOMPARE(c.message(QStringLiteral("a")).bodyText.size(), 50'000); // body untouched
        c.modifyLabels(QStringLiteral("missing"), {QStringLiteral("INBOX")}, {});
        QVERIFY(!c.contains(QStringLiteral("missing")));
        QCOMPARE(c.count(), 2);

        QStringList inbox = c.messageIds(QStringLiteral("INBOX"));
        inbox.sort();
        QCOMPARE(inbox, (QStringList{QStringLiteral("a"), QStringLiteral("b")}));
        QCOMPARE(c.messageIds(QStringLiteral("Label_1")), QStringList{QStringLiteral("a")});
        QVERIFY(c.messageIds(QStringLiteral("Label_9")).isEmpty());
    }

    // Opening a message or changing a label must not wait behind a queue of
    // background sync fetches.
    void interactiveCallsJumpTheBackgroundQueue()
    {
        Rig r;
        r.api->setMaxInFlight(1);
        QStringList order;
        for (int i = 0; i < 8; ++i) {
            r.api->getMessageMetadata(QStringLiteral("bg%1").arg(i), [&order, i](const QJsonObject &, const ApiError &) {
                order << QStringLiteral("bg%1").arg(i);
            });
        }
        r.api->getMessageFull(QStringLiteral("open-me"), [&order](const QJsonObject &, const ApiError &) {
            order << QStringLiteral("full");
        });
        r.api->modifyLabels(QStringLiteral("open-me"), {}, {QStringLiteral("UNREAD")},
                            [&order](const QJsonObject &, const ApiError &) { order << QStringLiteral("modify"); });
        QTRY_COMPARE_WITH_TIMEOUT(order.size(), 10, 20000);
        // bg0 was already on the wire; then the interactive ones, in the order
        // asked; then the rest of the background ones, in theirs.
        QCOMPARE(order, (QStringList{QStringLiteral("bg0"), QStringLiteral("full"), QStringLiteral("modify"),
                                     QStringLiteral("bg1"), QStringLiteral("bg2"), QStringLiteral("bg3"),
                                     QStringLiteral("bg4"), QStringLiteral("bg5"), QStringLiteral("bg6"),
                                     QStringLiteral("bg7")}));
    }

    // Writes are grouped: nested begin()/commit() pairs reach the file as one
    // commit, and commits don't fsync (WAL + synchronous=NORMAL).
    void writesAreBatched()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("zmail.db"));
        MailCache c;
        QVERIFY(c.open(path));
        QCOMPARE(c.pragma(QStringLiteral("journal_mode")).toString(), QStringLiteral("wal"));
        QCOMPARE(c.pragma(QStringLiteral("synchronous")).toInt(), 1); // NORMAL

        const QString count = QStringLiteral("SELECT COUNT(*) FROM messages");
        QVERIFY(c.begin());
        c.upsert(cached(QStringLiteral("a"), QStringLiteral("A"), 1)); // a transaction of its own, nested
        {
            const MailCache::Batch inner(c);
            c.upsert(cached(QStringLiteral("b"), QStringLiteral("B"), 2));
            c.setLabels(QStringLiteral("a"), {QStringLiteral("SENT")});
        }
        QCOMPARE(c.count(), 2);                // this connection sees its own writes
        QCOMPARE(onDisk(path, count).toInt(), 0); // nothing committed by the inner pairs
        QVERIFY(c.commit());
        QCOMPARE(onDisk(path, count).toInt(), 2);
        QCOMPARE(onDisk(path, QStringLiteral("SELECT COUNT(*) FROM message_labels")).toInt(), 3);
        QVERIFY(!c.commit()); // nothing open

        // Outside a batch each call commits itself.
        c.upsert(cached(QStringLiteral("c"), QStringLiteral("C"), 3));
        QCOMPARE(onDisk(path, count).toInt(), 3);
    }

    // Marking read / moving / trashing only rewrites labels: the full-text
    // index (which holds the whole body) must not be rewritten for that.
    void labelChangeDoesNotReindex()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("zmail.db"));
        MailCache c;
        QVERIFY(c.open(path));
        QVERIFY(c.hasFts5());
        c.upsert(cached(QStringLiteral("a"), QStringLiteral("Fishing trip"), 1));
        c.setBody(QStringLiteral("a"), QStringLiteral("bring the waders and the coffee"), {}, {});
        c.upsert(cached(QStringLiteral("b"), QStringLiteral("Budget review"), 2));

        const QString indexState =
            QStringLiteral("SELECT group_concat(id || ':' || length(block), ',') FROM messages_fts_data");
        const QString before = onDisk(path, indexState).toString();
        QVERIFY(!before.isEmpty());
        c.setLabels(QStringLiteral("a"), {QStringLiteral("INBOX")}); // mark read
        c.modifyLabels(QStringLiteral("a"), {QStringLiteral("TRASH")}, {QStringLiteral("INBOX")});
        QCOMPARE(onDisk(path, indexState).toString(), before);
        QCOMPARE(c.search(QStringLiteral("waders")), QStringList{QStringLiteral("a")});

        // Indexed columns still re-index: new subject, new body.
        c.upsert(cached(QStringLiteral("a"), QStringLiteral("Canoe trip"), 1));
        QVERIFY(onDisk(path, indexState).toString() != before);
        QVERIFY(c.search(QStringLiteral("fishing")).isEmpty());
        QCOMPARE(c.search(QStringLiteral("canoe")), QStringList{QStringLiteral("a")});
        QCOMPARE(c.search(QStringLiteral("waders")), QStringList{QStringLiteral("a")}); // body kept
        c.setBody(QStringLiteral("a"), QStringLiteral("bring the paddles"), {}, {});
        QVERIFY(c.search(QStringLiteral("waders")).isEmpty());
        QCOMPARE(c.search(QStringLiteral("paddles")), QStringList{QStringLiteral("a")});
        c.remove(QStringLiteral("b"));
        QVERIFY(c.search(QStringLiteral("budget")).isEmpty());
        // The index still agrees with the table (the statement fails if it doesn't).
        QVERIFY(runsOnDisk(path, QStringLiteral("INSERT INTO messages_fts(messages_fts, rank) VALUES ('integrity-check', 1)")));
        QCOMPARE(onDisk(path, QStringLiteral("SELECT COUNT(*) FROM messages_fts WHERE messages_fts MATCH 'paddles'")).toInt(), 1);
    }

    // A cache written by an older zmail has the trigger on every UPDATE.
    void oldUpdateTriggerIsReplaced()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("zmail.db"));
        const QString triggerSql = QStringLiteral("SELECT sql FROM sqlite_master WHERE type = 'trigger' AND name = 'messages_au'");
        {
            MailCache c;
            QVERIFY(c.open(path));
            c.upsert(cached(QStringLiteral("a"), QStringLiteral("Fishing trip"), 1));
        }
        QVERIFY(runsOnDisk(path, QStringLiteral("DROP TRIGGER messages_au")));
        QVERIFY(runsOnDisk(path, QStringLiteral(
            "CREATE TRIGGER messages_au AFTER UPDATE ON messages BEGIN "
            "INSERT INTO messages_fts(messages_fts, rowid, subject, from_name, from_addr, to_addr, snippet, body_text) "
            "VALUES ('delete', old.rowid, old.subject, old.from_name, old.from_addr, old.to_addr, old.snippet, "
            "old.body_text); "
            "INSERT INTO messages_fts(rowid, subject, from_name, from_addr, to_addr, snippet, body_text) VALUES "
            "(new.rowid, new.subject, new.from_name, new.from_addr, new.to_addr, new.snippet, new.body_text); END")));
        QVERIFY(onDisk(path, triggerSql).toString().contains(QStringLiteral("AFTER UPDATE ON messages")));

        MailCache c;
        QVERIFY(c.open(path));
        QVERIFY(onDisk(path, triggerSql).toString().contains(QStringLiteral("AFTER UPDATE OF subject")));
        QCOMPARE(c.search(QStringLiteral("fishing")), QStringList{QStringLiteral("a")});
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

    // A message whose metadata fetch fails for good must not be skipped: the
    // poll leaves historyId alone so the next one replays the range.
    void failedFetchDoesNotAdvanceHistory()
    {
        Rig r;
        r.g.seedSystemLabels();
        r.g.addMessage(r.msg(QStringLiteral("old"), {QStringLiteral("INBOX")}, 100), false);
        r.sync->start();
        QTRY_VERIFY_WITH_TIMEOUT(r.cache.count(QStringLiteral("INBOX")) == 1, 20000);
        QVERIFY(r.settle());
        const qint64 before = r.cache.historyId();

        QSignalSpy newMail(r.sync.get(), &SyncEngine::newMail);
        QSignalSpy errors(r.sync.get(), &SyncEngine::syncError);
        r.api->setMaxAttempts(2);
        const QString ok = r.g.addMessage(r.msg(QStringLiteral("arrives")));
        const QString late = r.g.addMessage(r.msg(QStringLiteral("arrives late")));
        r.g.addFault({QStringLiteral("/gmail/v1/users/me/messages/") + late, 500, 2, -1}); // both attempts
        r.sync->pollNow(true);
        QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 1, 20000);
        QVERIFY(r.settle());
        QVERIFY(r.cache.contains(ok));
        QVERIFY(!r.cache.contains(late));
        QCOMPARE(r.cache.historyId(), before); // not advanced past the missing message
        QCOMPARE(newMail.count(), 1);
        QCOMPARE(newMail.first().first().toStringList(), QStringList{ok});

        // Next poll: the same range again, and this time the fetch works.
        r.sync->pollNow(true);
        QTRY_VERIFY_WITH_TIMEOUT(r.cache.contains(late), 20000);
        QVERIFY(r.settle());
        QCOMPARE(r.cache.historyId(), r.g.historyId());
        QCOMPARE(newMail.count(), 2); // only the late one; "arrives" isn't announced twice
        QCOMPARE(newMail.last().first().toStringList(), QStringList{late});
        QCOMPARE(errors.count(), 1);
    }

    // A page load cut off by a full resync used to leave its label marked
    // "loading" for good, so the folder never paged again.
    void fetchMoreWorksAfterResyncInterruptsOne()
    {
        Rig r(2);
        r.g.seedSystemLabels();
        for (int i = 0; i < 5; ++i) {
            r.g.addMessage(r.msg(QStringLiteral("Message %1").arg(i), {QStringLiteral("INBOX")}, i + 1), false);
        }
        r.sync->start();
        QTRY_VERIFY_WITH_TIMEOUT(r.cache.count(QStringLiteral("INBOX")) == 2, 20000);
        QVERIFY(r.settle());

        r.g.addMessage(r.msg(QStringLiteral("Message new"), {QStringLiteral("INBOX")}, 0), false);
        r.g.expireHistory();
        r.sync->pollNow(true);                      // history.list -> 404 -> full resync
        r.sync->fetchMore(QStringLiteral("INBOX")); // still in flight when that starts
        QTRY_COMPARE_WITH_TIMEOUT(r.sync->fullSyncs(), 2, 20000);
        QVERIFY(r.settle());
        QCOMPARE(r.cache.count(QStringLiteral("INBOX")), 2);

        QVERIFY(r.sync->hasMore(QStringLiteral("INBOX")));
        r.sync->fetchMore(QStringLiteral("INBOX"));
        QTRY_COMPARE_WITH_TIMEOUT(r.cache.count(QStringLiteral("INBOX")), 6, 20000);
    }

    // Same for stop() + start() on one engine.
    void fetchMoreWorksAfterRestartInterruptsOne()
    {
        Rig r(2);
        r.g.seedSystemLabels();
        for (int i = 0; i < 5; ++i) {
            r.g.addMessage(r.msg(QStringLiteral("Message %1").arg(i), {QStringLiteral("INBOX")}, i + 1), false);
        }
        r.sync->start();
        QTRY_VERIFY_WITH_TIMEOUT(r.cache.count(QStringLiteral("INBOX")) == 2, 20000);
        QVERIFY(r.settle());

        r.sync->fetchMore(QStringLiteral("INBOX"));
        r.sync->stop(); // its answer is dropped
        r.sync->start();
        QVERIFY(r.settle());
        r.sync->fetchMore(QStringLiteral("INBOX"));
        QTRY_COMPARE_WITH_TIMEOUT(r.cache.count(QStringLiteral("INBOX")), 5, 20000);
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
