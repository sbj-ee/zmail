#include "AboutDialog.hpp"
#include "MainWindow.hpp"
#include "ui/MessageListModel.h"
#include "ui/MessageView.h"
#include "ui/ListDialog.h"
#include "core/Rules.h"
#include "ui/RulesDialog.h"
#include "ui/StripesDialog.h"
#include "ui/Theme.h"
#include "version.hpp"

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QHash>
#include <QDialogButtonBox>
#include <QFile>
#include <QListWidget>
#include <QPointer>
#include <QStackedWidget>
#include <QDialog>
#include <QHeaderView>
#include <QSet>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
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
        QCOMPARE(w.windowTitle(), QStringLiteral("zmail 0.6.15"));
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
        QCOMPARE(titles, (QStringList{"File", "Edit", "View", "Mailbox", "Message", "Transfer", "Settings", "Help"}));
    }

    void messageListTextSizeAndSpacingAreRemembered()
    {
        QSettings().remove(QStringLiteral("ui/listFontSize"));
        QSettings().remove(QStringLiteral("ui/listRowSpacing"));
        {
            MainWindow w;
            w.resize(1000, 700);
            w.show();
            auto *list = w.findChild<QTreeView *>(QStringLiteral("messageList"));
            QVERIFY(list && list->model()->rowCount() > 0);
            const auto rowHeight = [list]() { return list->visualRect(list->model()->index(0, 0)).height(); };
            QVERIFY(w.findChild<QAction *>(QStringLiteral("actionMessageList")));

            // Defaults: the application font, a little air between rows.
            QCOMPARE(w.listFontSize(), kListFontDefault);
            QCOMPARE(w.listRowSpacing(), kListSpacingDefault);
            const int appPt = QFontInfo(QApplication::font()).pointSize();
            QCOMPARE(QFontInfo(list->font()).pointSize(), appPt);
            const int comfortable = rowHeight();

            // The dialog previews live; Cancel puts everything back.
            ListDialog *dlg = w.showListDialog();
            QCOMPARE(dlg->fontSize(), kListFontDefault);
            dlg->findChild<QPushButton *>(QStringLiteral("listPresetCompact"))->click();
            QCOMPARE(rowHeight(), comfortable - kListSpacingDefault);
            dlg->findChild<QPushButton *>(QStringLiteral("listPresetRoomy"))->click();
            QCOMPARE(rowHeight(), comfortable - kListSpacingDefault + 12);
            dlg->fontSpin()->setValue(appPt + 6);
            QCOMPARE(QFontInfo(list->font()).pointSize(), appPt + 6);
            QVERIFY(rowHeight() > comfortable - kListSpacingDefault + 12); // taller text, taller rows
            dlg->reject();
            QCOMPARE(w.listFontSize(), kListFontDefault);
            QCOMPARE(QFontInfo(list->font()).pointSize(), appPt);
            QCOMPARE(rowHeight(), comfortable);

            // OK keeps them. Unread rows are bold at the list's size, and
            // the column headers follow.
            dlg = w.showListDialog();
            dlg->fontSpin()->setValue(appPt + 4);
            dlg->spacingSpin()->setValue(10);
            dlg->accept();
            QCOMPARE(w.listFontSize(), appPt + 4);
            QCOMPARE(w.listRowSpacing(), 10);
            QCOMPARE(QFontInfo(list->header()->font()).pointSize(), appPt + 4);
            bool sawBold = false;
            for (int r = 0; r < list->model()->rowCount(); ++r) {
                const QVariant v = list->model()->index(r, MessageListModel::Subject).data(Qt::FontRole);
                if (v.isValid()) {
                    const QFont f = v.value<QFont>().resolve(list->font());
                    QVERIFY(f.bold());
                    QCOMPARE(QFontInfo(f).pointSize(), appPt + 4);
                    sawBold = true;
                }
            }
            QVERIFY(sawBold);

            // "Default" hands the size back to the application font.
            dlg = w.showListDialog();
            QCOMPARE(dlg->fontSpin()->value(), appPt + 4);
            dlg->findChild<QPushButton *>(QStringLiteral("listFontDefault"))->click();
            QCOMPARE(dlg->fontSize(), kListFontDefault);
            QCOMPARE(QFontInfo(list->font()).pointSize(), appPt);
            dlg->reject();
            QCOMPARE(w.listFontSize(), appPt + 4);

            // Out-of-range values are clamped.
            w.setListAppearance(500, -3);
            QCOMPARE(w.listFontSize(), kListFontMax);
            QCOMPARE(w.listRowSpacing(), 0);
            w.setListAppearance(appPt + 4, 10);
        }
        const int appPt = QFontInfo(QApplication::font()).pointSize();
        QCOMPARE(QSettings().value(QStringLiteral("ui/listFontSize")).toInt(), appPt + 4);
        MainWindow again;
        QCOMPARE(again.listFontSize(), appPt + 4);
        QCOMPARE(again.listRowSpacing(), 10);
        QSettings().remove(QStringLiteral("ui/listFontSize"));
        QSettings().remove(QStringLiteral("ui/listRowSpacing"));
    }

    // Dates in the list read 01/01/2026: two-digit month and day, four-digit
    // year, whatever the system locale, and the Date column has room for it.
    void listDatesAreTwoDigitMonthDayAndFourDigitYear()
    {
        QCOMPARE(MessageListModel::formatDate(QDateTime(QDate(2026, 1, 1), QTime(9, 5))), QStringLiteral("01/01/2026  9:05 AM"));
        QCOMPARE(MessageListModel::formatDate(QDateTime(QDate(2026, 12, 31), QTime(23, 59))), QStringLiteral("12/31/2026  11:59 PM"));
        MainWindow w;
        w.resize(1200, 700);
        w.show();
        auto *list = w.findChild<QTreeView *>(QStringLiteral("messageList"));
        const QString shown = list->model()->index(0, MessageListModel::Date).data().toString();
        QVERIFY2(QRegularExpression(QStringLiteral("^\\d{2}/\\d{2}/\\d{4}  \\d{1,2}:\\d{2} [AP]M$")).match(shown).hasMatch(),
                 qPrintable(shown));
        QVERIFY(list->header()->sectionSize(MessageListModel::Date) >=
                list->fontMetrics().horizontalAdvance(QStringLiteral("12/31/2026  12:59 PM")) + 8);
    }

    // No key does two things: Qt treats a shortcut shared by two actions in
    // one window as ambiguous and runs neither.
    void noTwoActionsShareAShortcut()
    {
        MainWindow w;
        QHash<QString, QStringList> byKey;
        for (QAction *a : w.findChildren<QAction *>()) {
            if (!a->isVisible() && !a->property("placeholder").toBool()) {
                continue; // hidden twins (Mark Read / Unread swap places)
            }
            for (const QKeySequence &k : a->shortcuts()) {
                if (!k.isEmpty()) {
                    byKey[k.toString()] << (a->objectName().isEmpty() ? a->text() : a->objectName());
                }
            }
        }
        QStringList clashes;
        for (auto it = byKey.constBegin(); it != byKey.constEnd(); ++it) {
            QStringList who = it.value();
            who.removeDuplicates();
            // The toolbar button and the menu item for one command are two actions on purpose.
            QSet<QString> commands;
            for (QString name : who) {
                name.remove(QStringLiteral("menuAction"));
                name.remove(QStringLiteral("action"));
                commands.insert(name.toLower());
            }
            if (commands.size() > 1) {
                clashes << it.key() + QStringLiteral(": ") + who.join(QStringLiteral(" / "));
            }
        }
        QVERIFY2(clashes.isEmpty(), qPrintable(clashes.join(QStringLiteral("; "))));
    }

    // Settings is one window: the settings dialogs are its sections, with one
    // OK and one Cancel for all of them.
    void settingsAreOneWindow()
    {
        QSettings().remove(QStringLiteral("ui/rowStripes"));
        QSettings().remove(QStringLiteral("ui/listRowSpacing"));
        QFile::remove(zmail::Rules::defaultPath());
        MainWindow w;
        w.show();
        QAction *open = w.findChild<QAction *>(QStringLiteral("actionSettings"));
        QVERIFY(open);
        QCOMPARE(open->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_Comma));

        QDialog *win = w.showSettings(QStringLiteral("stripes"));
        auto *sections = win->findChild<QListWidget *>(QStringLiteral("settingsSections"));
        auto *pages = win->findChild<QStackedWidget *>(QStringLiteral("settingsPages"));
        QStringList titles;
        for (int i = 0; i < sections->count(); ++i) {
            titles << sections->item(i)->text();
        }
        QCOMPARE(titles, (QStringList{"Filters", "Signatures", "Stationery", "Message List", "Row Stripes", "Sounds", "Privacy"}));
        QCOMPARE(pages->count(), 7);
        QCOMPARE(sections->currentItem()->text(), QStringLiteral("Row Stripes")); // opened at the section asked for
        auto *stripes = win->findChild<StripesDialog *>();
        QVERIFY(pages->currentWidget()->isAncestorOf(stripes));
        QVERIFY(!stripes->isWindow()); // a page of this window, not a window of its own
        // Each page's own OK / Cancel are gone; the window has the only pair.
        for (int i = 0; i < pages->count(); ++i) {
            QDialog *page = pages->widget(i)->findChild<QDialog *>(QString(), Qt::FindDirectChildrenOnly);
            QVERIFY2(page, qPrintable(titles.at(i)));
            for (QDialogButtonBox *box : page->findChildren<QDialogButtonBox *>(QString(), Qt::FindDirectChildrenOnly)) {
                QVERIFY2(box->isHidden(), qPrintable(titles.at(i)));
            }
        }
        QVERIFY(win->findChild<QDialogButtonBox *>(QStringLiteral("settingsButtons"))->isVisibleTo(win));

        // A change previews at once; Escape inside a page cancels the whole
        // window (not just the page), and everything goes back.
        stripes->slider()->setValue(90);
        QCOMPARE(w.stripeStrength(), 90);
        sections->setCurrentRow(3);
        auto *listPage = win->findChild<ListDialog *>();
        QVERIFY(pages->currentWidget()->isAncestorOf(listPage));
        listPage->spacingSpin()->setValue(20);
        QCOMPARE(w.listRowSpacing(), 20);
        QPointer<QDialog> guard(win);
        QTest::keyClick(listPage, Qt::Key_Escape);
        QTRY_VERIFY(!guard);
        QCOMPARE(w.stripeStrength(), kStripeDefault);
        QCOMPARE(w.listRowSpacing(), kListSpacingDefault);

        // OK keeps what every section was set to, not only the one on show.
        win = w.showSettings();
        QCOMPARE(win->findChild<QListWidget *>(QStringLiteral("settingsSections"))->currentRow(), 0); // Filters first
        zmail::Rule rule;
        rule.name = QStringLiteral("From the settings window");
        rule.conditions.append({QStringLiteral("from"), QStringLiteral("contains"), QStringLiteral("x@example.com")});
        win->findChild<RulesDialog *>()->addRule(rule);
        win->findChild<StripesDialog *>()->slider()->setValue(70);
        win->findChild<ListDialog *>()->spacingSpin()->setValue(12);
        guard = win;
        win->findChild<QDialogButtonBox *>(QStringLiteral("settingsButtons"))->button(QDialogButtonBox::Ok)->click();
        QTRY_VERIFY(!guard);
        QCOMPARE(w.rules().rules.size(), 1);
        QCOMPARE(w.rules().rules.first().name, QStringLiteral("From the settings window"));
        QCOMPARE(w.stripeStrength(), 70);
        QCOMPARE(w.listRowSpacing(), 12);
        QCOMPARE(QSettings().value(QStringLiteral("ui/rowStripes")).toInt(), 70);
        MainWindow again;
        QCOMPARE(again.rules().rules.size(), 1);
        QCOMPARE(again.listRowSpacing(), 12);

        // The menu's old entries open the window at their section.
        for (QWidget *top : QApplication::topLevelWidgets()) {
            QVERIFY2(top->objectName() != QLatin1String("settingsWindow") || !top->isVisible(), "left open");
        }
        w.findChild<QAction *>(QStringLiteral("actionPrivacy"))->trigger();
        QDialog *viaMenu = w.findChild<QDialog *>(QStringLiteral("settingsWindow"));
        QVERIFY(viaMenu);
        QCOMPARE(viaMenu->findChild<QListWidget *>(QStringLiteral("settingsSections"))->currentItem()->text(), QStringLiteral("Privacy"));
        viaMenu->reject();
        QFile::remove(zmail::Rules::defaultPath());
        QSettings().remove(QStringLiteral("ui/rowStripes"));
        QSettings().remove(QStringLiteral("ui/listRowSpacing"));
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
                texts << a->iconText(); // the label under the icon
                QVERIFY2(!a->icon().isNull(), qPrintable(a->text()));
            }
        }
        QCOMPARE(texts, (QStringList{"Check Mail", "New Message", "Reply", "Reply All", "Forward", "Delete",
                                     "Junk", "Attach", "Sound"}));
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
        // Hide Spam (default) omits Junk; labels are still under Gmail Labels.
        QCOMPARE(top, (QStringList{"In", "Sent", "Snoozed", "Archive", "Trash", "Folders"}));
        QVERIFY(tree->topLevelItem(5)->childCount() >= 4);
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
        QCOMPARE(m->headerData(MessageListModel::Size, Qt::Horizontal).toString(), QStringLiteral("Size"));
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
        w.showSpamFolder(); // Contoso phish lives in Junk (hidden from In by default)
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
        QCOMPARE(suspicious, 1);
        w.selectMailbox(QStringLiteral("In"));
        m = w.findChild<QTreeView *>(QStringLiteral("messageList"))->model();
        tinted = 0;
        for (int r = 0; r < m->rowCount(); ++r) {
            if (m->index(r, MessageListModel::Subject).data(Qt::BackgroundRole).isValid()
                && !m->index(r, 0).data(MessageListModel::SuspiciousRole).toBool()) {
                ++tinted;
            }
        }
        QVERIFY(tinted >= 2);
    }

    void statusBarShowsSyncAndCounts()
    {
        MainWindow w;
        auto *sync = w.findChild<QLabel *>(QStringLiteral("syncLabel"));
        auto *counts = w.findChild<QLabel *>(QStringLiteral("countLabel"));
        auto *dot = w.findChild<QLabel *>(QStringLiteral("accountStatus"));
        QVERIFY(sync && counts && dot);
        QVERIFY(sync->text().contains(QStringLiteral("sync")));
        QVERIFY(!sync->text().startsWith(QStringLiteral("\u25cf"))); // circle is its own widget
        QCOMPARE(dot->text(), QStringLiteral("\u25cf"));
        QCOMPARE(dot->toolTip(), QStringLiteral("Disconnected"));
        QCOMPARE(dot->palette().color(QPalette::WindowText), disconnectedForeground(QApplication::palette()));
        // Hide Spam drops the sample Contoso phish from In (was 12 / 4 unread).
        QVERIFY(counts->text().startsWith(QStringLiteral("In: 11 messages, 3 unread")));
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
