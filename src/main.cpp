#include "MainWindow.hpp"
#include "ui/ComposeWindow.h"
#include "ui/Theme.h"
#include "version.hpp"

#include <QApplication>
#include <QCommandLineParser>

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
    QCommandLineOption theme(QStringLiteral("theme"), QStringLiteral("light, dark or system."),
                             QStringLiteral("mode"), QStringLiteral("light"));
    QCommandLineOption compose(QStringLiteral("compose"),
                               QStringLiteral("Open a sample reply in a compose window (preview)."));
    cli.addOption(theme);
    cli.addOption(compose);
    cli.process(app);

    const QString t = cli.value(theme).toLower();
    zmail::ui::applyTheme(t == QLatin1String("dark")     ? zmail::ui::ThemeMode::Dark
                          : t == QLatin1String("system") ? zmail::ui::ThemeMode::System
                                                         : zmail::ui::ThemeMode::Light);

    MainWindow w;
    w.show();
    if (cli.isSet(compose)) {
        w.openCompose(true);
    }
    return app.exec();
}
