// First-run dialog + MainWindow with a live (mock) Gmail session.
#include "LogCapture.h"
#include "MainWindow.hpp"
#include "core/ClientConfig.h"
#include "core/MailCache.h"
#include "core/MailSession.h"
#include "core/AuthManager.h"
#include "core/SyncEngine.h"
#include "core/TokenStore.h"
#include "mock/MockGoogle.h"
#include "ui/ConnectDialog.h"
#include "ui/NewMailSound.h"
#include "ui/SafeHtmlView.h"
#include "ui/Theme.h"

#include <QColor>
#include <QFile>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QImage>
#include <QTest>
#include <QTextBrowser>
#include <QToolBar>
#include <QTreeView>
#include <QTreeWidget>

#include <sys/stat.h>

using namespace zmail;
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
} // namespace

class TstConnect : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void firstRunDialogExplainsSetup()
    {
        MemoryTokenStore store;
        SessionOptions o;
        o.client.status = ClientConfig::LoadStatus::Missing;
        o.store = &store;
        o.rememberAccount = false;
        MailSession session(o);
        QCOMPARE(session.state(), MailSession::State::NeedsClient);
        ui::ConnectDialog dlg(&session);
        QCOMPARE(dlg.page(), ui::ConnectDialog::SetupPage);
        const QString steps = ui::ConnectDialog::setupStepsHtml();
        for (const char *must : {"Google Auth Platform", "External", "In production", "Desktop app", "Download JSON",
                                 "~/Downloads", "Gmail API", "~/.config/zmail/oauth-client.json", "0600"}) {
            QVERIFY2(steps.contains(QLatin1String(must)), must);
        }
        auto *choose = dlg.findChild<QPushButton *>(QStringLiteral("chooseClientButton"));
        QVERIFY(choose);
        QVERIFY(choose->text().startsWith(QStringLiteral("Choose client file")));
    }

    void chooseClientFileInstallsWith0600()
    {
        QTemporaryDir dir;
        const QString downloaded = dir.filePath(QStringLiteral("client_secret_test-client-42.json"));
        QFile f(downloaded);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"installed\":{\"client_id\":\"test-client-42.apps.example.test\",\"client_secret\":\"test-secret-xyz\",\"auth_uri\":\"https://accounts.google.com/o/oauth2/auth\",\"token_uri\":\"https://oauth2.googleapis.com/token\"}}");
        f.close();
        ::chmod(QFile::encodeName(downloaded).constData(), 0644);

        MemoryTokenStore store;
        SessionOptions o;
        o.client.status = ClientConfig::LoadStatus::Missing;
        o.store = &store;
        o.rememberAccount = false;
        MailSession session(o);
        session.setClientPath(dir.filePath(QStringLiteral("config/zmail/oauth-client.json")));
        ui::ConnectDialog dlg(&session);
        QVERIFY(dlg.installClientFile(downloaded));
        QCOMPARE(session.state(), MailSession::State::SignedOut);
        QCOMPARE(dlg.page(), ui::ConnectDialog::SignInPage);
        struct stat st{};
        QCOMPARE(::stat(QFile::encodeName(session.clientPath()).constData(), &st), 0);
        QCOMPARE(int(st.st_mode & 0777), 0600);
        QVERIFY(dlg.findChild<QPushButton *>(QStringLiteral("signInButton"))->isEnabled());
        QVERIFY(dlg.findChild<QLabel *>(QStringLiteral("clientLabel"))->text().contains(QStringLiteral("apps.example.test")));
        QVERIFY(!dlg.findChild<QLabel *>(QStringLiteral("clientLabel"))->text().contains(QStringLiteral("test-secret")));

        // Looser than 0600: warned, and sign-in is blocked until fixed.
        ::chmod(QFile::encodeName(session.clientPath()).constData(), 0644);
        session.reloadClient();
        QVERIFY(session.client().permissionsTooOpen);
        QVERIFY(!dlg.findChild<QPushButton *>(QStringLiteral("signInButton"))->isEnabled());
        dlg.findChild<QPushButton *>(QStringLiteral("fixPermsButton"))->click();
        QVERIFY(!session.client().permissionsTooOpen);
        QVERIFY(dlg.findChild<QPushButton *>(QStringLiteral("signInButton"))->isEnabled());

        // A "Web application" client is refused with a clear message.
        QFile web(dir.filePath(QStringLiteral("web.json")));
        QVERIFY(web.open(QIODevice::WriteOnly));
        web.write("{\"web\":{\"client_id\":\"x\",\"client_secret\":\"y\"}}");
        web.close();
        QVERIFY(!dlg.installClientFile(web.fileName()));
    }

    void signInThroughDialogShowsRealMail()
    {
        LogCapture log;
        MockGoogle g;
        QVERIFY(g.listen());
        g.seedDemo(10);
        MemoryTokenStore store;
        MailSession session(mockOptions(g, &store));
        MainWindow w;
        w.setSession(&session);
        w.show();
        QVERIFY(!w.isLive());
        QPointer<ui::ConnectDialog> dlg = w.showConnectDialog();
        QVERIFY(dlg);
        QCOMPARE(dlg->page(), ui::ConnectDialog::SignInPage);
        QTest::mouseClick(dlg->findChild<QPushButton *>(QStringLiteral("signInButton")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(session.state(), MailSession::State::SignedIn, 10000);
        QVERIFY(w.isLive());
        QTRY_VERIFY_WITH_TIMEOUT(session.cache()->count(QStringLiteral("INBOX")) > 10 && !session.sync()->isBusy(), 20000);
        QTRY_VERIFY(!dlg || !dlg->isVisible());

        auto *list = w.findChild<QTreeView *>(QStringLiteral("messageList"));
        QTRY_COMPARE(list->model()->rowCount(), session.cache()->count(QStringLiteral("INBOX")));
        auto *tree = w.findChild<QTreeWidget *>(QStringLiteral("mailboxTree"));
        QStringList names;
        for (QTreeWidgetItemIterator it(tree); *it; ++it) {
            names << (*it)->text(0);
        }
        QVERIFY(names.contains(QStringLiteral("Receipts")));
        QVERIFY(names.contains(QStringLiteral("Projects")));  // nested "Projects/zmail"
        QVERIFY(names.contains(QStringLiteral("zmail")));
        QVERIFY(w.findChild<QLabel *>(QStringLiteral("syncLabel"))->text().contains(g.email));

        // Open the newest (unread) message: full fetch + messages.modify -UNREAD.
        list->setCurrentIndex(list->model()->index(0, 0));
        auto *preview = w.findChild<ui::SafeHtmlView *>(QStringLiteral("previewPane"));
        QTRY_VERIFY_WITH_TIMEOUT(!preview->toPlainText().contains(QStringLiteral("Loading message")), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!g.modifyCalls.isEmpty(), 10000);
        QVERIFY(g.modifyCalls.first().endsWith(QStringLiteral(":-UNREAD")));

        // New mail arrives -> chime.
        MockGoogle::Message m;
        m.from = QStringLiteral("Grace Hopper <grace@example.org>");
        m.subject = QStringLiteral("Brand new");
        m.text = QStringLiteral("hi");
        m.labels = {QStringLiteral("INBOX"), QStringLiteral("UNREAD")};
        g.addMessage(m);
        w.newMailSound()->setEnabled(true);
        session.sync()->pollNow(true);
        QTRY_COMPARE_WITH_TIMEOUT(w.newMailSound()->playCount(), 1, 10000);

        // Revoked at Google -> back to a sign-in prompt, sample data.
        g.revokeRefreshToken();
        session.auth()->invalidateAccessToken();
        session.sync()->pollNow(true);
        QTRY_COMPARE_WITH_TIMEOUT(session.state(), MailSession::State::SignedOut, 10000);
        QVERIFY(!w.isLive());
        auto *again = w.findChild<ui::ConnectDialog *>();
        QVERIFY(again && again->isVisible());
        QVERIFY(again->findChild<QLabel *>(QStringLiteral("notice"))->text().contains(QStringLiteral("sign in again")));

        for (const char *secret : {MockGoogle::kClientSecret, MockGoogle::kRefreshToken, MockGoogle::kAccessPrefix}) {
            QVERIFY2(!log.all().contains(QLatin1String(secret)), secret);
        }
    }

    // The cache can't be opened (unwritable folder, full disk): the session
    // must say so, not run on with a database that silently drops every write.
    void unopenableCacheIsReportedNotIgnored()
    {
        MockGoogle g;
        QVERIFY(g.listen());
        g.seedSystemLabels();
        MemoryTokenStore store;
        QTemporaryDir dir;
        QFile blocker(dir.filePath(QStringLiteral("not-a-folder")));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        SessionOptions o = mockOptions(g, &store);
        o.cachePathOverride = blocker.fileName() + QStringLiteral("/zmail.db"); // its "folder" is a file

        MailSession session(o);
        MainWindow w;
        w.setSession(&session);
        QSignalSpy failed(&session, &MailSession::signInFailed);
        QSignalSpy ready(&session, &MailSession::ready);
        session.signIn();
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
        const QString reason = failed.first().first().toString();
        QVERIFY2(reason.contains(QStringLiteral("mail cache")), qPrintable(reason));
        QVERIFY2(reason.contains(o.cachePathOverride), qPrintable(reason));
        QCOMPARE(session.state(), MailSession::State::SignedOut);
        QVERIFY(!session.sync());
        QVERIFY(!session.cache());
        QCOMPARE(ready.count(), 0);
        QVERIFY(!w.isLive());
        // The user sees it: the banner under the toolbar carries the reason.
        auto *banner = w.findChild<QLabel *>(QStringLiteral("signInBannerText"));
        QVERIFY(banner);
        QVERIFY(banner->isVisibleTo(&w));
        QVERIFY2(banner->text().contains(QStringLiteral("mail cache")), qPrintable(banner->text()));
        // The saved sign-in survives for a retry.
        QVERIFY(store.values.contains(QStringLiteral("refresh-token:") + g.email));
    }

    void accountStatusCircleIsGreenWhenConnected()
    {
        MockGoogle g;
        QVERIFY(g.listen());
        g.seedDemo(5);
        MemoryTokenStore store;
        MailSession session(mockOptions(g, &store));
        MainWindow w;
        w.setSession(&session);
        w.show();
        auto *dot = w.findChild<QLabel *>(QStringLiteral("accountStatus"));
        QVERIFY(dot);
        QCOMPARE(dot->palette().color(QPalette::WindowText), ui::disconnectedForeground(QApplication::palette()));
        QCOMPARE(dot->toolTip(), QStringLiteral("Disconnected"));

        session.signIn();
        QTRY_COMPARE_WITH_TIMEOUT(session.state(), MailSession::State::SignedIn, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(session.cache()->count(QStringLiteral("INBOX")) > 0 && !session.sync()->isBusy(), 20000);
        QTRY_COMPARE(dot->toolTip(), QStringLiteral("Connected"));
        QCOMPARE(dot->palette().color(QPalette::WindowText), ui::connectedForeground(QApplication::palette()));
        QVERIFY(w.findChild<QLabel *>(QStringLiteral("syncLabel"))->text().contains(g.email));
        QVERIFY(!w.findChild<QLabel *>(QStringLiteral("syncLabel"))->text().startsWith(QStringLiteral("\u25cf")));
    }

    void accountStatusCircleIsRedOnSyncError()
    {
        MockGoogle g;
        QVERIFY(g.listen());
        g.seedDemo(5);
        MemoryTokenStore store;
        MailSession session(mockOptions(g, &store));
        MainWindow w;
        w.setSession(&session);
        w.show();
        session.signIn();
        QTRY_COMPARE_WITH_TIMEOUT(session.state(), MailSession::State::SignedIn, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!session.sync()->isBusy(), 20000);
        auto *dot = w.findChild<QLabel *>(QStringLiteral("accountStatus"));
        QTRY_COMPARE(dot->toolTip(), QStringLiteral("Connected"));

        g.addFault({QStringLiteral("/gmail/v1/users/me/history"), 500, 100, -1});
        QSignalSpy err(session.sync(), &SyncEngine::syncError);
        session.sync()->pollNow(true);
        QTRY_VERIFY_WITH_TIMEOUT(err.count() >= 1, 10000);
        QTRY_VERIFY(dot->toolTip().startsWith(QStringLiteral("Sync error:")));
        QCOMPARE(dot->palette().color(QPalette::WindowText), ui::suspiciousForeground(QApplication::palette()));
        QVERIFY(w.findChild<QLabel *>(QStringLiteral("syncLabel"))->text().contains(QStringLiteral("Checking for new mail failed")));
    }

    void signInTimeoutShowsBannerWithRetry()
    {
        // The browser never comes back. The main window says so itself (a
        // banner with Retry); nothing relies on raising a dialog, which
        // Wayland won't do over the browser.
        LogCapture log;
        MockGoogle g;
        QVERIFY(g.listen());
        g.seedDemo(3);
        MemoryTokenStore store;
        SessionOptions o = mockOptions(g, &store);
        int opened = 0;
        o.browserOpener = [&opened](const QUrl &) { ++opened; return true; }; // user wandered off
        MailSession session(o);
        session.auth()->setSignInTimeoutMs(300);
        MainWindow w;
        w.setSession(&session);
        w.show();
        QVERIFY(!w.findChild<QToolBar *>(QStringLiteral("signInBanner"))); // created on first use
        session.signIn();
        QCOMPARE(session.state(), MailSession::State::SigningIn);
        QTRY_COMPARE_WITH_TIMEOUT(session.state(), MailSession::State::SignedOut, 5000);
        auto *banner = w.findChild<QToolBar *>(QStringLiteral("signInBanner"));
        QVERIFY(banner);
        QTRY_VERIFY(banner->isVisible());
        const QString text = banner->findChild<QLabel *>(QStringLiteral("signInBannerText"))->text();
        QVERIFY2(text.contains(QLatin1String("timed out")), qPrintable(text));
        QCOMPARE(w.toolBarArea(banner), Qt::TopToolBarArea);
        QVERIFY(!banner->isMovable());
        QVERIFY(!banner->toggleViewAction()->isVisible());
        QVERIFY(log.all().contains(QLatin1String("stopped listening on port")));

        // Dismiss hides it; a second failure brings it back.
        banner->findChild<QPushButton *>(QStringLiteral("signInDismiss"))->click();
        QVERIFY(!banner->isVisible());
        session.signIn();
        QTRY_VERIFY_WITH_TIMEOUT(banner->isVisible(), 5000);
        QCOMPARE(opened, 2);

        // Retry: a fresh sign-in (this time the browser comes back).
        session.auth()->setBrowserOpener([](const QUrl &u) {
            QNetworkReply *r = browserNam()->get(QNetworkRequest(u));
            QObject::connect(r, &QNetworkReply::finished, r, &QObject::deleteLater);
            return true;
        });
        session.auth()->setSignInTimeoutMs(0); // default again
        banner->findChild<QPushButton *>(QStringLiteral("signInRetry"))->click();
        QVERIFY(!banner->isVisible());
        QTRY_COMPARE_WITH_TIMEOUT(session.state(), MailSession::State::SignedIn, 10000);
        QVERIFY(!banner->isVisible());
        QTRY_VERIFY(w.isLive());

        // Revoked at Google: the banner too, not only the dialog.
        QTRY_VERIFY_WITH_TIMEOUT(!session.sync()->isBusy(), 15000);
        g.revokeRefreshToken();
        session.auth()->invalidateAccessToken();
        session.sync()->pollNow(true);
        QTRY_COMPARE_WITH_TIMEOUT(session.state(), MailSession::State::SignedOut, 10000);
        QTRY_VERIFY(banner->isVisible());
        if (auto *dlg = w.findChild<ui::ConnectDialog *>()) {
            dlg->close();
        }
    }

    void sanitizerStripsActiveContent()
    {
        int blocked = 0;
        const QString out = ui::SafeHtmlView::sanitize(
            QStringLiteral("<html><head><style>p{}</style><script>alert(1)</script></head><body onload='x()'>"
                           "<p onclick=\"steal()\">Hi</p><a href='javascript:alert(1)'>x</a>"
                           "<iframe src='https://evil.example'></iframe><img src='https://t.example/p.gif'>"
                           "<img src=\"cid:part1\"></body></html>"),
            &blocked);
        QVERIFY(!out.contains(QStringLiteral("script"), Qt::CaseInsensitive));
        QVERIFY(!out.contains(QStringLiteral("onclick")));
        QVERIFY(!out.contains(QStringLiteral("javascript:")));
        QVERIFY(!out.contains(QStringLiteral("iframe")));
        QVERIFY(out.contains(QStringLiteral("<p>Hi</p>")));
        QCOMPARE(blocked, 2);

        ui::SafeHtmlView v;
        // Blocked resources come back as a 1x1 transparent image, never as a
        // null QVariant (which would make QTextDocument read the file itself).
        for (const char *u : {"https://t.example/p.gif", "file:///etc/passwd"}) {
            const QVariant r = v.loadResource(QTextDocument::ImageResource, QUrl(QString::fromLatin1(u)));
            QVERIFY2(r.canConvert<QImage>(), u);
            QCOMPARE(r.value<QImage>().size(), QSize(1, 1));
        }
        QVERIFY(!v.openLinks());
    }

    void newMailSoundIsBundled()
    {
        QVERIFY(QFile::exists(QStringLiteral(":/sounds/new-mail.wav")));
    }
};

QTEST_MAIN(TstConnect)
#include "tst_connect.moc"
