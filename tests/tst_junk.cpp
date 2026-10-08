// Junk: SyncEngine markJunk / markNotJunk (SPAM ± INBOX), Hide Spam filter,
// View > Show Spam, toolbar / Message menu actions.
#include "MainWindow.hpp"
#include "core/AuthManager.h"
#include "core/MailCache.h"
#include "core/MailSession.h"
#include "core/SyncEngine.h"
#include "core/TokenStore.h"
#include "mock/MockGoogle.h"
#include "ui/MessageListModel.h"
#include "ui/Theme.h"

#include <QAction>
#include <QApplication>
#include <QLineEdit>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTreeView>
#include <QTreeWidget>
#include <QSortFilterProxyModel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QtTest>
#include <memory>

using namespace zmail;
using namespace zmail::ui;
using zmail::test::MockGoogle;

namespace {
QNetworkAccessManager *browserNam()
{
    static QNetworkAccessManager nam;
    return &nam;
}

SessionOptions mockOptions(MockGoogle &g, MemoryTokenStore *store)
{
    SessionOptions o;
    o.client.status = ClientConfig::LoadStatus::Ok;
    o.client.config = g.clientConfig();
    o.store = store;
    o.apiBase = g.apiBase();
    o.revokeUri = g.revokeUri();
    o.cachePathOverride = QStringLiteral(":memory:");
    o.rememberAccount = false;
    o.pollIntervalMs = 3600 * 1000;
    o.backoffBaseMs = 5;
    o.browserOpener = [](const QUrl &u) {
        QNetworkReply *r = browserNam()->get(QNetworkRequest(u));
        QObject::connect(r, &QNetworkReply::finished, r, &QObject::deleteLater);
        return true;
    };
    return o;
}

struct Live
{
    MockGoogle g;
    MemoryTokenStore store;
    std::unique_ptr<MailSession> session;
    Live()
    {
        g.listen();
        g.seedSystemLabels();
    }
    QString seed(const QString &subject, QStringList labels)
    {
        MockGoogle::Message m;
        m.from = QStringLiteral("Priya Raman <priya.raman@example.com>");
        m.to = QStringLiteral("Demo User <demo.user@example.com>");
        m.subject = subject;
        m.text = QStringLiteral("Fake test mail.");
        m.labels = labels;
        m.date = QDateTime::currentDateTimeUtc().addSecs(-600);
        return g.addMessage(m, true);
    }
    bool start()
    {
        session = std::make_unique<MailSession>(mockOptions(g, &store));
        session->signIn();
        return QTest::qWaitFor([this] { return session->state() == MailSession::State::SignedIn; }, 10000) &&
               QTest::qWaitFor([this] { return session->sync() && !session->sync()->isBusy(); }, 15000);
    }
};
} // namespace

class TstJunk : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QSettings().clear();
    }

    void markJunkAddsSpamRemovesInbox()
    {
        Live L;
        const QString id = L.seed(QStringLiteral("Phish"), {QStringLiteral("INBOX"), QStringLiteral("UNREAD")});
        QVERIFY(L.start());
        L.session->sync()->markJunk(id);
        QVERIFY(L.session->cache()->message(id).labels.contains(QStringLiteral("SPAM")));
        QVERIFY(!L.session->cache()->message(id).labels.contains(QStringLiteral("INBOX")));
        QTRY_VERIFY(L.g.modifyCalls.contains(id + QStringLiteral(":+SPAM")));
        QTRY_VERIFY(L.g.modifyCalls.contains(id + QStringLiteral(":-INBOX")));
        QTRY_VERIFY(L.g.messages().value(id).labels.contains(QStringLiteral("SPAM")));
        QVERIFY(!L.g.messages().value(id).labels.contains(QStringLiteral("INBOX")));
    }

    void markNotJunkRestoresInbox()
    {
        Live L;
        const QString id = L.seed(QStringLiteral("False positive"), {QStringLiteral("SPAM")});
        QVERIFY(L.start());
        L.session->sync()->ensureLabel(QStringLiteral("SPAM"));
        QTRY_VERIFY(L.session->cache()->contains(id));
        L.session->sync()->markNotJunk(id);
        QVERIFY(L.session->cache()->message(id).labels.contains(QStringLiteral("INBOX")));
        QVERIFY(!L.session->cache()->message(id).labels.contains(QStringLiteral("SPAM")));
        QTRY_VERIFY(L.g.modifyCalls.contains(id + QStringLiteral(":+INBOX")));
        QTRY_VERIFY(L.g.modifyCalls.contains(id + QStringLiteral(":-SPAM")));
    }

    void hideSpamDefaultFiltersSampleInbox()
    {
        QSettings().setValue(QStringLiteral("mail/hideSpam"), true);
        MainWindow w;
        QVERIFY(w.hideSpam());
        auto *tree = w.findChild<QTreeWidget *>(QStringLiteral("mailboxTree"));
        QStringList top;
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            top << tree->topLevelItem(i)->text(0);
        }
        QVERIFY(!top.contains(QStringLiteral("Junk / Suspicious")));
        QCOMPARE(top, (QStringList{"In", "Out", "Snoozed", "Trash", "Folders"}));

        auto *list = w.findChild<QTreeView *>(QStringLiteral("messageList"));
        // Sample Contoso phish has In+Junk; Hide Spam drops it from In (12 -> 11).
        QCOMPARE(list->model()->rowCount(), 11);
        for (int r = 0; r < list->model()->rowCount(); ++r) {
            QVERIFY(!list->model()->index(r, 0).data(MessageListModel::SuspiciousRole).toBool());
        }
    }

    void showSpamOpensJunkFolder()
    {
        QSettings().setValue(QStringLiteral("mail/hideSpam"), true);
        MainWindow w;
        w.findChild<QAction *>(QStringLiteral("actionShowSpam"))->trigger();
        QCOMPARE(w.findChild<MessageFilterProxy *>()->mailbox(), QStringLiteral("Junk"));
        auto *tree = w.findChild<QTreeWidget *>(QStringLiteral("mailboxTree"));
        QStringList top;
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            top << tree->topLevelItem(i)->text(0);
        }
        QVERIFY(top.contains(QStringLiteral("Junk / Suspicious")));
        auto *list = w.findChild<QTreeView *>(QStringLiteral("messageList"));
        QVERIFY(list->model()->rowCount() >= 1);
        QVERIFY(list->model()->index(0, 0).data(MessageListModel::SuspiciousRole).toBool());
    }

    void unhideSpamShowsFolder()
    {
        QSettings().setValue(QStringLiteral("mail/hideSpam"), true);
        MainWindow w;
        w.setHideSpam(false);
        QVERIFY(!w.hideSpam());
        QCOMPARE(QSettings().value(QStringLiteral("mail/hideSpam")).toBool(), false);
        auto *tree = w.findChild<QTreeWidget *>(QStringLiteral("mailboxTree"));
        QStringList top;
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            top << tree->topLevelItem(i)->text(0);
        }
        QVERIFY(top.contains(QStringLiteral("Junk / Suspicious")));
    }

    void actionsExistWithShortcut()
    {
        MainWindow w;
        QAction *junk = w.findChild<QAction *>(QStringLiteral("menuActionJunk"));
        QAction *notJunk = w.findChild<QAction *>(QStringLiteral("menuActionNotJunk"));
        QAction *tb = w.findChild<QAction *>(QStringLiteral("actionJunk"));
        QVERIFY(junk && notJunk && tb);
        QCOMPARE(junk->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_J));
        QCOMPARE(notJunk->shortcut(), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_J));
        QVERIFY(junk->isVisible());
        QVERIFY(!notJunk->isVisible());
    }

    void liveJunkFromToolbar()
    {
        Live L;
        const QString id = L.seed(QStringLiteral("Toolbar junk"), {QStringLiteral("INBOX"), QStringLiteral("UNREAD")});
        QVERIFY(L.start());
        MainWindow w;
        w.setSession(L.session.get());
        QMetaObject::invokeMethod(L.session.get(), "ready");
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QTRY_VERIFY(w.isLive());
        auto *list = w.findChild<QTreeView *>(QStringLiteral("messageList"));
        auto *proxy = qobject_cast<QSortFilterProxyModel *>(list->model());
        auto *model = w.findChild<MessageListModel *>();
        int row = -1;
        for (int r = 0; r < proxy->rowCount(); ++r) {
            if (model->item(proxy->mapToSource(proxy->index(r, 0)).row()).id == id) {
                row = r;
            }
        }
        QVERIFY(row >= 0);
        list->setCurrentIndex(proxy->index(row, 0));
        QTRY_COMPARE(w.shownMessageId(), id);
        w.findChild<QAction *>(QStringLiteral("actionJunk"))->trigger();
        QTRY_VERIFY(L.g.modifyCalls.contains(id + QStringLiteral(":+SPAM")));
        QTRY_VERIFY(!L.session->cache()->message(id).labels.contains(QStringLiteral("INBOX")));
    }

    void junkAndNotJunkSelectNext()
    {
        // Like Delete: the selection moves to the next message (selectPastRemoved).
        Live L;
        const QString a = L.seed(QStringLiteral("Alpha"), {QStringLiteral("INBOX")});
        const QString b = L.seed(QStringLiteral("Bravo"), {QStringLiteral("INBOX")});
        const QString c = L.seed(QStringLiteral("Charlie"), {QStringLiteral("INBOX")});
        const QString d = L.seed(QStringLiteral("Delta"), {QStringLiteral("INBOX")});
        Q_UNUSED(a);
        QVERIFY(L.start());
        MainWindow w;
        w.setSession(L.session.get());
        QMetaObject::invokeMethod(L.session.get(), "ready");
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QTRY_VERIFY(w.isLive());
        auto *list = w.findChild<QTreeView *>(QStringLiteral("messageList"));
        QTRY_COMPARE(list->model()->rowCount(), 4);
        list->sortByColumn(MessageListModel::Subject, Qt::AscendingOrder);
        auto *proxy = qobject_cast<QSortFilterProxyModel *>(list->model());
        auto *model = w.findChild<MessageListModel *>();
        auto currentId = [&]() {
            const QModelIndex i = list->currentIndex();
            return i.isValid() ? model->item(proxy->mapToSource(i).row()).id : QString();
        };
        QAction *junk = w.findChild<QAction *>(QStringLiteral("actionJunk"));

        // Junk Bravo in In -> Charlie (the row below), shown in the preview;
        // then Charlie -> Delta.
        list->setCurrentIndex(proxy->index(1, 0));
        QTRY_COMPARE(w.shownMessageId(), b);
        junk->trigger();
        QCOMPARE(currentId(), c);
        QCOMPARE(w.shownMessageId(), c);
        QTRY_COMPARE(list->model()->rowCount(), 3);
        QCOMPARE(currentId(), c);
        junk->trigger();
        QCOMPARE(currentId(), d);
        QTRY_COMPARE(list->model()->rowCount(), 2);
        QCOMPARE(w.shownMessageId(), d);

        // Not Junk Bravo in Junk -> Charlie.
        w.showSpamFolder();
        QTRY_COMPARE(list->model()->rowCount(), 2); // Bravo, Charlie
        list->setCurrentIndex(proxy->index(0, 0));
        QTRY_COMPARE(w.shownMessageId(), b);
        w.findChild<QAction *>(QStringLiteral("menuActionNotJunk"))->trigger();
        QCOMPARE(currentId(), c);
        QTRY_COMPARE(list->model()->rowCount(), 1);
        QCOMPARE(currentId(), c);
        QCOMPARE(w.shownMessageId(), c);
    }
};

QTEST_MAIN(TstJunk)
#include "tst_junk.moc"
