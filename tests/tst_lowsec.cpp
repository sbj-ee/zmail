// Low-severity hardening from the 4333c43 security review: update-check
// URL, header injection through threading headers and forwarded parts'
// types, and mailto: links. (OAuth state handling is in tst_auth; action
// pinning is the check_action_pins test.)
#include "MainWindow.hpp"
#include "UpdateChecker.hpp"
#include "core/Mailto.h"
#include "core/MimeBuilder.h"
#include "core/ReplyBuilder.h"
#include "ui/ComposeWindow.h"
#include "ui/MessageView.h"
#include "ui/MessageWindow.h"
#include "ui/SafeHtmlView.h"

#include <QDesktopServices>
#include <QLineEdit>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTextEdit>
#include <QTreeView>
#include <QtTest>

using namespace zmail;

namespace {
QList<QByteArray> headerLines(const QByteArray &mime)
{
    return mime.left(mime.indexOf("\r\n\r\n")).split('\n');
}
} // namespace

// Catches anything handed to QDesktopServices::openUrl for a scheme.
class UrlCatcher : public QObject
{
    Q_OBJECT
public:
    QList<QUrl> opened;
public slots:
    void open(const QUrl &u) { opened << u; }
};

class TstLowSec : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QCoreApplication::setOrganizationName(QStringLiteral("zmail-tst-lowsec"));
        QStandardPaths::setTestModeEnabled(true);
        QSettings().clear();
    }

    // --- Update checker -------------------------------------------------
    void releasePageOnlyUnderTheRepo_data()
    {
        QTest::addColumn<QString>("htmlUrl");
        QTest::addColumn<QString>("opened");
        const QString releases = QStringLiteral("https://github.com/sbj-ee/zmail/releases");
        auto ok = [](const char *u) { return QString::fromLatin1(u); };
        QTest::newRow("release tag") << ok("https://github.com/sbj-ee/zmail/releases/tag/v0.4.0")
                                     << ok("https://github.com/sbj-ee/zmail/releases/tag/v0.4.0");
        QTest::newRow("other host") << ok("https://evil.example/zmail.deb") << releases;
        QTest::newRow("look-alike host") << ok("https://github.com.evil.example/sbj-ee/zmail/") << releases;
        QTest::newRow("other repo") << ok("https://github.com/sbj-ee/zmail-evil/releases") << releases;
        QTest::newRow("other owner") << ok("https://github.com/attacker/zmail/releases/tag/v9") << releases;
        QTest::newRow("http") << ok("http://github.com/sbj-ee/zmail/releases/tag/v0.4.0") << releases;
        QTest::newRow("userinfo") << ok("https://github.com/sbj-ee/zmail/@evil.example/") << ok("https://github.com/sbj-ee/zmail/@evil.example/");
        QTest::newRow("prefix with userinfo") << ok("https://github.com@evil.example/sbj-ee/zmail/") << releases;
        QTest::newRow("dot-dot") << ok("https://github.com/sbj-ee/zmail/../../attacker/x") << releases;
        QTest::newRow("newline") << ok("https://github.com/sbj-ee/zmail/releases\nfile:///etc") << releases;
        QTest::newRow("backslash") << ok("https://github.com/sbj-ee/zmail/\\\\evil.example") << releases;
        QTest::newRow("file") << ok("file:///etc/passwd") << releases;
        QTest::newRow("javascript") << ok("javascript:alert(1)") << releases;
        QTest::newRow("empty") << QString() << releases;
        QTest::newRow("no trailing slash") << ok("https://github.com/sbj-ee/zmail") << releases;
    }
    void releasePageOnlyUnderTheRepo()
    {
        QFETCH(QString, htmlUrl);
        QFETCH(QString, opened);
        QCOMPARE(UpdateChecker::releasePageUrl(htmlUrl), opened);
    }

    // --- Threading headers and attachment types -------------------------
    void threadingHeadersCantInjectHeaders()
    {
        OutgoingMessage m;
        m.from = QStringLiteral("Demo <demo@example.com>");
        m.to = QStringLiteral("priya@example.com");
        m.subject = QStringLiteral("Re: hi");
        m.text = QStringLiteral("hi");
        m.inReplyTo = QStringLiteral("<a1@evil.example>\r\nBcc: victim@example.com\r\n");
        m.references = {QStringLiteral("<r0@evil.example>"), QStringLiteral("<r1@evil.example>\nX-Injected: 1"),
                        QStringLiteral("\r\n"), QStringLiteral("<r2@evil.example>\x01\x1b\x7f")};
        const QByteArray mime = MimeBuilder::build(m);
        for (const QByteArray &line : headerLines(mime)) {
            QVERIFY2(line.endsWith('\r') || line == headerLines(mime).last(), line.constData()); // CRLF only
            QVERIFY2(!line.startsWith("Bcc:") && !line.startsWith("X-Injected"), line.constData());
            for (char c : line.chopped(line.endsWith('\r') ? 1 : 0)) {
                QVERIFY2((uchar(c) >= 0x20 && uchar(c) != 0x7f) || c == '\t', line.constData());
            }
        }
        QVERIFY(mime.contains("In-Reply-To: <a1@evil.example>Bcc: victim@example.com\r\n"));
        QVERIFY(mime.contains("<r1@evil.example>X-Injected: 1"));
        QVERIFY(mime.contains("<r2@evil.example>"));

        // Through the real reply path: a received Message-ID / References
        // with line breaks.
        CachedMessage o;
        o.id = QStringLiteral("m1");
        o.fromName = QStringLiteral("Mallory");
        o.fromAddr = QStringLiteral("mallory@evil.example");
        o.to = QStringLiteral("demo@example.com");
        o.subject = QStringLiteral("hi");
        o.bodyText = QStringLiteral("hi");
        o.messageIdHeader = QStringLiteral("<m1@evil.example>\r\nBcc: victim@example.com");
        o.references = QStringLiteral("<p0@evil.example>\r\nX-Injected: yes");
        for (auto kind : {ReplyBuilder::Kind::Reply, ReplyBuilder::Kind::ReplyAll, ReplyBuilder::Kind::Forward}) {
            ComposeWindow c;
            c.setConfirmOnClose(false);
            c.setDraft(ReplyBuilder::make(kind, o, QStringLiteral("demo@example.com")));
            const QByteArray out = MimeBuilder::build(c.message());
            for (const QByteArray &line : headerLines(out)) {
                QVERIFY2(!line.startsWith("Bcc:") && !line.startsWith("X-Injected"), line.constData());
            }
        }
    }

    void attachmentTypesAreSafe_data()
    {
        QTest::addColumn<QString>("type");
        QTest::addColumn<QString>("sent");
        QTest::newRow("plain") << QStringLiteral("image/png") << QStringLiteral("image/png");
        QTest::newRow("upper case") << QStringLiteral("IMAGE/PNG") << QStringLiteral("image/png");
        QTest::newRow("vendor") << QStringLiteral("application/vnd.openxmlformats-officedocument.wordprocessingml.document")
                                << QStringLiteral("application/vnd.openxmlformats-officedocument.wordprocessingml.document");
        QTest::newRow("trailing CRLF") << QStringLiteral("image/png\r\n") << QStringLiteral("image/png");
        QTest::newRow("CRLF header") << QStringLiteral("text/html\r\nX-Injected: 1") << QStringLiteral("application/octet-stream");
        QTest::newRow("LF boundary") << QStringLiteral("text/plain\n\n--boundary") << QStringLiteral("text/plain--boundary"); // one harmless token
        QTest::newRow("NUL") << (QStringLiteral("image/p") + QChar(0) + QStringLiteral("ng")) << QStringLiteral("image/png");
        QTest::newRow("parameters") << QStringLiteral("text/plain; charset=x\"; evil") << QStringLiteral("application/octet-stream");
        QTest::newRow("no subtype") << QStringLiteral("image") << QStringLiteral("application/octet-stream");
        QTest::newRow("garbage") << QStringLiteral("\x01\x02") << QStringLiteral("application/octet-stream");
        QTest::newRow("spaces") << QStringLiteral("image / png") << QStringLiteral("application/octet-stream");
    }
    void attachmentTypesAreSafe()
    {
        QFETCH(QString, type);
        QFETCH(QString, sent);
        QCOMPARE(MimeBuilder::safeMimeType(type), sent);
        OutgoingMessage m;
        m.from = QStringLiteral("demo@example.com");
        m.to = QStringLiteral("p@example.com");
        m.subject = QStringLiteral("Fwd: x");
        m.text = QStringLiteral("x");
        m.attachments << OutgoingAttachment{QStringLiteral("a.bin"), type, QByteArray("data")};
        const QByteArray mime = MimeBuilder::build(m);
        QVERIFY2(mime.contains("Content-Type: " + sent.toLatin1() + "; name=\"a.bin\"\r\n"), mime.constData());
        QVERIFY(!mime.contains("X-Injected"));
        QVERIFY(!mime.contains("\n--boundary"));
    }

    // --- mailto: ---------------------------------------------------------
    void mailtoFields_data()
    {
        QTest::addColumn<QString>("url");
        QTest::addColumn<QStringList>("fields"); // to, cc, bcc, subject, body
        auto F = [](const char *to, const char *cc, const char *bcc, const char *subject, const char *body) {
            return QStringList{QString::fromUtf8(to), QString::fromUtf8(cc), QString::fromUtf8(bcc),
                               QString::fromUtf8(subject), QString::fromUtf8(body)};
        };
        QTest::newRow("address") << "mailto:priya@example.com" << F("priya@example.com", "", "", "", "");
        QTest::newRow("all fields")
            << "mailto:a@example.com,b@example.com?cc=c@example.com&bcc=d@example.com&subject=Hello%20there&body=Line%201%0D%0ALine%202"
            << F("a@example.com,b@example.com", "c@example.com", "d@example.com", "Hello there", "Line 1\nLine 2");
        QTest::newRow("to= adds") << "mailto:a@example.com?to=b@example.com" << F("a@example.com, b@example.com", "", "", "", "");
        QTest::newRow("only query") << "mailto:?to=a@example.com&subject=x" << F("a@example.com", "", "", "x", "");
        QTest::newRow("encoded & and =") << "mailto:a@example.com?subject=Q%26A%3D1&body=x%26y"
                                         << F("a@example.com", "", "", "Q&A=1", "x&y");
        QTest::newRow("plus is literal") << "mailto:a+tag@example.com?subject=1+1" << F("a+tag@example.com", "", "", "1+1", "");
        QTest::newRow("upper-case keys") << "MAILTO:a@example.com?SUBJECT=Hi&Body=Yo&CC=c@example.com"
                                         << F("a@example.com", "c@example.com", "", "Hi", "Yo");
        QTest::newRow("attach ignored") << "mailto:a@example.com?attach=/etc/passwd&attachment=file:///home/me/.ssh/id_rsa&subject=s"
                                        << F("a@example.com", "", "", "s", "");
        QTest::newRow("other keys ignored")
            << "mailto:a@example.com?from=boss@example.com&in-reply-to=%3Cx@y%3E&X-Mailer=evil&reply-to=e@evil.example"
            << F("a@example.com", "", "", "", "");
        QTest::newRow("CRLF in headers") << "mailto:a@example.com%0D%0ABcc:%20x@evil.example?subject=Hi%0D%0ABcc:%20y@evil.example&cc=c@example.com%0A"
                                         << F("a@example.comBcc: x@evil.example", "c@example.com", "", "HiBcc: y@evil.example", "");
        QTest::newRow("first subject wins") << "mailto:a@example.com?subject=one&subject=two" << F("a@example.com", "", "", "one", "");
        QTest::newRow("utf-8") << "mailto:a@example.com?subject=Caf%C3%A9" << F("a@example.com", "", "", "Café", "");
        QTest::newRow("fragment dropped") << "mailto:a@example.com?subject=s#frag" << F("a@example.com", "", "", "s", "");
    }
    void mailtoFields()
    {
        QFETCH(QString, url);
        QFETCH(QStringList, fields);
        const MailtoFields f = Mailto::parse(QUrl(url));
        QCOMPARE((QStringList{f.to, f.cc, f.bcc, f.subject, f.body}), fields);
    }

    void mailtoLinkOpensZmailCompose()
    {
        UrlCatcher catcher; // QDesktopServices would hand mailto: to xdg-open
        QDesktopServices::setUrlHandler(QStringLiteral("mailto"), &catcher, "open");
        const QUrl link(QStringLiteral(
            "mailto:priya@example.com?cc=c@example.com&bcc=d@example.com&subject=Crew%20schedule"
            "&body=Hi%20Priya%2C%0A%3Cb%3Enot%20bold%3C%2Fb%3E&attach=%2Fetc%2Fpasswd"));

        MainWindow w;
        w.show();
        const int before = w.composers().size();
        emit w.messageView()->body()->anchorClicked(link); // what a click on the link does
        QCOMPARE(w.composers().size(), before + 1);
        ComposeWindow *c = w.composers().last();
        c->setConfirmOnClose(false);
        QCOMPARE(c->findChild<QLineEdit *>(QStringLiteral("fieldTo"))->text(), QStringLiteral("priya@example.com"));
        QCOMPARE(c->findChild<QLineEdit *>(QStringLiteral("fieldCc"))->text(), QStringLiteral("c@example.com"));
        QCOMPARE(c->findChild<QLineEdit *>(QStringLiteral("fieldBcc"))->text(), QStringLiteral("d@example.com"));
        QCOMPARE(c->findChild<QLineEdit *>(QStringLiteral("fieldSubject"))->text(), QStringLiteral("Crew schedule"));
        const OutgoingMessage m = c->message();
        QVERIFY2(m.text.startsWith(QStringLiteral("Hi Priya,\n<b>not bold</b>")), qPrintable(m.text)); // plain text
        QVERIFY(m.attachments.isEmpty()); // attach= ignored
        QVERIFY(catcher.opened.isEmpty());

        // The same from a message opened in its own window.
        auto *list = w.findChild<QTreeView *>(QStringLiteral("messageList"));
        QVERIFY(list && list->model()->rowCount() > 0);
        ui::MessageWindow *mw = w.openMessageWindow(list->model()->index(0, 0));
        QVERIFY(mw);
        auto *view = mw->findChild<ui::MessageView *>();
        emit view->body()->anchorClicked(QUrl(QStringLiteral("mailto:ops@example.com?subject=From%20window")));
        QCOMPARE(w.composers().size(), before + 2);
        QCOMPARE(w.composers().last()->findChild<QLineEdit *>(QStringLiteral("fieldSubject"))->text(),
                 QStringLiteral("From window"));
        w.composers().last()->setConfirmOnClose(false);
        QVERIFY(catcher.opened.isEmpty());
        QDesktopServices::unsetUrlHandler(QStringLiteral("mailto"));
    }
};

QTEST_MAIN(TstLowSec)
#include "tst_lowsec.moc"
