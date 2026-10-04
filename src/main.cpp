#include "MainWindow.hpp"
#include "version.hpp"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QString::fromLatin1(zmail::kAppName));
    QApplication::setApplicationVersion(QString::fromLatin1(zmail::kVersionString));
    QApplication::setOrganizationName(QStringLiteral("sbj-ee"));
    QApplication::setDesktopFileName(QStringLiteral("zmail"));

    MainWindow w;
    w.show();
    return app.exec();
}
