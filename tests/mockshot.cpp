// zmail against the in-process mock Google, for offline screenshots.
//   zmail_mockshot setup    first-run dialog (no client file yet)
//   zmail_mockshot signin   sign-in page of the dialog
//   zmail_mockshot synced   signed in, INBOX synced, an HTML message open
// Prints "READY" on stdout once the requested state is on screen.
#include "MainWindow.hpp"
#include "core/MailSession.h"
#include "core/SyncEngine.h"
#include "core/MailCache.h"
#include "core/TokenStore.h"
#include "mock/MockGoogle.h"
#include "ui/ConnectDialog.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QDir>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <QTreeView>
#include <cstdio>

using namespace zmail;

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("zmail-mockshot"));
    const QString mode = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("synced");
    ui::applyTheme(ui::ThemeMode::Light);

    test::MockGoogle g;
    g.email = QStringLiteral("alex.morgan@example.com");
    g.listen();
    g.seedDemo(60);
    MemoryTokenStore store;
    QNetworkAccessManager browser;

    SessionOptions o;
    o.client.status = mode == QLatin1String("setup") ? ClientConfig::LoadStatus::Missing : ClientConfig::LoadStatus::Ok;
    o.client.config = g.clientConfig();
    o.client.config.clientId = QString::fromLatin1(test::MockGoogle::kClientId);
    o.store = &store;
    o.apiBase = g.apiBase();
    o.revokeUri = g.revokeUri();
    o.cachePathOverride = QStringLiteral(":memory:");
    o.rememberAccount = false;
    o.initialCount = 500;
    o.browserOpener = [&browser](const QUrl &u) {
        QNetworkReply *r = browser.get(QNetworkRequest(u));
        QObject::connect(r, &QNetworkReply::finished, r, &QObject::deleteLater);
        return true;
    };
    MailSession session(o);
    session.setClientPath(QDir::homePath() + QStringLiteral("/.config/zmail/oauth-client.json"));

    MainWindow w;
    w.setSession(&session);
    w.move(40, 40);
    w.show();

    auto ready = [] {
        std::puts("READY");
        std::fflush(stdout);
    };
    if (mode == QLatin1String("setup") || mode == QLatin1String("signin")) {
        ui::ConnectDialog *dlg = w.showConnectDialog();
        dlg->move(260, 90);
        QTimer::singleShot(800, ready);
    } else {
        QObject::connect(&session, &MailSession::ready, &w, [&] {
            QObject::connect(session.sync(), &SyncEngine::idle, &w, [&] {
                static bool once = false;
                if (once || session.cache()->count(QStringLiteral("INBOX")) == 0) {
                    return;
                }
                once = true;
                // Open an HTML newsletter with a (blocked) tracking pixel once
                // the list has been rebuilt from the cache.
                QTimer::singleShot(800, &w, [&w, ready] {
                    auto *list = w.findChild<QTreeView *>(QStringLiteral("messageList"));
                    for (int r = 0; r < list->model()->rowCount(); ++r) {
                        if (list->model()->index(r, 7).data().toString().startsWith(QLatin1String("Issue 112"))) {
                            list->setCurrentIndex(list->model()->index(r, 0));
                            break;
                        }
                    }
                    QTimer::singleShot(1200, ready);
                });
            });
        });
        session.signIn();
    }
    return app.exec();
}
