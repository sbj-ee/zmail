// Styled signatures: the signature sanitizer (no scripts, handlers, local
// files or remote resources of any kind), the generated plain-text version,
// migration of plain-only signatures from before, the Settings → Signatures
// editor (toolbar, links, preview) and what compose puts in the sent MIME
// in HTML, Plain and Markdown modes. Offline; nothing is sent.
#include "core/MailCache.h"
#include "core/MimeBuilder.h"
#include "core/ReplyBuilder.h"
#include "core/RichText.h"
#include "core/Signatures.h"
#include "ui/ComposeWindow.h"
#include "ui/MessageView.h"
#include "ui/SafeHtmlView.h"
#include "ui/SignaturesDialog.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QTextDocumentFragment>
#include <QTimer>
#include <QToolBar>
#include <QFontComboBox>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextEdit>
#include <QtTest>

using namespace zmail;

namespace {
const QString kStyled = QStringLiteral(
    "<p><span style=\"font-weight:700; font-size:13pt\">Demo User</span><br>"
    "<span style=\"color:#1a5fb4; font-style:italic\">Field Operations</span><br>"
    "<a href=\"https://example.com/docs\">Docs</a> | <a href=\"mailto:demo.user@example.com\">demo.user@example.com</a></p>");

// Decoded text of every leaf part of a built message, keyed by content type.
QMultiHash<QString, QString> leafTexts(const QByteArray &raw)
{
    QMultiHash<QString, QString> out;
    const qsizetype end = raw.indexOf("\r\n\r\n");
    const QByteArray head = QByteArray(raw.left(end)).replace("\r\n ", " ").replace("\r\n\t", " ");
    const QByteArray body = raw.mid(end + 4);
    QString type, cte;
    for (const QByteArray &l : head.split('\n')) {
        const QString line = QString::fromLatin1(l).trimmed();
        if (line.startsWith(QLatin1String("content-type:"), Qt::CaseInsensitive)) type = line.mid(13).trimmed();
        if (line.startsWith(QLatin1String("content-transfer-encoding:"), Qt::CaseInsensitive)) cte = line.mid(26).trimmed().toLower();
    }
    if (type.startsWith(QLatin1String("multipart/"), Qt::CaseInsensitive)) {
        static const QRegularExpression re(QStringLiteral("boundary=\"?([^\";]+)\"?"));
        const QByteArray delim = "--" + re.match(type).captured(1).toLatin1();
        qsizetype pos = body.indexOf(delim);
        while (pos >= 0) {
            const qsizetype start = pos + delim.size();
            if (body.mid(start, 2) == "--") break;
            const qsizetype next = body.indexOf("\r\n" + delim, start);
            if (next < 0) break;
            const auto sub = leafTexts(body.mid(start + 2, next - start - 2));
            for (auto it = sub.cbegin(); it != sub.cend(); ++it) out.insert(it.key(), it.value());
            pos = next + 2;
        }
        return out;
    }
    QByteArray b = body;
    if (cte == QLatin1String("base64")) {
        b = QByteArray::fromBase64(b);
    } else if (cte == QLatin1String("quoted-printable")) {
        b.replace("=\r\n", "");
        b = QByteArray::fromPercentEncoding(b, '=');
    }
    out.insert(type.section(QLatin1Char(';'), 0, 0).trimmed().toLower(), QString::fromUtf8(b).replace(QStringLiteral("\r\n"), QStringLiteral("\n")));
    return out;
}

// Nothing in `html` that could run, load or reach outside the message.
void verifyInert(const QString &html)
{
    const QString h = html.toLower();
    for (const char *bad : {"<script", "<img", "<link", "<style", "<iframe", "<object", "<embed", "<form", "<meta",
                            "<base", "<svg", "javascript:", "vbscript:", "file:", "data:", "url(", "expression(",
                            "@import", "src=", "background=", "srcset", "evil.example", "onclick", "onerror",
                            "onload", "onmouseover", "alert("}) {
        QVERIFY2(!h.contains(QLatin1String(bad)), qPrintable(QStringLiteral("found %1 in\n%2").arg(QLatin1String(bad), html)));
    }
}

void selectText(QTextEdit *e, const QString &what)
{
    QTextCursor c = e->document()->find(what);
    QVERIFY2(!c.isNull(), qPrintable(what));
    e->setTextCursor(c);
}
} // namespace

class TstSignatures : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QSettings().clear();
    }

    // ---- sanitizer ------------------------------------------------------------------------

    void hostileSignatureIsDisarmed_data()
    {
        QTest::addColumn<QString>("html");
        QTest::addColumn<QString>("keep"); // visible text that must survive
        QTest::newRow("script") << QStringLiteral("<p>Demo<script>alert(1)</script> User</p><script src=\"https://evil.example/x.js\"></script>")
                                << QStringLiteral("Demo User");
        QTest::newRow("remote img") << QStringLiteral("<p>Demo User<img src=\"https://evil.example/track.gif?u=1\" width=1 height=1></p>")
                                    << QStringLiteral("Demo User");
        QTest::newRow("protocol-relative img") << QStringLiteral("<p>Demo User<img src=\"//evil.example/t.gif\"></p>")
                                               << QStringLiteral("Demo User");
        QTest::newRow("file img") << QStringLiteral("<p>Demo User<img src=\"file:///etc/passwd\"><img src=\"/home/demo/.ssh/id_rsa\"></p>")
                                  << QStringLiteral("Demo User");
        QTest::newRow("data img") << QStringLiteral("<p>Demo User<img src=\"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGNgYGD4DwABBAEAwS2OUAAAAABJRU5ErkJggg==\"></p>")
                                  << QStringLiteral("Demo User");
        QTest::newRow("on* handlers") << QStringLiteral("<p onclick=\"alert(1)\" onmouseover='alert(2)'><b onload=alert(3)>Demo User</b>"
                                                        "<a href=\"https://example.com\" onerror=\"alert(4)\">site</a></p>")
                                      << QStringLiteral("Demo User");
        QTest::newRow("style url()") << QStringLiteral("<p style=\"background:url(https://evil.example/bg.png); color:#c01c28\">"
                                                       "<span style=\"background-image: url('file:///etc/passwd')\">Demo User</span>"
                                                       "<span style=\"color:red;background:u\\72l(https://evil.example/e)\">!</span></p>")
                                     << QStringLiteral("Demo User");
        QTest::newRow("background attr") << QStringLiteral("<table background=\"https://evil.example/bg.png\"><tr><td background=\"file:///x\">Demo User</td></tr></table>")
                                         << QStringLiteral("Demo User");
        QTest::newRow("style/link/meta") << QStringLiteral("<style>@import url(https://evil.example/a.css); p{background:url(https://evil.example/b)}</style>"
                                                           "<link rel=stylesheet href=\"https://evil.example/c.css\"><meta http-equiv=refresh content=\"0;url=https://evil.example\">"
                                                           "<p>Demo User</p>")
                                         << QStringLiteral("Demo User");
        QTest::newRow("javascript href") << QStringLiteral("<p><a href=\"javascript:alert(1)\">Demo User</a> <a href=\"jav&#x61;script:alert(2)\">x</a>"
                                                           " <a href=\" JaVaScRiPt:alert(3)\">y</a> <a href=\"file:///etc/passwd\">z</a> <a href=\"data:text/html,hi\">w</a></p>")
                                         << QStringLiteral("Demo User");
        QTest::newRow("iframe/object/svg/form") << QStringLiteral("<iframe src=\"https://evil.example\"></iframe><object data=\"https://evil.example/x.swf\"></object>"
                                                                  "<svg><image href=\"https://evil.example/i\"/></svg><form action=\"https://evil.example\"><input name=a></form>"
                                                                  "<p>Demo User</p>")
                                                << QStringLiteral("Demo User");
        QTest::newRow("obfuscated") << QStringLiteral("<p>Demo User<IMG\nSRC=\"https://evil.example/a\"><img/src='https://evil.example/b'>"
                                                      "<sCrIpT>alert(1)</ScRiPt><scr<script>ipt>alert(2)</script></p>")
                                    << QStringLiteral("Demo User");
    }

    void hostileSignatureIsDisarmed()
    {
        QFETCH(QString, html);
        QFETCH(QString, keep);
        const QString clean = sanitizeSignatureHtml(html);
        verifyInert(clean);
        QVERIFY2(QTextDocumentFragment::fromHtml(clean).toPlainText().contains(keep), qPrintable(clean));
        QVERIFY2(!clean.contains(QStringLiteral("<a>")), qPrintable(clean));
        // Idempotent: what's stored sanitizes to itself.
        QCOMPARE(sanitizeSignatureHtml(clean), clean);
        // And through the store.
        QTemporaryDir dir;
        QSettings s(dir.filePath(QStringLiteral("sig.ini")), QSettings::IniFormat);
        SignatureStore store(&s);
        store.setAll({Signature{QStringLiteral("Hostile"), html, {}}});
        verifyInert(s.value(QStringLiteral("signatures/1/html")).toString());
        verifyInert(store.find(QStringLiteral("Hostile")).richHtml());
        verifyInert(store.find(QStringLiteral("Hostile")).plain());
    }

    void formattingAndLinksSurvive()
    {
        const QString clean = sanitizeSignatureHtml(kStyled);
        verifyInert(clean);
        QVERIFY2(clean.contains(QStringLiteral("font-weight:700")), qPrintable(clean));
        QVERIFY2(clean.contains(QStringLiteral("color:#1a5fb4")), qPrintable(clean));
        QVERIFY2(clean.contains(QStringLiteral("font-style:italic")), qPrintable(clean));
        QVERIFY2(clean.contains(QStringLiteral("font-size:13pt")), qPrintable(clean));
        QVERIFY2(clean.contains(QStringLiteral("href=\"https://example.com/docs\"")), qPrintable(clean));
        QVERIFY2(clean.contains(QStringLiteral("href=\"mailto:demo.user@example.com\"")), qPrintable(clean));
        QVERIFY(!clean.contains(QStringLiteral("-qt-")));
        QVERIFY(!clean.contains(QStringLiteral("<body")));
        QVERIFY(!clean.contains(QStringLiteral("<!--")));
        // Underline, font family, links with query strings, plain <b>/<i>/<u>.
        const QString more = sanitizeSignatureHtml(QStringLiteral(
            "<p><b>B</b> <i>I</i> <u>U</u> <span style=\"font-family:'Georgia'\">G</span> "
            "<a href=\"https://example.com/a?x=1&amp;y=2\">q</a></p>"));
        QVERIFY2(more.contains(QStringLiteral("font-weight:700")), qPrintable(more));
        QVERIFY(more.contains(QStringLiteral("font-style:italic")));
        QVERIFY(more.contains(QStringLiteral("text-decoration: underline")) || more.contains(QStringLiteral("text-decoration:underline")));
        QVERIFY(more.contains(QStringLiteral("Georgia")));
        QVERIFY2(more.contains(QStringLiteral("href=\"https://example.com/a?x=1&amp;y=2\"")), qPrintable(more));
        // Nothing visible: nothing stored.
        QCOMPARE(sanitizeSignatureHtml(QStringLiteral("<img src=\"https://evil.example/logo.png\">")), QString());
        QCOMPARE(sanitizeSignatureHtml(QStringLiteral("  ")), QString());
    }

    // ---- plain text -----------------------------------------------------------------------

    void plainTextIsGenerated()
    {
        QCOMPARE(signaturePlainFromHtml(kStyled),
                 QStringLiteral("Demo User\nField Operations\nDocs <https://example.com/docs> | demo.user@example.com"));
        // A link whose text isn't its address keeps both.
        QCOMPARE(signaturePlainFromHtml(QStringLiteral("<p><a href=\"mailto:ops@example.com\">Write to us</a></p>")),
                 QStringLiteral("Write to us <mailto:ops@example.com>"));
        // Paragraphs become lines; entities are decoded.
        QCOMPARE(signaturePlainFromHtml(QStringLiteral("<p>A &amp; B</p><p>&lt;C&gt;</p>")), QStringLiteral("A & B\n<C>"));
        // A dropped link keeps its text, without a target.
        QCOMPARE(signaturePlainFromHtml(QStringLiteral("<p><a href=\"javascript:alert(1)\">Click</a></p>")), QStringLiteral("Click"));

        Signature s{QStringLiteral("Work"), kStyled, {}};
        QCOMPARE(s.plain(), signaturePlainFromHtml(kStyled));
        QCOMPARE(SignatureStore::plainBlock(s), QStringLiteral("-- \n") + signaturePlainFromHtml(kStyled));
        // An explicit plain version still wins (settings from before).
        s.text = QStringLiteral("Demo User");
        QCOMPARE(s.plain(), QStringLiteral("Demo User"));
    }

    // ---- migration ------------------------------------------------------------------------

    void plainOnlySignatureMigrates()
    {
        QTemporaryDir dir;
        const QString ini = dir.filePath(QStringLiteral("old.ini"));
        {
            // Exactly what zmail 0.4.0 wrote for a plain-only signature.
            QSettings old(ini, QSettings::IniFormat);
            old.beginWriteArray(QStringLiteral("signatures"), 1);
            old.setArrayIndex(0);
            old.setValue(QStringLiteral("name"), QStringLiteral("Personal"));
            old.setValue(QStringLiteral("text"), QStringLiteral("Demo <demo@example.com>\nR&D, \"tab 3\""));
            old.endArray();
            old.setValue(QStringLiteral("signatureDefault"), QStringLiteral("Personal"));
        }
        QSettings s(ini, QSettings::IniFormat);
        SignatureStore store(&s);
        const Signature sig = store.find(QStringLiteral("Personal"));
        QVERIFY(sig.html.isEmpty());
        QCOMPARE(sig.plain(), QStringLiteral("Demo <demo@example.com>\nR&D, \"tab 3\""));
        QCOMPARE(sig.richHtml(), QStringLiteral("Demo &lt;demo@example.com&gt;<br>R&amp;D, &quot;tab 3&quot;"));
        QCOMPARE(signatureHtmlFromPlain(QStringLiteral("a\nb")), QStringLiteral("a<br>b"));

        // Plain and Markdown compose: the same text as before.
        for (auto f : {ComposeWindow::Format::Plain, ComposeWindow::Format::Html}) {
            ComposeWindow c;
            c.setConfirmOnClose(false);
            c.setSignatureStore(&store);
            c.setFormat(f);
            QCOMPARE(c.signatureName(), QStringLiteral("Personal"));
            c.findChild<QTextEdit *>(QStringLiteral("composeBody"))->textCursor().insertText(QStringLiteral("Hello"));
            const OutgoingMessage m = c.message();
            QVERIFY2(m.text.contains(QStringLiteral("Hello")), qPrintable(m.text));
            QVERIFY2(m.text.contains(QStringLiteral("\n-- \nDemo <demo@example.com>\nR&D, \"tab 3\"")), qPrintable(m.text));
            if (f == ComposeWindow::Format::Html) {
                QVERIFY2(m.html.contains(QStringLiteral("Demo &lt;demo@example.com&gt;")), qPrintable(m.html));
                QVERIFY(m.html.contains(QStringLiteral("R&amp;D")));
            } else {
                QVERIFY(m.html.isEmpty());
            }
        }

        // Opening and OK-ing the editor without touching it changes nothing.
        {
            SignaturesDialog d(&store);
            QCOMPARE(d.editor()->toPlainText(), QStringLiteral("Demo <demo@example.com>\nR&D, \"tab 3\""));
            QCOMPARE(d.plainPreview(), QStringLiteral("-- \nDemo <demo@example.com>\nR&D, \"tab 3\""));
            d.accept();
        }
        const Signature after = store.find(QStringLiteral("Personal"));
        QCOMPARE(after.text, sig.text);
        QCOMPARE(after.html, QString());
        QCOMPARE(store.defaultName(), QStringLiteral("Personal"));
    }

    // ---- editor -----------------------------------------------------------------------------

    void editorToolbarAndPreview()
    {
        QTemporaryDir dir;
        QSettings s(dir.filePath(QStringLiteral("sig.ini")), QSettings::IniFormat);
        SignatureStore store(&s);
        store.setAll({Signature{QStringLiteral("Work"), {}, QStringLiteral("Demo User\nField Operations\nDocs")}});
        SignaturesDialog d(&store);
        d.show();
        QTextEdit *e = d.editor();
        QVERIFY(d.findChild<QToolBar *>(QStringLiteral("signatureToolBar")));
        auto *bold = d.findChild<QAction *>(QStringLiteral("signatureBold"));
        auto *italic = d.findChild<QAction *>(QStringLiteral("signatureItalic"));
        auto *underline = d.findChild<QAction *>(QStringLiteral("signatureUnderline"));
        auto *font = d.findChild<QFontComboBox *>(QStringLiteral("signatureFont"));
        auto *size = d.findChild<QComboBox *>(QStringLiteral("signatureSize"));
        QVERIFY(bold && italic && underline && font && size && d.findChild<QAction *>(QStringLiteral("signatureColor")) &&
                d.findChild<QAction *>(QStringLiteral("signatureLink")));

        selectText(e, QStringLiteral("Demo User"));
        bold->trigger();
        QVERIFY(bold->isChecked());
        size->setCurrentText(QStringLiteral("14"));
        emit size->textActivated(QStringLiteral("14"));
        emit font->textActivated(QStringLiteral("Serif"));
        selectText(e, QStringLiteral("Field Operations"));
        italic->trigger();
        underline->trigger();
        d.setTextColor(QColor(0x1a, 0x5f, 0xb4));
        selectText(e, QStringLiteral("Docs"));
        QVERIFY(d.insertLink(QString(), QStringLiteral("https://example.com/docs")));

        // The toolbar follows the cursor.
        selectText(e, QStringLiteral("Demo"));
        QVERIFY(bold->isChecked());
        QVERIFY(!italic->isChecked());
        selectText(e, QStringLiteral("Field"));
        QVERIFY(italic->isChecked() && underline->isChecked() && !bold->isChecked());

        // Live preview: the sanitized HTML after a grey "-- ".
        const QString html = d.preview()->toHtml();
        QVERIFY2(html.contains(QStringLiteral("Demo User")) && html.contains(QStringLiteral("font-weight:700")), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("#1a5fb4")), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("href=\"https://example.com/docs\"")), qPrintable(html));
        QVERIFY(d.preview()->toPlainText().startsWith(QStringLiteral("-- ")));
        QCOMPARE(d.plainPreview(), QStringLiteral("-- \nDemo User\nField Operations\nDocs <https://example.com/docs>"));

        d.accept();
        const Signature saved = store.find(QStringLiteral("Work"));
        QVERIFY(saved.text.isEmpty()); // generated from the HTML from now on
        verifyInert(saved.html);
        QVERIFY2(saved.html.contains(QStringLiteral("font-weight:700")), qPrintable(saved.html));
        QVERIFY(saved.html.contains(QStringLiteral("font-size:14pt")));
        QVERIFY(saved.html.contains(QStringLiteral("Serif")));
        QVERIFY(saved.html.contains(QStringLiteral("font-style:italic")));
        QVERIFY(saved.html.contains(QStringLiteral("underline")));
        QVERIFY(saved.html.contains(QStringLiteral("color:#1a5fb4")));
        QVERIFY(saved.html.contains(QStringLiteral("href=\"https://example.com/docs\"")));
        QVERIFY(!saved.html.contains(QStringLiteral("-qt-")));
        QCOMPARE(saved.plain(), QStringLiteral("Demo User\nField Operations\nDocs <https://example.com/docs>"));
    }

    void editorLinks()
    {
        QTemporaryDir dir;
        QSettings s(dir.filePath(QStringLiteral("sig.ini")), QSettings::IniFormat);
        SignatureStore store(&s);
        store.setAll({Signature{QStringLiteral("Work"), {}, QStringLiteral("Demo User")}});
        SignaturesDialog d(&store);
        QTextEdit *e = d.editor();

        // Only http(s) and mailto.
        selectText(e, QStringLiteral("User"));
        for (const char *bad : {"javascript:alert(1)", "file:///etc/passwd", "data:text/html,x", "ftp://example.com",
                                "vbscript:x", "https://", "mailto:", "not a url"}) {
            QVERIFY2(!d.insertLink(QString(), QString::fromLatin1(bad)), bad);
        }
        QVERIFY(!e->toHtml().contains(QStringLiteral("href")));

        // Insert with new text at the end.
        QTextCursor c = e->textCursor();
        c.movePosition(QTextCursor::End);
        e->setTextCursor(c);
        e->insertPlainText(QStringLiteral("\n"));
        QVERIFY(d.insertLink(QStringLiteral("Write to us"), QStringLiteral("mailto:ops@example.com")));
        e->insertPlainText(QStringLiteral(" today")); // not part of the link
        QVERIFY(e->toPlainText().endsWith(QStringLiteral("Write to us today")));
        QCOMPARE(d.signatures().size(), 1);
        QCOMPARE(d.plainPreview(), QStringLiteral("-- \nDemo User\nWrite to us <mailto:ops@example.com> today"));

        // Edit: cursor inside the link, no selection -> the whole link changes.
        c = e->document()->find(QStringLiteral("Write"));
        c.clearSelection();
        c.movePosition(QTextCursor::Left, QTextCursor::MoveAnchor, 2);
        e->setTextCursor(c);
        QCOMPARE(e->textCursor().charFormat().anchorHref(), QStringLiteral("mailto:ops@example.com"));
        QVERIFY(d.insertLink(QStringLiteral("Our site"), QStringLiteral("https://example.com/")));
        QCOMPARE(d.plainPreview(), QStringLiteral("-- \nDemo User\nOur site <https://example.com/> today"));

        // Remove: empty address keeps the text.
        c = e->document()->find(QStringLiteral("site"));
        c.clearSelection();
        e->setTextCursor(c);
        QVERIFY(d.insertLink(QString(), QString()));
        QVERIFY2(!e->toHtml().contains(QStringLiteral("href")), qPrintable(e->toHtml()));
        QCOMPARE(d.plainPreview(), QStringLiteral("-- \nDemo User\nOur site today"));
        QVERIFY(!e->toHtml().contains(QStringLiteral("text-decoration: underline")));
        QVERIFY(!sanitizeSignatureHtml(e->toHtml()).contains(QStringLiteral("color:")));

        // The Link button's dialog: prefilled, OK disabled for bad schemes.
        selectText(e, QStringLiteral("Demo"));
        QTimer::singleShot(0, &d, [&d] {
            auto *dlg = d.findChild<QDialog *>(QStringLiteral("signatureLinkDialog"));
            QVERIFY(dlg);
            auto *text = dlg->findChild<QLineEdit *>(QStringLiteral("linkText"));
            auto *url = dlg->findChild<QLineEdit *>(QStringLiteral("linkUrl"));
            QCOMPARE(text->text(), QStringLiteral("Demo"));
            auto *ok = dlg->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
            url->setText(QStringLiteral("javascript:alert(1)"));
            QVERIFY(!ok->isEnabled());
            url->setText(QStringLiteral("https://example.com/demo"));
            QVERIFY(ok->isEnabled());
            ok->click();
        });
        d.editLink();
        QVERIFY2(d.plainPreview().contains(QStringLiteral("Demo <https://example.com/demo> User")), qPrintable(d.plainPreview()));
    }

    void pastedHtmlIsSanitized()
    {
        QTemporaryDir dir;
        QSettings s(dir.filePath(QStringLiteral("sig.ini")), QSettings::IniFormat);
        SignatureStore store(&s);
        store.setAll({Signature{QStringLiteral("Work"), {}, QStringLiteral("x")}});
        SignaturesDialog d(&store);
        QTextEdit *e = d.editor();
        e->selectAll();
        auto *mime = new QMimeData;
        mime->setHtml(QStringLiteral("<p style=\"background:url(https://evil.example/p)\"><b>Pasted</b>"
                                     "<img src=\"https://evil.example/logo.png\"><a href=\"javascript:alert(1)\">bad</a>"
                                     "<a href=\"https://example.com\">ok</a><script>alert(1)</script></p>"));
        mime->setText(QStringLiteral("Pasted bad ok"));
        QApplication::clipboard()->setMimeData(mime);
        e->paste();
        verifyInert(e->toHtml().section(QStringLiteral("<body"), 1));
        QVERIFY2(e->toPlainText().contains(QStringLiteral("Pasted")), qPrintable(e->toPlainText()));
        d.accept();
        const Signature saved = store.find(QStringLiteral("Work"));
        verifyInert(saved.html);
        QVERIFY(saved.html.contains(QStringLiteral("href=\"https://example.com\"")));
        QVERIFY(saved.html.contains(QStringLiteral("font-weight:700")));
    }

    // ---- compose / MIME -------------------------------------------------------------------

    void sentMimeHasBothParts_data()
    {
        QTest::addColumn<int>("format");
        QTest::addColumn<QString>("kind");
        for (int f : {int(ComposeWindow::Format::Html), int(ComposeWindow::Format::Plain), int(ComposeWindow::Format::Markdown)}) {
            for (const char *k : {"new", "reply", "forward"}) {
                QTest::addRow("%s/%s", f == 0 ? "html" : f == 1 ? "plain" : "markdown", k) << f << QString::fromLatin1(k);
            }
        }
    }

    void sentMimeHasBothParts()
    {
        QFETCH(int, format);
        QFETCH(QString, kind);
        const auto f = ComposeWindow::Format(format);
        QTemporaryDir dir;
        QSettings s(dir.filePath(QStringLiteral("sig.ini")), QSettings::IniFormat);
        SignatureStore store(&s);
        // Stored hostile on purpose: what goes out is still clean.
        store.setAll({Signature{QStringLiteral("Work"),
                                kStyled + QStringLiteral("<img src=\"https://evil.example/logo.png\"><script>alert(1)</script>"), {}}});
        store.setDefaultName(QStringLiteral("Work"));

        ComposeWindow c;
        c.setConfirmOnClose(false);
        c.setFormat(f);
        c.setSignatureStore(&store);
        if (kind == QLatin1String("new")) {
            c.findChild<QLineEdit *>(QStringLiteral("fieldTo"))->setText(QStringLiteral("priya.raman@example.com"));
        } else {
            CachedMessage orig;
            orig.id = QStringLiteral("m1");
            orig.threadId = QStringLiteral("t1");
            orig.fromName = QStringLiteral("Priya Raman");
            orig.fromAddr = QStringLiteral("priya.raman@example.com");
            orig.to = QStringLiteral("Demo User <demo.user@example.com>");
            orig.messageIdHeader = QStringLiteral("<m1@example.com>");
            orig.subject = QStringLiteral("Crew schedule");
            orig.internalDateMs = QDateTime::currentMSecsSinceEpoch();
            orig.bodyText = QStringLiteral("Can you look over tabs 2 and 3?");
            orig.bodyHtml = QStringLiteral("<p>Can you look over <b>tabs 2 and 3</b>?</p>");
            orig.hasBody = true;
            ComposeDraft d = ReplyBuilder::make(kind == QLatin1String("reply") ? ReplyBuilder::Kind::Reply : ReplyBuilder::Kind::Forward,
                                                orig, QStringLiteral("demo.user@example.com"));
            if (d.to.isEmpty()) d.to = QStringLiteral("priya.raman@example.com");
            c.setDraft(d);
        }
        c.findChild<QTextEdit *>(QStringLiteral("composeBody"))->textCursor().insertText(QStringLiteral("Body text"));
        OutgoingMessage m = c.message();
        m.from = QStringLiteral("Demo User <demo.user@example.com>");
        if (m.to.isEmpty()) m.to = QStringLiteral("priya.raman@example.com");
        const QByteArray raw = MimeBuilder::build(m);
        const auto parts = leafTexts(raw);

        const QString plain = parts.value(QStringLiteral("text/plain"));
        QVERIFY2(plain.contains(QStringLiteral("\n-- \nDemo User\nField Operations\n")), qPrintable(plain));
        QVERIFY2(plain.contains(QStringLiteral("Docs <https://example.com/docs>")), qPrintable(plain));
        QVERIFY(plain.indexOf(QStringLiteral("Body text")) < plain.indexOf(QStringLiteral("Field Operations")));
        QVERIFY(!plain.contains(QStringLiteral("evil.example")) && !plain.contains(QStringLiteral("alert(")));
        if (kind != QLatin1String("new")) {
            QVERIFY2(plain.contains(QStringLiteral("tabs 2 and 3")), qPrintable(plain));
            QVERIFY(plain.indexOf(QStringLiteral("Field Operations")) < plain.indexOf(QStringLiteral("tabs 2 and 3")));
        }
        if (f == ComposeWindow::Format::Plain) {
            QVERIFY(!parts.contains(QStringLiteral("text/html")));
            return;
        }
        QVERIFY2(raw.contains("multipart/alternative"), raw.left(600).constData());
        const QString html = parts.value(QStringLiteral("text/html"));
        QVERIFY2(html.contains(QStringLiteral("Field Operations")), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("href=\"https://example.com/docs\"")), qPrintable(html));
        verifyInert(html.section(QStringLiteral("Body text"), 1).section(QStringLiteral("tabs 2"), 0, 0));
        QVERIFY(!html.contains(QStringLiteral("evil.example")) && !html.contains(QStringLiteral("<script")));
        if (f == ComposeWindow::Format::Html) {
            // The styled version.
            QVERIFY2(html.contains(QStringLiteral("font-weight:700")) || html.contains(QStringLiteral("font-weight:600")), qPrintable(html));
            QVERIFY2(html.contains(QStringLiteral("#1a5fb4")), qPrintable(html));
            QVERIFY(html.contains(QStringLiteral("href=\"mailto:demo.user@example.com\"")));
        }
    }

    // The viewer on a plain-only copy: "text <url>" links the url (not
    // "url&gt"), and CRLF line ends are single line breaks.
    void viewerShowsPlainSignatureLinks()
    {
        zmail::ui::MessageView view;
        view.resize(700, 500);
        zmail::ui::ViewMessage v;
        v.id = QStringLiteral("p1");
        v.from = QStringLiteral("Demo User <demo.user@example.com>");
        v.subject = QStringLiteral("Plain");
        v.bodyText = QStringLiteral("Hi\r\n-- \r\nDocs <https://example.com/docs> | "
                                    "q <https://example.com/a?x=1&y=2>, \"https://example.com/quoted\"\r\n");
        view.setMessage(v);
        view.show();
        QCoreApplication::processEvents();
        const auto views = view.findChildren<zmail::ui::SafeHtmlView *>();
        QVERIFY(!views.isEmpty());
        QString html, text;
        for (auto *w : views) {
            if (w->toPlainText().contains(QStringLiteral("Docs"))) {
                html = w->toHtml();
                text = w->toPlainText();
            }
        }
        QVERIFY2(html.contains(QStringLiteral("href=\"https://example.com/docs\"")), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("href=\"https://example.com/a?x=1&amp;y=2\"")), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("href=\"https://example.com/quoted\"")), qPrintable(html));
        QVERIFY2(!html.contains(QStringLiteral("&amp;gt")) && !html.contains(QStringLiteral("docs&gt")), qPrintable(html));
        QVERIFY2(text.contains(QStringLiteral("Hi\n-- \nDocs <https://example.com/docs>")), qPrintable(text));
    }
};

QTEST_MAIN(TstSignatures)
#include "tst_signatures.moc"
