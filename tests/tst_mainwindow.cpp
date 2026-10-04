#include "AboutDialog.hpp"
#include "MainWindow.hpp"
#include "ui/MessageListModel.h"
#include "ui/MessageView.h"
#include "ui/StripesDialog.h"
#include "ui/Theme.h"
#include "version.hpp"

#include <QAction>
#include <QApplication>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSplitter>
#include <QTextBrowser>
#include <QToolBar>
#include <QTreeView>
#include <QTreeWidget>
#include <QtTest>

using namespace zmail::ui;

class TstMainWindow : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { applyTheme(ThemeMode::Light); }

    void titleComesFromProjectVersion()
    {
        MainWindow w;
        QCOMPARE(w.windowTitle(), QStringLiteral("zmail ") + QString::fromLatin1(zmail::kVersionString));
        QCOMPARE(w.windowTitle(), QStringLiteral("zmail 0.3.0"));
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

    void rowStripesSliderIsRemembered()
    {
        QSettings().remove(QStringLiteral("ui/rowStripes"));
        {
            MainWindow w;
            auto *list = w.findChild<QTreeView *>(QStringLiteral("messageList"));
            QCOMPARE(w.stripeStrength(), kStripeDefault);
            QVERIFY(list->alternatingRowColors());
            QCOMPARE(list->palette().color(QPalette::AlternateBase), stripeColor(QApplication::palette(), kStripeDefault));
            QVERIFY(w.findChild<QAction *>(QStringLiteral("actionRowStripes")));

            // Slider previews live; Cancel restores.
            StripesDialog *dlg = w.showStripesDialog();
            dlg->slider()->setValue(90);
            QCOMPARE(list->palette().color(QPalette::AlternateBase), stripeColor(QApplication::palette(), 90));
            dlg->reject();
            QCOMPARE(w.stripeStrength(), kStripeDefault);
            QCOMPARE(list->palette().color(QPalette::AlternateBase), stripeColor(QApplication::palette(), kStripeDefault));

            // Presets set the slider; Off turns stripes off.
            dlg = w.showStripesDialog();
            dlg->findChild<QPushButton *>(QStringLiteral("stripePresetOff"))->click();
            QCOMPARE(dlg->slider()->value(), 0);
            QVERIFY(!list->alternatingRowColors());
            dlg->findChild<QPushButton *>(QStringLiteral("stripePresetStrong"))->click();
            QCOMPARE(dlg->slider()->value(), stripePreset(StripeStrength::Strong));
            dlg->slider()->setValue(55);
            dlg->accept();
            QCOMPARE(w.stripeStrength(), 55);

            // Theme switch recomputes the stripe; other roles follow the theme.
            w.setTheme(ThemeMode::Dark);
            QCOMPARE(list->palette().color(QPalette::AlternateBase), stripeColor(darkPalette(), 55));
            QCOMPARE(list->palette().color(QPalette::Base), darkPalette().color(QPalette::Base));
            w.setTheme(ThemeMode::Light);
            QCOMPARE(list->palette().color(QPalette::Base), lightPalette().color(QPalette::Base));
        }
        QCOMPARE(QSettings().value(QStringLiteral("ui/rowStripes")).toInt(), 55);
        MainWindow again;
        QCOMPARE(again.stripeStrength(), 55);
        QSettings().remove(QStringLiteral("ui/rowStripes"));
    }

    void helpMenuHasAboutAndUpdates()
    {
        MainWindow w;
        QVERIFY(w.findChild<QMenu *>(QStringLiteral("menuHelp")));
        QVERIFY(w.findChild<QAction *>(QStringLiteral("actionAbout"))->isEnabled());
        QVERIFY(w.findChild<QAction *>(QStringLiteral("actionCheckForUpdates")));
    }

    void toolbarHasEudoraButtonsAndSearch()
    {
        MainWindow w;
        auto *tb = w.findChild<QToolBar *>(QStringLiteral("mainToolBar"));
        QVERIFY(tb);
        QCOMPARE(tb->toolButtonStyle(), Qt::ToolButtonTextUnderIcon);
        QStringList texts;
        for (QAction *a : tb->actions()) {
            if (!a->isSeparator() && !a->text().isEmpty()) {
                texts << a->text();
                QVERIFY2(!a->icon().isNull(), qPrintable(a->text()));
            }
        }
        QCOMPARE(texts, (QStringList{"Check Mail", "New Message", "Reply", "Reply All", "Forward", "Delete",
                                     "Attach"}));
        QVERIFY(w.findChild<QLineEdit *>(QStringLiteral("searchBox")));
    }

    void mailboxTreeHasEudoraMailboxesAndLabels()
    {
        MainWindow w;
        auto *tree = w.findChild<QTreeWidget *>(QStringLiteral("mailboxTree"));
        QVERIFY(tree);
        QStringList top;
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            top << tree->topLevelItem(i)->text(0);
        }
        QCOMPARE(top, (QStringList{"In", "Out", "Junk / Suspicious", "Trash", "Gmail Labels"}));
        QVERIFY(tree->topLevelItem(4)->childCount() >= 4);
    }

    void listAbovePreview()
    {
        MainWindow w;
        auto *split = w.findChild<QSplitter *>(QStringLiteral("listPreviewSplitter"));
        QVERIFY(split);
        QCOMPARE(split->orientation(), Qt::Vertical);
        QVERIFY(qobject_cast<QTreeView *>(split->widget(0)));
        QVERIFY(qobject_cast<zmail::ui::MessageView *>(split->widget(1)));
        QVERIFY(split->widget(1)->findChild<QTextBrowser *>(QStringLiteral("previewPane")));
    }

    void eudoraColumnsAndSorting()
    {
        MainWindow w;
        auto *list = w.findChild<QTreeView *>(QStringLiteral("messageList"));
        QAbstractItemModel *m = list->model();
        QCOMPARE(m->columnCount(), int(MessageListModel::ColumnCount));
        QCOMPARE(m->headerData(MessageListModel::Who, Qt::Horizontal).toString(), QStringLiteral("Who"));
        QCOMPARE(m->headerData(MessageListModel::Size, Qt::Horizontal).toString(), QStringLiteral("K"));
        QCOMPARE(m->headerData(MessageListModel::Subject, Qt::Horizontal).toString(), QStringLiteral("Subject"));
        QVERIFY(list->isSortingEnabled());

        list->sortByColumn(MessageListModel::Size, Qt::DescendingOrder);
        qint64 prev = std::numeric_limits<qint64>::max();
        for (int r = 0; r < m->rowCount(); ++r) {
            const qint64 v = m->index(r, MessageListModel::Size).data(MessageListModel::SortRole).toLongLong();
            QVERIFY(v <= prev);
            prev = v;
        }
    }

    void rowsAreTintedAndSuspiciousIsFixed()
    {
        MainWindow w;
        auto *m = w.findChild<QTreeView *>(QStringLiteral("messageList"))->model();
        int tinted = 0, suspicious = 0;
        for (int r = 0; r < m->rowCount(); ++r) {
            const QModelIndex i = m->index(r, MessageListModel::Subject);
            if (i.data(MessageListModel::SuspiciousRole).toBool()) {
                ++suspicious;
                QCOMPARE(i.data(Qt::ForegroundRole).value<QColor>(), suspiciousForeground(QApplication::palette()));
            } else if (i.data(Qt::BackgroundRole).isValid()) {
                ++tinted;
            }
        }
        QVERIFY(tinted >= 2);
        QCOMPARE(suspicious, 1);
    }

    void statusBarShowsSyncAndCounts()
    {
        MainWindow w;
        auto *sync = w.findChild<QLabel *>(QStringLiteral("syncLabel"));
        auto *counts = w.findChild<QLabel *>(QStringLiteral("countLabel"));
        QVERIFY(sync && counts);
        QVERIFY(sync->text().contains(QStringLiteral("sync")));
        QVERIFY(counts->text().startsWith(QStringLiteral("In: 12 messages, 4 unread")));
    }

    void darkThemeKeepsRuleColoursReadable()
    {
        MainWindow w;
        w.setTheme(ThemeMode::Dark);
        QVERIFY(isDark(QApplication::palette()));
        auto *m = w.findChild<QTreeView *>(QStringLiteral("messageList"))->model();
        for (int r = 0; r < m->rowCount(); ++r) {
            const QModelIndex i = m->index(r, MessageListModel::Subject);
            const QVariant fg = i.data(Qt::ForegroundRole), bg = i.data(Qt::BackgroundRole);
            if (fg.isValid() && bg.isValid()) {
                QVERIFY(contrastRatio(fg.value<QColor>(), bg.value<QColor>()) >= 4.5);
            }
        }
        w.setTheme(ThemeMode::Light);
        QVERIFY(!isDark(QApplication::palette()));
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
