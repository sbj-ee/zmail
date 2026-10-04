#include "AboutDialog.hpp"
#include "MainWindow.hpp"
#include "version.hpp"

#include <QAction>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QSplitter>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QtTest>

class TstMainWindow : public QObject
{
    Q_OBJECT

private slots:
    void titleComesFromProjectVersion()
    {
        MainWindow w;
        QCOMPARE(w.windowTitle(), QStringLiteral("zmail ") + QString::fromLatin1(zmail::kVersionString));
        QCOMPARE(w.windowTitle(), QStringLiteral("zmail 0.1.0"));
    }

    void menuBarIsInWindowNotGlobal()
    {
        MainWindow w;
        QVERIFY(!w.menuBar()->isNativeMenuBar());
    }

    void menusInOrder()
    {
        MainWindow w;
        QStringList titles;
        for (QAction *a : w.menuBar()->actions()) {
            titles << a->text().remove(QLatin1Char('&'));
        }
        QCOMPARE(titles, (QStringList{"File", "Edit", "View", "Message", "Settings", "Help"}));
    }

    void helpMenuHasAboutAndUpdates()
    {
        MainWindow w;
        auto *help = w.findChild<QMenu *>(QStringLiteral("menuHelp"));
        QVERIFY(help);
        QVERIFY(w.findChild<QAction *>(QStringLiteral("actionAbout")));
        QVERIFY(w.findChild<QAction *>(QStringLiteral("actionCheckForUpdates")));
        QVERIFY(w.findChild<QAction *>(QStringLiteral("actionAbout"))->isEnabled());
    }

    void threePaneLayout()
    {
        MainWindow w;
        auto *split = w.findChild<QSplitter *>(QStringLiteral("mainSplitter"));
        QVERIFY(split);
        QCOMPARE(split->count(), 3);
        QVERIFY(qobject_cast<QTreeWidget *>(split->widget(0)));
        QVERIFY(qobject_cast<QListWidget *>(split->widget(1)));
        QVERIFY(qobject_cast<QTextBrowser *>(split->widget(2)));
    }

    void placeholderRowsAreColourCoded()
    {
        MainWindow w;
        auto *list = w.findChild<QListWidget *>(QStringLiteral("messageList"));
        QVERIFY(list);
        int rule = 0;
        int suspicious = 0;
        for (int i = 0; i < list->count(); ++i) {
            const QListWidgetItem *it = list->item(i);
            const QString kind = it->data(Qt::UserRole).toString();
            if (kind == QLatin1String("rule")) {
                ++rule;
            } else if (kind == QLatin1String("suspicious")) {
                ++suspicious;
                QCOMPARE(it->foreground().color(), MainWindow::suspiciousForeground());
            }
        }
        QVERIFY(rule >= 2);
        QCOMPARE(suspicious, 1);
    }

    void aboutShowsVersionAndLicense()
    {
        const QString t = AboutDialog::aboutText();
        QVERIFY(t.contains(QString::fromLatin1(zmail::kVersionString)));
        QVERIFY(t.contains(QStringLiteral("MIT")));
    }
};

QTEST_MAIN(TstMainWindow)
#include "tst_mainwindow.moc"
