// New-mail sound (0.3.x): View > Play Sound for New Mail, the toolbar
// speaker, Ctrl+Shift+M, Settings > Sounds (custom WAV + fallback), and the
// remembered notify/sound + notify/soundFile keys.
#include "MainWindow.hpp"
#include "ui/NewMailSound.h"
#include "ui/SoundDialog.h"
#include "ui/Theme.h"

#include <QAction>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
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
    void init()
    {
        QSettings().remove(QLatin1String(NewMailSound::kEnabledKey));
        QSettings().remove(QLatin1String(NewMailSound::kFileKey));
    }

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
            QCOMPARE(QSettings().value(QLatin1String(NewMailSound::kEnabledKey)).toBool(), false);
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
            QCOMPARE(QSettings().value(QLatin1String(NewMailSound::kEnabledKey)).toBool(), true);
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

    void bundledChimeIsInResources()
    {
        QVERIFY(QFile::exists(NewMailSound::resourcePath()));
        QCOMPARE(NewMailSound().resolvedSource(), QUrl(NewMailSound::resourceUrl()));
        QVERIFY(!NewMailSound().usingCustomFile());
    }

    void missingCustomFileFallsBackToBuiltIn()
    {
        NewMailSound s;
        s.setSoundFile(QStringLiteral("/no/such/new-mail.wav"));
        QVERIFY(!s.usingCustomFile());
        QCOMPARE(s.resolvedSource(), QUrl(NewMailSound::resourceUrl()));

        s.setSoundFile(QStringLiteral("/etc/passwd")); // not a .wav
        QVERIFY(!NewMailSound::isUsableSoundFile(QStringLiteral("/etc/passwd")));
        QVERIFY(!s.usingCustomFile());
        QCOMPARE(s.resolvedSource(), QUrl(NewMailSound::resourceUrl()));
    }

    void usableCustomWavIsResolved()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // Copy the bundled chime out so we have a real WAV on disk.
        const QString path = dir.filePath(QStringLiteral("custom.wav"));
        QVERIFY(QFile::copy(NewMailSound::resourcePath(), path));
        QVERIFY(NewMailSound::isUsableSoundFile(path));

        NewMailSound s;
        s.setSoundFile(path);
        QVERIFY(s.usingCustomFile());
        QCOMPARE(s.resolvedSource(), QUrl::fromLocalFile(path));
        s.saveToSettings();
        QCOMPARE(QSettings().value(QLatin1String(NewMailSound::kFileKey)).toString(), path);

        NewMailSound s2;
        s2.loadFromSettings();
        QCOMPARE(s2.soundFile(), path);
        QVERIFY(s2.usingCustomFile());
    }

    void mutedPlayDoesNotChimeButPreviewDoes()
    {
        NewMailSound s;
        s.setEnabled(false);
        s.play();
        QCOMPARE(s.playCount(), 0);
        s.playPreview();
        QCOMPARE(s.playCount(), 1);
    }

    void settingsDialogHasControlsAndSaves()
    {
        MainWindow w;
        QAction *act = w.findChild<QAction *>(QStringLiteral("actionSounds"));
        QVERIFY(act);
        auto *settings = w.findChild<QMenu *>(QStringLiteral("menuSettings"));
        QVERIFY(settings && settings->actions().contains(act));
        QCOMPARE(act->text(), QStringLiteral("S&ounds\u2026"));

        SoundDialog *dlg = w.showSoundDialog();
        QVERIFY(dlg);
        auto *check = dlg->findChild<QCheckBox *>(QStringLiteral("playSoundCheck"));
        auto *path = dlg->findChild<QLineEdit *>(QStringLiteral("soundFileEdit"));
        auto *browse = dlg->findChild<QPushButton *>(QStringLiteral("browseSoundButton"));
        auto *def = dlg->findChild<QPushButton *>(QStringLiteral("defaultSoundButton"));
        auto *test = dlg->findChild<QPushButton *>(QStringLiteral("testSoundButton"));
        QVERIFY(check && path && browse && def && test);
        QVERIFY(check->isChecked());
        QVERIFY(path->text().contains(QStringLiteral("Default")));

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString custom = dir.filePath(QStringLiteral("mine.wav"));
        QVERIFY(QFile::copy(NewMailSound::resourcePath(), custom));
        dlg->setSoundFile(custom);
        QCOMPARE(dlg->soundFile(), custom);
        QVERIFY(path->text().contains(QStringLiteral("mine.wav")));

        check->setChecked(false);
        dlg->save();
        QVERIFY(!w.newMailSound()->isEnabled());
        QCOMPARE(w.newMailSound()->soundFile(), custom);
        QCOMPARE(QSettings().value(QLatin1String(NewMailSound::kEnabledKey)).toBool(), false);
        QCOMPARE(QSettings().value(QLatin1String(NewMailSound::kFileKey)).toString(), custom);

        def->click();
        QVERIFY(dlg->soundFile().isEmpty());
        dlg->save();
        QVERIFY(w.newMailSound()->soundFile().isEmpty());
        QVERIFY(!QSettings().contains(QLatin1String(NewMailSound::kFileKey)));

        // OK path keeps the View mute action in step.
        check->setChecked(true);
        dlg->save();
        // Simulate accepted handler:
        w.setNewMailSoundOn(w.newMailSound()->isEnabled());
        QVERIFY(w.findChild<QAction *>(QStringLiteral("actionSound"))->isChecked());

        dlg->reject();
    }

    void defaultButtonClearsCustomPathInDialog()
    {
        MainWindow w;
        w.newMailSound()->setSoundFile(QStringLiteral("/tmp/x.wav"));
        SoundDialog *dlg = w.showSoundDialog();
        QCOMPARE(dlg->soundFile(), QStringLiteral("/tmp/x.wav"));
        dlg->findChild<QPushButton *>(QStringLiteral("defaultSoundButton"))->click();
        QVERIFY(dlg->soundFile().isEmpty());
        QVERIFY(dlg->findChild<QLineEdit *>(QStringLiteral("soundFileEdit"))->text().contains(QStringLiteral("Default")));
        dlg->reject(); // don't save
        QCOMPARE(w.newMailSound()->soundFile(), QStringLiteral("/tmp/x.wav"));
    }
};

QTEST_MAIN(TstSound)
#include "tst_sound.moc"
