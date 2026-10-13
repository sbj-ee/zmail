// Offscreen screenshots of the "Gmail (app password)" account dialog, against
// local fake IMAP/SMTP servers and a made-up password (never a real one).
//   zmail_dialogshot <outdir>   writes apppassword-empty.png, -ok.png, -error.png, connect.png
//   zmail_dialogshot <outdir> imap   main window on an app-password (IMAP) account:
//                                    imap-mainwindow.png, imap-idle-newmail.png
//   zmail_dialogshot <outdir> imap-actions   changing mail over IMAP: imap-starred-trashed.png,
//                                    imap-search.png (X-GM-RAW), imap-attachment.png
//   zmail_dialogshot <outdir> imap-send   SMTP + drafts: imap-compose.png, imap-drafts.png,
//                                    imap-send-queued.png (with a fake SMTP server)
//   zmail_dialogshot <outdir> contacts   contacts-settings-menu.png, contacts-window.png, contacts-edit-legacy.png
//   zmail_dialogshot <outdir> imap-extras   imap-throttled.png (Gmail's connection limit, backing off),
//                                    imap-snoozed.png, imap-from-name.png
#include "core/MailSession.h"
#include "core/TokenStore.h"
#include "core/ConnectionTester.h"
#include "mock/FakeMailServer.h"
#include "mock/FakeImapServer.h"
#include "mock/FakeSmtpServer.h"
#include "core/MailCache.h"
#include "core/ReplyBuilder.h"
#include "core/Sender.h"
#include "ui/ComposeWindow.h"
#include "ui/ContactsWindow.h"
#include "ui/ContactEditDialog.h"
#include "core/ContactStore.h"
#include <QMenu>
#include <memory>
#include "core/ImapSyncEngine.h"
#include "core/ServerLimits.h"
#include <QDateTime>
#include <QTextEdit>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include "MainWindow.hpp"
#include "core/SyncEngine.h"
#include "core/MailCache.h"
#include <QSettings>
#include <QTreeView>
#include <QStatusBar>
#include <QSignalSpy>
#include "ui/AppPasswordDialog.h"
#include "ui/ConnectDialog.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QDir>
#include <QGroupBox>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QLabel>
#include <QTest>
#include <cstdio>

using namespace zmail;

namespace {
void shoot(QWidget *w, const QString &path)
{
    w->adjustSize();
    w->show();
    QTest::qWait(200);
    w->grab().save(path);
    std::printf("%s\n", qPrintable(path));
}
void fill(ui::AppPasswordDialog &d, const QString &pw)
{
    d.findChild<QLineEdit *>(QStringLiteral("displayNameEdit"))->setText(QStringLiteral("Alex Morgan"));
    d.findChild<QLineEdit *>(QStringLiteral("emailEdit"))->setText(QStringLiteral("alex.morgan@gmail.com"));
    d.findChild<QLineEdit *>(QStringLiteral("appPasswordEdit"))->setText(pw);
}
bool waitTest(ui::AppPasswordDialog &d)
{
    d.runTest();
    return QTest::qWaitFor([&] { return !d.tester()->running(); }, 10000);
}
} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("zmail-dialogshot"));
    ui::applyTheme(ui::ThemeMode::Light);
    const QString out = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QDir::currentPath();
    QDir().mkpath(out);
    MemoryTokenStore store;

    const QString mode = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString();
    if (mode == QLatin1String("contacts")) {
        // zmail-only contacts: the Settings menu without "Sync Contacts from
        // Google", the Contacts window without its sync button, and a contact
        // an older zmail copied from Google, still editable.
        MainWindow w;
        w.resize(1180, 740);
        w.show();
        QTest::qWait(300);
        auto *menu = w.findChild<QMenu *>(QStringLiteral("menuSettings"));
        menu->popup(w.mapToGlobal(QPoint(300, 30)));
        QTest::qWait(400);
        menu->grab().save(out + QStringLiteral("/contacts-settings-menu.png"));
        std::printf("%s\n", qPrintable(out + QStringLiteral("/contacts-settings-menu.png")));
        menu->hide();
        ContactStore cs;
        cs.open(QStringLiteral(":memory:"));
        Contact g;
        g.id = QStringLiteral("people/c1");
        g.displayName = QStringLiteral("Priya Raman");
        g.source = QStringLiteral("google");
        g.emails.append({QStringLiteral("priya@northwind.example"), true});
        cs.upsertContact(g);
        cs.setCategories(g.id, {QStringLiteral("Work")});
        cs.setFields(g.id, {{QStringLiteral("Phone"), QStringLiteral("555 0142")}});
        cs.setComment(g.id, QStringLiteral("Field schedule lead."));
        const QString dana = cs.createContact(QStringLiteral("Dana Whitfield"), {{QStringLiteral("dana@whitfield.example"), true}});
        cs.setCategories(dana, {QStringLiteral("Work")});
        cs.addLocalContact(QStringLiteral("Marcus Lee"), QStringLiteral("marcus.lee@example.org"));
        const QString jordan = cs.createContact(QStringLiteral("Jordan Kim"), {{QStringLiteral("jordan.kim@example.net"), true}});
        cs.setNickname(jordan, QStringLiteral("jk"));
        ui::ContactsWindow cw(&cs);
        cw.resize(1000, 560);
        cw.show();
        cw.selectContacts({g.id});
        QTest::qWait(500);
        cw.grab().save(out + QStringLiteral("/contacts-window.png"));
        std::printf("%s\n", qPrintable(out + QStringLiteral("/contacts-window.png")));
        std::unique_ptr<ui::ContactEditDialog> ed(cw.makeEditDialog(g.id));
        ed->show();
        QTest::qWait(400);
        ed->grab().save(out + QStringLiteral("/contacts-edit-legacy.png"));
        std::printf("%s\n", qPrintable(out + QStringLiteral("/contacts-edit-legacy.png")));
        return 0;
    }
    if (mode == QLatin1String("imap") || mode == QLatin1String("imap-actions") || mode == QLatin1String("imap-send")
        || mode == QLatin1String("imap-extras")) {
        const bool extras = mode == QLatin1String("imap-extras");
        const bool actions = mode == QLatin1String("imap-actions");
        const bool sending = mode == QLatin1String("imap-send");
        test::FakeImapServer imap;
        imap.seedDemo(24);
        store.values.insert(AppPasswordAccount::appPasswordKey(imap.user), imap.password); // the fake's made-up one
        AppPasswordAccount a;
        a.email = imap.user;
        a.imapHost = QStringLiteral("127.0.0.1");
        a.imapPort = imap.port();
        test::FakeSmtpServer smtp(&imap);
        a.smtpHost = QStringLiteral("127.0.0.1");
        a.smtpPort = smtp.port();
        if (extras) {
            a.displayName = QStringLiteral("Alex Morgan");
            imap.loginRefusals = {"NO [ALERT] Too many simultaneous connections. (Failure)"};
        }
        {
            QSettings s;
            s.clear();
            a.save(s);
        }
        SessionOptions o;
        o.client.status = ClientConfig::LoadStatus::Missing;
        o.store = &store;
        o.cachePathOverride = QStringLiteral(":memory:");
        o.contactsPathOverride = QStringLiteral(":memory:");
        o.imapPlaintextForTests = true;
        o.pollIntervalMs = 3600 * 1000;
        MailSession session(o);
        MainWindow w;
        w.setSession(&session);
        w.resize(1180, 740);
        w.show();
        session.restoreSaved();
        if (extras) {
            auto *eng = qobject_cast<ImapSyncEngine *>(session.sync());
            eng->setBackoffScaleForTests(0.05); // 60 s -> 3 s, time for the shot
            QTest::qWaitFor([&] { return eng->lastLimit() == ServerLimits::Kind::TooManyConnections; }, 5000);
            QTest::qWait(400);
            w.grab().save(out + QStringLiteral("/imap-throttled.png"));
            std::printf("%s\n", qPrintable(out + QStringLiteral("/imap-throttled.png")));
        }
        QSignalSpy idle(session.sync(), &SyncEngine::idle);
        if (!idle.wait(10000)) {
            return 2;
        }
        QTest::qWait(800);
        auto *list = w.findChild<QTreeView *>(QStringLiteral("messageList"));
        if (list && list->model()->rowCount() > 0) {
            list->setCurrentIndex(list->model()->index(0, 0));
        }
        QTest::qWait(1500);
        w.statusBar()->clearMessage();
        auto snap = [&](const char *name) {
            QTest::qWait(1200);
            const QString path = out + QLatin1Char('/') + QLatin1String(name);
            w.grab().save(path);
            std::printf("%s\n", qPrintable(path));
        };
        auto openBox = [&](const QString &key) { // as a click in the mailbox tree
            auto *tree = w.findChild<QTreeWidget *>(QStringLiteral("mailboxTree"));
            for (QTreeWidgetItemIterator it(tree); *it; ++it) {
                if ((*it)->data(0, Qt::UserRole).toString() == key) {
                    tree->setCurrentItem(*it);
                }
            }
            QTest::qWait(1800);
        };
        if (extras) {
            auto *sync = session.sync();
            const auto inbox = session.cache()->messages(QStringLiteral("INBOX"));
            const qint64 tomorrow = QDateTime::currentDateTime().addDays(1).date().startOfDay().addSecs(8 * 3600).toMSecsSinceEpoch();
            for (int i = 1; i < 3 && i < inbox.size(); ++i) {
                sync->snooze(inbox.at(i).id, tomorrow);
            }
            QTest::qWaitFor([&] { return !sync->isBusy(); }, 5000);
            openBox(QStringLiteral("Snoozed"));
            if (list && list->model()->rowCount() > 0) {
                list->setCurrentIndex(list->model()->index(0, 0));
            }
            w.statusBar()->showMessage(QStringLiteral("Snoozed 2 until tomorrow 8:00 AM (out of the Inbox on imap.gmail.com)"));
            snap("imap-snoozed.png");
            ComposeWindow c;
            c.setSession(&session);
            ComposeDraft d;
            d.to = QStringLiteral("Priya Raman <priya@northwind.example>");
            d.subject = QStringLiteral("From line with your name");
            c.setDraft(d);
            c.findChild<QTextEdit *>(QStringLiteral("composeBody"))
                ->setPlainText(QStringLiteral("From now shows \u201cAlex Morgan <alex.morgan@gmail.com>\u201d: set in the account "
                                              "dialog or Settings \u2192 Your Name\u2026"));
            c.resize(820, 420);
            c.show();
            QTest::qWait(700);
            c.grab().save(out + QStringLiteral("/imap-from-name.png"));
            std::printf("%s\n", qPrintable(out + QStringLiteral("/imap-from-name.png")));
            QSettings().clear();
            return 0;
        }
        if (sending) {
            {
                ComposeWindow c;
                c.setSession(&session);
                ComposeDraft d;
                d.to = QStringLiteral("Priya Raman <priya@northwind.example>");
                d.cc = QStringLiteral("dana@whitfield.example");
                d.subject = QStringLiteral("Budget notes for the Q4 schedule");
                c.setDraft(d);
                c.findChild<QTextEdit *>(QStringLiteral("composeBody"))
                    ->setPlainText(QStringLiteral("Priya,\n\nRough numbers below; I'll finish them tomorrow. Saved to "
                                                  "[Gmail]/Drafts over IMAP so it shows on the phone too.\n\nAlex"));
                c.resize(820, 560);
                c.show();
                QSignalSpy saved(&c, &ComposeWindow::draftSaved);
                c.saveDraft();
                saved.wait(5000);
                QTest::qWait(600);
                c.grab().save(out + QStringLiteral("/imap-compose.png"));
                std::printf("%s\n", qPrintable(out + QStringLiteral("/imap-compose.png")));
                c.close();
            }
            openBox(QStringLiteral("gmail:DRAFT"));
            QTest::qWait(1200);
            if (list && list->model()->rowCount() > 0) {
                list->setCurrentIndex(list->model()->index(0, 0));
            }
            w.statusBar()->showMessage(QStringLiteral("Draft saved to [Gmail]/Drafts (IMAP APPEND)"));
            snap("imap-drafts.png");
            for (const char *subj : {"Site visit moved to Thursday", "Invoice 4471 follow-up"}) {
                MailCache::QueuedMessage q;
                q.mime = QByteArray("From: Alex Morgan <alex.morgan@gmail.com>\r\nTo: Marcus Lee <marcus.lee@example.org>\r\n"
                                    "Subject: ") + subj + "\r\nMIME-Version: 1.0\r\nContent-Type: text/plain\r\n\r\n"
                                    "Sent with File > Send Queued Messages over SMTP.\r\n";
                q.subject = QString::fromLatin1(subj);
                q.to = QStringLiteral("Marcus Lee <marcus.lee@example.org>");
                session.cache()->addQueued(q);
            }
            w.findChild<QAction *>(QStringLiteral("actionSendQueued"))->trigger();
            QTest::qWaitFor([&] { return smtp.delivered.size() == 2 && session.cache()->queued().isEmpty(); }, 10000);
            QTest::qWait(1500);
            const QString status = w.statusBar()->currentMessage();
            openBox(QStringLiteral("Out"));
            QTest::qWait(1500);
            if (list && list->model()->rowCount() > 0) {
                list->setCurrentIndex(list->model()->index(0, 0));
            }
            w.statusBar()->showMessage(status.isEmpty() ? QStringLiteral("2 messages sent over SMTP") : status);
            snap("imap-send-queued.png");
            QSettings().clear();
            return 0;
        }
        if (actions) {
            auto *sync = session.sync();
            auto idOf = [&](const char *subject) {
                for (quint64 id = 0x18f3a0c2b4d50001ULL; id < 0x18f3a0c2b4d50100ULL; ++id) {
                    if (auto *m = imap.find(id); m && m->raw.contains(subject)) {
                        return QString::number(id, 16);
                    }
                }
                return QString();
            };
            // Star one, trash two (one bulk MOVE), mark one read: all over STORE / UID MOVE.
            sync->setFlag(idOf("Spare parts order"), QStringLiteral("red"));
            sync->trashMany({idOf("Lunch on Thursday?"), idOf("free drink")});
            sync->markRead(idOf("safety stand-down"));
            QTest::qWaitFor([&] { return !sync->isBusy(); }, 5000);
            w.statusBar()->showMessage(QStringLiteral("Starred 1, moved 2 to Trash, marked 1 read on imap.gmail.com (fake)"));
            snap("imap-starred-trashed.png");
            auto *search = w.findChild<QLineEdit *>(QStringLiteral("searchBox"));
            search->setText(QStringLiteral("from:hydro"));
            Q_EMIT search->returnPressed();
            QTest::qWait(2500);
            snap("imap-search.png");
            search->clear();
            QSignalSpy got(sync, &SyncEngine::newMail);
            QTest::qWaitFor([&] { return imap.anyIdling(); }, 5000);
            const QByteArray pdf("%PDF-1.4 fake demo bytes\n");
            imap.deliver("From: Jordan Kim <jordan.kim@example.net>\r\nTo: alex.morgan@gmail.com\r\n"
                         "Subject: Breaker spec sheet (attachment over IMAP)\r\nMIME-Version: 1.0\r\n"
                         "Content-Type: multipart/mixed; boundary=b1\r\n\r\n--b1\r\nContent-Type: text/plain\r\n\r\n"
                         "Spec sheet attached; File > Save Attachments fetches it over IMAP.\r\n--b1\r\n"
                         "Content-Type: application/pdf; name=\"breaker-spec.pdf\"\r\n"
                         "Content-Disposition: attachment; filename=\"breaker-spec.pdf\"\r\n"
                         "Content-Transfer-Encoding: base64\r\n\r\n" + pdf.toBase64() + "\r\n--b1--\r\n");
            got.wait(5000);
            QTest::qWait(800);
            if (list && list->model()->rowCount() > 0) {
                list->setCurrentIndex(list->model()->index(0, 0));
            }
            QTest::qWait(1500);
            w.statusBar()->clearMessage();
            snap("imap-attachment.png");
            QSettings().clear();
            return 0;
        }
        w.grab().save(out + QStringLiteral("/imap-mainwindow.png"));
        std::printf("%s\n", qPrintable(out + QStringLiteral("/imap-mainwindow.png")));
        QSignalSpy newMail(session.sync(), &SyncEngine::newMail);
        QTest::qWaitFor([&] { return imap.anyIdling(); }, 5000);
        imap.deliver(test::FakeImapServer::makeMessage(QStringLiteral("Nia Okafor"), QStringLiteral("nia@example.org"),
                                                       QStringLiteral("Pushed by IDLE: crew roster for Friday"),
                                                       QStringLiteral("Roster attached below. This arrived over IMAP IDLE.")));
        if (!newMail.wait(5000)) {
            return 3;
        }
        QTest::qWait(1200);
        w.grab().save(out + QStringLiteral("/imap-idle-newmail.png"));
        std::printf("%s\n", qPrintable(out + QStringLiteral("/imap-idle-newmail.png")));
        QSettings().clear();
        return 0;
    }

    {
        ui::AppPasswordDialog d(&store);
        shoot(&d, out + QStringLiteral("/apppassword-empty.png"));
    }
    // A passing Test. The fakes listen on localhost; the screenshot shows
    // Gmail's real host names, which is what the dialog sends by default.
    for (const bool ok : {true, false}) {
        test::FakeServer imap(test::FakeServer::Imap, ok);
        test::FakeServer smtp(test::FakeServer::Smtp, true);
        ui::AppPasswordDialog d(&store);
        d.tester()->setPlaintextForTests(true);
        fill(d, QStringLiteral("abcd efgh ijkl mnop")); // made up
        auto *servers = d.findChild<QGroupBox *>(QStringLiteral("serversBox"));
        servers->setChecked(true);
        d.findChild<QLineEdit *>(QStringLiteral("imapHost"))->setText(QStringLiteral("127.0.0.1"));
        d.findChild<QSpinBox *>(QStringLiteral("imapPort"))->setValue(imap.port());
        d.findChild<QLineEdit *>(QStringLiteral("smtpHost"))->setText(QStringLiteral("127.0.0.1"));
        d.findChild<QSpinBox *>(QStringLiteral("smtpPort"))->setValue(smtp.port());
        if (!waitTest(d)) {
            return 1;
        }
        // Show the defaults the user would actually have (the test hit localhost).
        const bool saveOn = d.findChild<QPushButton *>(QStringLiteral("saveButton"))->isEnabled();
        servers->blockSignals(true);
        for (auto *e : d.findChildren<QLineEdit *>()) {
            e->blockSignals(true);
        }
        for (auto *s : d.findChildren<QSpinBox *>()) {
            s->blockSignals(true);
        }
        servers->setChecked(false);
        d.findChild<QLineEdit *>(QStringLiteral("imapHost"))->setText(QStringLiteral("imap.gmail.com"));
        d.findChild<QSpinBox *>(QStringLiteral("imapPort"))->setValue(993);
        d.findChild<QLineEdit *>(QStringLiteral("smtpHost"))->setText(QStringLiteral("smtp.gmail.com"));
        d.findChild<QSpinBox *>(QStringLiteral("smtpPort"))->setValue(465);
        d.findChild<QPushButton *>(QStringLiteral("saveButton"))->setEnabled(saveOn);
        QString shown;
        for (const QString &line : d.statusText().split(QLatin1Char('\n'))) {
            shown += (shown.isEmpty() ? QString() : QStringLiteral("\n"))
                + QString(line).replace(QStringLiteral("127.0.0.1"),
                                        line.startsWith(QLatin1String("IMAP")) ? QStringLiteral("imap.gmail.com")
                                                                               : QStringLiteral("smtp.gmail.com"));
        }
        if (auto *l = d.findChild<QLabel *>(QStringLiteral("testStatus"))) {
            l->setText(shown);
        }
        shoot(&d, out + (ok ? QStringLiteral("/apppassword-ok.png") : QStringLiteral("/apppassword-error.png")));
    }
    {
        SessionOptions o;
        o.client.status = ClientConfig::LoadStatus::Missing;
        o.store = &store;
        o.rememberAccount = false;
        MailSession session(o);
        ui::ConnectDialog c(&session);
        shoot(&c, out + QStringLiteral("/connect-with-app-password-link.png"));
    }
    return 0;
}
