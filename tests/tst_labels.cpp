// Labels as folders: message counts (labels.get), create/rename/delete folder,
// and moveToLabel (one folder per message: add the target, remove INBOX and
// every other user label; INBOX as the target moves it back).
#include "core/AuthManager.h"
#include "core/GmailClient.h"
#include "core/MailCache.h"
#include "core/SyncEngine.h"
#include "core/TokenStore.h"
#include "mock/MockGoogle.h"

#include <QNetworkAccessManager>
#include <QSignalSpy>
#include <QTest>
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

    Rig()
    {
        g.listen();
        store.values.insert(QStringLiteral("refresh-token:") + g.email,
                            QString::fromLatin1(MockGoogle::kRefreshToken));
        auth = std::make_unique<AuthManager>(g.clientConfig(), &store, &nam);
        bool restored = false;
        auth->restore(g.email, [&restored](bool, const QString &) { restored = true; });
        (void)QTest::qWaitFor([&restored] { return restored; }, 5000);
        api = std::make_unique<GmailClient>(auth.get(), &nam, g.apiBase());
        api->setBackoffBaseMs(5);
        cache.open(QStringLiteral(":memory:"));
        sync = std::make_unique<SyncEngine>(api.get(), &cache);
        sync->setInitialCount(50);
        sync->setPollInterval(3600 * 1000);
    }

    MockGoogle::Message msg(const QString &subject, QStringList labels, int minutesAgo = 0)
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
};
} // namespace

class TstLabels : public QObject
{
    Q_OBJECT
private slots:
    void countsComeFromLabelsGet()
    {
        Rig r;
        r.g.seedSystemLabels();
        r.g.addLabel({QStringLiteral("Label_9"), QStringLiteral("Receipts"), QStringLiteral("user"),
                      QStringLiteral("#e08e0b")});
        for (int i = 0; i < 5; ++i) {
            QStringList labs{QStringLiteral("INBOX"), QStringLiteral("Label_9")};
            if (i < 2) {
                labs << QStringLiteral("UNREAD");
            }
            r.g.addMessage(r.msg(QStringLiteral("R%1").arg(i), labs, i), false);
        }
        r.sync->start();
        QTRY_VERIFY_WITH_TIMEOUT(r.cache.historyId() > 0 && !r.sync->isBusy(), 20000);

        QVERIFY(r.g.count(QStringLiteral("GET /gmail/v1/users/me/labels")) >= 1);
        // One GET /labels/<id> per label (system + user).
        QVERIFY(r.g.count(QStringLiteral("GET /gmail/v1/users/me/labels/")) >= 5);

        auto find = [&](const QString &id) -> CachedLabel {
            for (const CachedLabel &l : r.cache.labels()) {
                if (l.id == id) {
                    return l;
                }
            }
            return {};
        };
        QCOMPARE(find(QStringLiteral("Label_9")).total, 5);
        QCOMPARE(find(QStringLiteral("Label_9")).unread, 2);
        QCOMPARE(find(QStringLiteral("INBOX")).total, 5);
    }

    void createRenameDeleteFolderKeepsMessages()
    {
        Rig r;
        r.g.seedSystemLabels();
        r.sync->start();
        QTRY_VERIFY_WITH_TIMEOUT(!r.sync->isBusy() && r.cache.historyId() > 0, 20000);

        QSignalSpy spy(r.sync.get(), &SyncEngine::labelsChanged);
        const int before = spy.count();
        r.sync->createLabel(QStringLiteral("Projects/Alpha"));
        QTRY_VERIFY_WITH_TIMEOUT(spy.count() > before, 10000);

        auto idOf = [&](const QString &name) {
            for (const CachedLabel &l : r.cache.labels()) {
                if (l.name == name && l.type == QLatin1String("user")) {
                    return l.id;
                }
            }
            return QString();
        };
        const QString id = idOf(QStringLiteral("Projects/Alpha"));
        QVERIFY(!id.isEmpty());

        const int afterCreate = spy.count();
        r.sync->renameLabel(id, QStringLiteral("Projects/Beta"));
        QTRY_VERIFY_WITH_TIMEOUT(spy.count() > afterCreate, 10000);
        // (labelsChanged also fires for the create's own refresh finishing.)
        QTRY_COMPARE_WITH_TIMEOUT(idOf(QStringLiteral("Projects/Beta")), id, 10000);
        QVERIFY(idOf(QStringLiteral("Projects/Alpha")).isEmpty());

        const QString mid =
            r.g.addMessage(r.msg(QStringLiteral("Keep me"), {QStringLiteral("INBOX"), id}), true);
        CachedMessage local;
        local.id = mid;
        local.threadId = mid;
        local.subject = QStringLiteral("Keep me");
        local.labels = {QStringLiteral("INBOX"), id};
        local.internalDateMs = QDateTime::currentMSecsSinceEpoch();
        r.cache.upsert(local);

        const int afterRename = spy.count();
        r.sync->deleteLabel(id);
        QTRY_VERIFY_WITH_TIMEOUT(spy.count() > afterRename, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(idOf(QStringLiteral("Projects/Beta")).isEmpty(), 10000);
        QVERIFY(r.g.messages().contains(mid));
        QVERIFY(!r.g.messages().value(mid).labels.contains(id));
        QVERIFY(r.g.messages().value(mid).labels.contains(QStringLiteral("INBOX")));
        QVERIFY(!r.cache.message(mid).labels.contains(id));
    }

    void moveToLabelIsTrueFolderMove()
    {
        Rig r;
        r.g.seedSystemLabels();
        r.g.addLabel({QStringLiteral("Label_1"), QStringLiteral("Work"), QStringLiteral("user"),
                      QStringLiteral("#7b3fb5")});
        r.g.addLabel({QStringLiteral("Label_2"), QStringLiteral("Family"), QStringLiteral("user"),
                      QStringLiteral("#00897b")});
        const QString mid = r.g.addMessage(
            r.msg(QStringLiteral("Move me"),
                  {QStringLiteral("INBOX"), QStringLiteral("UNREAD"), QStringLiteral("Label_1")}),
            false);

        r.sync->start();
        QTRY_VERIFY_WITH_TIMEOUT(!r.sync->isBusy(), 20000);
        if (!r.cache.contains(mid)) {
            CachedMessage local;
            local.id = mid;
            local.threadId = mid;
            local.subject = QStringLiteral("Move me");
            local.labels = {QStringLiteral("INBOX"), QStringLiteral("UNREAD"), QStringLiteral("Label_1")};
            local.internalDateMs = QDateTime::currentMSecsSinceEpoch();
            r.cache.upsert(local);
        }

        r.sync->moveToLabel(mid, QStringLiteral("Label_2"));
        QTRY_VERIFY_WITH_TIMEOUT(r.g.messages().value(mid).labels.contains(QStringLiteral("Label_2")), 10000);

        const QStringList got = r.g.messages().value(mid).labels;
        QVERIFY(got.contains(QStringLiteral("Label_2")));
        QVERIFY(!got.contains(QStringLiteral("INBOX")));
        QVERIFY(!got.contains(QStringLiteral("Label_1")));
        QVERIFY(got.contains(QStringLiteral("UNREAD")));

        const QStringList local = r.cache.message(mid).labels;
        QVERIFY(local.contains(QStringLiteral("Label_2")));
        QVERIFY(!local.contains(QStringLiteral("INBOX")));
        QVERIFY(!local.contains(QStringLiteral("Label_1")));
    }

    // A multi-select move sends one modify per message; the sidebar counts are
    // followed at once from the cache, and confirmed with Gmail once
    // afterwards for just the folders that changed, not once per message.
    void movingSeveralMessagesRefreshesLabelsOnce()
    {
        Rig r;
        r.g.seedSystemLabels();
        r.g.addLabel({QStringLiteral("Label_2"), QStringLiteral("Family"), QStringLiteral("user"), {}});
        QStringList ids;
        for (int i = 0; i < 8; ++i) {
            ids << r.g.addMessage(r.msg(QStringLiteral("Move %1").arg(i), {QStringLiteral("INBOX")}, i), false);
        }
        r.sync->start();
        QTRY_VERIFY_WITH_TIMEOUT(r.cache.count(QStringLiteral("INBOX")) == 8 && !r.sync->isBusy(), 20000);
        QTest::qWait(100); // the follow-up history poll
        QTRY_VERIFY_WITH_TIMEOUT(!r.sync->isBusy(), 20000);

        auto labelLists = [&r] {
            int n = 0;
            for (const QString &rq : r.g.requests) {
                if (rq == QLatin1String("GET /gmail/v1/users/me/labels")) {
                    ++n;
                }
            }
            return n;
        };
        const int before = labelLists();
        QVERIFY(before >= 1);
        for (const QString &id : std::as_const(ids)) {
            r.sync->moveToLabel(id, QStringLiteral("Label_2"));
        }
        QCOMPARE(r.cache.count(QStringLiteral("Label_2")), 8); // optimistic
        QTRY_VERIFY_WITH_TIMEOUT(std::all_of(ids.begin(), ids.end(), [&r](const QString &id) {
            return r.g.messages().value(id).labels.contains(QStringLiteral("Label_2"));
        }), 20000);
        // The counts follow at once, from the cache, before Gmail is asked:
        const auto total = [&r](const QString &id) {
            for (const CachedLabel &l : r.cache.labels()) {
                if (l.id == id) {
                    return l.total;
                }
            }
            return -1;
        };
        QCOMPARE(total(QStringLiteral("Label_2")), 8);
        QCOMPARE(total(QStringLiteral("INBOX")), 0);
        // ...and then Gmail is asked for the two folders that changed, once
        // each, not for the whole list of labels and every label's counts.
        const auto gets = [&r](const QString &id) {
            return int(r.g.requests.count(QStringLiteral("GET /gmail/v1/users/me/labels/") + id));
        };
        const int inboxGets = gets(QStringLiteral("INBOX"));
        const int folderGets = gets(QStringLiteral("Label_2"));
        QTRY_COMPARE_WITH_TIMEOUT(gets(QStringLiteral("Label_2")), folderGets + 1, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(gets(QStringLiteral("INBOX")), inboxGets + 1, 10000);
        QTest::qWait(1000); // and no more follow
        QCOMPARE(gets(QStringLiteral("Label_2")), folderGets + 1);
        QCOMPARE(labelLists(), before); // no full refresh
        QCOMPARE(total(QStringLiteral("Label_2")), 8);
        QCOMPARE(total(QStringLiteral("INBOX")), 0);
        QCOMPARE(r.cache.count(QStringLiteral("INBOX")), 0);
        QCOMPARE(r.cache.count(QStringLiteral("Label_2")), 8);
    }

    // A folder renamed or deleted while a label refresh is under way: that
    // refresh has already listed the labels, and the change's own refresh used
    // to be folded into it, so the sidebar kept the old name (or the deleted
    // folder) until some later refresh.
    void folderChangeDuringLabelRefreshIsNotLost()
    {
        Rig r;
        r.api->setBackoffBaseMs(200);
        r.g.seedSystemLabels();
        r.sync->start();
        QTRY_VERIFY_WITH_TIMEOUT(!r.sync->isBusy() && r.cache.historyId() > 0, 20000);
        QTest::qWait(100); // the follow-up history poll
        QTRY_VERIFY_WITH_TIMEOUT(!r.sync->isBusy(), 20000);

        auto idOf = [&r](const QString &name) {
            for (const CachedLabel &l : r.cache.labels()) {
                if (l.name == name && l.type == QLatin1String("user")) {
                    return l.id;
                }
            }
            return QString();
        };
        // The refresh after the create lists the labels, then can't finish
        // for a while: one labels.get keeps failing.
        r.g.addFault({QStringLiteral("/gmail/v1/users/me/labels/INBOX"), 503, 2, -1});
        r.sync->createLabel(QStringLiteral("Alpha"));
        QTRY_VERIFY_WITH_TIMEOUT(!idOf(QStringLiteral("Alpha")).isEmpty(), 10000); // listed; counts still coming
        const QString id = idOf(QStringLiteral("Alpha"));

        r.sync->renameLabel(id, QStringLiteral("Beta"));
        QTRY_COMPARE_WITH_TIMEOUT(idOf(QStringLiteral("Beta")), id, 10000);
        QVERIFY(idOf(QStringLiteral("Alpha")).isEmpty());

        r.g.addFault({QStringLiteral("/gmail/v1/users/me/labels/INBOX"), 503, 2, -1});
        r.sync->createLabel(QStringLiteral("Gamma"));
        QTRY_VERIFY_WITH_TIMEOUT(!idOf(QStringLiteral("Gamma")).isEmpty(), 10000);
        r.sync->deleteLabel(id);
        QTRY_VERIFY_WITH_TIMEOUT(idOf(QStringLiteral("Beta")).isEmpty(), 10000);
        QVERIFY(!idOf(QStringLiteral("Gamma")).isEmpty());
    }

    // A move that lands while a label refresh is under way used to be folded
    // into that refresh, which may already have read the folder's old count:
    // the sidebar then stayed wrong until some later refresh.
    void moveDuringLabelRefreshStillUpdatesCounts()
    {
        Rig r;
        r.api->setBackoffBaseMs(200);
        // First in the list, so its count is the first one a refresh reads.
        r.g.addLabel({QStringLiteral("Label_2"), QStringLiteral("Family"), QStringLiteral("user"), {}});
        r.g.seedSystemLabels();
        r.g.addLabel({QStringLiteral("Label_slow"), QStringLiteral("Slow"), QStringLiteral("user"), {}});
        const QString a = r.g.addMessage(r.msg(QStringLiteral("Move A"), {QStringLiteral("INBOX")}, 1), false);
        const QString b = r.g.addMessage(r.msg(QStringLiteral("Move B"), {QStringLiteral("INBOX")}, 2), false);
        r.sync->start();
        QTRY_VERIFY_WITH_TIMEOUT(r.cache.count(QStringLiteral("INBOX")) == 2 && !r.sync->isBusy(), 20000);
        QTest::qWait(100); // the follow-up history poll
        QTRY_VERIFY_WITH_TIMEOUT(!r.sync->isBusy(), 20000);

        auto cachedTotal = [&r] {
            for (const CachedLabel &l : r.cache.labels()) {
                if (l.id == QLatin1String("Label_2")) {
                    return l.total;
                }
            }
            return -1;
        };
        const QString getTarget = QStringLiteral("GET /gmail/v1/users/me/labels/Label_2");
        QCOMPARE(cachedTotal(), 0);
        const int gets = r.g.count(getTarget);

        // The refresh after move A can't finish for a while: one labels.get keeps failing.
        r.g.addFault({QStringLiteral("/gmail/v1/users/me/labels/Label_slow"), 503, 2, -1});
        r.sync->moveToLabel(a, QStringLiteral("Label_2"));
        QTRY_VERIFY_WITH_TIMEOUT(r.g.count(getTarget) > gets, 10000); // it has read Label_2: 1 message
        r.sync->moveToLabel(b, QStringLiteral("Label_2"));
        QTRY_VERIFY_WITH_TIMEOUT(r.g.messages().value(b).labels.contains(QStringLiteral("Label_2")), 10000);

        QTRY_COMPARE_WITH_TIMEOUT(cachedTotal(), 2, 10000);
    }

    // One folder per message: a move drops every other user label, wherever
    // the message was listed when it was dragged, and INBOX moves it back.
    void messageLivesInOneFolder()
    {
        Rig r;
        r.g.seedSystemLabels();
        r.g.addLabel({QStringLiteral("Label_1"), QStringLiteral("Work"), QStringLiteral("user"), {}});
        r.g.addLabel({QStringLiteral("Label_2"), QStringLiteral("Family"), QStringLiteral("user"), {}});
        r.g.addLabel({QStringLiteral("Label_3"), QStringLiteral("Travel"), QStringLiteral("user"), {}});
        const QStringList start{QStringLiteral("INBOX"), QStringLiteral("STARRED"), QStringLiteral("Label_1"),
                                QStringLiteral("Label_3")};
        const QString mid = r.g.addMessage(r.msg(QStringLiteral("From inbox"), start), false);
        r.sync->start();
        QTRY_VERIFY_WITH_TIMEOUT(!r.sync->isBusy(), 20000);
        if (!r.cache.contains(mid)) {
            CachedMessage local;
            local.id = mid;
            local.threadId = mid;
            local.subject = QStringLiteral("From inbox");
            local.labels = start;
            local.internalDateMs = QDateTime::currentMSecsSinceEpoch();
            r.cache.upsert(local);
        }
        const auto userLabels = [&r, &mid]() {
            QStringList out;
            for (const QString &l : r.g.messages().value(mid).labels) {
                if (l.startsWith(QLatin1String("Label_"))) {
                    out << l;
                }
            }
            out.sort();
            return out;
        };

        r.sync->moveToLabel(mid, QStringLiteral("Label_2"));
        QTRY_COMPARE_WITH_TIMEOUT(userLabels(), QStringList{QStringLiteral("Label_2")}, 10000);
        QStringList got = r.g.messages().value(mid).labels;
        QVERIFY(!got.contains(QStringLiteral("INBOX")));
        QVERIFY(got.contains(QStringLiteral("STARRED"))); // system labels are not folders
        QCOMPARE(r.cache.message(mid).labels.filter(QStringLiteral("Label_")), QStringList{QStringLiteral("Label_2")});

        // Already there and nowhere else: nothing is sent.
        const int modifies = r.g.count(QStringLiteral("POST /gmail/v1/users/me/messages/"));
        r.sync->moveToLabel(mid, QStringLiteral("Label_2"));
        QTest::qWait(200);
        QCOMPARE(r.g.count(QStringLiteral("POST /gmail/v1/users/me/messages/")), modifies);

        // Back to the Inbox: out of its folder.
        r.sync->moveToLabel(mid, QStringLiteral("INBOX"));
        QTRY_VERIFY_WITH_TIMEOUT(r.g.messages().value(mid).labels.contains(QStringLiteral("INBOX")), 10000);
        QCOMPARE(userLabels(), QStringList{});
        got = r.g.messages().value(mid).labels;
        QVERIFY(got.contains(QStringLiteral("STARRED")));
        QVERIFY(!r.cache.message(mid).labels.contains(QStringLiteral("Label_2")));
    }

    // Deleting takes a message out of its folder (one folder per message,
    // and Trash is one); Undo puts it back.
    void trashLeavesTheFolderAndUndoReturnsIt()
    {
        Rig r;
        r.g.seedSystemLabels();
        r.g.addLabel({QStringLiteral("Label_1"), QStringLiteral("Work"), QStringLiteral("user"), {}});
        const QStringList start{QStringLiteral("STARRED"), QStringLiteral("Label_1")};
        const QString mid = r.g.addMessage(r.msg(QStringLiteral("Filed"), start), false);
        r.sync->start();
        QTRY_VERIFY_WITH_TIMEOUT(!r.sync->isBusy(), 20000);
        if (!r.cache.contains(mid)) {
            CachedMessage local;
            local.id = mid;
            local.threadId = mid;
            local.subject = QStringLiteral("Filed");
            local.labels = start;
            local.internalDateMs = QDateTime::currentMSecsSinceEpoch();
            r.cache.upsert(local);
        }

        QSignalSpy trashed(r.sync.get(), &SyncEngine::trashSucceeded);
        r.sync->trash(mid);
        QVERIFY(!r.cache.message(mid).labels.contains(QStringLiteral("Label_1"))); // at once
        QTRY_COMPARE_WITH_TIMEOUT(trashed.size(), 1, 10000);
        QStringList got = r.g.messages().value(mid).labels;
        QVERIFY(got.contains(QStringLiteral("TRASH")));
        QVERIFY(!got.contains(QStringLiteral("Label_1")));
        QVERIFY(got.contains(QStringLiteral("STARRED")));
        QVERIFY(!r.cache.message(mid).labels.contains(QStringLiteral("Label_1")));

        QVERIFY(r.sync->untrash(mid));
        QTRY_VERIFY_WITH_TIMEOUT(r.g.messages().value(mid).labels.contains(QStringLiteral("Label_1")), 10000);
        got = r.g.messages().value(mid).labels;
        QVERIFY(!got.contains(QStringLiteral("TRASH")));
        QTRY_VERIFY(r.cache.message(mid).labels.contains(QStringLiteral("Label_1")));

        // Undo before Gmail has answered: still back in its folder.
        r.sync->trash(mid);
        QVERIFY(r.sync->untrash(mid));
        QTRY_VERIFY_WITH_TIMEOUT(!r.g.messages().value(mid).labels.contains(QStringLiteral("TRASH"))
                                     && r.g.untrashCalls.size() == 2,
                                 10000);
        QTRY_VERIFY_WITH_TIMEOUT(r.g.messages().value(mid).labels.contains(QStringLiteral("Label_1")), 10000);
        QTest::qWait(300); // nothing arrives late and takes it off again
        QVERIFY(r.g.messages().value(mid).labels.contains(QStringLiteral("Label_1")));
        QVERIFY(r.cache.message(mid).labels.contains(QStringLiteral("Label_1")));
    }

    // Right-click > Empty Folder: all of the folder's mail goes to Trash,
    // cached or not, in pages; the folder stays and other mail is untouched.
    void emptyFolderTrashesEverythingInIt()
    {
        Rig r;
        r.g.seedSystemLabels();
        r.g.addLabel({QStringLiteral("Label_1"), QStringLiteral("Alerts"), QStringLiteral("user"), {}});
        r.g.addLabel({QStringLiteral("Label_2"), QStringLiteral("Family"), QStringLiteral("user"), {}});
        QStringList filed;
        for (int i = 0; i < 520; ++i) { // more than one page of 500
            filed << r.g.addMessage(r.msg(QStringLiteral("Alert %1").arg(i),
                                          i % 2 ? QStringList{QStringLiteral("Label_1")}
                                                : QStringList{QStringLiteral("INBOX"), QStringLiteral("UNREAD"),
                                                              QStringLiteral("Label_1")}),
                                    false);
        }
        const QString other = r.g.addMessage(r.msg(QStringLiteral("Family"), {QStringLiteral("Label_2")}), false);
        const QString inbox = r.g.addMessage(r.msg(QStringLiteral("Inbox"), {QStringLiteral("INBOX")}), false);
        r.sync->start();
        QTRY_VERIFY_WITH_TIMEOUT(!r.sync->isBusy(), 30000);

        r.sync->emptyLabel(QStringLiteral("Label_1"));
        const auto left = [&r, &filed]() {
            int n = 0;
            for (const QString &id : filed) {
                n += r.g.messages().value(id).labels.contains(QStringLiteral("Label_1"));
            }
            return n;
        };
        QTRY_COMPARE_WITH_TIMEOUT(left(), 0, 30000);
        for (const QString &id : std::as_const(filed)) {
            const QStringList l = r.g.messages().value(id).labels;
            QVERIFY(l.contains(QStringLiteral("TRASH")));
            QVERIFY(!l.contains(QStringLiteral("INBOX")));
        }
        QCOMPARE(r.g.messages().value(other).labels, QStringList{QStringLiteral("Label_2")});
        QCOMPARE(r.g.messages().value(inbox).labels, QStringList{QStringLiteral("INBOX")});
        // The cache follows for the mail it had (the Inbox half was synced).
        QTRY_COMPARE_WITH_TIMEOUT(r.cache.messageIds(QStringLiteral("Label_1")).size(), 0, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(r.cache.labels().size() > 0 && labelTotal(r, QStringLiteral("Label_1")) == 0, true, 20000);
        bool stillThere = false;
        for (const CachedLabel &l : r.cache.labels()) {
            stillThere = stillThere || l.id == QLatin1String("Label_1");
        }
        QVERIFY(stillThere);
    }

    static int labelTotal(Rig &r, const QString &id)
    {
        for (const CachedLabel &l : r.cache.labels()) {
            if (l.id == id) {
                return l.total;
            }
        }
        return -1;
    }
};

QTEST_MAIN(TstLabels)
#include "tst_labels.moc"
