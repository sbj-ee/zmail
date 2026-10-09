#include "ui/StationeryDialog.h"
#include "core/Stationery.h"
#include "core/Limits.h"
#include "ui/ComposeWindow.h"
#include "ui/Theme.h"

#include <QFile>
#include <QLineEdit>
#include <QTemporaryDir>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QListWidget>
#include <QAction>
#include <QComboBox>
#include <QProgressBar>
#include <QTextEdit>
#include <QToolBar>
#include <QtTest>

using zmail::limits::SizeLevel;

class TstCompose : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { zmail::ui::applyTheme(zmail::ui::ThemeMode::Light); }

    void eudoraHeaderBlockOrder()
    {
        ComposeWindow c;
        QCOMPARE(c.headerFieldOrder(), (QStringList{"To", "From", "Subject", "Cc", "Bcc", "Attached"}));
    }

    void formattingToolbar()
    {
        ComposeWindow c;
        QVERIFY(c.findChild<QToolBar *>(QStringLiteral("formatToolBar")));
        for (const char *n : {"actionBold", "actionItalic", "actionUnderline", "actionBullets", "actionNumbers",
                              "actionLink", "actionQuote"}) {
            QVERIFY2(c.findChild<QAction *>(QString::fromLatin1(n)), n);
        }
        auto *fmt = c.findChild<QComboBox *>(QStringLiteral("formatCombo"));
        QVERIFY(fmt);
        QCOMPARE(fmt->count(), 3); // HTML, Plain text, Markdown
    }

    void titleFollowsSubject()
    {
        ComposeWindow c;
        c.loadSampleReply();
        QVERIFY(c.windowTitle().startsWith(QStringLiteral("Re: Q4 budget review")));
        QVERIFY(c.windowTitle().endsWith(QStringLiteral("zmail 0.6.9")));
    }

    void sizeMeterLevelsAndSendBlocking()
    {
        ComposeWindow c;
        c.loadSampleReply();
        auto *send = c.findChild<QAction *>(QStringLiteral("actionSend"));
        QVERIFY(c.findChild<QProgressBar *>(QStringLiteral("sizeMeter")));
        QCOMPARE(c.sizeLevel(), SizeLevel::Ok);
        QVERIFY(send->isEnabled());

        // 15 MB of attachments -> about 20.5 MB encoded: warn, still sendable.
        c.setAttachments({{QStringLiteral("scan.pdf"), 15'000'000}});
        QCOMPARE(c.sizeLevel(), SizeLevel::Warn);
        QVERIFY(send->isEnabled());

        // 18.5 MB looks under 25 MB but is over the limit once base64-encoded.
        c.setAttachments({{QStringLiteral("video.mp4"), 18'500'000}});
        QCOMPARE(c.sizeLevel(), SizeLevel::Blocked);
        QVERIFY(!send->isEnabled());
        QVERIFY(c.encodedSize() > zmail::limits::kSendLimitBytes);
    }

    // Stationery, as in Eudora: messages kept as templates.
    void stationeryStoreKeepsTemplates()
    {
        QTemporaryDir dir;
        zmail::StationeryStore store(dir.filePath(QStringLiteral("sub/stationery.json")));
        QVERIFY(store.all().isEmpty());
        QVERIFY(store.save({QStringLiteral("Thanks"), {}, {}, QStringLiteral("Thank you"), QStringLiteral("Thanks very much!\nSteve")}));
        QVERIFY(store.save({QStringLiteral(" Away "), QStringLiteral("team@example.org"), QStringLiteral("boss@example.com"), {},
                            QStringLiteral("I'm out until Monday.")}));
        QVERIFY(!store.save({QStringLiteral("  "), {}, {}, {}, QStringLiteral("nameless")}));
        QList<zmail::Stationery> all = store.all();
        QCOMPARE(all.size(), 2);
        QCOMPARE(all.first().name, QStringLiteral("Away")); // by name, trimmed
        QCOMPARE(store.find(QStringLiteral("THANKS")).body, QStringLiteral("Thanks very much!\nSteve"));
        QVERIFY(store.find(QStringLiteral("nope")).name.isEmpty());
        // The same name replaces; a restart (another store on the file) sees it.
        QVERIFY(store.save({QStringLiteral("thanks"), {}, {}, QStringLiteral("Thank you"), QStringLiteral("Much obliged.")}));
        zmail::StationeryStore again(dir.filePath(QStringLiteral("sub/stationery.json")));
        QCOMPARE(again.all().size(), 2);
        QCOMPARE(again.find(QStringLiteral("Thanks")).body, QStringLiteral("Much obliged."));
        QVERIFY(again.remove(QStringLiteral("AWAY")));
        QVERIFY(!again.remove(QStringLiteral("Away")));
        QCOMPARE(store.all().size(), 1);
    }

    void stationeryDialogEditsTheList()
    {
        zmail::ui::StationeryDialog d({{QStringLiteral("Thanks"), {}, {}, QStringLiteral("Thank you"), QStringLiteral("Thanks!")}});
        auto *list = d.findChild<QListWidget *>(QStringLiteral("stationeryList"));
        QCOMPARE(list->count(), 1);
        QCOMPARE(d.findChild<QLineEdit *>(QStringLiteral("stationeryName"))->text(), QStringLiteral("Thanks"));
        d.findChild<QPushButton *>(QStringLiteral("stationeryNew"))->click();
        QCOMPARE(d.items().size(), 2);
        QCOMPARE(d.current(), 1);
        d.findChild<QLineEdit *>(QStringLiteral("stationeryName"))->setText(QStringLiteral("Away"));
        d.findChild<QLineEdit *>(QStringLiteral("stationeryTo"))->setText(QStringLiteral("team@example.org"));
        d.findChild<QLineEdit *>(QStringLiteral("stationerySubject"))->setText(QStringLiteral("Out of office"));
        d.findChild<QPlainTextEdit *>(QStringLiteral("stationeryBody"))->setPlainText(QStringLiteral("Back Monday."));
        QCOMPARE(list->item(1)->text(), QStringLiteral("Away"));
        QCOMPARE(d.items().last(), (zmail::Stationery{QStringLiteral("Away"), QStringLiteral("team@example.org"), {},
                                                      QStringLiteral("Out of office"), QStringLiteral("Back Monday.")}));
        QCOMPARE(d.items().first().body, QStringLiteral("Thanks!")); // the other one is untouched
        d.setCurrent(0);
        d.findChild<QPushButton *>(QStringLiteral("stationeryDelete"))->click();
        QCOMPARE(d.items().size(), 1);
        QCOMPARE(d.findChild<QLineEdit *>(QStringLiteral("stationeryName"))->text(), QStringLiteral("Away"));
        d.removeCurrent();
        QVERIFY(!d.findChild<QWidget *>(QStringLiteral("stationeryEditor"))->isEnabled());
    }

    void composeUsesAndSavesStationery()
    {
        QFile::remove(zmail::StationeryStore::defaultPath());
        const zmail::Stationery away{QStringLiteral("Away"), QStringLiteral("team@example.org"), QStringLiteral("boss@example.com"),
                                     QStringLiteral("Out of office"), QStringLiteral("I'm out until Monday.\nBack then.")};
        ComposeWindow c;
        auto *to = c.findChild<QLineEdit *>(QStringLiteral("fieldTo"));
        auto *cc = c.findChild<QLineEdit *>(QStringLiteral("fieldCc"));
        auto *subject = c.findChild<QLineEdit *>(QStringLiteral("fieldSubject"));
        auto *body = c.findChild<QTextEdit *>(QStringLiteral("composeBody"));
        QVERIFY(c.findChild<QWidget *>(QStringLiteral("stationeryButton")));
        c.setStationery(away);
        QCOMPARE(to->text(), QStringLiteral("team@example.org"));
        QCOMPARE(cc->text(), QStringLiteral("boss@example.com"));
        QCOMPARE(subject->text(), QStringLiteral("Out of office"));
        QVERIFY(body->toPlainText().startsWith(QStringLiteral("I'm out until Monday.\nBack then.")));
        QCOMPARE(c.message().text.section(QLatin1Char('\n'), 0, 0), QStringLiteral("I'm out until Monday."));

        // What is already filled in is kept: only the text is added.
        ComposeWindow r;
        r.findChild<QLineEdit *>(QStringLiteral("fieldTo"))->setText(QStringLiteral("dana@example.net"));
        r.findChild<QLineEdit *>(QStringLiteral("fieldSubject"))->setText(QStringLiteral("Re: Lunch"));
        r.setStationery(away);
        QCOMPARE(r.findChild<QLineEdit *>(QStringLiteral("fieldTo"))->text(), QStringLiteral("dana@example.net"));
        QCOMPARE(r.findChild<QLineEdit *>(QStringLiteral("fieldSubject"))->text(), QStringLiteral("Re: Lunch"));
        QVERIFY(r.findChild<QTextEdit *>(QStringLiteral("composeBody"))->toPlainText().startsWith(QStringLiteral("I'm out")));

        // Save This Message as Stationery: what was written, by name.
        ComposeWindow w;
        w.findChild<QLineEdit *>(QStringLiteral("fieldTo"))->setText(QStringLiteral("eli@example.com"));
        w.findChild<QLineEdit *>(QStringLiteral("fieldSubject"))->setText(QStringLiteral("Invoice"));
        w.findChild<QTextEdit *>(QStringLiteral("composeBody"))->setPlainText(QStringLiteral("Please find the invoice attached.\n\n"));
        QVERIFY(w.saveAsStationery(QStringLiteral(" Invoice cover ")));
        const zmail::Stationery saved = zmail::StationeryStore().find(QStringLiteral("invoice cover"));
        QCOMPARE(saved, (zmail::Stationery{QStringLiteral("Invoice cover"), QStringLiteral("eli@example.com"), {},
                                           QStringLiteral("Invoice"), QStringLiteral("Please find the invoice attached.")}));
        QVERIFY(!w.saveAsStationery(QStringLiteral("  ")));
        QFile::remove(zmail::StationeryStore::defaultPath());
    }
};

QTEST_MAIN(TstCompose)
#include "tst_compose.moc"
