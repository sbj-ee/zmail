// Folders on a "Gmail (app password)" account, in the main window: nested
// create under an existing folder (Finance -> Finance/Bills), drag-and-drop
// onto any folder (including one made a moment ago), the right-click and
// Transfer menus, rename/delete of nested folders and the counts. Against
// the fake IMAP server; made-up passwords only.
#include "MainWindow.hpp"
#include "core/AppPasswordAccount.h"
#include "core/ImapSyncEngine.h"
#include "core/MailCache.h"
#include "core/MailSession.h"
#include "core/TokenStore.h"
#include "mock/FakeImapServer.h"
#include "ui/MessageListModel.h"

#include <QApplication>
#include <QDropEvent>
#include <QMenu>
#include <QMimeData>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>
#include <QTreeView>
#include <QTreeWidget>

using namespace zmail;
using zmail::test::FakeImapServer;
using zmail::ui::MessageListModel;

namespace {

struct Rig
{
    FakeImapServer imap;
    MemoryTokenStore store;
    std::unique_ptr<MailSession> session;
    std::unique_ptr<MainWindow> w;
    QTreeWidget *tree = nullptr;

    Rig()
    {
        imap.seedDemo(10);
        imap.userFolders.append(QStringLiteral("Finance")); // Stephen's existing label
        store.values.insert(AppPasswordAccount::appPasswordKey(imap.user), imap.password);
        AppPasswordAccount a;
        a.email = imap.user;
        a.imapHost = QStringLiteral("127.0.0.1");
        a.imapPort = imap.port();
        a.smtpHost = QStringLiteral("127.0.0.1");
        a.smtpPort = 1;
        QSettings s;
        s.clear();
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
        w->resize(1100, 760);
        w->setSession(session.get());
        session->restoreSaved();
        w->show();
        tree = w->findChild<QTreeWidget *>(QStringLiteral("mailboxTree"));
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
    QStringList menuFor(QTreeWidgetItem *row)
    {
        tree->scrollToItem(row);
        emit tree->customContextMenuRequested(tree->visualItemRect(row).center());
        QMenu *menu = nullptr;
        (void)QTest::qWaitFor([&] {
            for (QWidget *x : QApplication::topLevelWidgets()) {
                if (auto *m = qobject_cast<QMenu *>(x); m && m->isVisible() && m->objectName() == QLatin1String("mailboxMenu")) {
                    menu = m;
                }
            }
            return menu != nullptr;
        }, 3000);
        QStringList names;
        if (menu) {
            for (QAction *a : menu->actions()) {
                names << a->objectName();
            }
            menu->close();
        }
        return names;
    }
    bool settle()
    {
        return QTest::qWaitFor([this] { return session->sync() && !session->sync()->isBusy() && imap.anyIdling(); }, 10000);
    }
    // Drag `id` from wherever onto the sidebar row `key`; true if the drop was taken.
    bool drag(const QString &id, const QString &key)
    {
        QTreeWidgetItem *row = item(key);
        if (!row) {
            return false;
        }
        tree->scrollToItem(row);
        const QPoint at = tree->visualItemRect(row).center();
        QMimeData mime;
        mime.setData(QByteArray(MessageListModel::kMessageIdsMime), id.toUtf8());
        QDragEnterEvent enter(at, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &enter);
        QDragMoveEvent move(at, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &move);
        if (!move.isAccepted() || tree->property("dropTarget").toString() != key) {
            return false;
        }
        QDropEvent drop(at, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &drop);
        return drop.isAccepted();
    }
    quint64 msgid(const char *subject)
    {
        for (quint64 id = 0x18f3a0c2b4d50001ULL; id < 0x18f3a0c2b4d50000ULL + 500; ++id) {
            if (auto *m = imap.find(id); m && m->raw.contains(subject)) {
                return id;
            }
        }
        return 0;
    }
};

} // namespace

class TstImapFolders : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    // Bug 1: right-click Finance > New Folder makes Finance/Bills, shown nested.
    void newFolderUnderAnExistingFolder()
    {
        Rig r;
        QTRY_VERIFY_WITH_TIMEOUT(r.item(QStringLiteral("gmail:Finance")), 15000);
        QVERIFY(r.settle());
        const QStringList names = r.menuFor(r.item(QStringLiteral("gmail:Finance")));
        QVERIFY2(names.contains(QStringLiteral("actionNewFolder")), qPrintable(names.join(',')));
        QVERIFY(names.contains(QStringLiteral("actionRenameFolder")));
        QVERIFY(names.contains(QStringLiteral("actionDeleteFolder")));
        QCOMPARE(r.w->newFolderPrefixForTests(QStringLiteral("gmail:Finance")), QStringLiteral("Finance/"));

        r.session->sync()->createLabel(QStringLiteral("Finance/Bills"));
        QTRY_VERIFY_WITH_TIMEOUT(r.imap.userFolders.contains(QStringLiteral("Finance/Bills")), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(r.item(QStringLiteral("gmail:Finance/Bills")), 10000);
        QTreeWidgetItem *bills = r.item(QStringLiteral("gmail:Finance/Bills"));
        QCOMPARE(bills->text(0), QStringLiteral("Bills"));
        QCOMPARE(bills->parent(), r.item(QStringLiteral("gmail:Finance"))); // nested under Finance
        // and a sub-folder of the sub-folder works the same way
        QCOMPARE(r.w->newFolderPrefixForTests(QStringLiteral("gmail:Finance/Bills")), QStringLiteral("Finance/Bills/"));
        QVERIFY(r.settle());
    }

    // Bug 2: a folder made in zmail takes dropped mail; the counts follow.
    void dragOntoANewFolderMovesTheMessage()
    {
        Rig r;
        QTRY_VERIFY_WITH_TIMEOUT(r.item(QStringLiteral("gmail:Finance")), 15000);
        QVERIFY(r.settle());
        r.session->sync()->createLabel(QStringLiteral("Bills"));
        QTRY_VERIFY_WITH_TIMEOUT(r.item(QStringLiteral("gmail:Bills")), 10000);
        QVERIFY(r.settle());
        const quint64 id = r.msgid("Q4 field schedule");
        QVERIFY(id);
        const QString hex = QString::number(id, 16);
        QVERIFY(r.drag(hex, QStringLiteral("gmail:Bills")));
        QTRY_VERIFY_WITH_TIMEOUT(r.imap.find(id)->labels.contains(QStringLiteral("Bills")), 10000);
        QVERIFY(!r.imap.find(id)->labels.contains(QStringLiteral("\\Inbox")));
        QTRY_COMPARE_WITH_TIMEOUT(r.item(QStringLiteral("gmail:Bills"))->text(1).isEmpty(), false, 10000); // count shows
        // The Transfer menu lists it too.
        QTRY_VERIFY(r.w->findChild<QAction *>(QStringLiteral("transfer_Bills")));
        // Nested: drag into Finance/Bills, then back to In.
        r.session->sync()->createLabel(QStringLiteral("Finance/Bills"));
        QTRY_VERIFY_WITH_TIMEOUT(r.item(QStringLiteral("gmail:Finance/Bills")), 10000);
        QVERIFY(r.settle());
        QVERIFY(r.drag(hex, QStringLiteral("gmail:Finance/Bills")));
        QTRY_VERIFY_WITH_TIMEOUT(r.imap.find(id)->labels.contains(QStringLiteral("Finance/Bills")), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!r.imap.find(id)->labels.contains(QStringLiteral("Bills")), 10000);
        QVERIFY(r.drag(hex, QStringLiteral("In")));
        QTRY_VERIFY_WITH_TIMEOUT(r.imap.find(id)->labels.contains(QStringLiteral("\\Inbox")), 10000);
        QVERIFY(r.settle());
    }

    // Renaming a parent carries its sub-folders (Gmail does this server-side),
    // and the messages filed in them keep showing there.
    void renameAndDeleteNestedFolders()
    {
        Rig r;
        QTRY_VERIFY_WITH_TIMEOUT(r.item(QStringLiteral("gmail:Finance")), 15000);
        QVERIFY(r.settle());
        r.session->sync()->createLabel(QStringLiteral("Finance/Bills"));
        QTRY_VERIFY_WITH_TIMEOUT(r.item(QStringLiteral("gmail:Finance/Bills")), 10000);
        QVERIFY(r.settle());
        const quint64 id = r.msgid("Q4 field schedule");
        const QString hex = QString::number(id, 16);
        QVERIFY(r.drag(hex, QStringLiteral("gmail:Finance/Bills")));
        QTRY_VERIFY_WITH_TIMEOUT(r.imap.find(id)->labels.contains(QStringLiteral("Finance/Bills")), 10000);
        QVERIFY(r.settle());

        r.session->sync()->renameLabel(QStringLiteral("Finance"), QStringLiteral("Money"));
        QTRY_VERIFY_WITH_TIMEOUT(r.item(QStringLiteral("gmail:Money/Bills")), 10000);
        QVERIFY(!r.item(QStringLiteral("gmail:Finance/Bills")));
        QVERIFY(r.imap.find(id)->labels.contains(QStringLiteral("Money/Bills")));
        QTRY_VERIFY_WITH_TIMEOUT(r.session->cache()->summary(hex).labels.contains(QStringLiteral("Money/Bills")), 10000);
        QVERIFY(!r.session->cache()->summary(hex).labels.contains(QStringLiteral("Finance/Bills")));
        QVERIFY(r.settle());

        r.session->sync()->deleteLabel(QStringLiteral("Money/Bills"));
        QTRY_VERIFY_WITH_TIMEOUT(!r.item(QStringLiteral("gmail:Money/Bills")), 10000);
        QVERIFY(r.item(QStringLiteral("gmail:Money")));
        QVERIFY(!r.imap.find(id)->labels.contains(QStringLiteral("Money/Bills")));
        QVERIFY(r.settle());
    }
};

QTEST_MAIN(TstImapFolders)
#include "tst_imapfolders.moc"
