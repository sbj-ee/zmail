// "Gmail (app password)" account, part 3: changing mail over IMAP (flags,
// X-GM-LABELS, UID MOVE), folders, Trash and Undo, Junk, bulk actions,
// Gmail-syntax search (X-GM-RAW) and attachments, against the fake server.
// Made-up passwords only.
#include "core/AppPasswordAccount.h"
#include "core/ImapSyncEngine.h"
#include "core/MailCache.h"
#include "core/MessageParser.h"
#include "mock/FakeImapServer.h"

#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>

using namespace zmail;
using zmail::test::FakeImapServer;

namespace {

struct Rig
{
    FakeImapServer server;
    MailCache cache;
    std::unique_ptr<ImapSyncEngine> sync;

    explicit Rig(int seed = 10, int initial = 500)
    {
        server.seedDemo(seed);
        cache.open(QStringLiteral(":memory:"));
        AppPasswordAccount a;
        a.email = server.user;
        a.imapHost = QStringLiteral("127.0.0.1");
        a.imapPort = server.port();
        sync = std::make_unique<ImapSyncEngine>(
            a, [this](std::function<void(bool, const QString &, const QString &)> cb) { cb(true, server.password, {}); },
            &cache);
        sync->setPlaintextForTests(true);
        sync->setPollInterval(3600 * 1000);
        sync->setInitialCount(initial);
        QSignalSpy idle(sync.get(), &SyncEngine::idle);
        sync->start();
        if (!idle.wait(10000)) {
            qFatal("no initial sync");
        }
    }
    quint64 msgid(const char *subject)
    {
        for (quint64 id = 0x18f3a0c2b4d50001ULL; id < 0x18f3a0c2b4d50000ULL + 500; ++id) {
            if (auto *m = server.find(id); m && m->raw.contains(subject)) {
                return id;
            }
        }
        return 0;
    }
    static QString hex(quint64 id) { return QString::number(id, 16); }
    QStringList labels(quint64 id) { return server.find(id)->labels; }
    QSet<QString> flags(quint64 id) { return server.find(id)->flags; }
    // Every queued change has reached the server and the engine is idle again.
    bool settle()
    {
        return QTest::qWaitFor([this] { return !sync->isBusy() && server.anyIdling(); }, 10000);
    }
    int commandsLike(const char *needle) const
    {
        int n = 0;
        for (const QByteArray &c : server.commands) {
            n += c.contains(needle) ? 1 : 0;
        }
        return n;
    }
};

} // namespace

class TstImapActions : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void readUnreadAndFlags()
    {
        Rig r;
        const quint64 id = r.msgid("Spare parts order"); // among the newest: unread
        QVERIFY(id);
        QVERIFY(r.cache.summary(Rig::hex(id)).unread());
        r.sync->markRead(Rig::hex(id));
        QVERIFY(!r.cache.summary(Rig::hex(id)).unread()); // optimistic
        QTRY_VERIFY(r.flags(id).contains(QStringLiteral("\\Seen")));
        r.sync->markUnread(Rig::hex(id));
        QTRY_VERIFY(!r.flags(id).contains(QStringLiteral("\\Seen")));
        r.sync->setFlag(Rig::hex(id), QStringLiteral("red"));
        QTRY_VERIFY(r.flags(id).contains(QStringLiteral("\\Flagged")));
        QCOMPARE(r.cache.flags().value(Rig::hex(id)), QStringLiteral("red"));
        r.sync->setFlag(Rig::hex(id), {});
        QTRY_VERIFY(!r.flags(id).contains(QStringLiteral("\\Flagged")));
        QVERIFY(r.commandsLike("+FLAGS.SILENT (\\Seen)") >= 1);
        QVERIFY(r.settle());
    }

    void moveToFolderAndUndo()
    {
        Rig r;
        const quint64 id = r.msgid("Q4 field schedule"); // Inbox + Important
        QVERIFY(r.labels(id).contains(QStringLiteral("\\Important")));
        const QStringList before = r.cache.summary(Rig::hex(id)).labels;
        r.sync->moveToLabel(Rig::hex(id), QStringLiteral("Receipts"));
        QVERIFY(r.cache.summary(Rig::hex(id)).labels.contains(QStringLiteral("Receipts")));
        QVERIFY(!r.cache.summary(Rig::hex(id)).labels.contains(QStringLiteral("INBOX")));
        QTRY_VERIFY(r.labels(id).contains(QStringLiteral("Receipts")));
        QVERIFY(!r.labels(id).contains(QStringLiteral("\\Inbox")));
        QVERIFY(!r.labels(id).contains(QStringLiteral("\\Important")));
        QVERIFY(r.commandsLike("-X-GM-LABELS.SILENT (\"\\\\Inbox\" \"\\\\Important\")") == 1);

        r.sync->restoreFolders(Rig::hex(id), before);
        QTRY_VERIFY(r.labels(id).contains(QStringLiteral("\\Inbox")));
        QVERIFY(r.labels(id).contains(QStringLiteral("\\Important")));
        QVERIFY(!r.labels(id).contains(QStringLiteral("Receipts")));
        QVERIFY(r.settle());
        QVERIFY(r.cache.summary(Rig::hex(id)).labels.contains(QStringLiteral("INBOX")));
    }

    void trashAndUndo()
    {
        Rig r;
        const quint64 id = r.msgid("substation inspection photos"); // Inbox + folder
        QVERIFY(r.labels(id).contains(QStringLiteral("Projects/Substation")));
        QSignalSpy ok(r.sync.get(), &SyncEngine::trashSucceeded);
        r.sync->trash(Rig::hex(id));
        QVERIFY(r.cache.summary(Rig::hex(id)).labels.contains(QStringLiteral("TRASH")));
        QVERIFY(ok.wait(5000));
        QCOMPARE(r.labels(id), QStringList{QStringLiteral("\\Trash")});
        QVERIFY(r.commandsLike("UID MOVE") >= 1);
        QVERIFY(r.sync->untrash(Rig::hex(id)));
        QTRY_VERIFY(r.labels(id).contains(QStringLiteral("\\Inbox")));
        QTRY_VERIFY(r.labels(id).contains(QStringLiteral("Projects/Substation")));
        QVERIFY(!r.labels(id).contains(QStringLiteral("\\Trash")));
        QVERIFY(!r.sync->untrash(Rig::hex(id))); // only once
        QVERIFY(r.settle());
        QVERIFY(r.cache.summary(Rig::hex(id)).labels.contains(QStringLiteral("INBOX")));
    }

    void bulkTrashIsOneMove()
    {
        Rig r(12);
        QStringList ids;
        QList<quint64> raw;
        for (const char *s : {"Lunch on Thursday?", "Your October statement", "Spare parts order"}) {
            raw.append(r.msgid(s));
            ids.append(Rig::hex(raw.last()));
        }
        const int movesBefore = r.commandsLike("UID MOVE");
        QSignalSpy ok(r.sync.get(), &SyncEngine::trashSucceeded);
        r.sync->trashMany(ids);
        QTRY_COMPARE(ok.count(), 3);
        for (const quint64 id : raw) {
            QCOMPARE(r.labels(id), QStringList{QStringLiteral("\\Trash")});
        }
        QCOMPARE(r.commandsLike("UID MOVE") - movesBefore, 1);
    }

    void junkAndBack()
    {
        Rig r;
        const quint64 id = r.msgid("free drink");
        r.sync->markJunk(Rig::hex(id));
        QTRY_COMPARE(r.labels(id), QStringList{QStringLiteral("\\Spam")});
        r.sync->markNotJunk(Rig::hex(id));
        QTRY_VERIFY(r.labels(id).contains(QStringLiteral("\\Inbox")));
        QVERIFY(!r.labels(id).contains(QStringLiteral("\\Spam")));
        QVERIFY(r.settle());
    }

    void refusalRollsBack()
    {
        Rig r;
        const quint64 id = r.msgid("Lunch on Thursday?");
        const QStringList before = r.cache.summary(Rig::hex(id)).labels;
        r.server.refuseChanges = true;
        QSignalSpy err(r.sync.get(), &SyncEngine::syncError);
        QSignalSpy failed(r.sync.get(), &SyncEngine::trashFailed);
        r.sync->trash(Rig::hex(id));
        QVERIFY(failed.wait(5000));
        QVERIFY(!err.isEmpty());
        QCOMPARE(r.cache.summary(Rig::hex(id)).labels, before);
        QVERIFY(r.labels(id).contains(QStringLiteral("\\Inbox")));
        r.sync->moveToLabel(Rig::hex(id), QStringLiteral("Receipts"));
        QTRY_VERIFY(err.count() >= 2);
        QTRY_COMPARE(r.cache.summary(Rig::hex(id)).labels, before);
    }

    void foldersCreateRenameDelete()
    {
        Rig r;
        QString created;
        bool done = false;
        r.sync->createLabel(QStringLiteral("Vendors/Caf\u00e9"), {}, [&](const QString &id) {
            created = id;
            done = true;
        });
        QTRY_VERIFY(done);
        QCOMPARE(created, QStringLiteral("Vendors/Caf\u00e9"));
        QVERIFY(r.server.userFolders.contains(QStringLiteral("Vendors/Caf\u00e9")));
        auto hasLabel = [&r](const QString &id) {
            for (const CachedLabel &l : r.cache.labels()) {
                if (l.id == id) {
                    return true;
                }
            }
            return false;
        };
        QVERIFY(hasLabel(created));
        const quint64 id = r.msgid("Your October statement"); // in Receipts
        r.sync->renameLabel(QStringLiteral("Receipts"), QStringLiteral("Bills"));
        QTRY_VERIFY(hasLabel(QStringLiteral("Bills")));
        QVERIFY(!hasLabel(QStringLiteral("Receipts")));
        QVERIFY(r.labels(id).contains(QStringLiteral("Bills")));
        QVERIFY(r.cache.summary(Rig::hex(id)).labels.contains(QStringLiteral("Bills")));
        r.sync->deleteLabel(QStringLiteral("Bills"));
        QTRY_VERIFY(!hasLabel(QStringLiteral("Bills")));
        QVERIFY(!r.labels(id).contains(QStringLiteral("Bills")));
        QVERIFY(r.labels(id).contains(QStringLiteral("\\Inbox"))); // the mail itself stays
        QVERIFY(r.settle());
    }

    void emptyFolderUndoAndEmptyTrash()
    {
        Rig r(16);
        const QString folder = QStringLiteral("Projects/Substation");
        QSignalSpy emptied(r.sync.get(), &SyncEngine::labelEmptied);
        r.sync->emptyLabel(folder);
        QVERIFY(emptied.wait(5000));
        const QStringList ids = emptied.first().at(1).toStringList();
        QCOMPARE(ids.size(), 4);
        for (const QString &id : ids) {
            QCOMPARE(r.labels(id.toULongLong(nullptr, 16)), QStringList{QStringLiteral("\\Trash")});
        }
        r.sync->unemptyLabel(folder, ids);
        QTRY_VERIFY(r.labels(ids.first().toULongLong(nullptr, 16)).contains(folder));
        QVERIFY(r.settle());

        r.sync->trash(Rig::hex(r.msgid("Lunch on Thursday?")));
        QVERIFY(r.settle());
        QSignalSpy trashEmptied(r.sync.get(), &SyncEngine::trashEmptied);
        r.sync->emptyTrash();
        QVERIFY(trashEmptied.wait(5000));
        QVERIFY(trashEmptied.first().first().toInt() >= 1);
        QVERIFY(r.cache.purged().contains(Rig::hex(r.msgid("Lunch on Thursday?"))));
    }

    void clearImportantFromFiledMail()
    {
        Rig r(10);
        const quint64 id = r.msgid("Q4 field schedule");
        r.server.setLabels(id, {QStringLiteral("\\Important"), QStringLiteral("Receipts")}); // filed, not in Inbox
        QVERIFY(r.settle());
        QSignalSpy cleared(r.sync.get(), &SyncEngine::importantCleared);
        r.sync->clearImportantFromFiled();
        QVERIFY(cleared.wait(5000));
        QCOMPARE(cleared.first().at(0).toInt(), 1);
        QVERIFY(cleared.first().at(1).toBool());
        QCOMPARE(r.labels(id), QStringList{QStringLiteral("Receipts")});
    }

    void gmailSyntaxSearchReachesUncachedMail()
    {
        Rig r(24, /*initial=*/5); // most of the Inbox isn't cached
        QVERIFY(r.sync->supportsServerSearch());
        QStringList found;
        QString error;
        bool done = false;
        r.sync->serverSearch(QStringLiteral("from:hydro statement"), [&](const QStringList &ids, const QString &e) {
            found = ids;
            error = e;
            done = true;
        });
        QTRY_VERIFY(done);
        QVERIFY(error.isEmpty());
        QCOMPARE(found.size(), 3); // seeded 3 times in 24
        for (const QString &id : found) {
            QVERIFY(r.cache.contains(id)); // cached so the list can show them
            QVERIFY(r.cache.summary(id).fromAddr.contains(QStringLiteral("hydro")));
        }
        QVERIFY(r.commandsLike("X-GM-RAW \"from:hydro statement\"") == 1);
        done = false;
        r.sync->serverSearch(QStringLiteral("is:starred"), [&](const QStringList &ids, const QString &) {
            found = ids;
            done = true;
        });
        QTRY_VERIFY(done);
        QVERIFY(!found.isEmpty());
    }

    void attachmentsArriveInline()
    {
        Rig r(3);
        const QByteArray pdf("%PDF-1.4 fake test bytes\n\x01\x02\x03", 28);
        const QByteArray raw =
            "From: Jordan Kim <jordan.kim@example.net>\r\nTo: alex.morgan@gmail.com\r\nSubject: Breaker spec sheet\r\n"
            "Message-ID: <spec@example.net>\r\nMIME-Version: 1.0\r\nContent-Type: multipart/mixed; boundary=b1\r\n\r\n"
            "--b1\r\nContent-Type: text/plain\r\n\r\nSpec sheet attached.\r\n"
            "--b1\r\nContent-Type: application/pdf; name=\"breaker-spec.pdf\"\r\n"
            "Content-Disposition: attachment; filename=\"breaker-spec.pdf\"\r\nContent-Transfer-Encoding: base64\r\n\r\n"
            + pdf.toBase64() + "\r\n--b1--\r\n";
        QSignalSpy newMail(r.sync.get(), &SyncEngine::newMail);
        QTRY_VERIFY(r.server.anyIdling());
        const quint64 id = r.server.deliver(raw);
        QVERIFY(newMail.wait(5000));
        QJsonObject json;
        QString error;
        bool done = false;
        r.sync->fetchFull(Rig::hex(id), [&](const QJsonObject &j, const QString &e) {
            json = j;
            error = e;
            done = true;
        });
        QTRY_VERIFY(done);
        QVERIFY(error.isEmpty());
        const auto refs = MessageParser::bodyFromFull(json).attachmentRefs;
        QCOMPARE(refs.size(), 1);
        QCOMPARE(refs.first().fileName, QStringLiteral("breaker-spec.pdf"));
        QVERIFY(refs.first().attachmentId.isEmpty());
        QCOMPARE(refs.first().inlineData, pdf);
        QVERIFY(!r.flags(id).contains(QStringLiteral("\\Seen"))); // fetched with PEEK
    }
};

QTEST_MAIN(TstImapActions)
#include "tst_imapactions.moc"
