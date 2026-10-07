#include "core/Limits.h"
#include "ui/ComposeWindow.h"
#include "ui/Theme.h"

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
        QVERIFY(c.windowTitle().endsWith(QStringLiteral("zmail 0.5.6")));
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
};

QTEST_MAIN(TstCompose)
#include "tst_compose.moc"
