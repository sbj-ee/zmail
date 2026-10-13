// Offscreen screenshots of the "Gmail (app password)" account dialog, against
// local fake IMAP/SMTP servers and a made-up password (never a real one).
//   zmail_dialogshot <outdir>   writes apppassword-empty.png, -ok.png, -error.png, connect.png
//   zmail_dialogshot <outdir> imap   main window on an app-password (IMAP) account:
//                                    imap-mainwindow.png, imap-idle-newmail.png
//   zmail_dialogshot <outdir> imap-actions   changing mail over IMAP: imap-starred-trashed.png,
//                                    imap-search.png (X-GM-RAW), imap-attachment.png
#include "core/MailSession.h"
#include "core/TokenStore.h"
#include "core/ConnectionTester.h"
#include "mock/FakeMailServer.h"
#include "mock/FakeImapServer.h"
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
    if (mode == QLatin1String("imap") || mode == QLatin1String("imap-actions")) {
        const bool actions = mode == QLatin1String("imap-actions");
        test::FakeImapServer imap;
        imap.seedDemo(24);
        store.values.insert(AppPasswordAccount::appPasswordKey(imap.user), imap.password); // the fake's made-up one
        AppPasswordAccount a;
        a.email = imap.user;
        a.imapHost = QStringLiteral("127.0.0.1");
        a.imapPort = imap.port();
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
