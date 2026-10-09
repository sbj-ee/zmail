// Delete outside the Inbox (0.4.2): in a Gmail label view (and Sent, Snoozed,
// Junk, Search results) Delete must trash the message exactly like the Inbox
// does: users.messages.trash goes out, the cache gets TRASH, the row leaves
// the list and the selection moves on; Undo brings it back. If Gmail refuses,
// the row and the selection go back where they were and the error is shown.
// All mail here is fake (MockGoogle, example.com addresses).
#include "MainWindow.hpp"
#include "core/ContactStore.h"
#include "ui/StationeryDialog.h"
#include "core/Stationery.h"
#include "core/MailCache.h"
#include "core/MailSession.h"
#include "core/SyncEngine.h"
#include "core/TokenStore.h"
#include "mock/MockGoogle.h"
#include "ui/ComposeWindow.h"
#include "ui/MailboxWindow.h"
#include "ui/MessageWindow.h"
#include "ui/ListDialog.h"
#include "ui/MessageListModel.h"
#include "ui/MessageView.h"
#include "ui/SafeHtmlView.h"
#include "ui/Theme.h"

#include <QAbstractItemView>
#include <QAction>
#include <QCompleter>
#include <QClipboard>
#include <QPrinter>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QTextFrame>
#include <QFile>
#include <QFileInfo>
#include <QTextCursor>
#include <QTextEdit>
#include <QDialog>
#include <QDrag>
#include <QDragEnterEvent>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStatusBar>
#include <QToolButton>
#include <QTreeView>
#include <QRadioButton>
#include <QDateTimeEdit>
#include <QTimer>
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
    o.contactsPathOverride = QStringLiteral(":memory:");
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

    // A folder with more mail than it has loaded: once the list is too
    // short to scroll (here, after a Delete), the next page comes by itself.
    void shortListLoadsTheNextPage()
    {
        Fixture f;
        QVERIFY(f.open(QStringLiteral("gmail:") + kLabel, {kLabel}));
        // Five older messages the app hasn't listed yet, as if the folder's
        // first page had ended after Charlie.
        QStringList older;
        for (int i = 0; i < 5; ++i) {
            MockGoogle::Message m;
            m.from = QStringLiteral("Priya Raman <priya.raman@example.com>");
            m.subject = QStringLiteral("Older %1").arg(i);
            m.text = QStringLiteral("Fake test mail.");
            m.labels = {kLabel};
            m.date = QDateTime::currentDateTimeUtc().addDays(-1 - i);
            older << f.g.addMessage(m, false);
        }
        f.session->cache()->setMeta(QStringLiteral("pageToken:") + kLabel, QStringLiteral("3"));
        QCOMPARE(f.proxy->rowCount(), 3);

        QTest::keyClick(f.list, Qt::Key_Delete);
        QTRY_COMPARE_WITH_TIMEOUT(f.proxy->rowCount(), 7, 10000); // Alpha, Charlie and the five older ones
        const QStringList shown = f.visibleIds();
        for (const QString &id : std::as_const(older)) {
            QVERIFY(shown.contains(id));
        }
        QVERIFY(!f.session->sync()->hasMore(kLabel));

        // A page token Gmail refuses: the listing starts over instead of sticking.
        MockGoogle::Message m;
        m.from = QStringLiteral("Priya Raman <priya.raman@example.com>");
        m.subject = QStringLiteral("Oldest");
        m.text = QStringLiteral("Fake test mail.");
        m.labels = {kLabel};
        m.date = QDateTime::currentDateTimeUtc().addDays(-30);
        const QString oldest = f.g.addMessage(m, false);
        f.g.addFault({QStringLiteral("/gmail/v1/users/me/messages"), 400, 1, -1});
        f.session->cache()->setMeta(QStringLiteral("pageToken:") + kLabel, QStringLiteral("expired"));
        f.session->sync()->fetchMore(kLabel);
        QTRY_VERIFY_WITH_TIMEOUT(f.visibleIds().contains(oldest), 10000);
    }

    // A sync that lands while the mouse button is down (clicking unread mail
    // causes one: it is marked read) must not cost the list its press: the
    // drag still starts, and the pointer doesn't sweep up a selection.
    void dragStartsEvenIfTheListReloadsMidPress()
    {
        Fixture f;
        QVERIFY(f.open(QStringLiteral("In"), {QStringLiteral("INBOX")}));
        const auto rows = [&]() {
            QList<int> out;
            for (const QModelIndex &i : f.list->selectionModel()->selectedRows()) {
                out << i.row();
            }
            std::sort(out.begin(), out.end());
            return out;
        };
        const auto at = [&](int r) { return f.list->visualRect(f.proxy->index(r, MessageListModel::Subject)).center(); };
        QTest::mousePress(f.list->viewport(), Qt::LeftButton, Qt::NoModifier, at(0));
        QMetaObject::invokeMethod(f.session->sync(), "messagesChanged");
        QTest::qWait(400); // the reload has run
        QTest::mouseMove(f.list->viewport(), at(0) + QPoint(0, 4));
        QTest::mouseMove(f.list->viewport(), at(2));
        QCOMPARE(f.list->findChildren<QDrag *>().size(), 1);
        QCOMPARE(rows(), QList<int>{0});
        QTest::mouseRelease(f.list->viewport(), Qt::LeftButton, Qt::NoModifier, at(2));
    }

    // Shift+click and Shift+arrows extend from the current message, upwards
    // or downwards, also when it was made current by the program (after a
    // Delete, a reload) rather than by a click.
    void rangeSelectionRunsEitherWayFromTheCurrentMessage()
    {
        Fixture f;
        QVERIFY(f.open(QStringLiteral("In"), {QStringLiteral("INBOX")}));
        const auto rows = [&]() {
            QList<int> out;
            for (const QModelIndex &i : f.list->selectionModel()->selectedRows()) {
                out << i.row();
            }
            std::sort(out.begin(), out.end());
            return out;
        };
        const auto at = [&](int r) { return f.list->visualRect(f.proxy->index(r, MessageListModel::Subject)).center(); };
        QCOMPARE(f.list->currentIndex().row(), 1); // Bravo
        QTest::mouseClick(f.list->viewport(), Qt::LeftButton, Qt::ShiftModifier, at(0));
        QCOMPARE(rows(), (QList<int>{0, 1}));
        f.list->setCurrentIndex(f.proxy->index(1, 0));
        QTest::mouseClick(f.list->viewport(), Qt::LeftButton, Qt::ShiftModifier, at(2));
        QCOMPARE(rows(), (QList<int>{1, 2}));
        f.list->setCurrentIndex(f.proxy->index(1, 0));
        QTest::keyClick(f.list, Qt::Key_Up, Qt::ShiftModifier);
        QCOMPARE(rows(), (QList<int>{0, 1}));
        f.list->setCurrentIndex(f.proxy->index(1, 0));
        QTest::keyClick(f.list, Qt::Key_Down, Qt::ShiftModifier);
        QCOMPARE(rows(), (QList<int>{1, 2}));
        // After a reload with the last row current: up from there.
        f.list->setCurrentIndex(f.proxy->index(2, 0));
        QMetaObject::invokeMethod(f.session->sync(), "messagesChanged");
        QTest::qWait(400);
        QTest::keyClick(f.list, Qt::Key_Up, Qt::ShiftModifier);
        QCOMPARE(rows(), (QList<int>{1, 2}));
        QTest::keyClick(f.list, Qt::Key_Up, Qt::ShiftModifier);
        QCOMPARE(rows(), (QList<int>{0, 1, 2}));
    }

    // Right-click a message: its sender can be added to Contacts, unless
    // that address is a contact already.
    void contextMenuAddsTheSenderToContacts()
    {
        Fixture f;
        QVERIFY(f.open(QStringLiteral("In"), {QStringLiteral("INBOX")}));
        const QString address = QStringLiteral("priya.raman@example.com");
        QVERIFY(f.session->contacts() && f.session->contacts()->isOpen());
        QVERIFY(!f.session->contacts()->hasEmail(address));
        const auto openMenu = [&]() -> QMenu * {
            emit f.list->customContextMenuRequested(f.list->visualRect(f.list->currentIndex()).center());
            QMenu *found = nullptr;
            (void)QTest::qWaitFor([&] {
                for (QWidget *w : QApplication::topLevelWidgets()) {
                    if (auto *m = qobject_cast<QMenu *>(w); m && m->isVisible() && m->objectName() == QLatin1String("messageListMenu")) {
                        found = m;
                    }
                }
                return found != nullptr;
            }, 3000);
            return found;
        };
        const auto addAction = [](QMenu *menu) -> QAction * {
            for (QAction *a : menu->actions()) {
                if (a->objectName() == QLatin1String("actionAddToContacts")) {
                    return a;
                }
            }
            return nullptr;
        };
        QMenu *menu = openMenu();
        QVERIFY(menu);
        QAction *add = addAction(menu);
        QVERIFY(add);
        QVERIFY(add->text().contains(address));
        add->trigger();
        menu->close();
        QVERIFY(f.session->contacts()->hasEmail(address));
        const QList<Contact> saved = f.session->contacts()->contacts(address, 10);
        QCOMPARE(saved.size(), 1);
        QCOMPARE(saved.first().displayName, QStringLiteral("Priya Raman"));
        QCOMPARE(saved.first().source, QStringLiteral("local"));

        // Already a contact (hidden ones count too): not offered again.
        f.session->contacts()->setHidden({saved.first().id}, true);
        menu = openMenu();
        QVERIFY(menu);
        QVERIFY(!addAction(menu));
        menu->close();
        QCOMPARE(f.session->contacts()->contacts(address, 10).size(), 1);
    }

    // Writing to a nickname: suggested while typing, written out when the
    // field is left, and sent to the addresses even if it never was.
    void composeExpandsNicknames()
    {
        Fixture f;
        QVERIFY(f.open(QStringLiteral("In"), {QStringLiteral("INBOX")}));
        ContactStore *contacts = f.session->contacts();
        const QString ada = contacts->addLocalContact(QStringLiteral("Ada Lovelace"), QStringLiteral("ada@example.org"));
        const QString cy = contacts->addLocalContact(QStringLiteral("Cy Vance"), QStringLiteral("cy@example.com"));
        contacts->setNickname(ada, QStringLiteral("ada"));
        contacts->addToCategory({ada, cy}, QStringLiteral("Crew"));

        ComposeWindow c;
        c.setSession(f.session.get());
        c.show();
        c.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&c));
        auto *to = c.findChild<QLineEdit *>(QStringLiteral("fieldTo"));
        auto *cc = c.findChild<QLineEdit *>(QStringLiteral("fieldCc"));
        to->setFocus();
        QTest::keyClicks(to, QStringLiteral("ad"));
        QVERIFY(to->completer());
        QCOMPARE(to->completer()->model()->index(0, 0).data().toString(), QStringLiteral("ada")); // the nickname, first
        QTest::keyClicks(to, QStringLiteral("a"));
        // Not left yet: what will be sent is already the address.
        QCOMPARE(c.message().to, QStringLiteral("Ada Lovelace <ada@example.org>"));
        // Leaving the field writes it out.
        to->completer()->popup()->hide();
        cc->setFocus();
        QTRY_COMPARE(to->text(), QStringLiteral("Ada Lovelace <ada@example.org>"));
        QTest::keyClicks(cc, QStringLiteral("crew, someone@example.net"));
        cc->completer()->popup()->hide();
        to->setFocus();
        QTRY_COMPARE(cc->text(), QStringLiteral("Ada Lovelace <ada@example.org>, Cy Vance <cy@example.com>, someone@example.net"));
        // A word that is no nickname is left as typed.
        to->setText(QStringLiteral("nobody"));
        QCOMPARE(c.message().to, QStringLiteral("nobody"));
    }

    // File > Save Attachments, File > Print, Edit > Copy, View as Plain Text.
    void saveAttachmentsPrintCopyAndPlainText()
    {
        Fixture f;
        QVERIFY(f.open(QStringLiteral("In"), {QStringLiteral("INBOX")}));
        MockGoogle::Message m;
        m.from = QStringLiteral("Priya Raman <priya.raman@example.com>");
        m.to = QStringLiteral("Demo User <demo.user@example.com>");
        m.subject = QStringLiteral("Files for you");
        m.text = QStringLiteral("Plain part.");
        m.html = QStringLiteral("<table><tr><td><b>Rich</b> part</td><td>second cell</td></tr></table>");
        m.labels = {QStringLiteral("INBOX")};
        m.date = QDateTime::currentDateTimeUtc();
        m.attachments = {QStringLiteral("report.pdf"), QStringLiteral("../../etc/evil.txt"), QStringLiteral(".hidden")};
        m.attachmentData = {QByteArray("PDF BYTES"), QByteArray("not so evil"), QByteArray("dot")};
        const QString id = f.g.addMessage(m, true);
        f.session->sync()->pollNow(true);
        QTRY_COMPARE_WITH_TIMEOUT(f.proxy->rowCount(), 4, 10000);
        f.list->setCurrentIndex(f.proxy->mapFromSource(f.model->index(f.model->rowForId(id), 0)));
        QTRY_COMPARE_WITH_TIMEOUT(f.w->shownMessageId(), id, 5000);
        auto *view = f.w->findChild<MessageView *>(QStringLiteral("messageView"));
        QTRY_VERIFY_WITH_TIMEOUT(view->body()->toPlainText().contains(QStringLiteral("Rich")), 5000);

        // Save Attachments: every one, under safe names, never over a file
        // that is there already.
        QTemporaryDir dir;
        f.w->saveAttachments(dir.path());
        QTRY_COMPARE_WITH_TIMEOUT(f.w->savedAttachments().size(), 3, 10000);
        const auto read = [&dir](const QString &name) {
            QFile file(dir.filePath(name));
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("<missing>");
        };
        QCOMPARE(read(QStringLiteral("report.pdf")), QByteArray("PDF BYTES"));
        QCOMPARE(read(QStringLiteral("evil.txt")), QByteArray("not so evil")); // inside the folder, whatever the sender called it
        QCOMPARE(read(QStringLiteral("hidden")), QByteArray("dot"));
        for (const QString &path : f.w->savedAttachments()) {
            QCOMPARE(QFileInfo(path).absolutePath(), QDir(dir.path()).absolutePath());
        }
        f.w->saveAttachments(dir.path());
        QTRY_COMPARE_WITH_TIMEOUT(f.w->savedAttachments().size(), 3, 10000);
        QCOMPARE(read(QStringLiteral("report (2).pdf")), QByteArray("PDF BYTES"));
        QCOMPARE(read(QStringLiteral("report.pdf")), QByteArray("PDF BYTES"));

        // Print: header and body, here to a PDF.
        QPrinter pdf;
        pdf.setOutputFormat(QPrinter::PdfFormat);
        pdf.setOutputFileName(dir.filePath(QStringLiteral("out.pdf")));
        QVERIFY(f.w->printMessage(&pdf));
        QVERIFY(QFileInfo(dir.filePath(QStringLiteral("out.pdf"))).size() > 500);
        const std::unique_ptr<QTextDocument> printable(view->printableDocument());
        const QString page = printable->toPlainText();
        QVERIFY(page.contains(QStringLiteral("Files for you")));
        QVERIFY(page.contains(QStringLiteral("priya.raman@example.com")));
        QVERIFY(page.contains(QStringLiteral("report.pdf")));
        QVERIFY(page.indexOf(QStringLiteral("Files for you")) < page.indexOf(QStringLiteral("Rich")));

        // View as Plain Text: the text part instead of the HTML, remembered.
        QAction *plain = f.w->findChild<QAction *>(QStringLiteral("actionPlainText"));
        QVERIFY(plain && plain->isCheckable() && !plain->isChecked());
        QCOMPARE(view->body()->document()->rootFrame()->childFrames().size(), 1); // the HTML's table
        plain->setChecked(true);
        QVERIFY(view->plainText());
        QVERIFY(view->body()->toPlainText().contains(QStringLiteral("Plain part.")));
        QVERIFY(!view->body()->toPlainText().contains(QStringLiteral("Rich")));
        QCOMPARE(view->body()->document()->rootFrame()->childFrames().size(), 0);
        QVERIFY(QSettings().value(QStringLiteral("viewer/plainText")).toBool());
        plain->setChecked(false);
        QTRY_VERIFY(view->body()->toPlainText().contains(QStringLiteral("Rich")));

        // Copy: the text selected in the message, or else the selected
        // messages a line each.
        view->body()->selectAll();
        f.w->findChild<QAction *>(QStringLiteral("actionCopy"))->trigger();
        QVERIFY(QGuiApplication::clipboard()->text().contains(QStringLiteral("second cell")));
        QTextCursor none = view->body()->textCursor();
        none.clearSelection();
        view->body()->setTextCursor(none);
        f.list->setFocus();
        f.w->copySelection();
        const QString line = QGuiApplication::clipboard()->text();
        QVERIFY2(line.contains(QStringLiteral("Files for you")) && line.contains(QStringLiteral("Priya Raman")), qPrintable(line));
        QSettings().remove(QStringLiteral("viewer/plainText"));
    }

    // The queue, as in Eudora: Send Later puts a message in Out, Send
    // Queued Messages delivers what is waiting, and nothing goes sooner.
    void sendLaterQueuesAndSendQueuedDelivers()
    {
        Fixture f;
        QVERIFY(f.open(QStringLiteral("In"), {QStringLiteral("INBOX")}));
        const QString sendPath = QStringLiteral("POST /gmail/v1/users/me/messages/send");
        const auto write = [&](const QString &to, const QString &subject) {
            ComposeWindow *c = f.w->openCompose();
            c->findChild<QLineEdit *>(QStringLiteral("fieldTo"))->setText(to);
            c->findChild<QLineEdit *>(QStringLiteral("fieldSubject"))->setText(subject);
            c->findChild<QTextEdit *>(QStringLiteral("composeBody"))->setPlainText(QStringLiteral("Body of ") + subject);
            return c;
        };
        QVERIFY(f.w->findChild<QAction *>(QStringLiteral("actionSendQueued")));
        QCOMPARE(f.w->queuedCount(), 0);

        // An address that isn't one is refused, exactly as Send refuses it.
        QPointer<ComposeWindow> bad = write(QStringLiteral("not an address"), QStringLiteral("Nope"));
        bad->queue();
        QVERIFY(bad && bad->isVisible());
        QCOMPARE(f.w->queuedCount(), 0);
        delete bad.data();

        QPointer<ComposeWindow> first = write(QStringLiteral("Dana Whitfield <dana@example.org>"), QStringLiteral("First queued"));
        first->queue();
        QTRY_VERIFY(!first); // closed without asking about a draft
        QPointer<ComposeWindow> second = write(QStringLiteral("eli@example.com"), QStringLiteral("Second queued"));
        second->queue();
        QTRY_VERIFY(!second);
        QCOMPARE(f.w->queuedCount(), 2);
        QCOMPARE(f.g.count(sendPath), 0); // nothing has gone anywhere

        // They wait in Out, marked Q, and can be read there.
        f.selectView(QStringLiteral("Out"));
        QTRY_COMPARE(f.proxy->rowCount(), 2);
        f.list->sortByColumn(MessageListModel::Subject, Qt::AscendingOrder);
        QCOMPARE(f.proxy->index(0, MessageListModel::Status).data().toString(), QStringLiteral("Q"));
        QCOMPARE(f.proxy->index(0, MessageListModel::Who).data().toString(), QStringLiteral("Dana Whitfield"));
        f.list->setCurrentIndex(f.proxy->index(0, 0));
        auto *view = f.w->findChild<MessageView *>(QStringLiteral("messageView"));
        QTRY_VERIFY(view->body()->toPlainText().contains(QStringLiteral("Body of First queued")));
        QVERIFY(view->headerText().contains(QStringLiteral("dana@example.org")));
        // The queue is kept in the cache's own table: a full resync leaves it.
        f.session->cache()->clearMessages();
        QCOMPARE(f.w->queuedCount(), 2);

        // Gmail refuses the first: it stays, with the reason; the second goes.
        f.g.addFault({QStringLiteral("/gmail/v1/users/me/messages/send"), 400, 1, -1});
        f.w->sendQueued();
        QTRY_COMPARE_WITH_TIMEOUT(f.w->queuedCount(), 1, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(f.g.count(sendPath), 2, 10000);
        const QList<MailCache::QueuedMessage> left = f.session->cache()->queued();
        QCOMPARE(left.first().subject, QStringLiteral("First queued"));
        QVERIFY(!left.first().error.isEmpty());
        bool delivered = false;
        for (const MockGoogle::Message &m : f.g.messages()) {
            delivered = delivered || m.subject == QLatin1String("Second queued");
        }
        QVERIFY(delivered);

        // Again: this time it goes, and the queue is empty.
        f.w->findChild<QAction *>(QStringLiteral("actionSendQueued"))->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(f.w->queuedCount(), 0, 10000);
        QCOMPARE(f.g.count(sendPath), 3);

        // Delete on a queued message takes it out of the queue, unsent.
        QPointer<ComposeWindow> third = write(QStringLiteral("eli@example.com"), QStringLiteral("Third queued"));
        third->queue();
        QTRY_VERIFY(!third);
        int row = -1;
        QTRY_VERIFY_WITH_TIMEOUT((row = f.model->rowForId(QStringLiteral("queued:%1").arg(f.session->cache()->queued().first().id))) >= 0, 5000);
        f.list->setCurrentIndex(f.proxy->mapFromSource(f.model->index(row, 0)));
        const int trashCalls = f.g.trashCalls.size();
        QTRY_COMPARE(f.w->shownMessageId(), QStringLiteral("queued:%1").arg(f.session->cache()->queued().first().id));
        f.w->findChild<QAction *>(QStringLiteral("menuActionDelete"))->trigger(); // the compose windows took the focus
        QCOMPARE(f.w->queuedCount(), 0);
        QCOMPARE(f.g.trashCalls.size(), trashCalls); // it was never mail
        f.w->sendQueued();
        QTest::qWait(200);
        QCOMPARE(f.g.count(sendPath), 3);
    }

    // Eudora's Mailbox and Transfer menus: the sidebar's mailboxes, to go
    // to or to move the selected messages into, and its shortcut keys.
    void mailboxAndTransferMenus()
    {
        Fixture f;
        QVERIFY(f.open(QStringLiteral("In"), {QStringLiteral("INBOX")}));
        auto *mailbox = f.w->findChild<QMenu *>(QStringLiteral("menuMailbox"));
        auto *transfer = f.w->findChild<QMenu *>(QStringLiteral("menuTransfer"));
        QVERIFY(mailbox && transfer);
        const auto named = [&](const char *name) { return f.w->findChild<QAction *>(QString::fromLatin1(name)); };
        QTRY_VERIFY_WITH_TIMEOUT(named("transfer_Label_7"), 10000); // once the account's folders are in
        QVERIFY(named("mailbox_In") && named("mailbox_Out") && named("mailbox_Trash"));
        QVERIFY(named("mailbox_gmail:Label_7"));
        QCOMPARE(named("mailbox_In")->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_1));
        QCOMPARE(named("mailbox_gmail:Label_7")->text(), QStringLiteral("Projects"));
        // Transfer offers only places mail can go: In, folders, Trash.
        QVERIFY(named("transfer_INBOX") && named("transfer_TRASH"));
        QVERIFY(!f.w->findChild<QAction *>(QStringLiteral("transfer_")));
        for (QAction *a : transfer->findChildren<QAction *>()) {
            QVERIFY2(!a->objectName().contains(QStringLiteral("Out")) && !a->objectName().contains(QStringLiteral("STARRED")),
                     qPrintable(a->objectName()));
        }

        // Transfer the current message (Bravo) to the folder: it leaves the
        // Inbox list and the selection moves on.
        named("transfer_Label_7")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(f.g.messages().value(f.b).labels.contains(kLabel), 10000);
        QVERIFY(!f.g.messages().value(f.b).labels.contains(QStringLiteral("INBOX")));
        QTRY_COMPARE_WITH_TIMEOUT(f.proxy->rowCount(), 2, 10000);
        QVERIFY(f.list->currentIndex().isValid());
        QVERIFY(f.currentId() != f.b);

        // Mailbox goes there; Transfer > In brings it back.
        named("mailbox_gmail:Label_7")->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(f.proxy->rowCount(), 1, 10000);
        QCOMPARE(f.visibleIds(), QStringList{f.b});
        f.list->setCurrentIndex(f.proxy->index(0, 0));
        named("transfer_INBOX")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(f.g.messages().value(f.b).labels.contains(QStringLiteral("INBOX")), 10000);
        QVERIFY(!f.g.messages().value(f.b).labels.contains(kLabel));
        named("mailbox_In")->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(f.proxy->rowCount(), 3, 10000);

        // Transfer > Trash is Delete. The same menu is in the right-click menu.
        f.list->setCurrentIndex(f.proxy->mapFromSource(f.model->index(f.model->rowForId(f.a), 0)));
        QTRY_COMPARE(f.w->shownMessageId(), f.a);
        named("transfer_TRASH")->trigger();
        QTRY_COMPARE(f.g.trashCalls, QStringList{f.a});

        // Eudora's keys.
        QVERIFY(named("menuActionDelete")->shortcuts().contains(QKeySequence(Qt::CTRL | Qt::Key_D)));
        QCOMPARE(named("actionSendQueued")->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_T));
        QCOMPARE(named("actionFilterMessages")->shortcut(), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_J));
        QCOMPARE(named("actionContacts")->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_L));
        QCOMPARE(named("actionAddSenderToContacts")->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_K));
        ComposeWindow c;
        QVERIFY(c.findChild<QAction *>(QStringLiteral("actionSend"))->shortcuts().contains(QKeySequence(Qt::CTRL | Qt::Key_E)));
        QCOMPARE(c.findChild<QAction *>(QStringLiteral("actionComposeAttach"))->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_H));
    }

    // File > New Message With and Message > Reply With start from stationery.
    void newMessageWithAndReplyWithStationery()
    {
        QFile::remove(StationeryStore::defaultPath());
        QVERIFY(StationeryStore().save({QStringLiteral("Thanks"), {}, {}, QStringLiteral("Thank you"), QStringLiteral("Thanks very much!")}));
        Fixture f;
        QVERIFY(f.open(QStringLiteral("In"), {QStringLiteral("INBOX")}));
        // The menus list what is saved now.
        for (const char *name : {"menuNewMessageWith", "menuReplyWith"}) {
            auto *menu = f.w->findChild<QMenu *>(QString::fromLatin1(name));
            QVERIFY2(menu, name);
            emit menu->aboutToShow();
            QCOMPARE(menu->actions().first()->text(), QStringLiteral("Thanks"));
        }
        QVERIFY(f.w->findChild<QAction *>(QStringLiteral("actionStationery")));

        ComposeWindow *fresh = f.w->newMessageWith(QStringLiteral("Thanks"));
        QVERIFY(fresh);
        QCOMPARE(fresh->findChild<QLineEdit *>(QStringLiteral("fieldSubject"))->text(), QStringLiteral("Thank you"));
        QVERIFY(fresh->findChild<QTextEdit *>(QStringLiteral("composeBody"))->toPlainText().startsWith(QStringLiteral("Thanks very much!")));
        delete fresh;

        // A reply: to the sender, about their subject, the stationery above
        // their quoted message, and only once.
        ComposeWindow *reply = f.w->replyWith(QStringLiteral("Thanks"));
        QVERIFY(reply);
        auto *body = reply->findChild<QTextEdit *>(QStringLiteral("composeBody"));
        QTRY_VERIFY_WITH_TIMEOUT(body->toPlainText().contains(QStringLiteral("Fake test mail.")), 10000); // the quote arrived
        QCOMPARE(reply->findChild<QLineEdit *>(QStringLiteral("fieldTo"))->text(), QStringLiteral("Priya Raman <priya.raman@example.com>"));
        QCOMPARE(reply->findChild<QLineEdit *>(QStringLiteral("fieldSubject"))->text(), QStringLiteral("Re: Bravo zebra"));
        const QString text = body->toPlainText();
        QVERIFY(text.startsWith(QStringLiteral("Thanks very much!")));
        QCOMPARE(text.count(QStringLiteral("Thanks very much!")), 1);
        QVERIFY(text.indexOf(QStringLiteral("Thanks very much!")) < text.indexOf(QStringLiteral("Fake test mail.")));
        // Saved from here, it keeps what was written, not what was quoted.
        QCOMPARE(reply->asStationery(QStringLiteral("x")).body, QStringLiteral("Thanks very much!"));
        delete reply;

        // Edited in Settings > Stationery.
        StationeryDialog *dlg = f.w->showStationeryDialog();
        QCOMPARE(dlg->items().size(), 1);
        dlg->add({QStringLiteral("Away"), {}, {}, {}, QStringLiteral("Back Monday.")});
        dlg->accept();
        QCOMPARE(StationeryStore().all().size(), 2);
        QFile::remove(StationeryStore::defaultPath());
    }

    // A mailbox in a window of its own, as in Eudora: the same live list.
    void mailboxOpensInItsOwnWindow()
    {
        Fixture f;
        QVERIFY(f.open(QStringLiteral("In"), {QStringLiteral("INBOX"), kLabel}));
        const QString folder = QLatin1String("gmail:") + kLabel;
        QTRY_VERIFY_WITH_TIMEOUT(f.w->findChild<QAction *>(QStringLiteral("mailbox_") + folder), 10000);
        QVERIFY(f.w->findChild<QAction *>(QStringLiteral("actionMailboxWindow")));

        MailboxWindow *mw = f.w->openMailboxWindow(folder);
        QVERIFY(mw);
        QVERIFY(mw->isWindow() && mw->isVisible());
        QVERIFY2(mw->windowTitle().startsWith(QStringLiteral("Projects")), qPrintable(mw->windowTitle()));
        QTRY_COMPARE_WITH_TIMEOUT(mw->proxy()->rowCount(), 3, 10000);
        QCOMPARE(mw->findChild<QLabel *>(QStringLiteral("mailboxWindowCount"))->text().trimmed(), QStringLiteral("3 messages, 0 unread"));
        // Asked for again: the same window, not a second one.
        QCOMPARE(f.w->openMailboxWindow(folder), mw);
        QCOMPARE(f.w->mailboxWindows().size(), 1);
        // The main window is untouched, and can show another mailbox meanwhile.
        QCOMPARE(f.proxy->rowCount(), 3); // still In
        f.selectView(QStringLiteral("Trash"));
        QTRY_COMPARE(f.proxy->rowCount(), 0);
        QCOMPARE(mw->proxy()->rowCount(), 3);

        // Open a message from it (it isn't in the main list's mailbox now).
        mw->list()->sortByColumn(MessageListModel::Subject, Qt::AscendingOrder);
        mw->list()->setCurrentIndex(mw->proxy()->index(0, 0));
        QCOMPARE(mw->selectedIds(), QStringList{f.a});
        emit mw->list()->doubleClicked(mw->proxy()->index(0, 0));
        QTRY_COMPARE(f.w->messageWindows().size(), 1);
        QCOMPARE(f.w->messageWindows().first()->messageId(), f.a);
        f.w->messageWindows().first()->close();

        // Its preview pane shows the message selected in it.
        QTRY_VERIFY(mw->preview()->headerText().contains(QStringLiteral("Alpha zebra")));
        QTRY_VERIFY(mw->preview()->body()->toPlainText().contains(QStringLiteral("Fake test mail.")));

        // Right-click: the full set of commands, for this window's selection
        // (not for whatever is selected in the main window).
        const auto popMenu = [&]() -> QMenu * {
            emit mw->menuRequested(mw->selectedIds(), QPoint(10, 10));
            QMenu *menu = nullptr;
            (void)QTest::qWaitFor([&] {
                for (QWidget *w : QApplication::topLevelWidgets()) {
                    if (auto *m = qobject_cast<QMenu *>(w); m && m->isVisible() && m->objectName() == QLatin1String("mailboxWindowMenu")) {
                        menu = m;
                    }
                }
                return menu != nullptr;
            }, 3000);
            return menu;
        };
        QMenu *menu = popMenu();
        QVERIFY(menu);
        for (const char *name : {"mwOpen", "mwReply", "mwReplyAll", "mwForward", "mwMarkRead", "mwMarkUnread", "mwDelete"}) {
            QVERIFY2(menu->findChild<QAction *>(QString::fromLatin1(name)), name);
        }
        QAction *red = menu->findChild<QAction *>(QStringLiteral("actionFlag_red"));
        QVERIFY(red);
        red->trigger();
        menu->close();
        QTRY_VERIFY_WITH_TIMEOUT(f.g.messages().value(f.a).labels.contains(QStringLiteral("STARRED")), 10000);
        QVERIFY(!f.g.messages().value(f.b).labels.contains(QStringLiteral("STARRED")));
        QTRY_COMPARE(f.model->item(f.model->rowForId(f.a)).flag, QStringLiteral("red"));
        menu = popMenu();
        QVERIFY(menu);
        menu->findChild<QAction *>(QStringLiteral("mwMarkUnread"))->trigger();
        menu->close();
        QTRY_VERIFY_WITH_TIMEOUT(f.g.messages().value(f.a).labels.contains(QStringLiteral("UNREAD")), 10000);
        // Once the menu is gone, the main window's commands are its own again:
        // flagging there (nothing is selected in its Trash) touches nothing here.
        QTest::qWait(50);
        f.w->setFlagOnSelected(QStringLiteral("blue"));
        QTest::qWait(100);
        QCOMPARE(f.model->item(f.model->rowForId(f.a)).flag, QStringLiteral("red"));

        // Delete from it: Gmail trashes the message, and it leaves the
        // window's list and turns up in the main window's Trash.
        emit mw->deleteRequested(mw->selectedIds());
        QTRY_COMPARE_WITH_TIMEOUT(f.g.trashCalls, QStringList{f.a}, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(mw->proxy()->rowCount(), 2, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(f.proxy->rowCount(), 1, 10000);

        // The list's look follows Settings > Message List.
        f.w->setListAppearance(0, 0);
        const int compact = mw->list()->visualRect(mw->proxy()->index(0, 0)).height();
        f.w->setListAppearance(0, 14);
        QCOMPARE(mw->list()->visualRect(mw->proxy()->index(0, 0)).height(), compact + 14);
        f.w->setListAppearance(0, kListSpacingDefault);

        // The current mailbox with no key; search results have no window.
        MailboxWindow *trash = f.w->openMailboxWindow();
        QVERIFY(trash && trash != mw);
        QCOMPARE(trash->mailbox(), QStringLiteral("Trash"));
        QCOMPARE(trash->proxy()->rowCount(), 1);
        QVERIFY(!f.w->openMailboxWindow(QStringLiteral("Search")));
        QCOMPARE(f.w->mailboxWindows().size(), 2);
        trash->close();
        QTRY_COMPARE(f.w->mailboxWindows().size(), 1);
    }

    // Everyday polish (0.6.7): right-clicking the sidebar keeps your place,
    // folders are listed directly in the menus, empty views say so, Select
    // All, a shortcut list, and flags can be searched for.
    void everydayPolish()
    {
        Fixture f;
        QVERIFY(f.open(QStringLiteral("In"), {QStringLiteral("INBOX")}));
        auto *tree = f.w->findChild<QTreeWidget *>(QStringLiteral("mailboxTree"));
        const auto item = [tree](const QString &key) -> QTreeWidgetItem * {
            for (QTreeWidgetItemIterator it(tree); *it; ++it) {
                if ((*it)->data(0, Qt::UserRole).toString() == key) {
                    return *it;
                }
            }
            return nullptr;
        };
        const auto menuFor = [&](QTreeWidgetItem *row) {
            emit tree->customContextMenuRequested(tree->visualItemRect(row).center());
            QMenu *menu = nullptr;
            (void)QTest::qWaitFor([&] {
                for (QWidget *w : QApplication::topLevelWidgets()) {
                    if (auto *m = qobject_cast<QMenu *>(w); m && m->isVisible() && m->objectName() == QLatin1String("mailboxMenu")) {
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
        };
        const QString folder = QLatin1String("gmail:") + kLabel;
        QTRY_VERIFY(item(folder));

        // Right-click Trash from In: the menu is Trash's, and you are still in In.
        QStringList names = menuFor(item(QStringLiteral("Trash")));
        QVERIFY(names.contains(QStringLiteral("actionEmptyTrash")));
        QVERIFY(!names.contains(QStringLiteral("actionNewFolder"))); // Trash isn't where folders go
        QCOMPARE(tree->currentItem()->data(0, Qt::UserRole).toString(), QStringLiteral("In"));
        QCOMPARE(f.proxy->rowCount(), 3);
        QCOMPARE(f.currentId(), f.b);
        names = menuFor(item(folder));
        QVERIFY(names.contains(QStringLiteral("actionNewFolder")) && names.contains(QStringLiteral("actionEmptyFolder")));
        QCOMPARE(f.proxy->rowCount(), 3);

        // Folders sit directly in Mailbox and Transfer, under a "Folders" heading in the sidebar.
        auto *mailbox = f.w->findChild<QMenu *>(QStringLiteral("menuMailbox"));
        auto *transfer = f.w->findChild<QMenu *>(QStringLiteral("menuTransfer"));
        QVERIFY(mailbox->actions().contains(f.w->findChild<QAction *>(QStringLiteral("mailbox_") + folder)));
        QVERIFY(transfer->actions().contains(f.w->findChild<QAction *>(QStringLiteral("transfer_") + kLabel)));
        QCOMPARE(item(folder)->parent()->text(0), QStringLiteral("Folders"));
        QVERIFY(item(QStringLiteral("gmail:STARRED")));
        QCOMPARE(item(QStringLiteral("gmail:STARRED"))->text(0), QStringLiteral("Flagged"));

        // Select All: every message in the list.
        f.list->setFocus();
        f.w->findChild<QAction *>(QStringLiteral("actionSelectAll"))->trigger();
        QCOMPARE(f.list->selectionModel()->selectedRows().size(), 3);
        QCOMPARE(f.w->findChild<QAction *>(QStringLiteral("actionSelectAll"))->shortcut(), QKeySequence(QKeySequence::SelectAll));

        // An empty mailbox and an empty preview say what they are.
        auto *hint = f.w->findChild<QLabel *>(QStringLiteral("emptyListHint"));
        QVERIFY(hint && hint->isHidden());
        f.selectView(QStringLiteral("Trash"));
        QTRY_VERIFY(!hint->isHidden());
        QCOMPARE(hint->text(), QStringLiteral("Trash is empty."));
        auto *view = f.w->findChild<MessageView *>(QStringLiteral("messageView"));
        QTRY_COMPARE(view->body()->placeholderText(), QStringLiteral("Select a message to read it."));
        f.selectView(folder);
        QTRY_COMPARE(hint->text(), QStringLiteral("No messages in Projects."));
        f.selectView(QStringLiteral("In"));
        QTRY_VERIFY(hint->isHidden());
        f.list->setCurrentIndex(f.proxy->index(0, 0));
        QTRY_VERIFY(view->body()->placeholderText().isEmpty());

        // Help > Keyboard Shortcuts lists the keys the menus really have.
        QDialog *keys = f.w->showShortcuts();
        auto *list = keys->findChild<QTreeWidget *>(QStringLiteral("shortcutsList"));
        QHash<QString, QString> byCommand;
        for (int i = 0; i < list->topLevelItemCount(); ++i) {
            byCommand.insert(list->topLevelItem(i)->text(0).trimmed(), list->topLevelItem(i)->text(1));
        }
        QCOMPARE(byCommand.value(QStringLiteral("Filter Messages")), QStringLiteral("Ctrl+Alt+J"));
        QCOMPARE(byCommand.value(QStringLiteral("Mark as Junk")), QStringLiteral("Ctrl+J"));
        QCOMPARE(byCommand.value(QStringLiteral("Send Queued Messages")), QStringLiteral("Ctrl+T"));
        QVERIFY(byCommand.value(QStringLiteral("Delete")).contains(QStringLiteral("Ctrl+D")));
        QVERIFY(byCommand.contains(QStringLiteral("In")) && byCommand.contains(QStringLiteral("Send Later (at a time, or queued in Sent)")));
        QVERIFY(!byCommand.contains(QStringLiteral("Account"))); // not built: not listed
        keys->close();

        // Flags can be searched for, by any colour or one.
        f.w->setFlagOnSelected(QStringLiteral("red"));
        QTRY_COMPARE(f.model->item(f.model->rowForId(f.currentId())).flag, QStringLiteral("red"));
        MailItem red = f.model->item(f.model->rowForId(f.currentId()));
        MailItem plain = red;
        plain.flag.clear();
        const auto match = [](const char *query, const MailItem &m) {
            return MessageFilterProxy::matches(MessageFilterProxy::parseSearch(QString::fromLatin1(query)), m);
        };
        QVERIFY(match("is:flagged", red) && !match("is:flagged", plain));
        QVERIFY(match("flag:red", red) && match("flag:RED", red) && !match("flag:blue", red));
        QVERIFY(match("-flag:blue", red) && !match("-is:flagged", red) && match("-is:flagged", plain));
        QVERIFY(match("flag:any zebra", red) && !match("flag:red nosuchword", red));
        auto *search = f.w->findChild<QLineEdit *>(QStringLiteral("searchBox"));
        search->setText(QStringLiteral("is:flagged"));
        QTRY_COMPARE_WITH_TIMEOUT(f.proxy->rowCount(), 1, 5000);
        search->setText(QStringLiteral("flag:green"));
        QTRY_COMPARE_WITH_TIMEOUT(f.proxy->rowCount(), 0, 5000);
        QTRY_COMPARE(hint->text(), QStringLiteral("No messages match your search."));
        search->clear();
        QTRY_COMPARE_WITH_TIMEOUT(f.proxy->rowCount(), 3, 5000);
    }

    // Undo (the status-bar bar, Edit > Undo, Ctrl+Z) covers a move, Empty
    // Folder and Empty Trash as well as Delete.
    void undoCoversMovesAndEmptying()
    {
        Fixture f;
        QVERIFY(f.open(QStringLiteral("In"), {QStringLiteral("INBOX"), QStringLiteral("STARRED")}));
        QAction *undo = f.w->findChild<QAction *>(QStringLiteral("actionUndoDelete"));
        QVERIFY(undo && !undo->isEnabled());
        const auto labels = [&](const QString &id) { return f.g.messages().value(id).labels; };

        // A transfer: back to the Inbox, out of the folder, its star untouched.
        QTRY_VERIFY_WITH_TIMEOUT(f.w->findChild<QAction *>(QStringLiteral("transfer_Label_7")), 10000);
        f.w->findChild<QAction *>(QStringLiteral("transfer_Label_7"))->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(labels(f.b).contains(kLabel) && !labels(f.b).contains(QStringLiteral("INBOX")), 10000);
        QVERIFY(undo->isEnabled());
        QCOMPARE(undo->text().remove(QLatin1Char('&')), QStringLiteral("Undo Move"));
        QVERIFY(!f.undoBar()->isHidden());
        QCOMPARE(f.undoBar()->findChild<QLabel *>(QStringLiteral("undoDeleteLabel"))->text(), QStringLiteral("Moved to Projects."));
        undo->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(labels(f.b).contains(QStringLiteral("INBOX")) && !labels(f.b).contains(kLabel), 10000);
        QVERIFY(labels(f.b).contains(QStringLiteral("STARRED")));
        QVERIFY(!undo->isEnabled());
        QVERIFY(f.undoBar()->isHidden());
        QTRY_COMPARE_WITH_TIMEOUT(f.proxy->rowCount(), 3, 10000);

        // Delete still has its own Undo.
        f.list->setCurrentIndex(f.proxy->mapFromSource(f.model->index(f.model->rowForId(f.a), 0)));
        QTRY_COMPARE(f.w->shownMessageId(), f.a);
        f.w->findChild<QAction *>(QStringLiteral("menuActionDelete"))->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(labels(f.a).contains(QStringLiteral("TRASH")), 10000);
        QCOMPARE(undo->text().remove(QLatin1Char('&')), QStringLiteral("Undo Delete"));
        undo->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!labels(f.a).contains(QStringLiteral("TRASH")), 10000);

        // Empty Folder: everything in it to Trash, and back.
        f.session->sync()->moveToLabel(f.a, kLabel);
        f.session->sync()->moveToLabel(f.c, kLabel);
        QTRY_VERIFY_WITH_TIMEOUT(labels(f.a).contains(kLabel) && labels(f.c).contains(kLabel), 10000);
        f.session->sync()->emptyLabel(kLabel);
        QTRY_VERIFY_WITH_TIMEOUT(labels(f.a).contains(QStringLiteral("TRASH")) && labels(f.c).contains(QStringLiteral("TRASH")), 10000);
        QTRY_COMPARE(undo->text().remove(QLatin1Char('&')), QStringLiteral("Undo Empty Folder"));
        QVERIFY(!labels(f.a).contains(kLabel));
        undo->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(labels(f.a).contains(kLabel) && labels(f.c).contains(kLabel), 10000);
        QVERIFY(!labels(f.a).contains(QStringLiteral("TRASH")) && !labels(f.c).contains(QStringLiteral("TRASH")));
        QVERIFY(labels(f.b).contains(QStringLiteral("INBOX"))); // the one that wasn't in the folder

        // Empty Trash: hidden, and shown again.
        f.session->sync()->trash(f.b);
        QTRY_VERIFY_WITH_TIMEOUT(labels(f.b).contains(QStringLiteral("TRASH")), 10000);
        f.selectView(QStringLiteral("Trash"));
        QTRY_COMPARE_WITH_TIMEOUT(f.proxy->rowCount(), 1, 10000);
        f.w->emptyTrash(false);
        QTRY_COMPARE_WITH_TIMEOUT(f.proxy->rowCount(), 0, 10000);
        QTRY_COMPARE(undo->text().remove(QLatin1Char('&')), QStringLiteral("Undo Empty Trash"));
        undo->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(f.proxy->rowCount(), 1, 10000);
        QVERIFY(f.session->cache()->purged().isEmpty());
        QCOMPARE(undo->text().remove(QLatin1Char('&')), QStringLiteral("Undo Delete"));
    }

    // A queued message opens in the compose window again, to be changed:
    // sending or queueing it replaces the queued one; closing leaves it.
    void queuedMessageCanBeEdited()
    {
        Fixture f;
        QVERIFY(f.open(QStringLiteral("In"), {QStringLiteral("INBOX")}));
        ComposeWindow *c = f.w->openCompose();
        c->findChild<QLineEdit *>(QStringLiteral("fieldTo"))->setText(QStringLiteral("Dana Whitfield <dana@example.org>"));
        c->findChild<QLineEdit *>(QStringLiteral("fieldCc"))->setText(QStringLiteral("eli@example.com"));
        c->findChild<QLineEdit *>(QStringLiteral("fieldSubject"))->setText(QStringLiteral("Draft plan"));
        c->findChild<QTextEdit *>(QStringLiteral("composeBody"))->setPlainText(QStringLiteral("First version of the plan."));
        c->addAttachment({QStringLiteral("plan.txt"), 4, {}, QByteArray("PLAN"), QStringLiteral("text/plain")});
        QPointer<ComposeWindow> guard(c);
        c->queue();
        QTRY_VERIFY(!guard);
        QCOMPARE(f.w->queuedCount(), 1);
        const QString row1 = QStringLiteral("queued:%1").arg(f.session->cache()->queued().first().id);

        // Open it (double-click, Ctrl+O, a mailbox window: all come here).
        QTRY_VERIFY(f.model->rowForId(row1) >= 0);
        QVERIFY(!f.w->openMessageWindowFor(row1)); // no reading window for it...
        QCOMPARE(f.w->composers().size(), 1);     // ...a compose window instead
        ComposeWindow *edit = f.w->composers().first();
        QCOMPARE(edit->queuedId(), f.session->cache()->queued().first().id);
        QCOMPARE(edit->findChild<QLineEdit *>(QStringLiteral("fieldTo"))->text(), QStringLiteral("Dana Whitfield <dana@example.org>"));
        QCOMPARE(edit->findChild<QLineEdit *>(QStringLiteral("fieldCc"))->text(), QStringLiteral("eli@example.com"));
        QCOMPARE(edit->findChild<QLineEdit *>(QStringLiteral("fieldSubject"))->text(), QStringLiteral("Draft plan"));
        QVERIFY(edit->findChild<QTextEdit *>(QStringLiteral("composeBody"))->toPlainText().contains(QStringLiteral("First version of the plan.")));
        QCOMPARE(edit->attachments().size(), 1);
        QCOMPARE(edit->attachments().first().name, QStringLiteral("plan.txt"));
        QCOMPARE(edit->attachments().first().data, QByteArray("PLAN"));
        QCOMPARE(f.w->editQueued(row1), edit); // asked again: the same window

        // Closed without sending: still queued, unchanged.
        delete edit;
        QCOMPARE(f.w->queuedCount(), 1);
        QCOMPARE(f.session->cache()->queued().first().subject, QStringLiteral("Draft plan"));

        // Changed and queued again: one entry, the new one.
        edit = f.w->editQueued(row1);
        QVERIFY(edit);
        edit->findChild<QLineEdit *>(QStringLiteral("fieldSubject"))->setText(QStringLiteral("Final plan"));
        guard = edit;
        edit->queue();
        QTRY_VERIFY(!guard);
        QCOMPARE(f.w->queuedCount(), 1);
        QCOMPARE(f.session->cache()->queued().first().subject, QStringLiteral("Final plan"));
        const QString row2 = QStringLiteral("queued:%1").arg(f.session->cache()->queued().first().id);
        QVERIFY(row2 != row1);
        QTRY_VERIFY(f.model->rowForId(row2) >= 0 && f.model->rowForId(row1) < 0);

        // Opened once more and sent from there: it leaves the queue.
        edit = f.w->editQueued(row2);
        QVERIFY(edit);
        guard = edit;
        edit->findChild<QAction *>(QStringLiteral("actionSend"))->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!guard, 10000);
        QCOMPARE(f.w->queuedCount(), 0);
        bool delivered = false;
        for (const MockGoogle::Message &m : f.g.messages()) {
            delivered = delivered || m.subject == QLatin1String("Final plan");
        }
        QVERIFY(delivered);
        QTRY_VERIFY(f.model->rowForId(row2) < 0);

        // One queued by an older zmail has nothing to reopen from.
        MailCache::QueuedMessage old;
        old.mime = "Subject: old\r\n\r\nold";
        old.to = QStringLiteral("eli@example.com");
        old.subject = QStringLiteral("old");
        const qint64 oldId = f.session->cache()->addQueued(old);
        QVERIFY(!f.w->editQueued(QStringLiteral("queued:%1").arg(oldId)));
        QCOMPARE(f.w->queuedCount(), 1);
    }

    // Load All Messages: every message of a mailbox, not just the first
    // pages; then a big selection is deleted in batches, and Undo still works.
    void loadAllThenDeleteMany()
    {
        Fixture f;
        QVERIFY(f.open(QStringLiteral("gmail:IMPORTANT"), {QStringLiteral("INBOX"), QStringLiteral("IMPORTANT")}));
        QStringList older;
        for (int i = 0; i < 60; ++i) {
            MockGoogle::Message m;
            m.from = QStringLiteral("Old <old@example.com>");
            m.subject = QStringLiteral("Important %1").arg(i);
            m.text = QStringLiteral("Fake test mail.");
            m.labels = {QStringLiteral("IMPORTANT"), i % 2 ? QStringLiteral("INBOX") : kLabel};
            m.date = QDateTime::currentDateTimeUtc().addDays(-1 - i);
            older << f.g.addMessage(m, false);
        }
        f.session->sync()->setPageSize(10);
        f.session->cache()->setMeta(QStringLiteral("pageToken:IMPORTANT"), QStringLiteral("3")); // more to come
        QCOMPARE(f.proxy->rowCount(), 3);
        QVERIFY(f.w->findChild<QAction *>(QStringLiteral("actionLoadAll")));

        QSignalSpy finished(f.session->sync(), &SyncEngine::loadAllFinished);
        f.w->loadAllMessages();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000);
        QCOMPARE(finished.first().at(1).toInt(), 63);
        QVERIFY(finished.first().at(2).toBool()); // complete
        QTRY_COMPARE_WITH_TIMEOUT(f.proxy->rowCount(), 63, 10000);
        QVERIFY(!f.session->sync()->hasMore(QStringLiteral("IMPORTANT")));
        QVERIFY(f.session->sync()->loadingAll().isEmpty());
        // Asked again with nothing left: done at once.
        f.w->loadAllMessages();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 5000);

        // Select everything and delete: one batch request, not 63.
        const QString batch = QStringLiteral("POST /gmail/v1/users/me/messages/batchModify");
        f.list->selectAll();
        f.w->findChild<QAction *>(QStringLiteral("menuActionDelete"))->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(f.proxy->rowCount(), 0, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(f.g.count(batch), 1, 10000);
        QVERIFY(f.g.trashCalls.isEmpty());
        for (const QString &id : older + QStringList{f.a, f.b, f.c}) {
            const QStringList l = f.g.messages().value(id).labels;
            QVERIFY(l.contains(QStringLiteral("TRASH")));
            QVERIFY(!l.contains(QStringLiteral("INBOX")) && !l.contains(kLabel)); // out of the Inbox and its folder
        }
        // Undo puts them back where they were.
        f.w->findChild<QAction *>(QStringLiteral("actionUndoDelete"))->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(f.proxy->rowCount(), 63, 20000);
        QTRY_VERIFY_WITH_TIMEOUT(!f.g.messages().value(older.first()).labels.contains(QStringLiteral("TRASH")), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(f.g.messages().value(older.first()).labels.contains(kLabel), 20000);

        // If Gmail refuses the batch, they are deleted one by one instead.
        f.g.addFault({QStringLiteral("/gmail/v1/users/me/messages/batchModify"), 400, 1, -1});
        f.list->setCurrentIndex(f.proxy->index(0, 0)); // Delete needs a current message
        QTRY_VERIFY(f.w->findChild<QAction *>(QStringLiteral("menuActionDelete"))->isEnabled());
        f.list->selectAll();
        f.w->findChild<QAction *>(QStringLiteral("menuActionDelete"))->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(f.g.trashCalls.size(), 63, 30000);
        QTRY_COMPARE_WITH_TIMEOUT(f.proxy->rowCount(), 0, 10000);
    }

    // The sidebar's counts are live: they move with the mail as it is
    // deleted, read, moved or arrives, and show unread of total.
    void sidebarCountsAreLive()
    {
        Fixture f;
        f.g.listen();
        MockGoogle::Message u;
        u.from = QStringLiteral("Priya Raman <priya.raman@example.com>");
        u.subject = QStringLiteral("Unread one");
        u.text = QStringLiteral("Fake test mail.");
        u.labels = {QStringLiteral("INBOX"), QStringLiteral("UNREAD")};
        u.date = QDateTime::currentDateTimeUtc().addSecs(-600);
        const QString unread1 = f.g.addMessage(u, true);
        u.subject = QStringLiteral("Unread two");
        const QString unread2 = f.g.addMessage(u, true);
        QVERIFY(f.open(QStringLiteral("gmail:") + kLabel, {kLabel})); // Alpha, Bravo, Charlie filed and read
        auto *tree = f.w->findChild<QTreeWidget *>(QStringLiteral("mailboxTree"));
        const auto row = [tree](const QString &key) -> QTreeWidgetItem * {
            for (QTreeWidgetItemIterator it(tree); *it; ++it) {
                if ((*it)->data(0, Qt::UserRole).toString() == key) {
                    return *it;
                }
            }
            return nullptr;
        };
        const auto count = [&row](const QString &key) { return row(key) ? row(key)->text(1) : QStringLiteral("?"); };
        const QString folder = QLatin1String("gmail:") + kLabel;

        // Unread of total while something is unread, in bold; just the total otherwise.
        QTRY_COMPARE_WITH_TIMEOUT(count(QStringLiteral("In")), QStringLiteral("2 / 2"), 10000);
        QVERIFY(row(QStringLiteral("In"))->font(0).bold());
        QCOMPARE(row(QStringLiteral("In"))->toolTip(1), QStringLiteral("2 unread of 2"));
        QCOMPARE(count(folder), QStringLiteral("3"));
        QVERIFY(!row(folder)->font(0).bold());
        QCOMPARE(count(QStringLiteral("Trash")), QString());

        // Delete from the folder: its count and Trash's change with the list,
        // without waiting for Gmail. (No request has been answered yet.)
        const int answered = f.g.trashCalls.size();
        f.w->findChild<QAction *>(QStringLiteral("menuActionDelete"))->trigger(); // Bravo
        QCOMPARE(f.g.trashCalls.size(), answered);
        QTRY_COMPARE_WITH_TIMEOUT(count(folder), QStringLiteral("2"), 2000);
        QCOMPARE(count(QStringLiteral("Trash")), QStringLiteral("1"));
        QTRY_COMPARE_WITH_TIMEOUT(f.g.trashCalls.size(), answered + 1, 10000);
        QTest::qWait(1200); // Gmail's own numbers arrive and agree
        QCOMPARE(count(folder), QStringLiteral("2"));
        QCOMPARE(count(QStringLiteral("Trash")), QStringLiteral("1"));

        // Reading a message takes one off its mailbox's unread.
        f.session->sync()->markRead(unread1);
        QTRY_COMPARE_WITH_TIMEOUT(count(QStringLiteral("In")), QStringLiteral("1 / 2"), 5000);
        // Moving one into the folder: out of In, into the folder, still unread there.
        f.session->sync()->moveToLabel(unread2, kLabel);
        QTRY_COMPARE_WITH_TIMEOUT(count(QStringLiteral("In")), QStringLiteral("1"), 5000);
        QCOMPARE(count(folder), QStringLiteral("1 / 3"));
        QVERIFY(row(folder)->font(0).bold() && !row(QStringLiteral("In"))->font(0).bold());

        // Mail that arrives (or changes elsewhere) shows at the next check.
        u.subject = QStringLiteral("Just in");
        f.g.addMessage(u, true);
        f.session->sync()->pollNow(true);
        QTRY_COMPARE_WITH_TIMEOUT(count(QStringLiteral("In")), QStringLiteral("1 / 2"), 10000);
        QTest::qWait(1200);
        QCOMPARE(count(QStringLiteral("In")), QStringLiteral("1 / 2"));
        QCOMPARE(count(folder), QStringLiteral("1 / 3"));
    }

    // Send Later asks when. A time: zmail sends it once that time has come.
    // Held: it waits for File > Send Queued Messages, as before.
    void sendLaterTakesATimeOrHolds()
    {
        Fixture f;
        QVERIFY(f.open(QStringLiteral("In"), {QStringLiteral("INBOX")}));
        const auto compose = [&f](const QString &subject) {
            ComposeWindow *c = f.w->openCompose();
            c->findChild<QLineEdit *>(QStringLiteral("fieldTo"))->setText(QStringLiteral("dana@example.org"));
            c->findChild<QLineEdit *>(QStringLiteral("fieldSubject"))->setText(subject);
            c->findChild<QTextEdit *>(QStringLiteral("composeBody"))->setPlainText(QStringLiteral("Later."));
            return c;
        };
        // Answer the dialog once it is up.
        const auto answer = [](std::function<void(QDialog *)> how) {
            QTimer::singleShot(300, qApp, [how]() {
                for (QWidget *w : QApplication::topLevelWidgets()) {
                    if (w->objectName() == QLatin1String("sendLaterDialog")) {
                        auto *dlg = qobject_cast<QDialog *>(w);
                        how(dlg);
                        dlg->accept();
                    }
                }
            });
        };
        const int sentBefore = f.g.sendCalls;

        // A time two days off.
        const QDateTime when = QDateTime::currentDateTime().addDays(2);
        QPointer<ComposeWindow> c = compose(QStringLiteral("In two days"));
        answer([when](QDialog *dlg) {
            QVERIFY(dlg->findChild<QRadioButton *>(QStringLiteral("sendLaterAt"))->isChecked());
            dlg->findChild<QDateTimeEdit *>(QStringLiteral("sendLaterDateTime"))->setDateTime(when);
        });
        c->findChild<QAction *>(QStringLiteral("actionSendLater"))->trigger();
        QTRY_VERIFY(!c);
        QCOMPARE(f.w->queuedCount(), 1);
        QVERIFY(qAbs(f.session->cache()->queued().first().sendAtMs - when.toMSecsSinceEpoch()) < 60 * 1000);
        f.w->sendDueQueued(); // not yet
        QTest::qWait(300);
        QCOMPARE(f.g.sendCalls, sentBefore);
        QCOMPARE(f.w->queuedCount(), 1);

        // Held for Send Queued Messages: no time.
        c = compose(QStringLiteral("Held"));
        answer([](QDialog *dlg) { dlg->findChild<QRadioButton *>(QStringLiteral("sendLaterHold"))->setChecked(true); });
        c->findChild<QAction *>(QStringLiteral("actionSendLater"))->trigger();
        QTRY_VERIFY(!c);
        QCOMPARE(f.w->queuedCount(), 2);
        QCOMPARE(f.session->cache()->queued().last().sendAtMs, qint64(0));

        // One whose time has come goes by itself; the other two stay.
        c = compose(QStringLiteral("Due"));
        c->queue(QDateTime::currentMSecsSinceEpoch() - 1000);
        QTRY_COMPARE_WITH_TIMEOUT(f.g.sendCalls, sentBefore + 1, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(f.w->queuedCount(), 2, 10000);
        QStringList left;
        for (const auto &q : f.session->cache()->queued()) {
            left << q.subject;
        }
        QCOMPARE(left, (QStringList{QStringLiteral("In two days"), QStringLiteral("Held")}));
    }

    // Archive (Message menu and the list's right-click menu): the first use
    // makes the Archive folder, which has a row of its own above Trash.
    void archiveMakesItsFolderAndFilesTheMessage()
    {
        Fixture f;
        QVERIFY(f.open(QStringLiteral("In"), {QStringLiteral("INBOX"), QStringLiteral("IMPORTANT")}));
        auto *tree = f.w->findChild<QTreeWidget *>(QStringLiteral("mailboxTree"));
        const auto archiveRow = [tree]() -> QTreeWidgetItem * {
            for (int i = 0; i < tree->topLevelItemCount(); ++i) {
                if (tree->topLevelItem(i)->text(0) == QLatin1String("Archive")) {
                    return tree->topLevelItem(i);
                }
            }
            return nullptr;
        };
        QVERIFY(archiveRow());
        QCOMPARE(archiveRow()->data(0, Qt::UserRole).toString(), QStringLiteral("Archive")); // no folder yet
        QAction *archive = f.w->findChild<QAction *>(QStringLiteral("menuActionArchive"));
        QVERIFY(archive && archive->isEnabled());

        // Gmail keeps "Archive" for itself, and here "Archived" too: the
        // folder gets the next name, and is still the Archive row.
        f.g.reservedLabelNames << QStringLiteral("Archived");
        archive->trigger(); // Bravo
        QTRY_VERIFY_WITH_TIMEOUT(archiveRow() && archiveRow()->data(0, Qt::UserRole).toString().startsWith(QLatin1String("gmail:")), 10000);
        const QString id = archiveRow()->data(0, Qt::UserRole).toString().mid(6);
        QString named;
        for (const zmail::CachedLabel &l : f.session->cache()->labels()) {
            if (l.id == id) {
                named = l.name;
            }
        }
        QCOMPARE(named, QStringLiteral("Archives"));
        QCOMPARE(archiveRow()->text(0), QStringLiteral("Archive"));
        QTRY_VERIFY_WITH_TIMEOUT(f.g.messages().value(f.b).labels.contains(id), 10000);
        QVERIFY(!f.g.messages().value(f.b).labels.contains(QStringLiteral("INBOX")));
        QVERIFY(!f.g.messages().value(f.b).labels.contains(QStringLiteral("IMPORTANT")));
        QTRY_COMPARE_WITH_TIMEOUT(f.proxy->rowCount(), 2, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(archiveRow()->text(1), QStringLiteral("1"), 10000);
        // It is the Archive row only, not also a folder under Folders.
        int rows = 0;
        for (QTreeWidgetItemIterator it(tree); *it; ++it) {
            rows += (*it)->data(0, Qt::UserRole).toString() == QLatin1String("gmail:") + id;
        }
        QCOMPARE(rows, 1);

        // The second one goes to the same folder.
        f.list->setCurrentIndex(f.proxy->mapFromSource(f.model->index(f.model->rowForId(f.a), 0)));
        QTRY_COMPARE(f.w->shownMessageId(), f.a);
        archive->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(f.g.messages().value(f.a).labels.contains(id), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(archiveRow()->text(1), QStringLiteral("2"), 10000);
    }

    // Gmail refuses every name: nothing is moved, and the status bar says so.
    // Opening the still-empty Archive row asks Gmail for nothing.
    void archiveSaysSoWhenGmailRefusesTheFolder()
    {
        Fixture f;
        QVERIFY(f.open(QStringLiteral("In"), {QStringLiteral("INBOX")}));
        f.g.reservedLabelNames << QStringLiteral("Archived") << QStringLiteral("Archives")
                               << QStringLiteral("Archived Mail") << QStringLiteral("zmail Archive");
        f.w->findChild<QAction *>(QStringLiteral("menuActionArchive"))->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(f.w->statusBar()->currentMessage().contains(QStringLiteral("nothing was moved")), 10000);
        QVERIFY(f.g.messages().value(f.b).labels.contains(QStringLiteral("INBOX")));
        QCOMPARE(f.proxy->rowCount(), 3);

        const int lists = f.g.count(QStringLiteral("GET /gmail/v1/users/me/messages"));
        f.selectView(QStringLiteral("Archive"));
        QTest::qWait(500);
        QCOMPARE(f.proxy->rowCount(), 0);
        QCOMPARE(f.g.count(QStringLiteral("GET /gmail/v1/users/me/messages")), lists);
    }

    // Empty Trash remembers what it hid, and the sidebar takes that many off
    // Gmail's Trash total. Gmail erases old Trash by itself, so the
    // remembered set can outnumber what is left; the count must not fall
    // below the Trash mail that is on show.
    void trashCountSurvivesAStaleEmptiedSet()
    {
        Fixture f;
        QVERIFY(f.open(QStringLiteral("In"), {QStringLiteral("INBOX")}));
        auto *tree = f.w->findChild<QTreeWidget *>(QStringLiteral("mailboxTree"));
        const auto trashCount = [tree]() {
            for (QTreeWidgetItemIterator it(tree); *it; ++it) {
                if ((*it)->data(0, Qt::UserRole).toString() == QLatin1String("Trash")) {
                    return (*it)->text(1);
                }
            }
            return QStringLiteral("?");
        };
        f.session->sync()->trash(f.a);
        f.session->sync()->trash(f.c);
        QTRY_COMPARE_WITH_TIMEOUT(trashCount(), QStringLiteral("2"), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(f.g.messages().value(f.c).labels.contains(QStringLiteral("TRASH")), 10000);
        QTest::qWait(1200); // Gmail's own numbers are in

        // Five messages emptied long ago, since erased by Gmail.
        f.session->cache()->setPurged({QStringLiteral("gone1"), QStringLiteral("gone2"), QStringLiteral("gone3"),
                                       QStringLiteral("gone4"), QStringLiteral("gone5")});
        emit f.session->sync()->countsChanged();
        QCOMPARE(trashCount(), QStringLiteral("2"));

        // The next look at Gmail's Trash count notices, and forgets them.
        f.session->sync()->trash(f.b);
        QTRY_VERIFY_WITH_TIMEOUT(f.session->cache()->purged().isEmpty(), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(trashCount(), QStringLiteral("3"), 10000);
    }

    // Drag a message from the list onto a folder in the sidebar.
    void dragOntoAFolderMovesTheMessage()
    {
        Fixture f;
        QVERIFY(f.open(QStringLiteral("In"), {QStringLiteral("INBOX")}));
        auto *tree = f.w->findChild<QTreeWidget *>(QStringLiteral("mailboxTree"));
        QTreeWidgetItem *folder = nullptr;
        const auto findFolder = [&]() {
            folder = nullptr;
            for (QTreeWidgetItemIterator it(tree); *it; ++it) {
                if ((*it)->data(0, Qt::UserRole).toString() == QLatin1String("gmail:") + kLabel) {
                    folder = *it;
                }
            }
            return folder != nullptr;
        };
        QTRY_VERIFY_WITH_TIMEOUT(findFolder(), 10000); // the sidebar fills in once the labels are in
        tree->scrollToItem(folder);
        const QPoint at = tree->visualItemRect(folder).center();
        QVERIFY(tree->viewport()->acceptDrops());
        QVERIFY(f.list->dragEnabled());

        // What the list hands to a drag: the selected message.
        const QModelIndex row = f.list->currentIndex();
        QVERIFY(f.proxy->flags(row) & Qt::ItemIsDragEnabled);
        std::unique_ptr<QMimeData> mime(f.proxy->mimeData({row}));
        QVERIFY(mime && mime->hasFormat(QByteArray(MessageListModel::kMessageIdsMime)));

        QDragEnterEvent enter(at, Qt::CopyAction | Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &enter);
        QVERIFY(enter.isAccepted());
        QCOMPARE(tree->property("dropTarget").toString(), QString());
        QDragMoveEvent move(at, Qt::CopyAction | Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &move);
        QVERIFY(move.isAccepted());
        // The folder under the drag is lit...
        QCOMPARE(tree->property("dropTarget").toString(), QLatin1String("gmail:") + kLabel);
        const QPoint rowEdge(tree->viewport()->width() - 4, at.y()); // right of the text, inside the row
        const QColor lit = tree->viewport()->grab().toImage().pixelColor(rowEdge);
        // ...a row that takes no mail is not (the "Gmail Labels" heading)...
        QTreeWidgetItem *heading = folder->parent();
        QVERIFY(heading);
        QDragMoveEvent off(tree->visualItemRect(heading).center(), Qt::CopyAction | Qt::MoveAction, mime.get(),
                           Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &off);
        QVERIFY(!off.isAccepted());
        QCOMPARE(tree->property("dropTarget").toString(), QString());
        const QColor plain = tree->viewport()->grab().toImage().pixelColor(rowEdge);
        QVERIFY2(lit != plain, qPrintable(lit.name() + QLatin1Char(' ') + plain.name())); // it is drawn differently
        // ...and leaving the sidebar puts it out.
        QDragMoveEvent back(at, Qt::CopyAction | Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &back);
        QCOMPARE(tree->property("dropTarget").toString(), QLatin1String("gmail:") + kLabel);
        QDragLeaveEvent leave;
        QApplication::sendEvent(tree->viewport(), &leave);
        QCOMPARE(tree->property("dropTarget").toString(), QString());
        // Back in, and dropped.
        QDragEnterEvent again(at, Qt::CopyAction | Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &again);
        QDragMoveEvent over(at, Qt::CopyAction | Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &over);
        QVERIFY(over.isAccepted());
        QCOMPARE(tree->property("dropTarget").toString(), QLatin1String("gmail:") + kLabel);
        QDropEvent drop(at, Qt::CopyAction | Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &drop);
        QVERIFY(drop.isAccepted());
        QCOMPARE(tree->property("dropTarget").toString(), QString()); // dropped: no longer lit
        QTRY_VERIFY_WITH_TIMEOUT(f.g.messages().value(f.b).labels.contains(kLabel), 10000);
        QVERIFY(!f.g.messages().value(f.b).labels.contains(QStringLiteral("INBOX")));
        QTRY_COMPARE_WITH_TIMEOUT(f.proxy->rowCount(), 2, 10000); // it left the Inbox list
    }

    // After Empty Trash the list is empty though Gmail's Trash is not.
    // "The list is short, load more" must not then page through all of it,
    // fetching thousands of messages nobody will see: that kept zmail busy
    // for minutes and left a second Empty Trash waiting behind it (0.6.3).
    void emptiedTrashIsNotPagedThroughAgain()
    {
        Fixture f;
        QVERIFY(f.open(QStringLiteral("Trash"), {QStringLiteral("TRASH")}));
        // Thirty more in Gmail's Trash that zmail has not listed yet.
        for (int i = 0; i < 30; ++i) {
            MockGoogle::Message m;
            m.from = QStringLiteral("Old <old@example.com>");
            m.subject = QStringLiteral("Old trash %1").arg(i);
            m.text = QStringLiteral("Fake test mail.");
            m.labels = {QStringLiteral("TRASH")};
            m.date = QDateTime::currentDateTimeUtc().addDays(-10 - i);
            f.g.addMessage(m, false);
        }
        f.session->sync()->setPageSize(10);
        f.session->cache()->setMeta(QStringLiteral("pageToken:TRASH"), QStringLiteral("3"));
        const QString getMessage = QStringLiteral("GET /gmail/v1/users/me/messages/");
        const QString list = QStringLiteral("GET /gmail/v1/users/me/messages");

        QSignalSpy emptied(f.session->sync(), &SyncEngine::trashEmptied);
        f.w->emptyTrash(false);
        QTRY_COMPARE_WITH_TIMEOUT(emptied.size(), 1, 10000);
        QCOMPARE(emptied.first().first().toInt(), 33);
        QTRY_COMPARE(f.proxy->rowCount(), 0);
        const int fetched = f.g.count(getMessage);
        const int listed = f.g.count(list) - fetched; // the prefix counts both
        QTest::qWait(1500);
        QCOMPARE(f.g.count(getMessage), fetched);                   // no emptied message is downloaded
        QVERIFY2(f.g.count(list) - f.g.count(getMessage) - listed <= 1, // at most one more look at the listing
                 qPrintable(QString::number(f.g.count(list) - f.g.count(getMessage) - listed)));

        // And emptying again, with more deleted since, still works at once.
        const QString later = f.seed(QStringLiteral("Later zebra"), {QStringLiteral("TRASH")}, 0);
        f.session->sync()->pollNow(true);
        QTRY_COMPARE_WITH_TIMEOUT(f.proxy->rowCount(), 1, 10000);
        f.w->emptyTrash(false);
        QTRY_COMPARE_WITH_TIMEOUT(emptied.size(), 2, 10000);
        QCOMPARE(emptied.last().first().toInt(), 34);
        QTRY_COMPARE(f.proxy->rowCount(), 0);
    }

    // Right-click Trash offers Empty Trash, wherever you were when you did.
    void rightClickTrashOffersEmptyTrash_data()
    {
        QTest::addColumn<QString>("from");
        QTest::addColumn<QStringList>("labels");
        QTest::newRow("from In") << "In" << QStringList{"INBOX"};
        QTest::newRow("from Trash itself") << "Trash" << QStringList{"TRASH"};
        QTest::newRow("from a folder") << "gmail:Label_7" << QStringList{kLabel};
        QTest::newRow("from Junk") << "Junk" << QStringList{"SPAM"};
        QTest::newRow("from Search results") << "Search" << QStringList{"INBOX", kLabel};
        QTest::newRow("from Out") << "Out" << QStringList{"SENT"};
    }
    void rightClickTrashOffersEmptyTrash()
    {
        QFETCH(QString, from);
        QFETCH(QStringList, labels);
        Fixture f;
        QVERIFY(f.open(from, labels));
        auto *tree = f.w->findChild<QTreeWidget *>(QStringLiteral("mailboxTree"));
        QTreeWidgetItem *trash = nullptr;
        const auto findTrash = [&]() {
            trash = nullptr;
            for (QTreeWidgetItemIterator it(tree); *it; ++it) {
                if ((*it)->data(0, Qt::UserRole).toString() == QLatin1String("Trash")) {
                    trash = *it;
                }
            }
            return trash != nullptr;
        };
        QTRY_VERIFY(findTrash());
        emit tree->customContextMenuRequested(tree->visualItemRect(trash).center());
        QMenu *menu = nullptr;
        (void)QTest::qWaitFor([&] {
            for (QWidget *w : QApplication::topLevelWidgets()) {
                if (auto *m = qobject_cast<QMenu *>(w); m && m->isVisible() && m->objectName() == QLatin1String("mailboxMenu")) {
                    menu = m;
                }
            }
            return menu != nullptr;
        }, 3000);
        QVERIFY(menu);
        QStringList names;
        for (QAction *a : menu->actions()) {
            names << a->objectName();
        }
        menu->close();
        QVERIFY2(names.contains(QStringLiteral("actionEmptyTrash")), qPrintable(names.join(QLatin1Char(' '))));
    }

    // Coloured flags: the colour is zmail's, "flagged" is Gmail's star.
    void flagsInColours()
    {
        Fixture f;
        QVERIFY(f.open(QStringLiteral("In"), {QStringLiteral("INBOX")}));
        const auto flagOf = [&](const QString &id) { return f.model->item(f.model->rowForId(id)).flag; };
        const auto starred = [&](const QString &id) { return f.g.messages().value(id).labels.contains(QStringLiteral("STARRED")); };
        QVERIFY(f.w->findChild<QAction *>(QStringLiteral("actionFlag_red")));
        QVERIFY(f.w->findChild<QAction *>(QStringLiteral("actionClearFlag")));
        QCOMPARE(flagOf(f.b), QString());

        // From the menu: Bravo (the current message) gets a blue flag.
        f.w->findChild<QAction *>(QStringLiteral("actionFlag_blue"))->trigger();
        QTRY_COMPARE(flagOf(f.b), QStringLiteral("blue"));
        QTRY_VERIFY(starred(f.b));
        const QModelIndex cell = f.proxy->index(f.list->currentIndex().row(), MessageListModel::Priority);
        QVERIFY(!cell.data(Qt::DecorationRole).isNull());
        QVERIFY(cell.data(Qt::ToolTipRole).toString().startsWith(QStringLiteral("Blue flag")));
        // Another colour: no second call to Gmail, it is starred already.
        const int calls = f.g.modifyCalls.size();
        f.w->setFlagOnSelected(QStringLiteral("green"));
        QTRY_COMPARE(flagOf(f.b), QStringLiteral("green"));
        QCOMPARE(f.g.modifyCalls.size(), calls);

        // Several at once, then cleared.
        f.list->selectAll();
        f.w->setFlagOnSelected(QStringLiteral("purple"));
        QTRY_VERIFY(starred(f.a) && starred(f.b) && starred(f.c));
        QTRY_COMPARE(flagOf(f.a), QStringLiteral("purple"));
        f.w->findChild<QAction *>(QStringLiteral("actionClearFlag"))->trigger();
        QTRY_VERIFY(!starred(f.a) && !starred(f.b) && !starred(f.c));
        QTRY_COMPARE(flagOf(f.c), QString());

        // A click in the flag column: the colour used last, then off again.
        const auto flagCell = [&](const QString &id) {
            const QModelIndex i = f.proxy->mapFromSource(f.model->index(f.model->rowForId(id), MessageListModel::Priority));
            return f.list->visualRect(i).center();
        };
        QTest::mouseClick(f.list->viewport(), Qt::LeftButton, Qt::NoModifier, flagCell(f.a));
        QTRY_COMPARE(flagOf(f.a), QStringLiteral("purple"));
        QTRY_VERIFY(starred(f.a));
        QCOMPARE(flagOf(f.b), QString()); // only the clicked one
        QTest::mouseClick(f.list->viewport(), Qt::LeftButton, Qt::NoModifier, flagCell(f.a));
        QTRY_COMPARE(flagOf(f.a), QString());
        QTRY_VERIFY(!starred(f.a));

        // The colour outlives a full resync (which empties the message cache).
        f.w->setFlagOnSelected(QStringLiteral("orange"));
        QTRY_VERIFY(starred(f.a));
        QCOMPARE(f.session->cache()->flags().value(f.a), QStringLiteral("orange"));
        f.session->cache()->clearMessages();
        QCOMPARE(f.session->cache()->flags().value(f.a), QStringLiteral("orange"));
    }

    // Starred in Gmail, never flagged here: Gmail's colour.
    void starredElsewhereShowsAsYellow()
    {
        Fixture f;
        QVERIFY(f.open(QStringLiteral("In"), {QStringLiteral("INBOX"), QStringLiteral("STARRED")}));
        QCOMPARE(f.model->item(f.model->rowForId(f.b)).flag, QStringLiteral("yellow"));
    }

    // Right-click Trash > Empty Trash: everything in it leaves zmail, the
    // count goes to nothing, and mail deleted afterwards still shows.
    void emptyTrashClearsTheTrash()
    {
        Fixture f;
        QVERIFY(f.open(QStringLiteral("Trash"), {QStringLiteral("TRASH")}));
        auto *tree = f.w->findChild<QTreeWidget *>(QStringLiteral("mailboxTree"));
        const auto trashCount = [tree]() {
            for (QTreeWidgetItemIterator it(tree); *it; ++it) {
                if ((*it)->data(0, Qt::UserRole).toString() == QLatin1String("Trash")) {
                    return (*it)->text(1);
                }
            }
            return QStringLiteral("?");
        };
        const QString before = trashCount();
        QVERIFY2(before.contains(QLatin1Char('3')), qPrintable(before));
        QSignalSpy emptied(f.session->sync(), &SyncEngine::trashEmptied);
        f.w->emptyTrash(false);
        QTRY_COMPARE_WITH_TIMEOUT(emptied.size(), 1, 10000);
        QCOMPARE(emptied.first().first().toInt(), 3);
        QTRY_COMPARE_WITH_TIMEOUT(f.proxy->rowCount(), 0, 10000);
        QTRY_VERIFY2(!trashCount().contains(QLatin1Char('3')), qPrintable(trashCount()));
        QCOMPARE(f.session->cache()->purged().size(), 3);
        // Gmail still has them: zmail may move mail to Trash, not erase it.
        QVERIFY(f.g.messages().value(f.a).labels.contains(QStringLiteral("TRASH")));

        // Something deleted after that is in the Trash as usual.
        const QString later = f.seed(QStringLiteral("Later zebra"), {QStringLiteral("TRASH")}, 0);
        f.session->sync()->pollNow(true);
        QTRY_COMPARE_WITH_TIMEOUT(f.proxy->rowCount(), 1, 10000);
        QCOMPARE(f.visibleIds(), QStringList{later});
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
        QTRY_VERIFY(f.session->cache()->message(f.b).labels.contains(QStringLiteral("TRASH")));
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
