// Message-list actions (PR "list actions + undo"): right-click menus on the
// list and the mailboxes, Enter to open, Delete with Undo (users.messages
// .untrash against the mock), Delete disabled in an empty mailbox, Mark
// Read/Unread, compose Ctrl+B/I/U/K and shortcut hints in tooltips.
#include "MainWindow.hpp"
#include "core/AuthManager.h"
#include "core/MailCache.h"
#include "core/MailSession.h"
#include "core/SyncEngine.h"
#include "core/TokenStore.h"
#include "mock/MockGoogle.h"
#include "ui/ComposeWindow.h"
#include "ui/MessageListModel.h"
#include "ui/MessageWindow.h"
#include "ui/Theme.h"

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTextEdit>
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
    QString seed(const QString &subject, bool unread)
    {
        MockGoogle::Message m;
        m.from = QStringLiteral("Priya Raman <priya.raman@example.com>");
        m.to = QStringLiteral("Demo User <demo.user@example.com>");
        m.subject = subject;
        m.text = QStringLiteral("Fake test mail.");
        m.labels = {QStringLiteral("INBOX")};
        if (unread) {
            m.labels << QStringLiteral("UNREAD");
        }
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

QStringList actionNames(QMenu *m)
{
    QStringList out;
    for (QAction *a : m->actions()) {
        if (!a->isSeparator() && a->isVisible()) {
            out << a->objectName();
        }
    }
    return out;
}

QMenu *waitMenu(const char *name)
{
    QMenu *found = nullptr;
    (void)QTest::qWaitFor([&] {
        for (QWidget *w : QApplication::topLevelWidgets()) {
            if (auto *m = qobject_cast<QMenu *>(w); m && m->isVisible() && m->objectName() == QLatin1String(name)) {
                found = m;
                return true;
            }
        }
        return false;
    }, 3000);
    return found;
}
} // namespace

class TstListActions : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QSettings().clear();
        applyTheme(ThemeMode::Light);
    }

    void listContextMenu()
    {
        MainWindow w;
        w.show();
        auto *list = w.findChild<QTreeView *>(QStringLiteral("messageList"));
        QVERIFY(list->model()->rowCount() > 1);
        QCOMPARE(list->contextMenuPolicy(), Qt::CustomContextMenu);
        // Right-click row 1: it becomes current and the menu opens.
        const QRect r = list->visualRect(list->model()->index(1, 0));
        emit list->customContextMenuRequested(r.center());
        QMenu *menu = waitMenu("messageListMenu");
        QVERIFY(menu);
        QCOMPARE(list->currentIndex().row(), 1);
        const QStringList names = actionNames(menu);
        for (const char *n : {"actionOpenMessage", "menuActionReply", "menuActionReplyAll", "menuActionForward",
                              "menuActionJunk", "menuActionDelete", "actionCopyAddress"}) {
            QVERIFY2(names.contains(QLatin1String(n)), n);
        }
        // Exactly one of Mark Read / Mark Unread, matching the row.
        QCOMPARE(int(names.contains(QStringLiteral("actionMarkRead"))) +
                     int(names.contains(QStringLiteral("actionMarkUnread"))), 1);
        // The menu shows the menu-bar shortcut (it's the same action).
        QAction *del = w.findChild<QAction *>(QStringLiteral("menuActionDelete"));
        QCOMPARE(del->shortcut(), QKeySequence(QKeySequence::Delete));
        menu->close();
        QTRY_VERIFY(del->isVisible());
        QVERIFY(w.findChild<QAction *>(QStringLiteral("actionMarkRead"))->isVisible());
    }

    void mailboxContextMenu()
    {
        MainWindow w;
        w.show();
        auto *tree = w.findChild<QTreeWidget *>(QStringLiteral("mailboxTree"));
        QVERIFY(tree);
        QCOMPARE(tree->contextMenuPolicy(), Qt::CustomContextMenu);
        QTreeWidgetItem *inbox = tree->topLevelItem(0);
        emit tree->customContextMenuRequested(tree->visualItemRect(inbox).center());
        QMenu *menu = waitMenu("mailboxMenu");
        QVERIFY(menu);
        const QStringList names = actionNames(menu);
        for (const char *n : {"actionMarkAllRead", "actionCheckMail", "actionExpandAll", "actionCollapseAll"}) {
            QVERIFY2(names.contains(QLatin1String(n)), n);
        }
        // Sample Inbox has unread mail; Mark All as Read clears it.
        QAction *all = menu->findChild<QAction *>(QStringLiteral("actionMarkAllRead"));
        QVERIFY(all && all->isEnabled());
        all->trigger();
        menu->close();
        auto *model = w.findChild<MessageListModel *>();
        QVERIFY(model);
        const QString key = inbox->data(0, Qt::UserRole).toString();
        for (const MailItem &m : model->items()) {
            if (m.mailboxes.contains(key)) {
                QVERIFY(m.status != MailStatus::Unread);
            }
        }
        QVERIFY(inbox->text(1).isEmpty()); // count gone
    }

    void markReadUnreadSample()
    {
        MainWindow w;
        w.show();
        auto *list = w.findChild<QTreeView *>(QStringLiteral("messageList"));
        auto *model = w.findChild<MessageListModel *>();
        list->setCurrentIndex(list->model()->index(0, 0));
        auto *proxy = qobject_cast<QSortFilterProxyModel *>(list->model());
        const int row = proxy->mapToSource(list->currentIndex()).row();
        w.findChild<QAction *>(QStringLiteral("actionMarkUnread"))->trigger();
        QCOMPARE(model->item(row).status, MailStatus::Unread);
        w.findChild<QAction *>(QStringLiteral("actionMarkRead"))->trigger();
        QCOMPARE(model->item(row).status, MailStatus::Read);
    }

    void deleteDisabledInEmptyMailbox()
    {
        MainWindow w;
        w.show();
        auto *list = w.findChild<QTreeView *>(QStringLiteral("messageList"));
        auto *tree = w.findChild<QTreeWidget *>(QStringLiteral("mailboxTree"));
        QAction *del = w.findChild<QAction *>(QStringLiteral("actionDelete"));
        QAction *menuDel = w.findChild<QAction *>(QStringLiteral("menuActionDelete"));
        list->setCurrentIndex(list->model()->index(0, 0));
        QVERIFY(del->isEnabled());
        QVERIFY(menuDel->isEnabled());
        // Every mailbox: Delete only when there's a message to delete.
        for (QTreeWidgetItemIterator it(tree); *it; ++it) {
            if ((*it)->data(0, Qt::UserRole).toString().isEmpty()) {
                continue;
            }
            tree->setCurrentItem(*it);
            QCOMPARE(del->isEnabled(), list->model()->rowCount() > 0);
            QCOMPARE(menuDel->isEnabled(), list->model()->rowCount() > 0);
        }
        // An empty mailbox (sample mail has none, so an empty one by key).
        w.selectMailbox(QStringLiteral("label:No Such Label"));
        QCOMPARE(list->model()->rowCount(), 0);
        QVERIFY(!del->isEnabled());
        QVERIFY(!menuDel->isEnabled());
        QVERIFY(!w.findChild<QAction *>(QStringLiteral("actionMarkRead"))->isEnabled());
        w.selectMailbox(tree->topLevelItem(0)->data(0, Qt::UserRole).toString());
        QVERIFY(del->isEnabled());
    }

    void toolbarTooltipsNameShortcuts()
    {
        MainWindow w;
        const QString n = QKeySequence(Qt::CTRL | Qt::Key_R).toString(QKeySequence::NativeText);
        QCOMPARE(w.findChild<QAction *>(QStringLiteral("actionReply"))->toolTip(), QStringLiteral("Reply to sender (%1)").arg(n));
        for (const char *obj : {"actionCheckMail", "actionNewMessage", "actionReply", "actionReplyAll", "actionForward",
                                "actionDelete"}) {
            const QString tip = w.findChild<QAction *>(QString::fromLatin1(obj))->toolTip();
            QVERIFY2(tip.endsWith(QLatin1Char(')')) && tip.contains(QLatin1String(" (")), qPrintable(tip));
            QVERIFY2(tip.count(QLatin1Char('(')) == 1, qPrintable(tip)); // no doubled hint
        }
        QCOMPARE(w.findChild<QAction *>(QStringLiteral("actionAttach"))->toolTip(), QStringLiteral("New message with an attachment"));
    }

    void composeFormatShortcuts()
    {
        ComposeWindow c;
        c.setConfirmOnClose(false);
        c.show();
        QVERIFY(QTest::qWaitForWindowActive(&c));
        auto *body = c.findChild<QTextEdit *>(QStringLiteral("composeBody"));
        body->setFocus();
        QAction *bold = c.findChild<QAction *>(QStringLiteral("actionBold"));
        QAction *italic = c.findChild<QAction *>(QStringLiteral("actionItalic"));
        QAction *underline = c.findChild<QAction *>(QStringLiteral("actionUnderline"));
        QAction *link = c.findChild<QAction *>(QStringLiteral("actionLink"));
        QCOMPARE(bold->shortcut(), QKeySequence(QKeySequence::Bold));
        QCOMPARE(italic->shortcut(), QKeySequence(QKeySequence::Italic));
        QCOMPARE(underline->shortcut(), QKeySequence(QKeySequence::Underline));
        QCOMPARE(link->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_K));
        const QString kb = QKeySequence(QKeySequence::Bold).toString(QKeySequence::NativeText);
        QCOMPARE(bold->toolTip(), QStringLiteral("Bold (%1)").arg(kb));
        QVERIFY(link->toolTip().endsWith(QKeySequence(Qt::CTRL | Qt::Key_K).toString(QKeySequence::NativeText) + ')'));

        QTest::keyClick(body, Qt::Key_B, Qt::ControlModifier);
        QVERIFY(bold->isChecked());
        QTest::keyClicks(body, QStringLiteral("hi"));
        QCOMPARE(body->currentCharFormat().fontWeight(), int(QFont::Bold));
        QTest::keyClick(body, Qt::Key_B, Qt::ControlModifier);
        QVERIFY(!bold->isChecked());
        QTest::keyClick(body, Qt::Key_I, Qt::ControlModifier);
        QVERIFY(italic->isChecked());
        QTest::keyClick(body, Qt::Key_U, Qt::ControlModifier);
        QVERIFY(underline->isChecked());

        // Ctrl+K reaches Insert link instead of QTextEdit's delete-to-end.
        QSignalSpy linkSpy(link, &QAction::triggered);
        QTimer::singleShot(0, &c, [] {
            // Cancel the address prompt.
            QTimer::singleShot(200, [] {
                if (QWidget *m = QApplication::activeModalWidget()) {
                    m->close();
                }
            });
        });
        QTest::keyClick(body, Qt::Key_K, Qt::ControlModifier);
        QTRY_COMPARE(linkSpy.size(), 1);
        QVERIFY(body->toPlainText().contains(QLatin1String("hi")));
    }

    // ---- live, against the mock --------------------------------------------

    void enterOpensDeleteUndoRestores()
    {
        Live L;
        const QString keep = L.seed(QStringLiteral("Older note"), false);
        const QString id = L.seed(QStringLiteral("Quarterly numbers"), true);
        Q_UNUSED(keep);
        QVERIFY(L.start());
        MainWindow w;
        w.setSession(L.session.get());
        QMetaObject::invokeMethod(L.session.get(), "ready");
        w.show();
        QVERIFY(QTest::qWaitForWindowActive(&w));
        QTRY_VERIFY(w.isLive());
        auto *list = w.findChild<QTreeView *>(QStringLiteral("messageList"));
        QTRY_COMPARE(list->model()->rowCount(), 2);
        QAction *del = w.findChild<QAction *>(QStringLiteral("menuActionDelete"));
        QVERIFY(!del->isEnabled()); // nothing selected
        w.selectMailbox(QStringLiteral("Trash")); // empty in the mock
        QTRY_COMPARE(list->model()->rowCount(), 0);
        QVERIFY(!del->isEnabled());
        QVERIFY(!w.findChild<QAction *>(QStringLiteral("actionDelete"))->isEnabled());
        w.selectMailbox(QStringLiteral("In"));
        QTRY_COMPARE(list->model()->rowCount(), 2);
        int row = -1;
        auto *proxy = qobject_cast<QSortFilterProxyModel *>(list->model());
        auto *model = w.findChild<MessageListModel *>();
        for (int r = 0; r < proxy->rowCount(); ++r) {
            if (model->item(proxy->mapToSource(proxy->index(r, 0)).row()).id == id) {
                row = r;
            }
        }
        QVERIFY(row >= 0);
        list->setFocus();
        list->setCurrentIndex(proxy->index(row, 0));
        QTRY_COMPARE(w.shownMessageId(), id);
        QTRY_VERIFY(del->isEnabled());

        // Enter opens the message in its own window.
        QTest::keyClick(list, Qt::Key_Return);
        QTRY_COMPARE(w.messageWindows().size(), 1);
        w.messageWindows().first()->close();
        w.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&w));
        list->setFocus();

        // Delete key -> trash, with Undo offered in the status bar.
        QTest::keyClick(list, Qt::Key_Delete);
        QTRY_COMPARE(L.g.trashCalls, QStringList{id});
        QTRY_COMPARE(list->model()->rowCount(), 1);
        auto *bar = w.statusBar()->findChild<QWidget *>(QStringLiteral("undoDeleteBar"));
        QVERIFY(bar && bar->isVisible());
        auto *undoBtn = bar->findChild<QToolButton *>(QStringLiteral("undoDeleteButton"));
        QCOMPARE(undoBtn->text(), QStringLiteral("Undo"));
        QVERIFY(undoBtn->toolTip().contains(QKeySequence(QKeySequence::Undo).toString(QKeySequence::NativeText)));
        QAction *undo = w.findChild<QAction *>(QStringLiteral("actionUndoDelete"));
        QVERIFY(undo->isEnabled());

        // Undo -> untrash, and INBOX/UNREAD (which untrash leaves off) put back.
        undoBtn->click();
        QVERIFY(!bar->isVisible());
        QVERIFY(!undo->isEnabled());
        QTRY_COMPARE(L.g.untrashCalls, QStringList{id});
        QTRY_VERIFY(L.g.modifyCalls.join(' ').contains(id + QStringLiteral(":+INBOX")));
        QTRY_VERIFY(L.g.messages().value(id).labels.contains(QStringLiteral("INBOX")));
        QVERIFY(!L.g.messages().value(id).labels.contains(QStringLiteral("TRASH")));
        QTRY_COMPARE(list->model()->rowCount(), 2);
        QVERIFY(L.session->cache()->message(id).labels.contains(QStringLiteral("INBOX")));
    }

    void undoBeforeTrashAnswerStillRestores()
    {
        // Undo pressed before Gmail answered the trash: the untrash waits
        // for it rather than racing it.
        Live L;
        const QString id = L.seed(QStringLiteral("Fast fingers"), false);
        QVERIFY(L.start());
        SyncEngine *sync = L.session->sync();
        sync->trash(id);
        QVERIFY(sync->untrash(id)); // same tick: trash still in flight
        QVERIFY(L.session->cache()->message(id).labels.contains(QStringLiteral("INBOX")));
        QTRY_COMPARE(L.g.trashCalls, QStringList{id});
        QTRY_COMPARE(L.g.untrashCalls, QStringList{id});
        QTRY_VERIFY(L.g.messages().value(id).labels.contains(QStringLiteral("INBOX")));
        QVERIFY(!L.g.messages().value(id).labels.contains(QStringLiteral("TRASH")));
        QVERIFY(!sync->untrash(id)); // nothing left to undo
    }

    void undoExpires()
    {
        QVERIFY(MainWindow::kUndoDeleteMs >= 5000 && MainWindow::kUndoDeleteMs <= 15000);
    }

    void markUnreadLive()
    {
        Live L;
        const QString id = L.seed(QStringLiteral("Read me"), false);
        QVERIFY(L.start());
        L.session->sync()->markUnread(id);
        QVERIFY(L.session->cache()->message(id).unread());
        QTRY_VERIFY(L.g.modifyCalls.contains(id + QStringLiteral(":+UNREAD")));
        QTRY_VERIFY(L.g.messages().value(id).labels.contains(QStringLiteral("UNREAD")));
    }
};

QTEST_MAIN(TstListActions)
#include "tst_listactions.moc"
