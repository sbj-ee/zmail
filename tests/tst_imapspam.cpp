// Gmail's Spam ([Gmail]/Spam) on an app-password (IMAP) account: always in
// the sidebar as Junk / Suspicious with its count (even with Hide Spam on,
// the default), Junk moves a message there, Not Junk moves it back to the
// Inbox. Against the fake IMAP server; made-up passwords only.
#include "MainWindow.hpp"
#include "core/AppPasswordAccount.h"
#include "core/MailSession.h"
#include "core/TokenStore.h"
#include "mock/FakeImapServer.h"

#include <QAction>
#include <QSettings>
#include <QStandardPaths>
#include <QTest>
#include <QTreeView>
#include <QTreeWidget>

using namespace zmail;
using zmail::test::FakeImapServer;

namespace {

QByteArray mail(const char *from, const char *subject, const char *date)
{
    return QByteArray("From: ") + from + "\r\nTo: Stephen Johnson <stephen@gmail.com>\r\nSubject: " + subject
           + "\r\nDate: " + date + "\r\nMessage-ID: <" + QByteArray(subject).toHex().left(24)
           + "@mail.test>\r\nContent-Type: text/plain; charset=utf-8\r\n\r\nHello.\r\n";
}

struct Rig
{
    FakeImapServer imap;
    MemoryTokenStore store;
    std::unique_ptr<MailSession> session;
    std::unique_ptr<MainWindow> w;
    QTreeWidget *tree = nullptr;
    QTreeView *list = nullptr;
    quint64 keep = 0, junk = 0, spam = 0;

    Rig()
    {
        keep = imap.deliver(mail("Dana Whitfield <dana.whitfield@lakeshore-electric.com>", "Panel schedule for the Oak St job",
                                 "Mon, 12 Oct 2026 09:14:00 -0500"));
        junk = imap.deliver(mail("Prize Desk <winner@prize-desk.biz>", "You have been selected",
                                 "Tue, 13 Oct 2026 06:02:00 -0500"));
        spam = imap.deliver(mail("Crypto Alerts <alerts@coin-boost.top>", "Double your balance today",
                                 "Mon, 12 Oct 2026 22:40:00 -0500"),
                            {QStringLiteral("\\Spam")});
        store.values.insert(AppPasswordAccount::appPasswordKey(imap.user), imap.password);
        AppPasswordAccount a;
        a.email = imap.user;
        a.imapHost = QStringLiteral("127.0.0.1");
        a.imapPort = imap.port();
        a.smtpHost = QStringLiteral("127.0.0.1");
        a.smtpPort = 1;
        QSettings s;
        s.clear(); // Hide Spam stays at its default (on)
        a.save(s);
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
        list = w->findChild<QTreeView *>(QStringLiteral("messageList"));
    }
    ~Rig()
    {
        w.reset();
        session.reset();
        QSettings().clear();
    }
    QTreeWidgetItem *item(const QString &key) const
    {
        for (QTreeWidgetItemIterator it(tree); *it; ++it) {
            if ((*it)->data(0, Qt::UserRole).toString() == key) {
                return *it;
            }
        }
        return nullptr;
    }
    QString count(const QString &key) const { return item(key) ? item(key)->text(1) : QStringLiteral("<none>"); }
    // The total of "unread / total" (or of a bare "total"): opening a message
    // may mark it read, so the unread half depends on timing.
    QString total(const QString &key) const { return count(key).section(QLatin1Char('/'), -1).trimmed(); }
    bool select(const char *subject)
    {
        auto *m = list->model();
        for (int r = 0; r < m->rowCount(); ++r) {
            for (int c = 0; c < m->columnCount(); ++c) {
                if (m->index(r, c).data().toString() == QLatin1String(subject)) {
                    list->setCurrentIndex(m->index(r, 0));
                    return true;
                }
            }
        }
        return false;
    }
    void shot(const char *name)
    {
        if (const QString dir = qEnvironmentVariable("ZMAIL_SHOT_DIR"); !dir.isEmpty()) {
            QTest::qWait(300);
            w->grab().save(dir + QLatin1Char('/') + QLatin1String(name) + QStringLiteral(".png"));
        }
    }
};

} // namespace

class TstImapSpam : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void spamIsAStandardMailbox()
    {
        Rig r;
        QTRY_VERIFY_WITH_TIMEOUT(r.w->isLive(), 15000);
        QVERIFY(r.w->findChild<QAction *>(QStringLiteral("actionHideSpam"))->isChecked()); // default on
        QTRY_VERIFY_WITH_TIMEOUT(r.item(QStringLiteral("Junk")), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(r.count(QStringLiteral("Junk")), QStringLiteral("1 / 1"), 10000);

        // Junk: Inbox -> [Gmail]/Spam.
        QTRY_VERIFY_WITH_TIMEOUT(r.select("You have been selected"), 10000);
        r.w->findChild<QAction *>(QStringLiteral("actionJunk"))->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(r.imap.find(r.junk)->labels.contains(QStringLiteral("\\Spam")), 10000);
        QVERIFY(!r.imap.find(r.junk)->labels.contains(QStringLiteral("\\Inbox")));
        QTRY_COMPARE_WITH_TIMEOUT(r.total(QStringLiteral("Junk")), QStringLiteral("2"), 10000);

        // The Junk mailbox lists both.
        r.tree->setCurrentItem(r.item(QStringLiteral("Junk"))); // as a click: loads [Gmail]/Spam
        QTRY_VERIFY_WITH_TIMEOUT(r.select("Double your balance today"), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(r.select("You have been selected"), 10000);
        r.list->clearSelection();
        r.list->setCurrentIndex({});
        r.shot("imap-spam-mailbox");

        // Not Junk: Spam -> Inbox.
        QVERIFY(r.select("Double your balance today"));
        r.w->findChild<QAction *>(QStringLiteral("menuActionNotJunk"))->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(r.imap.find(r.spam)->labels.contains(QStringLiteral("\\Inbox")), 10000);
        QVERIFY(!r.imap.find(r.spam)->labels.contains(QStringLiteral("\\Spam")));
        QTRY_COMPARE_WITH_TIMEOUT(r.total(QStringLiteral("Junk")), QStringLiteral("1"), 10000);
        r.tree->setCurrentItem(r.item(QStringLiteral("In")));
        QTRY_VERIFY_WITH_TIMEOUT(r.select("Double your balance today"), 10000);
        QVERIFY(!r.select("You have been selected")); // still in Spam, kept out of In by Hide Spam
    }
};

QTEST_MAIN(TstImapSpam)
#include "tst_imapspam.moc"
