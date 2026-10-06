// zmail against the in-process mock Google, for offline screenshots.
//   zmail_mockshot setup    first-run dialog (no client file yet)
//   zmail_mockshot signin   sign-in page of the dialog
//   zmail_mockshot synced   signed in, INBOX synced, an HTML message open
//   zmail_mockshot compose  Reply to a synced message: signature, attachments, quote
//   zmail_mockshot offline  sample data, grey account circle (not signed in)
//   zmail_mockshot error    signed in, then a forced sync failure (red circle)
//   zmail_mockshot multiselect          three rows selected (Shift-range)
//   zmail_mockshot multiselect-deleted  same, then Delete on the selection
// Prints "READY" on stdout once the requested state is on screen.
#include "MainWindow.hpp"
#include "core/MailSession.h"
#include "core/SyncEngine.h"
#include "core/MailCache.h"
#include "core/TokenStore.h"
#include "core/Signatures.h"
#include "ui/ComposeWindow.h"
#include "mock/MockGoogle.h"
#include "ui/ConnectDialog.h"
#include "ui/Theme.h"

#include <QAction>
#include <QLabel>
#include <QStatusBar>
#include <QApplication>
#include <QSettings>
#include <QTextEdit>
#include <QDir>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <QTreeView>
#include <QItemSelectionModel>
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
    if (mode == QLatin1String("compose")) {
        SignatureStore sigs;
        sigs.setAll({{QStringLiteral("Work"),
                      QStringLiteral("<p><b>Alex Morgan</b><br><span style='color:#6b7280'>Field Operations "
                                     "\u00b7 (555) 014-2290</span></p>"),
                      {}},
                     {QStringLiteral("Personal"), {}, QStringLiteral("Alex")}});
        sigs.setDefaultName(QStringLiteral("Work"));
        g.displayName = QStringLiteral("Alex Morgan");
    }
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
    } else if (mode == QLatin1String("offline")) {
        w.resize(1100, 720);
        QTimer::singleShot(800, ready);
    } else {
        QObject::connect(&session, &MailSession::ready, &w, [&] {
            QObject::connect(session.sync(), &SyncEngine::idle, &w, [&] {
                static bool once = false;
                if (once || session.cache()->count(QStringLiteral("INBOX")) == 0) {
                    return;
                }
                once = true;
                if (mode == QLatin1String("error")) {
                    g.addFault({QStringLiteral("/gmail/v1/users/me/history"), 500, 100, -1});
                    QObject::connect(session.sync(), &SyncEngine::syncError, &w, [ready, &w](const QString &) {
                        // Wait a tick so the status circle paints red after the paired idle.
                        QTimer::singleShot(400, &w, [ready, &w] {
                            w.statusBar()->clearMessage();
                            auto *dot = w.findChild<QLabel *>(QStringLiteral("accountStatus"));
                            if (dot && dot->toolTip().startsWith(QStringLiteral("Sync error:"))) {
                                ready();
                            } else {
                                QTimer::singleShot(400, ready);
                            }
                        });
                    }, Qt::SingleShotConnection);
                    session.sync()->pollNow(true);
                    return;
                }
                // Open an HTML newsletter with a (blocked) tracking pixel once
                // the list has been rebuilt from the cache.
                const bool compose = mode == QLatin1String("compose");
                const bool multi = mode.startsWith(QLatin1String("multiselect"));
                const bool multiDeleted = mode == QLatin1String("multiselect-deleted");
                QTimer::singleShot(800, &w, [&w, ready, compose, multi, multiDeleted] {
                    auto *list = w.findChild<QTreeView *>(QStringLiteral("messageList"));
                    if (multi) {
                        // Select the first three visible rows (Shift-range style).
                        const int cols = list->model()->columnCount();
                        QItemSelection sel(list->model()->index(0, 0),
                                           list->model()->index(2, cols - 1));
                        list->selectionModel()->select(sel, QItemSelectionModel::ClearAndSelect);
                        list->selectionModel()->setCurrentIndex(
                            list->model()->index(0, 0), QItemSelectionModel::NoUpdate);
                        list->scrollTo(list->model()->index(0, 0));
                        if (multiDeleted) {
                            QTimer::singleShot(400, &w, [&w, ready] {
                                w.findChild<QAction *>(QStringLiteral("actionDelete"))->trigger();
                                QTimer::singleShot(1200, ready);
                            });
                            return;
                        }
                        QTimer::singleShot(1200, ready);
                        return;
                    }
                    const QLatin1String want = compose ? QLatin1String("Q4 budget review") : QLatin1String("Issue 112");
                    for (int r = 0; r < list->model()->rowCount(); ++r) {
                        if (list->model()->index(r, 7).data().toString().startsWith(want)) {
                            list->setCurrentIndex(list->model()->index(r, 0));
                            break;
                        }
                    }
                    if (!compose) {
                        QTimer::singleShot(1200, ready);
                        return;
                    }
                    QTimer::singleShot(800, &w, [&w, ready] {
                        w.findChild<QAction *>(QStringLiteral("actionReply"))->trigger();
                        QTimer::singleShot(1200, &w, [&w, ready] {
                            ComposeWindow *c = w.composers().value(0);
                            if (!c) {
                                return;
                            }
                            c->setConfirmOnClose(false);
                            QTextCursor cur(c->findChild<QTextEdit *>(QStringLiteral("composeBody"))->document());
                            cur.insertHtml(QStringLiteral(
                                "Hi Priya,<br><br>Thanks, this looks good. Two notes:<ul>"
                                "<li><b>Tab 2:</b> the travel line still shows the old per-diem.</li>"
                                "<li><b>Tab 3:</b> can we split contractor hours by quarter?</li></ul>"
                                "I've attached Hannah's <i>site estimate</i> and my markup."));
                            ComposeWindow::Attachment a, b;
                            a.name = QStringLiteral("site-estimate.pdf");
                            a.data = QByteArray(1'184'512, 'x');
                            a.bytes = a.data.size();
                            b.name = QStringLiteral("Q4-budget-markup.xlsx");
                            b.data = QByteArray(421'880, 'y');
                            b.bytes = b.data.size();
                            c->addAttachment(a);
                            c->addAttachment(b);
                            c->resize(900, 700);
                            c->move(120, 60);
                            c->raise();
                            c->activateWindow();
                            QTimer::singleShot(1200, ready);
                        });
                    });
                });
            });
        });
        session.signIn();
    }
    return app.exec();
}
