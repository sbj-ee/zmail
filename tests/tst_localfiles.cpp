// Mail HTML must never make zmail read local files: not in the viewer
// (SafeHtmlView), not in a reply/forward quote (ReplyBuilder + the compose
// editor). QTextDocument reads a resource from disk itself when
// loadResource() returns nothing, so <img src="/dev/zero"> (bad_alloc) or
// <img src="/home/me/.ssh/id_rsa"> used to reach the disk.
#include "core/ReplyBuilder.h"
#include "ui/ComposeWindow.h"
#include "ui/SafeHtmlView.h"

#include <QBuffer>
#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QTextEdit>
#include <QtTest>

using namespace zmail;

namespace {
// The secret files are 7x5 PNGs with a marker appended: if one is ever read,
// the document holds a 7x5 image or bytes containing the marker.
const QByteArray kMarker = "ZMAIL-SECRET-6f1c0d";

bool leaked(QTextDocument *doc, const QUrl &url)
{
    const QVariant v = doc->resource(QTextDocument::ImageResource, url);
    if (v.userType() == QMetaType::QByteArray) {
        return v.toByteArray().contains(kMarker) || v.toByteArray().size() > 0;
    }
    const QImage img = v.value<QImage>();
    return img.width() > 1 || img.height() > 1;
}
} // namespace

class TstLocalFiles : public QObject
{
    Q_OBJECT
    QTemporaryDir m_dir;
    QString m_secret; // absolute path

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        QImage img(7, 5, QImage::Format_RGB32);
        img.fill(Qt::red);
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        img.save(&buf, "PNG");
        png += kMarker;
        m_secret = m_dir.filePath(QStringLiteral("secret.png"));
        QFile f(m_secret);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(png);
        f.close();
        QVERIFY(QDir::setCurrent(m_dir.path())); // for the relative-path rows
    }

    void viewerNeverReadsLocalFiles_data()
    {
        QTest::addColumn<QString>("src");
        QTest::newRow("absolute path") << m_secret;
        QTest::newRow("relative path") << QStringLiteral("secret.png");
        QTest::newRow("dot-relative path") << QStringLiteral("./secret.png");
        QTest::newRow("file: URL") << QUrl::fromLocalFile(m_secret).toString();
        QTest::newRow("FILE: uppercase") << QStringLiteral("FILE://") + m_secret;
        QTest::newRow("file:///etc/passwd") << QStringLiteral("file:///etc/passwd");
        QTest::newRow("/etc/passwd") << QStringLiteral("/etc/passwd");
        QTest::newRow("/dev/zero") << QStringLiteral("/dev/zero");
        QTest::newRow("file:///dev/zero") << QStringLiteral("file:///dev/zero");
    }
    void viewerNeverReadsLocalFiles()
    {
        QFETCH(QString, src);
        const QString html = QStringLiteral("<p>Hi</p><img src=\"%1\" width=\"7\"><table background=\"%1\"><tr>"
                                            "<td style=\"background-image:url('%1')\">x</td></tr></table>")
                                 .arg(src);
        // 1. Even unsanitized HTML can't make the view read the file (or hang
        //    on /dev/zero): every blocked resource is a 1x1 transparent image.
        QElapsedTimer t;
        t.start();
        ui::SafeHtmlView v;
        v.resize(400, 300);
        v.setHtml(html);
        v.show();
        QCoreApplication::processEvents();
        QVERIFY(!leaked(v.document(), QUrl(src)));
        const QVariant r = v.loadResource(QTextDocument::ImageResource, QUrl(src));
        QCOMPARE(r.value<QImage>().size(), QSize(1, 1));
        QVERIFY(t.elapsed() < 5000);

        // 2. The sanitizer drops the reference.
        const QString clean = ui::SafeHtmlView::sanitize(html);
        QVERIFY2(!clean.contains(src), qPrintable(clean));
        QVERIFY(clean.contains(QStringLiteral("<p>Hi</p>")));
    }

    void dataImagesStillRender()
    {
        QImage img(3, 2, QImage::Format_RGB32);
        img.fill(Qt::blue);
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        img.save(&buf, "PNG");
        const QString url = QStringLiteral("data:image/png;base64,") + QString::fromLatin1(png.toBase64());
        QVERIFY(ui::SafeHtmlView::sanitize(QStringLiteral("<img src=\"%1\">").arg(url)).contains(url));
        ui::SafeHtmlView v;
        QCOMPARE(v.loadResource(QTextDocument::ImageResource, QUrl(url)).value<QImage>().size(), QSize(3, 2));
        // SVG can reference other files: not rendered.
        const QString svg = QStringLiteral("data:image/svg+xml;base64,") +
                            QString::fromLatin1(QByteArray("<svg xmlns='http://www.w3.org/2000/svg'/>").toBase64());
        QVERIFY(!ui::SafeHtmlView::sanitize(QStringLiteral("<img src=\"%1\">").arg(svg)).contains(svg));
        QCOMPARE(v.loadResource(QTextDocument::ImageResource, QUrl(svg)).value<QImage>().size(), QSize(1, 1));
    }

    void sanitizerSeesThroughObfuscation_data()
    {
        QTest::addColumn<QString>("html");
        QTest::newRow("entity slashes") << QStringLiteral("<img src=\"&#47;dev&#47;zero\">");
        QTest::newRow("hex entities") << QStringLiteral("<img src=\"&#x2F;etc&#x2F;passwd\">");
        QTest::newRow("named colon") << QStringLiteral("<img src=\"file&colon;///etc/passwd\">");
        QTest::newRow("tab in scheme") << QStringLiteral("<img src=\"fi\tle:///etc/passwd\">");
        QTest::newRow("quoted >") << QStringLiteral("<img alt=\">\" src=\"/etc/passwd\">");
        QTest::newRow("unquoted") << QStringLiteral("<img src=/etc/passwd>");
        QTest::newRow("second src") << QStringLiteral("<img src=\"https://x.example/a.png\" src=\"/etc/passwd\">");
        QTest::newRow("qrc") << QStringLiteral("<img src=\"qrc:/icons/lucide/inbox.svg\">");
        QTest::newRow("unterminated") << QStringLiteral("<img alt='x src=/etc/passwd");
    }
    void sanitizerSeesThroughObfuscation()
    {
        QFETCH(QString, html);
        const QString clean = ui::SafeHtmlView::sanitize(QStringLiteral("<p>ok</p>") + html, nullptr, true);
        QVERIFY2(!clean.contains(QStringLiteral("<img"), Qt::CaseInsensitive), qPrintable(clean));
    }

    void replyAndForwardNeverReadLocalFiles_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("reply") << int(ReplyBuilder::Kind::Reply);
        QTest::newRow("reply all") << int(ReplyBuilder::Kind::ReplyAll);
        QTest::newRow("forward") << int(ReplyBuilder::Kind::Forward);
    }
    void replyAndForwardNeverReadLocalFiles()
    {
        QFETCH(int, kind);
        const QStringList srcs{m_secret,
                               QStringLiteral("secret.png"),
                               QUrl::fromLocalFile(m_secret).toString(),
                               QStringLiteral("FILE://") + m_secret,
                               QStringLiteral("file:///etc/passwd"),
                               QStringLiteral("/dev/zero")};
        CachedMessage m;
        m.id = QStringLiteral("m1");
        m.fromName = QStringLiteral("Mallory");
        m.fromAddr = QStringLiteral("mallory@evil.example");
        m.to = QStringLiteral("me@example.com");
        m.subject = QStringLiteral("look");
        m.messageIdHeader = QStringLiteral("<m1@evil.example>");
        m.bodyText = QStringLiteral("look");
        m.bodyHtml = QStringLiteral("<p>look</p><script>x()</script>");
        for (const QString &s : srcs) {
            m.bodyHtml += QStringLiteral("<img src=\"%1\">").arg(s);
        }
        const ComposeDraft d = ReplyBuilder::make(ReplyBuilder::Kind(kind), m, QStringLiteral("me@example.com"));

        // 1. The compose editor won't read files, neither for the quote nor
        //    for HTML that bypassed the quote sanitizer (pasted, or an old draft).
        QElapsedTimer t;
        t.start();
        ComposeWindow c;
        c.setFormat(ComposeWindow::Format::Html);
        c.setDraft(d);
        auto *body = c.findChild<QTextEdit *>(QStringLiteral("composeBody"));
        QVERIFY(body);
        QTextCursor cur(body->document());
        cur.movePosition(QTextCursor::End);
        cur.insertHtml(m.bodyHtml); // raw, unsanitized
        c.resize(600, 400);
        c.show();
        QCoreApplication::processEvents();
        for (const QString &s : srcs) {
            QVERIFY2(!leaked(body->document(), QUrl(s)), qPrintable(s));
            QCOMPARE(body->loadResource(QTextDocument::ImageResource, QUrl(s)).value<QImage>().size(), QSize(1, 1));
        }
        QVERIFY(!body->toHtml().contains(QString::fromLatin1(kMarker)));
        QVERIFY(t.elapsed() < 5000);

        // 2. The quote itself is sanitized: no local references, no script.
        for (const QString &s : srcs) {
            QVERIFY2(!d.quotedHtml.contains(s), qPrintable(d.quotedHtml));
        }
        QVERIFY(!d.quotedHtml.contains(QStringLiteral("<script")));
        QVERIFY(d.quotedHtml.contains(QStringLiteral("look")));
    }
};

QTEST_MAIN(TstLocalFiles)
#include "tst_localfiles.moc"
