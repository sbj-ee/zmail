#include "MainWindow.hpp"
#include "core/MailSession.h"
#include "ui/ComposeWindow.h"
#include "ui/Theme.h"
#include "version.hpp"

#include <QApplication>
#include <QCommandLineParser>
#include <QSettings>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QString::fromLatin1(zmail::kAppName));
    QApplication::setApplicationVersion(QString::fromLatin1(zmail::kVersionString));
    QApplication::setOrganizationName(QStringLiteral("sbj-ee"));
    QApplication::setDesktopFileName(QStringLiteral("zmail"));

    QCommandLineParser cli;
    cli.setApplicationDescription(QStringLiteral("zmail: Gmail with Eudora-style sounds"));
    cli.addHelpOption();
    cli.addVersionOption();
    QCommandLineOption theme(QStringLiteral("theme"),
                             QStringLiteral("light, dark, system, boilermakers, badgers, packers or custom:<file stem> "
                                            "(default: the last one picked in View > Theme)."),
                             QStringLiteral("mode"));
    QCommandLineOption compose(QStringLiteral("compose"),
                               QStringLiteral("Open a sample reply in a compose window (preview)."));
    QCommandLineOption offline(QStringLiteral("offline"),
                               QStringLiteral("Don't connect to Gmail; show the built-in sample data."));
    cli.addOption(theme);
    cli.addOption(compose);
    cli.addOption(offline);
    cli.process(app);

    // --theme wins; otherwise View > Theme's last choice (QSettings ui/theme).
    const QString t = cli.isSet(theme) ? cli.value(theme)
                                       : QSettings().value(QLatin1String(zmail::ui::kThemeSettingKey)).toString();
    zmail::ui::applyThemeId(t); // built-in or custom (View > Theme > Theme Editor)

    MainWindow w;
    w.show();
    if (!cli.isSet(offline)) {
        // Reads ~/.config/zmail/oauth-client.json; refresh token from the keyring.
        auto *session = zmail::MailSession::createDefault(&app);
        w.setSession(session);
        session->restoreSaved();
        if (session->state() != zmail::MailSession::State::Restoring &&
            session->state() != zmail::MailSession::State::SignedIn) {
            w.showConnectDialog(); // first run, or signed out last time
        }
    }
    if (cli.isSet(compose)) {
        w.openCompose(true);
    }
    return app.exec();
}
