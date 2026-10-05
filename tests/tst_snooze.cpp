// Snooze presets, wakeDue, and Gmail INBOX remove/restore against the mock.
#include "core/MailCache.h"
#include "core/SnoozeTimes.h"
#include "core/SyncEngine.h"
#include "core/AuthManager.h"
#include "core/GmailClient.h"
#include "core/TokenStore.h"
#include "mock/MockGoogle.h"

#include <QDateTime>
#include <QNetworkAccessManager>
#include <QTest>
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
        sync->setPollInterval(3600 * 1000);
    }
};
} // namespace

class TstSnooze : public QObject
{
    Q_OBJECT
private slots:
    void presetsLocalTime()
    {
        // Wednesday 2026-10-07 10:00 local.
        const QDateTime now(QDate(2026, 10, 7), QTime(10, 0), Qt::LocalTime);
        QCOMPARE(SnoozeTimes::laterToday(now), now.addSecs(3 * 3600));
        QCOMPARE(SnoozeTimes::tomorrowMorning(now), QDateTime(QDate(2026, 10, 8), QTime(8, 0), Qt::LocalTime));
        QCOMPARE(SnoozeTimes::thisWeekend(now), QDateTime(QDate(2026, 10, 10), QTime(8, 0), Qt::LocalTime)); // Sat
        QCOMPARE(SnoozeTimes::nextWeek(now), QDateTime(QDate(2026, 10, 12), QTime(8, 0), Qt::LocalTime)); // Mon
        // Saturday before 8 → today 8 AM.
        const QDateTime satMorn(QDate(2026, 10, 10), QTime(7, 0), Qt::LocalTime);
        QCOMPARE(SnoozeTimes::thisWeekend(satMorn), QDateTime(QDate(2026, 10, 10), QTime(8, 0), Qt::LocalTime));
        // Monday → next Monday, not today.
        const QDateTime mon(QDate(2026, 10, 12), QTime(9, 0), Qt::LocalTime);
        QCOMPARE(SnoozeTimes::nextWeek(mon), QDateTime(QDate(2026, 10, 19), QTime(8, 0), Qt::LocalTime));
        QVERIFY(SnoozeTimes::isDue(now.toMSecsSinceEpoch() - 1, now));
        QVERIFY(!SnoozeTimes::isDue(now.toMSecsSinceEpoch() + 60'000, now));
    }

    void cacheRoundTripAndBadge()
    {
        MailCache c;
        QVERIFY(c.open(QStringLiteral(":memory:")));
        QCOMPARE(c.meta(QStringLiteral("schema")).toInt(), 2);
        CachedMessage m;
        m.id = QStringLiteral("m1");
        m.subject = QStringLiteral("Later");
        m.labels = {QStringLiteral("INBOX")};
        c.upsert(m);
        const qint64 wake = QDateTime::currentMSecsSinceEpoch() + 60'000;
        c.setSnooze(QStringLiteral("m1"), wake, true);
        QCOMPARE(c.snooze(QStringLiteral("m1")).wakeMs, wake);
        QVERIFY(c.snooze(QStringLiteral("m1")).hadInbox);
        QCOMPARE(c.snoozes(true).size(), 1);
        QVERIFY(c.dueSnoozes(wake - 1).isEmpty());
        QCOMPARE(c.dueSnoozes(wake + 1), QStringList{QStringLiteral("m1")});
        c.markSnoozeWoke(QStringLiteral("m1"));
        QCOMPARE(c.snooze(QStringLiteral("m1")).wakeMs, 0);
        QVERIFY(c.snooze(QStringLiteral("m1")).badge);
        QCOMPARE(c.snoozeBadgeIds(), QStringList{QStringLiteral("m1")});
        c.clearSnoozeBadge(QStringLiteral("m1"));
        QVERIFY(c.snoozeBadgeIds().isEmpty());
    }

    void snoozeRemovesInboxWakeRestores()
    {
        Rig r;
        r.g.seedSystemLabels();
        MockGoogle::Message msg;
        msg.from = QStringLiteral("Ada <ada@example.org>");
        msg.to = QStringLiteral("me@example.com");
        msg.subject = QStringLiteral("Snooze me");
        msg.text = QStringLiteral("body");
        msg.labels = {QStringLiteral("INBOX"), QStringLiteral("UNREAD")};
        msg.date = QDateTime::currentDateTimeUtc();
        const QString id = r.g.addMessage(msg, false);
        r.sync->start();
        QTRY_VERIFY_WITH_TIMEOUT(r.cache.contains(id) && !r.sync->isBusy(), 20000);

        const qint64 wake = QDateTime::currentMSecsSinceEpoch() + 3600'000;
        r.sync->snooze(id, wake);
        QVERIFY(r.cache.snooze(id).wakeMs > 0);
        QVERIFY(!r.cache.message(id).labels.contains(QStringLiteral("INBOX")));
        QTRY_VERIFY(r.g.modifyCalls.contains(id + QStringLiteral(":-INBOX")));

        // Force due: rewrite wake into the past.
        r.cache.setSnooze(id, QDateTime::currentMSecsSinceEpoch() - 1000, true);
        QCOMPARE(r.sync->wakeDue(), 1);
        QVERIFY(r.cache.message(id).labels.contains(QStringLiteral("INBOX")));
        QVERIFY(r.cache.snooze(id).badge);
        QTRY_VERIFY(r.g.modifyCalls.contains(id + QStringLiteral(":+INBOX")));
    }

    void unsnoozeCancels()
    {
        Rig r;
        r.g.seedSystemLabels();
        MockGoogle::Message msg;
        msg.from = QStringLiteral("Ada <ada@example.org>");
        msg.to = QStringLiteral("me@example.com");
        msg.subject = QStringLiteral("Cancel snooze");
        msg.text = QStringLiteral("body");
        msg.labels = {QStringLiteral("INBOX")};
        msg.date = QDateTime::currentDateTimeUtc();
        const QString id = r.g.addMessage(msg, false);
        r.sync->start();
        QTRY_VERIFY_WITH_TIMEOUT(r.cache.contains(id) && !r.sync->isBusy(), 20000);
        r.sync->snooze(id, QDateTime::currentMSecsSinceEpoch() + 999'000);
        QTRY_VERIFY(!r.cache.message(id).labels.contains(QStringLiteral("INBOX")));
        r.sync->unsnooze(id);
        QVERIFY(r.cache.snooze(id).messageId.isEmpty());
        QVERIFY(r.cache.message(id).labels.contains(QStringLiteral("INBOX")));
    }
};

QTEST_MAIN(TstSnooze)
#include "tst_snooze.moc"
