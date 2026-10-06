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
#include "ui/MessageView.h"
#include "ui/MessageWindow.h"
#include "ui/SelectionAfterRemoval.h"
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

    // ---- selection after Delete ---------------------------------------------

    void selectionAfterRemoval_data()
    {
        QTest::addColumn<int>("count");
        QTest::addColumn<QList<int>>("removed");
        QTest::addColumn<int>("before");
        QTest::addColumn<int>("after");
        QTest::newRow("middle -> next") << 5 << QList<int>{2} << 3 << 2;
        QTest::newRow("first -> next") << 5 << QList<int>{0} << 1 << 0;
        QTest::newRow("last -> previous") << 5 << QList<int>{4} << 3 << 3;
        QTest::newRow("only row -> none") << 1 << QList<int>{0} << -1 << -1;
        QTest::newRow("empty list") << 0 << QList<int>{0} << -1 << -1;
        QTest::newRow("nothing removed") << 3 << QList<int>{} << -1 << -1;
        QTest::newRow("multi contiguous -> below") << 6 << QList<int>{1, 2, 3} << 4 << 1;
        QTest::newRow("multi gaps -> below bottom-most") << 6 << QList<int>{4, 1} << 5 << 3;
        QTest::newRow("multi at bottom -> closest above") << 6 << QList<int>{5, 3, 4} << 2 << 2;
        QTest::newRow("multi at bottom skips removed") << 6 << QList<int>{5, 4, 2} << 3 << 2;
        QTest::newRow("multi all -> none") << 3 << QList<int>{0, 1, 2} << -1 << -1;
        QTest::newRow("dupes / out of range ignored") << 4 << QList<int>{1, 1, 9, -1} << 2 << 1;
    }
    void selectionAfterRemoval()
    {
        QFETCH(int, count);
        QFETCH(QList<int>, removed);
        QFETCH(int, before);
        QFETCH(int, after);
        const SelectionAfterRemoval t = zmail::ui::selectionAfterRemoval(count, removed);
        QCOMPARE(t.rowBefore, before);
        QCOMPARE(t.rowAfter, after);
    }

    void deleteSelectsNextInSortedList()
    {
        Live L;
        const QDateTime now = QDateTime::currentDateTimeUtc();
        auto seed = [&](const QString &subject, int minutesAgo) {
            MockGoogle::Message m;
            m.from = QStringLiteral("Priya Raman <priya.raman@example.com>");
            m.to = QStringLiteral("Demo User <demo.user@example.com>");
            m.subject = subject;
            m.text = QStringLiteral("Fake test mail.");
            m.labels = {QStringLiteral("INBOX")};
            m.date = now.addSecs(-60 * minutesAgo);
            return L.g.addMessage(m, true);
        };
        // Dates in the opposite order to the subjects: sorting by Subject must win.
        const QString a = seed(QStringLiteral("Alpha"), 1);
        const QString b = seed(QStringLiteral("Bravo"), 2);
        const QString c = seed(QStringLiteral("Charlie"), 3);
        const QString d = seed(QStringLiteral("Delta"), 4);
        const QString e = seed(QStringLiteral("Echo"), 5);
        const QString f = seed(QStringLiteral("Foxtrot"), 6);
        QVERIFY(L.start());
        MainWindow w;
        w.setSession(L.session.get());
        QMetaObject::invokeMethod(L.session.get(), "ready");
        w.show();
        QVERIFY(QTest::qWaitForWindowActive(&w));
        QTRY_VERIFY(w.isLive());
        auto *list = w.findChild<QTreeView *>(QStringLiteral("messageList"));
        QTRY_COMPARE(list->model()->rowCount(), 6);
        list->sortByColumn(MessageListModel::Subject, Qt::AscendingOrder);
        auto *proxy = qobject_cast<QSortFilterProxyModel *>(list->model());
        auto *model = w.findChild<MessageListModel *>();
        auto idAt = [&](int r) { return model->item(proxy->mapToSource(proxy->index(r, 0)).row()).id; };
        auto currentId = [&]() { return list->currentIndex().isValid() ? idAt(list->currentIndex().row()) : QString(); };
        auto previewSubject = [&]() { return w.messageView()->message().subject; };
        QAction *toolbarDelete = w.findChild<QAction *>(QStringLiteral("actionDelete"));
        QCOMPARE((QStringList{idAt(0), idAt(1), idAt(2), idAt(3), idAt(4), idAt(5)}), (QStringList{a, b, c, d, e, f}));

        // Toolbar Delete on Bravo -> Charlie (the row below), shown in the preview.
        list->setFocus();
        list->setCurrentIndex(proxy->index(1, 0));
        QTRY_COMPARE(w.shownMessageId(), b);
        toolbarDelete->trigger();
        QCOMPARE(currentId(), c);
        QCOMPARE(w.shownMessageId(), c);
        QCOMPARE(previewSubject(), QStringLiteral("Charlie"));
        QTRY_COMPARE(list->model()->rowCount(), 5); // after the (async) model refresh...
        QCOMPARE(currentId(), c);                   // ...still Charlie
        QCOMPARE(w.shownMessageId(), c);
        QVERIFY(list->selectionModel()->isRowSelected(list->currentIndex().row(), {}));
        QVERIFY(toolbarDelete->isEnabled());
        QTRY_COMPARE(L.g.trashCalls, QStringList{b});
        QTRY_VERIFY(L.g.messages().value(b).labels.contains(QStringLiteral("TRASH")));
        QCOMPARE(currentId(), c); // Gmail's answer (a second refresh) keeps it too

        // Delete key on the last row (Foxtrot) -> the previous one (Echo).
        list->setCurrentIndex(proxy->index(4, 0));
        QTRY_COMPARE(w.shownMessageId(), f);
        QTest::keyClick(list, Qt::Key_Delete);
        QCOMPARE(currentId(), e);
        QTRY_COMPARE(list->model()->rowCount(), 4);
        QCOMPARE(currentId(), e);
        QCOMPARE(w.shownMessageId(), e);
        QCOMPARE(previewSubject(), QStringLiteral("Echo"));

        // Deleted from its own window while selected -> next row too.
        list->setCurrentIndex(proxy->index(0, 0)); // Alpha
        QTRY_COMPARE(w.shownMessageId(), a);
        MessageWindow *mw = w.openMessageWindow(list->currentIndex());
        QVERIFY(mw);
        mw->deleteAction()->trigger();
        QTRY_COMPARE(list->model()->rowCount(), 3);
        QCOMPARE(currentId(), c);
        QCOMPARE(w.shownMessageId(), c);

        // The chosen neighbour leaves too before the refresh (e.g. trashed
        // elsewhere): fall back to the same position.
        w.findChild<QAction *>(QStringLiteral("menuActionDelete"))->trigger(); // Charlie -> Delta
        QCOMPARE(currentId(), d);
        L.session->sync()->trash(d); // same tick, before the debounced reload
        QTRY_COMPARE(list->model()->rowCount(), 1);
        QTRY_COMPARE(currentId(), e);
        QCOMPARE(w.shownMessageId(), e);
        QCOMPARE(previewSubject(), QStringLiteral("Echo"));

        // Last message gone -> nothing selected, preview cleared.
        toolbarDelete->trigger();
        QVERIFY(!list->currentIndex().isValid());
        QTRY_COMPARE(list->model()->rowCount(), 0);
        QVERIFY(!list->currentIndex().isValid());
        QVERIFY(w.shownMessageId().isEmpty());
        QVERIFY(previewSubject().isEmpty());
        QVERIFY(!toolbarDelete->isEnabled());
    }

    void deleteOnlyMessageSelectsNothing()
    {
        Live L;
        const QString id = L.seed(QStringLiteral("Last one"), false);
        QVERIFY(L.start());
        MainWindow w;
        w.setSession(L.session.get());
        QMetaObject::invokeMethod(L.session.get(), "ready");
        w.show();
        QVERIFY(QTest::qWaitForWindowActive(&w));
        auto *list = w.findChild<QTreeView *>(QStringLiteral("messageList"));
        QTRY_COMPARE(list->model()->rowCount(), 1);
        list->setCurrentIndex(list->model()->index(0, 0));
        QTRY_COMPARE(w.shownMessageId(), id);
        w.findChild<QAction *>(QStringLiteral("actionDelete"))->trigger();
        QVERIFY(!list->currentIndex().isValid());
        QVERIFY(w.shownMessageId().isEmpty());
        QTRY_COMPARE(list->model()->rowCount(), 0);
        QVERIFY(!list->currentIndex().isValid());
        QVERIFY(!w.findChild<QAction *>(QStringLiteral("actionDelete"))->isEnabled());
    }
};

QTEST_MAIN(TstListActions)
#include "tst_listactions.moc"
