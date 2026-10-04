// New-mail sound on/off (0.3.x P1): View > Play Sound for New Mail, the
// toolbar speaker, Ctrl+Shift+M, the tooltip naming state and key, the
// icon swap and the remembered setting.
#include "MainWindow.hpp"
#include "ui/NewMailSound.h"
#include "ui/Theme.h"

#include <QAction>
#include <QFile>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QStandardPaths>
#include <QToolBar>
#include <QToolButton>
#include <QtTest>

using namespace zmail::ui;

class TstSound : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        applyTheme(ThemeMode::Light);
    }
    void init() { QSettings().remove(QStringLiteral("notify/sound")); }

    void onByDefaultWithMenuItemAndToolbarButton()
    {
        MainWindow w;
        QVERIFY(w.newMailSoundOn());
        QVERIFY(w.newMailSound()->isEnabled());
        QAction *a = w.findChild<QAction *>(QStringLiteral("actionSound"));
        QVERIFY(a);
        QVERIFY(a->isCheckable());
        QVERIFY(a->isChecked());
        QCOMPARE(a->shortcut(), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M));
        QCOMPARE(a->property("lucide").toString(), QStringLiteral("volume-2"));
        QVERIFY(!a->icon().isNull());

        // In the View menu...
        auto *view = w.findChild<QMenu *>(QStringLiteral("menuView"));
        QVERIFY(view && view->actions().contains(a));
        QCOMPARE(a->text(), QStringLiteral("Play &Sound for New Mail"));
        QVERIFY(!a->isIconVisibleInMenu()); // check box, not a pressed icon
        // ...and on the toolbar, labelled "Sound".
        auto *tb = w.findChild<QToolBar *>(QStringLiteral("mainToolBar"));
        QVERIFY(tb->actions().contains(a));
        QCOMPARE(a->iconText(), QStringLiteral("Sound"));
        QVERIFY(qobject_cast<QToolButton *>(tb->widgetForAction(a)));

        const QString key = QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M).toString(QKeySequence::NativeText);
        QCOMPARE(a->toolTip(), QStringLiteral("New-mail sound is on. Click to mute (%1)").arg(key));
    }

    void toggleMutesSwapsIconAndPersists()
    {
        {
            MainWindow w;
            QAction *a = w.findChild<QAction *>(QStringLiteral("actionSound"));
            a->trigger(); // as a click on the toolbar button or the menu item
            QVERIFY(!a->isChecked());
            QVERIFY(!w.newMailSoundOn());
            QVERIFY(!w.newMailSound()->isEnabled());
            QCOMPARE(a->property("lucide").toString(), QStringLiteral("volume-x"));
            QVERIFY(a->toolTip().startsWith(QLatin1String("New-mail sound is muted.")));
            QVERIFY(a->toolTip().endsWith(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M).toString(QKeySequence::NativeText) + ')'));
            // Muted: new mail doesn't chime.
            w.newMailSound()->play();
            QCOMPARE(w.newMailSound()->playCount(), 0);
            QCOMPARE(QSettings().value(QStringLiteral("notify/sound")).toBool(), false);
        }
        {
            MainWindow w; // remembered
            QAction *a = w.findChild<QAction *>(QStringLiteral("actionSound"));
            QVERIFY(!a->isChecked());
            QVERIFY(!w.newMailSound()->isEnabled());
            QCOMPARE(a->property("lucide").toString(), QStringLiteral("volume-x"));
            w.setNewMailSoundOn(true); // the API keeps the action in step
            QVERIFY(a->isChecked());
            QCOMPARE(a->property("lucide").toString(), QStringLiteral("volume-2"));
            QCOMPARE(QSettings().value(QStringLiteral("notify/sound")).toBool(), true);
        }
    }

    void ctrlShiftMToggles()
    {
        MainWindow w;
        w.show();
        QVERIFY(QTest::qWaitForWindowActive(&w));
        QAction *a = w.findChild<QAction *>(QStringLiteral("actionSound"));
        QVERIFY(a->isChecked());
        QTest::keyClick(&w, Qt::Key_M, Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(!a->isChecked());
        QVERIFY(!w.newMailSound()->isEnabled());
        QTest::keyClick(&w, Qt::Key_M, Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(a->isChecked());
        QVERIFY(w.newMailSound()->isEnabled());
    }

    void iconsAreBundledWithLicense()
    {
        for (const char *n : {"volume-2", "volume-x"}) {
            QVERIFY2(QFile::exists(QStringLiteral(":/icons/lucide/%1.svg").arg(QLatin1String(n))), n);
        }
    }
};

QTEST_MAIN(TstSound)
#include "tst_sound.moc"
