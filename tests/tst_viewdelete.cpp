// Delete outside the Inbox (0.4.2): in a Gmail label view (and Sent, Snoozed,
// Junk, Search results) Delete must trash the message exactly like the Inbox
// does: users.messages.trash goes out, the cache gets TRASH, the row leaves
// the list and the selection moves on; Undo brings it back. If Gmail refuses,
// the row and the selection go back where they were and the error is shown.
// All mail here is fake (MockGoogle, example.com addresses).
#include "MainWindow.hpp"
#include "core/MailCache.h"
#include "core/MailSession.h"
#include "core/SyncEngine.h"
#include "core/TokenStore.h"
#include "mock/MockGoogle.h"
#include "ui/MessageListModel.h"
#include "ui/MessageView.h"
#include "ui/Theme.h"

#include <QAction>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
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

const QString kLabel = QStringLiteral("Label_7");

// Three fake messages, Alpha / Bravo / Charlie, in the view under test.
struct Fixture
{
    MockGoogle g;
    MemoryTokenStore store;
    std::unique_ptr<MailSession> session;
    std::unique_ptr<MainWindow> w;
    QString a, b, c;
    QTreeView *list = nullptr;
    QSortFilterProxyModel *proxy = nullptr;
    MessageListModel *model = nullptr;

    QString seed(const QString &subject, const QStringList &labels, int minutesAgo)
    {
        MockGoogle::Message m;
        m.from = QStringLiteral("Priya Raman <priya.raman@example.com>");
        m.to = QStringLiteral("Demo User <demo.user@example.com>");
        m.subject = subject;
        m.text = QStringLiteral("Fake test mail.");
        m.labels = labels;
        m.date = QDateTime::currentDateTimeUtc().addSecs(-60 * minutesAgo);
        return g.addMessage(m, true);
    }

    // view: "gmail:Label_7", "gmail:STARRED", "Out", "Junk", "Snoozed", "Search", "In"
    bool open(const QString &view, const QStringList &labels)
    {
        g.listen();
        g.seedSystemLabels();
        g.addLabel({kLabel, QStringLiteral("Projects"), QStringLiteral("user"), {}});
        a = seed(QStringLiteral("Alpha zebra"), labels, 1);
        b = seed(QStringLiteral("Bravo zebra"), labels, 2);
        c = seed(QStringLiteral("Charlie zebra"), labels, 3);
        session = std::make_unique<MailSession>(mockOptions(g, &store));
        session->signIn();
        if (!QTest::qWaitFor([this] { return session->state() == MailSession::State::SignedIn; }, 10000) ||
            !QTest::qWaitFor([this] { return session->sync() && !session->sync()->isBusy(); }, 15000)) {
            return false;
        }
        if (view == QLatin1String("Snoozed")) {
            const qint64 wake = QDateTime::currentMSecsSinceEpoch() + 24 * 3600 * 1000;
            for (const QString &id : {a, b, c}) {
                session->sync()->snooze(id, wake);
            }
        }
        w = std::make_unique<MainWindow>();
        w->setSession(session.get());
        QMetaObject::invokeMethod(session.get(), "ready");
        w->show();
        if (!QTest::qWaitForWindowActive(w.get()) || !QTest::qWaitFor([this] { return w->isLive(); }, 5000)) {
            return false;
        }
        list = w->findChild<QTreeView *>(QStringLiteral("messageList"));
        proxy = qobject_cast<QSortFilterProxyModel *>(list->model());
        model = w->findChild<MessageListModel *>();
        if (view == QLatin1String("Search")) {
            w->findChild<QLineEdit *>(QStringLiteral("searchBox"))->setText(QStringLiteral("zebra"));
        } else {
            selectView(view);
        }
        if (!QTest::qWaitFor([this] { return proxy->rowCount() == 3; }, 10000)) {
            qWarning() << "view" << view << "has" << proxy->rowCount() << "rows";
            return false;
        }
        list->sortByColumn(MessageListModel::Subject, Qt::AscendingOrder);
        list->setFocus();
        list->setCurrentIndex(proxy->index(1, 0)); // Bravo
        return QTest::qWaitFor([this] { return w->shownMessageId() == b; }, 3000);
    }
    // Click the mailbox in the tree like a user (that also fetches the
    // label's first page); Junk is hidden by Hide Spam, so select it by key.
    void selectView(const QString &view)
    {
        auto *tree = w->findChild<QTreeWidget *>(QStringLiteral("mailboxTree"));
        for (QTreeWidgetItemIterator it(tree); *it; ++it) {
            if ((*it)->data(0, Qt::UserRole).toString() == view) {
                tree->setCurrentItem(*it);
                return;
            }
        }
        w->selectMailbox(view);
        session->sync()->ensureLabel(view == QLatin1String("Junk") ? QStringLiteral("SPAM") : view.section(QLatin1Char(':'), 1));
    }
    QString idAt(int r) const { return model->item(proxy->mapToSource(proxy->index(r, 0)).row()).id; }
    QString currentId() const { return list->currentIndex().isValid() ? idAt(list->currentIndex().row()) : QString(); }
    QStringList visibleIds() const
    {
        QStringList out;
        for (int r = 0; r < proxy->rowCount(); ++r) {
            out << idAt(r);
        }
        return out;
    }
    QWidget *undoBar() const { return w->statusBar()->findChild<QWidget *>(QStringLiteral("undoDeleteBar")); }
};
} // namespace

class TstViewDelete : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QSettings().clear();
        applyTheme(ThemeMode::Light);
    }

    void deleteTrashesInEveryView_data()
    {
        QTest::addColumn<QString>("view");
        QTest::addColumn<QStringList>("labels");
        QTest::newRow("Inbox") << "In" << QStringList{"INBOX"};
        QTest::newRow("Gmail label, archived") << "gmail:Label_7" << QStringList{kLabel};
        QTest::newRow("Gmail label, also in Inbox") << "gmail:Label_7" << QStringList{"INBOX", kLabel};
        QTest::newRow("Starred") << "gmail:STARRED" << QStringList{"INBOX", "STARRED"};
        QTest::newRow("Out (Sent)") << "Out" << QStringList{"SENT"};
        QTest::newRow("Junk") << "Junk" << QStringList{"SPAM"};
        QTest::newRow("Snoozed") << "Snoozed" << QStringList{"INBOX"};
        QTest::newRow("Search results") << "Search" << QStringList{"INBOX", kLabel};
    }
    void deleteTrashesInEveryView()
    {
        QFETCH(QString, view);
        QFETCH(QStringList, labels);
        Fixture f;
        QVERIFY(f.open(view, labels));

        QTest::keyClick(f.list, Qt::Key_Delete);
        // The selection moves on to Charlie at once...
        QCOMPARE(f.currentId(), f.c);
        // ...the right call goes out and Gmail trashes it...
        QTRY_COMPARE(f.g.trashCalls, QStringList{f.b});
        QTRY_VERIFY(f.g.messages().value(f.b).labels.contains(QStringLiteral("TRASH")));
        // ...the cache agrees, and the row leaves the list (and stays gone
        // after Gmail's answer refreshes it).
        QVERIFY(f.session->cache()->message(f.b).labels.contains(QStringLiteral("TRASH")));
        QTRY_COMPARE(f.proxy->rowCount(), 2);
        QTest::qWait(400);
        QCOMPARE(f.visibleIds(), (QStringList{f.a, f.c}));
        QCOMPARE(f.currentId(), f.c);
        QCOMPARE(f.w->shownMessageId(), f.c);
        QCOMPARE(f.w->messageView()->message().subject, QStringLiteral("Charlie zebra"));
        // In the Trash mailbox now.
        if (view != QLatin1String("Search")) {
            f.w->selectMailbox(QStringLiteral("Trash"));
            QTRY_VERIFY(f.visibleIds().contains(f.b));
            f.selectView(view);
            QTRY_COMPARE(f.proxy->rowCount(), 2);
        }

        // Undo puts it back in the same view.
        QWidget *bar = f.undoBar();
        QVERIFY(bar && bar->isVisible());
        bar->findChild<QToolButton *>(QStringLiteral("undoDeleteButton"))->click();
        QTRY_COMPARE(f.g.untrashCalls, QStringList{f.b});
        QTRY_VERIFY(!f.g.messages().value(f.b).labels.contains(QStringLiteral("TRASH")));
        QTRY_COMPARE(f.proxy->rowCount(), 3);
        QVERIFY(f.visibleIds().contains(f.b));
        if (view == QLatin1String("Snoozed")) {
            // Still snoozed, so still out of the Inbox until it wakes.
            QVERIFY(f.session->cache()->snooze(f.b).wakeMs > 0);
            QVERIFY(!f.session->cache()->message(f.b).labels.contains(QStringLiteral("INBOX")));
        } else {
            for (const QString &l : labels) {
                QTRY_VERIFY2(f.session->cache()->message(f.b).labels.contains(l), qPrintable(l));
            }
        }
    }

    void failedDeleteRestoresRowAndSelection_data()
    {
        QTest::addColumn<QString>("view");
        QTest::addColumn<QStringList>("labels");
        QTest::newRow("Inbox") << "In" << QStringList{"INBOX"};
        QTest::newRow("Gmail label") << "gmail:Label_7" << QStringList{kLabel};
    }
    void failedDeleteRestoresRowAndSelection()
    {
        QFETCH(QString, view);
        QFETCH(QStringList, labels);
        Fixture f;
        QVERIFY(f.open(view, labels));
        // Gmail refuses this one trash call (400: not retried).
        f.g.addFault({QStringLiteral("/gmail/v1/users/me/messages/") + f.b + QStringLiteral("/trash"), 400, 1, -1});

        QTest::keyClick(f.list, Qt::Key_Delete);
        QCOMPARE(f.currentId(), f.c); // optimistic, as before
        QTRY_VERIFY(f.g.count(QStringLiteral("POST /gmail/v1/users/me/messages/") + f.b + QStringLiteral("/trash")) == 1);
        QVERIFY(f.g.trashCalls.isEmpty()); // the fault answered, not the handler

        // The row and the selection come back to Bravo, with the error shown.
        QTRY_COMPARE(f.currentId(), f.b);
        QCOMPARE(f.proxy->rowCount(), 3);
        QCOMPARE(f.visibleIds(), (QStringList{f.a, f.b, f.c}));
        QTRY_COMPARE(f.w->shownMessageId(), f.b);
        QTRY_COMPARE(f.w->messageView()->message().subject, QStringLiteral("Bravo zebra"));
        QVERIFY2(f.w->statusBar()->currentMessage().contains(QLatin1String("Moving to Trash failed")),
                 qPrintable(f.w->statusBar()->currentMessage()));
        QVERIFY(!f.session->cache()->message(f.b).labels.contains(QStringLiteral("TRASH")));
        QVERIFY(!f.g.messages().value(f.b).labels.contains(QStringLiteral("TRASH")));
        // Nothing to undo.
        QWidget *bar = f.undoBar();
        QVERIFY(!bar || !bar->isVisible());
        QVERIFY(!f.w->findChild<QAction *>(QStringLiteral("actionUndoDelete"))->isEnabled());

        // The next try works.
        QTest::keyClick(f.list, Qt::Key_Delete);
        QTRY_COMPARE(f.g.trashCalls, QStringList{f.b});
        QTRY_COMPARE(f.proxy->rowCount(), 2);
        QCOMPARE(f.currentId(), f.c);
    }

    void failedDeleteKeepsAnUnrelatedSelection()
    {
        // The user moved on before Gmail refused: don't yank the selection back.
        Fixture f;
        QVERIFY(f.open(QStringLiteral("gmail:Label_7"), {kLabel}));
        f.g.addFault({QStringLiteral("/gmail/v1/users/me/messages/") + f.b + QStringLiteral("/trash"), 400, 1, -1});
        QTest::keyClick(f.list, Qt::Key_Delete);
        QCOMPARE(f.currentId(), f.c);
        f.list->setCurrentIndex(f.proxy->index(0, 0)); // Alpha, same tick
        QTRY_VERIFY(f.g.count(QStringLiteral("POST /gmail/v1/users/me/messages/") + f.b + QStringLiteral("/trash")) == 1);
        QTRY_COMPARE(f.proxy->rowCount(), 3);
        QTest::qWait(300);
        QCOMPARE(f.currentId(), f.a);
        QCOMPARE(f.w->shownMessageId(), f.a);
    }

    void trashedSnoozeDoesNotWakeIntoInbox()
    {
        // A snoozed message deleted before it wakes must not come back to In.
        Fixture f;
        QVERIFY(f.open(QStringLiteral("Snoozed"), {QStringLiteral("INBOX")}));
        QTest::keyClick(f.list, Qt::Key_Delete);
        QTRY_COMPARE(f.g.trashCalls, QStringList{f.b});
        QTRY_VERIFY(f.session->cache()->message(f.b).labels.contains(QStringLiteral("TRASH")));
        f.session->sync()->wakeDue(QDateTime::currentMSecsSinceEpoch() + 48 * 3600 * 1000);
        QTest::qWait(300);
        QVERIFY(!f.session->cache()->message(f.b).labels.contains(QStringLiteral("INBOX")));
        QVERIFY(!f.g.modifyCalls.contains(f.b + QStringLiteral(":+INBOX")));
        QVERIFY(f.session->cache()->message(f.a).labels.contains(QStringLiteral("INBOX"))); // others still wake
    }
};

QTEST_MAIN(TstViewDelete)
#include "tst_viewdelete.moc"
