// Sending (0.3.0) against the mock Gmail API: MIME building (headers,
// encoded-words, base64), Markdown/HTML alternatives, reply/forward
// threading, simple vs resumable uploads (with a resume), the 25 MB block,
// zip-the-attachments, drafts, signatures and spell check. Nothing here
// talks to Google.
#include "LogCapture.h"
#include "MainWindow.hpp"
#include "core/AuthManager.h"
#include "core/GmailClient.h"
#include "core/Limits.h"
#include "core/MailCache.h"
#include "core/MailSession.h"
#include "core/Markdown.h"
#include "core/MessageParser.h"
#include "core/MimeBuilder.h"
#include "core/ReplyBuilder.h"
#include "core/RichText.h"
#include "core/Sender.h"
#include "core/Signatures.h"
#include "core/SpellChecker.h"
#include "core/SyncEngine.h"
#include "core/TokenStore.h"
#include "core/Zip.h"
#include "mock/MockGoogle.h"
#include "ui/ComposeWindow.h"
#include "ui/Theme.h"

#include <QAction>
#include <QComboBox>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QTextEdit>
#include <QTreeView>
#include <QTextCursor>
#include <QtTest>
#include <memory>

using namespace zmail;
using zmail::test::MockGoogle;

namespace {
QNetworkAccessManager *browserNam()
{
    static QNetworkAccessManager nam;
    return &nam;
}

SessionOptions mockOptions(MockGoogle &g, MemoryTokenStore *store)
{
    SessionOptions o;
    o.client.status = ClientConfig::LoadStatus::Ok;
    o.client.config = g.clientConfig();
    o.store = store;
    o.apiBase = g.apiBase();
    o.revokeUri = g.revokeUri();
    o.cachePathOverride = QStringLiteral(":memory:");
    o.rememberAccount = false;
    o.pollIntervalMs = 3600 * 1000;
    o.backoffBaseMs = 5;
    o.browserOpener = [](const QUrl &u) {
        QNetworkReply *r = browserNam()->get(QNetworkRequest(u));
        QObject::connect(r, &QNetworkReply::finished, r, &QObject::deleteLater);
        return true;
    };
    return o;
}

// Mock server + signed-in session (cache, sync, sender).
struct Live
{
    MockGoogle g;
    MemoryTokenStore store;
    std::unique_ptr<MailSession> session;

    Live()
    {
        g.listen();
        g.seedSystemLabels();
    }
    bool start()
    {
        session = std::make_unique<MailSession>(mockOptions(g, &store));
        session->signIn();
        return QTest::qWaitFor([this] { return session->state() == MailSession::State::SignedIn; }, 10000) &&
               QTest::qWaitFor([this] { return session->sync() && !session->sync()->isBusy(); }, 15000) &&
               QTest::qWaitFor([this] { return !session->fromHeader().isEmpty() && session->fromHeader().contains('<'); }, 5000);
    }
    QString seed(const QString &subject, const QString &refs = {}, const QString &replyTo = {}, const QString &cc = {})
    {
        MockGoogle::Message m;
        m.from = QStringLiteral("Priya Raman <priya.raman@example.com>");
        m.to = QStringLiteral("Demo User <demo.user@example.com>, Hannah Lindqvist <h.lindqvist@example.com>");
        m.cc = cc;
        m.replyTo = replyTo;
        m.references = refs;
        m.subject = subject;
        m.text = QStringLiteral("Can you look over tabs 2 and 3?\nThanks!");
        m.labels = {QStringLiteral("INBOX")};
        m.date = QDateTime::currentDateTimeUtc().addSecs(-600);
        return g.addMessage(m, true);
    }
    CachedMessage fetch(const QString &id)
    {
        session->sync()->pollNow(true);
        (void)QTest::qWaitFor([&] { return session->cache()->contains(id) && !session->sync()->isBusy(); }, 15000);
        CachedMessage out;
        bool done = false;
        session->sync()->fetchBody(id, [&](const CachedMessage &c, const QString &) { out = c; done = true; });
        (void)QTest::qWaitFor([&] { return done; }, 10000);
        return out;
    }
};

// Splits a built message into header map + body.
struct Parsed
{
    QMultiHash<QString, QString> headers; // lower-cased name -> unfolded value
    QByteArray body;
    QList<QByteArray> headerLines;        // raw physical lines
};
Parsed parse(const QByteArray &raw)
{
    Parsed p;
    const qsizetype end = raw.indexOf("\r\n\r\n");
    const QByteArray head = raw.left(end);
    p.body = raw.mid(end + 4);
    p.headerLines = head.split('\n');
    QString cur;
    QString val;
    auto flush = [&] {
        if (!cur.isEmpty()) p.headers.insert(cur, val);
    };
    for (QByteArray line : p.headerLines) {
        line.chop(line.endsWith('\r') ? 1 : 0);
        if (line.startsWith(' ') || line.startsWith('\t')) {
            val += QString::fromLatin1(line);
            continue;
        }
        flush();
        const qsizetype c = line.indexOf(':');
        cur = QString::fromLatin1(line.left(c)).toLower();
        val = QString::fromLatin1(line.mid(c + 1)).trimmed();
    }
    flush();
    return p;
}

// Decodes RFC 2047 encoded-words (B and Q).
QString decodeWords(const QString &v)
{
    static const QRegularExpression re(QStringLiteral("=\\?UTF-8\\?([BbQq])\\?([^?]*)\\?=(\\s+(?==\\?))?"));
    QString out;
    qsizetype last = 0;
    for (auto it = re.globalMatch(v); it.hasNext();) {
        const auto m = it.next();
        out += v.mid(last, m.capturedStart() - last);
        const QByteArray b = m.captured(1).toUpper() == QLatin1String("B")
                                 ? QByteArray::fromBase64(m.captured(2).toLatin1())
                                 : QByteArray::fromPercentEncoding(m.captured(2).toLatin1().replace('_', ' '), '=');
        out += QString::fromUtf8(b);
        last = m.capturedEnd();
    }
    return out + v.mid(last);
}

QString boundaryOf(const QString &contentType)
{
    static const QRegularExpression re(QStringLiteral("boundary=\"?([^\";]+)\"?"));
    return re.match(contentType).captured(1);
}

// Returns the parts of a multipart body (each: raw headers + body).
QList<QByteArray> partsOf(const QByteArray &body, const QString &boundary)
{
    const QByteArray delim = "--" + boundary.toLatin1();
    QList<QByteArray> out;
    qsizetype pos = body.indexOf(delim);
    while (pos >= 0) {
        const qsizetype start = pos + delim.size();
        if (body.mid(start, 2) == "--") break;
        const qsizetype next = body.indexOf("\r\n" + delim, start);
        if (next < 0) break;
        out << body.mid(start + 2, next - start - 2);
        pos = next + 2;
    }
    return out;
}

QByteArray noise(qsizetype n, quint32 seed = 7)
{
    QByteArray b(n, Qt::Uninitialized);
    QRandomGenerator rng(seed);
    rng.fillRange(reinterpret_cast<quint32 *>(b.data()), n / 4);
    return b;
}

QByteArray prose(qsizetype n)
{
    const QByteArray line = "The quick brown fox jumps over the lazy dog; tab 2 travel line, tab 3 contractors.\n";
    QByteArray b;
    b.reserve(n + line.size());
    for (int i = 0; b.size() < n; ++i) b += QByteArray::number(i) + ' ' + line;
    b.truncate(n);
    return b;
}

bool noBareLf(const QByteArray &raw)
{
    for (qsizetype i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\n' && (i == 0 || raw[i - 1] != '\r')) return false;
    }
    return true;
}

// Decoded text of every leaf part of a raw message, keyed by content type.
QMultiHash<QString, QString> leafTexts(const QByteArray &raw)
{
    QMultiHash<QString, QString> out;
    const Parsed p = parse(raw);
    const QString type = p.headers.value(QStringLiteral("content-type"));
    if (type.startsWith(QLatin1String("multipart/"), Qt::CaseInsensitive)) {
        for (const QByteArray &part : partsOf(p.body, boundaryOf(type))) {
            const auto sub = leafTexts(part);
            for (auto it = sub.cbegin(); it != sub.cend(); ++it) {
                out.insert(it.key(), it.value());
            }
        }
        return out;
    }
    const QString cte = p.headers.value(QStringLiteral("content-transfer-encoding")).toLower();
    QByteArray body = p.body;
    if (cte == QLatin1String("base64")) {
        body = QByteArray::fromBase64(body);
    } else if (cte == QLatin1String("quoted-printable")) {
        body.replace("=\r\n", "");
        body = QByteArray::fromPercentEncoding(body, '=');
    }
    out.insert(type.section(QLatin1Char(';'), 0, 0).trimmed().toLower(), QString::fromUtf8(body));
    return out;
}
} // namespace

class TstSend : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QSettings().clear();
        qputenv("ZMAIL_USER_DICTIONARY", QFile::encodeName(m_tmp.filePath(QStringLiteral("user-dictionary.txt"))));
        ui::applyTheme(ui::ThemeMode::Light);
    }
    void init() { QSettings().clear(); }

    // ---- MIME ------------------------------------------------------------------

    void mimeHeadersAndBase64()
    {
        OutgoingMessage m;
        m.from = QStringLiteral("Demo User <demo.user@example.com>");
        m.to = QStringLiteral("\"Raman, Priya\" <priya.raman@example.com>, h.lindqvist@example.com");
        m.cc = QStringLiteral("José Núñez <jose@example.es>");
        m.bcc = QStringLiteral("audit@example.com");
        m.subject = QStringLiteral("Q4 budget — numbers attached ✓");
        m.text = QStringLiteral("Hi Priya,\n\nNumbers attached. Café at 3?\n");
        m.html = QStringLiteral("<p>Hi Priya,</p><p>Numbers attached. <b>Café</b> at 3?</p>");
        const QByteArray pdf = noise(10'000);
        m.attachments = {{QStringLiteral("site-estimate.pdf"), {}, pdf}};
        const QByteArray raw = MimeBuilder::build(m);

        QVERIFY(noBareLf(raw));
        const Parsed p = parse(raw);
        for (const QByteArray &l : p.headerLines) QVERIFY2(l.size() <= 79, l.constData()); // 78 + CR
        QCOMPARE(p.headers.value("mime-version"), QStringLiteral("1.0"));
        QVERIFY(p.headers.value("message-id").contains(QRegularExpression(QStringLiteral("^<[^@\\s]+@example\\.com>$"))));
        QVERIFY(QDateTime::fromString(p.headers.value("date"), Qt::RFC2822Date).isValid());
        QCOMPARE(p.headers.value("from"), QStringLiteral("Demo User <demo.user@example.com>"));
        QCOMPARE(p.headers.value("to"),
                 QStringLiteral("\"Raman, Priya\" <priya.raman@example.com>, h.lindqvist@example.com"));
        QVERIFY(p.headers.value("cc").startsWith(QStringLiteral("=?UTF-8?")));
        QCOMPARE(decodeWords(p.headers.value("cc")), QStringLiteral("José Núñez <jose@example.es>"));
        QCOMPARE(p.headers.value("bcc"), QStringLiteral("audit@example.com")); // Gmail strips it on delivery
        QCOMPARE(decodeWords(p.headers.value("subject")), m.subject);
        QVERIFY(p.headers.value("user-agent").startsWith(QStringLiteral("zmail/0.4.2")));

        // multipart/mixed( multipart/alternative(text, html), attachment )
        const QString ct = p.headers.value("content-type");
        QVERIFY(ct.startsWith(QStringLiteral("multipart/mixed")));
        const QList<QByteArray> top = partsOf(p.body, boundaryOf(ct));
        QCOMPARE(top.size(), 2);
        const Parsed alt = parse(top[0]);
        QVERIFY(alt.headers.value("content-type").startsWith(QStringLiteral("multipart/alternative")));
        const QList<QByteArray> altParts = partsOf(alt.body, boundaryOf(alt.headers.value("content-type")));
        QCOMPARE(altParts.size(), 2);
        const Parsed text = parse(altParts[0]);
        const Parsed html = parse(altParts[1]);
        QVERIFY(text.headers.value("content-type").startsWith(QStringLiteral("text/plain; charset=UTF-8")));
        QVERIFY(html.headers.value("content-type").startsWith(QStringLiteral("text/html; charset=UTF-8")));
        // Non-ASCII bodies go base64 with 76-column lines, and decode back exactly.
        QCOMPARE(text.headers.value("content-transfer-encoding"), QStringLiteral("base64"));
        for (const QByteArray &l : text.body.split('\n')) QVERIFY(l.trimmed().size() <= 76);
        QCOMPARE(QString::fromUtf8(QByteArray::fromBase64(text.body)), QString(m.text).replace(QStringLiteral("\n"), QStringLiteral("\r\n")));
        QCOMPARE(QString::fromUtf8(QByteArray::fromBase64(html.body)).remove(QLatin1Char('\r')), m.html);

        const Parsed att = parse(top[1]);
        QVERIFY(att.headers.value("content-type").startsWith(QStringLiteral("application/pdf")));
        QVERIFY(att.headers.value("content-disposition").startsWith(QStringLiteral("attachment;")));
        QVERIFY(att.headers.value("content-disposition").contains(QStringLiteral("filename=\"site-estimate.pdf\"")));
        QCOMPARE(att.headers.value("content-transfer-encoding"), QStringLiteral("base64"));
        const QList<QByteArray> lines = att.body.trimmed().split('\n');
        for (int i = 0; i < lines.size(); ++i) {
            QCOMPARE(lines[i].endsWith('\r') || i == lines.size() - 1, true);
            QVERIFY(lines[i].trimmed().size() <= limits::kBase64LineLength);
        }
        QCOMPARE(QByteArray::fromBase64(att.body), pdf);
        // The size estimate the meter uses matches the encoding.
        QCOMPARE(att.body.trimmed().size(), limits::base64MimeSize(pdf.size()));
    }

    void asciiTextStays7bitAndPlainOnly()
    {
        OutgoingMessage m;
        m.from = QStringLiteral("demo.user@example.com");
        m.to = QStringLiteral("a@example.com");
        m.subject = QStringLiteral("Plain");
        m.text = QStringLiteral("Just text.\nTwo lines.");
        const Parsed p = parse(MimeBuilder::build(m));
        QVERIFY(p.headers.value("content-type").startsWith(QStringLiteral("text/plain; charset=UTF-8")));
        QCOMPARE(p.headers.value("content-transfer-encoding"), QStringLiteral("7bit"));
        QCOMPARE(p.body, QByteArray("Just text.\r\nTwo lines.\r\n"));
        QCOMPARE(p.headers.value("subject"), QStringLiteral("Plain"));
    }

    void longSubjectFoldsAndRoundTrips()
    {
        const QString subj = QStringLiteral("Ünïcödé ") + QString(QStringLiteral("long subject words ")).repeated(10);
        const QByteArray enc = MimeBuilder::foldHeader("Subject", MimeBuilder::encodeHeaderText(subj));
        for (const QByteArray &l : enc.split('\n')) QVERIFY2(l.size() <= 79, l.constData());
        const Parsed p = parse("From: a@b.c\r\n" + enc + "\r\n");
        QCOMPARE(decodeWords(p.headers.value("subject")), subj);
        // ASCII text is left alone; one encoded word never exceeds 75 chars.
        QCOMPARE(MimeBuilder::encodeHeaderText(QStringLiteral("Hello")), QByteArray("Hello"));
        for (const QByteArray &w : MimeBuilder::encodeHeaderText(subj).split(' ')) QVERIFY(w.size() <= 75);
    }

    void nonAsciiFilenameUsesRfc2231()
    {
        OutgoingMessage m;
        m.from = QStringLiteral("demo.user@example.com");
        m.to = QStringLiteral("a@example.com");
        m.text = QStringLiteral("x");
        m.attachments = {{QStringLiteral("Résumé 2026.pdf"), {}, QByteArray("%PDF-1.4 fake")}};
        const QByteArray raw = MimeBuilder::build(m);
        QVERIFY(raw.contains("filename*=UTF-8''R%C3%A9sum%C3%A9%202026.pdf"));
        QVERIFY(raw.contains("name=\"=?UTF-8?B?"));
    }

    void addressListsSplitOnUnquotedCommas()
    {
        QCOMPARE(MimeBuilder::splitAddresses(QStringLiteral("\"Raman, Priya\" <p@x.com>, b@x.com,, <c@x.com>")),
                 (QStringList{QStringLiteral("\"Raman, Priya\" <p@x.com>"), QStringLiteral("b@x.com"), QStringLiteral("<c@x.com>")}));
    }

    // ---- formats -----------------------------------------------------------------

    void markdownRendersAndKeepsATextAlternative()
    {
        const QString src = QStringLiteral("# Plan\n\n**Bold** and ~~gone~~, see https://example.com\n\n"
                                           "| a | b |\n|---|---|\n| 1 | 2 |\n\n<script>alert(1)</script>\n\n> quoted\n");
        const QString html = markdown::toHtml(src);
        QVERIFY(html.contains(QStringLiteral("<h1>Plan</h1>")));
        QVERIFY(html.contains(QStringLiteral("<strong>Bold</strong>")));
        QVERIFY(html.contains(QStringLiteral("<del>gone</del>")));
        QVERIFY(html.contains(QStringLiteral("<table>")));
        QVERIFY(html.contains(QStringLiteral("href=\"https://example.com\"")));
        QVERIFY(html.contains(QStringLiteral("<blockquote>")));
        QVERIFY(!html.contains(QStringLiteral("<script>"))); // raw HTML is off
        QVERIFY(html.contains(QStringLiteral("&lt;script&gt;")));
        QVERIFY(markdown::toEmailHtml(src).contains(QStringLiteral("<blockquote style=\"")));  // inline CSS
        const QString text = markdown::toPlainText(src);
        QVERIFY(text.contains(QStringLiteral("Bold")));
        QVERIFY(text.contains(QStringLiteral("> quoted")));

        // Through the compose window: multipart/alternative with both parts.
        ComposeWindow c;
        c.setConfirmOnClose(false);
        c.findChild<QLineEdit *>(QStringLiteral("fieldTo"))->setText(QStringLiteral("a@example.com"));
        c.setFormat(ComposeWindow::Format::Markdown);
        c.setBodyText(QStringLiteral("Hello **world**\n\n- one\n- two"));
        const OutgoingMessage m = c.message();
        QVERIFY(m.html.contains(QStringLiteral("<strong>world</strong>")));
        QVERIFY(m.html.contains(QStringLiteral("<li>one</li>")));
        QVERIFY(m.text.contains(QStringLiteral("Hello **world**")) || m.text.contains(QStringLiteral("Hello world")));
        const Parsed p = parse(c.buildMime());
        QVERIFY(p.headers.value("content-type").startsWith(QStringLiteral("multipart/alternative")));
    }

    void htmlComposeProducesPlainAlternative()
    {
        QTextDocument d;
        d.setHtml(QStringLiteral("<p>Hi <b>Priya</b>,</p><ul><li>one</li><li>two</li></ul><ol><li>first</li></ol>"
                                 "<p>See <a href='https://example.com/q4'>the sheet</a>.</p>"));
        const QString t = richtext::toPlainText(&d);
        QVERIFY2(t.contains(QStringLiteral("Hi Priya,")), qPrintable(t));
        QVERIFY2(t.contains(QStringLiteral("- one\n- two")), qPrintable(t));
        QVERIFY2(t.contains(QStringLiteral("1. first")), qPrintable(t));
        QVERIFY2(t.contains(QStringLiteral("the sheet <https://example.com/q4>")), qPrintable(t));
        QCOMPARE(richtext::quotePlain(QStringLiteral("a\nb")), QStringLiteral("> a\n> b"));

        // Plain format sends text/plain only.
        ComposeWindow c;
        c.setConfirmOnClose(false);
        c.findChild<QLineEdit *>(QStringLiteral("fieldTo"))->setText(QStringLiteral("a@example.com"));
        c.setFormat(ComposeWindow::Format::Plain);
        c.setBodyText(QStringLiteral("plain only"));
        QVERIFY(c.message().html.isEmpty());
        QVERIFY(parse(c.buildMime()).headers.value("content-type").startsWith(QStringLiteral("text/plain")));
        // HTML format: alternative with both.
        c.setFormat(ComposeWindow::Format::Html);
        QVERIFY(!c.message().html.isEmpty());
        QVERIFY(c.message().text.contains(QStringLiteral("plain only")));
        QVERIFY(parse(c.buildMime()).headers.value("content-type").startsWith(QStringLiteral("multipart/alternative")));
    }

    // ---- threading -----------------------------------------------------------------

    void replyBuilderRules()
    {
        QCOMPARE(ReplyBuilder::replySubject(QStringLiteral("Re: RE: Budget")), QStringLiteral("Re: Budget"));
        QCOMPARE(ReplyBuilder::replySubject(QStringLiteral("Budget")), QStringLiteral("Re: Budget"));
        QCOMPARE(ReplyBuilder::forwardSubject(QStringLiteral("Fwd: FW: Budget")), QStringLiteral("Fwd: Budget"));
        QCOMPARE(ReplyBuilder::replyReferences(QStringLiteral("<a@x> <b@x>"), QStringLiteral("<c@x>")),
                 (QStringList{QStringLiteral("<a@x>"), QStringLiteral("<b@x>"), QStringLiteral("<c@x>")}));
        QString many;
        for (int i = 0; i < 30; ++i) many += QStringLiteral("<%1@x> ").arg(i);
        const QStringList trimmed = ReplyBuilder::replyReferences(many, QStringLiteral("<new@x>"));
        QCOMPARE(trimmed.size(), 20);
        QCOMPARE(trimmed.first(), QStringLiteral("<0@x>")); // the thread root stays
        QCOMPARE(trimmed.last(), QStringLiteral("<new@x>"));

        CachedMessage o;
        o.fromName = QStringLiteral("Priya Raman");
        o.fromAddr = QStringLiteral("priya.raman@example.com");
        o.to = QStringLiteral("Demo User <demo.user@example.com>, Hannah <h@example.com>, priya.raman@example.com");
        o.cc = QStringLiteral("DEMO.USER@example.com, Omar <omar@example.net>");
        o.subject = QStringLiteral("Budget");
        o.messageIdHeader = QStringLiteral("<orig@example.com>");
        o.threadId = QStringLiteral("t1");
        o.bodyText = QStringLiteral("line one\nline two");
        o.internalDateMs = QDateTime(QDate(2026, 10, 3), QTime(20, 12), QTimeZone::utc()).toMSecsSinceEpoch();

        const ComposeDraft r = ReplyBuilder::make(ReplyBuilder::Kind::Reply, o, QStringLiteral("demo.user@example.com"));
        QCOMPARE(r.to, QStringLiteral("Priya Raman <priya.raman@example.com>"));
        QVERIFY(r.cc.isEmpty());
        QCOMPARE(r.inReplyTo, QStringLiteral("<orig@example.com>"));
        QCOMPARE(r.threadId, QStringLiteral("t1"));
        QVERIFY(r.quotedText.contains(QStringLiteral("Priya Raman <priya.raman@example.com> wrote:\n> line one\n> line two")));

        const ComposeDraft all = ReplyBuilder::make(ReplyBuilder::Kind::ReplyAll, o, QStringLiteral("demo.user@example.com"));
        QCOMPARE(all.to, QStringLiteral("Priya Raman <priya.raman@example.com>, Hannah <h@example.com>")); // no self, no dup
        QCOMPARE(all.cc, QStringLiteral("Omar <omar@example.net>"));

        o.replyTo = QStringLiteral("budget-list@example.com");
        QCOMPARE(ReplyBuilder::make(ReplyBuilder::Kind::Reply, o, QStringLiteral("demo.user@example.com")).to,
                 QStringLiteral("budget-list@example.com"));

        const ComposeDraft f = ReplyBuilder::make(ReplyBuilder::Kind::Forward, o, QStringLiteral("demo.user@example.com"));
        QVERIFY(f.to.isEmpty());
        QCOMPARE(f.subject, QStringLiteral("Fwd: Budget"));
        QVERIFY(f.threadId.isEmpty());
        QVERIFY(f.quotedText.contains(QStringLiteral("---------- Forwarded message ---------")));
        QVERIFY(f.quotedText.contains(QStringLiteral("line one")));
    }

    void replyThreadsThroughMock()
    {
        LogCapture log;
        Live L;
        const QString root = L.seed(QStringLiteral("Q4 budget review"));
        const QString rootMsgId = L.g.messages().value(root).messageIdHeader;
        MockGoogle::Message second;
        second.from = QStringLiteral("Priya Raman <priya.raman@example.com>");
        second.to = QStringLiteral("demo.user@example.com");
        second.subject = QStringLiteral("Re: Q4 budget review");
        second.threadId = L.g.messages().value(root).threadId;
        second.references = rootMsgId;
        second.inReplyTo = rootMsgId;
        second.text = QStringLiteral("Updated numbers.");
        second.labels = {QStringLiteral("INBOX")};
        const QString parentId = L.g.addMessage(second, true);
        QVERIFY(L.start());

        const CachedMessage parent = L.fetch(parentId);
        QCOMPARE(parent.messageIdHeader, QStringLiteral("<%1@mock.example>").arg(parentId));
        QCOMPARE(parent.references, rootMsgId);

        const ComposeDraft d = ReplyBuilder::make(ReplyBuilder::Kind::Reply, parent, L.session->account());
        ComposeWindow c;
        c.setConfirmOnClose(false);
        c.setSession(L.session.get());
        c.setDraft(d);
        QCOMPARE(c.findChild<QLineEdit *>(QStringLiteral("fieldSubject"))->text(), QStringLiteral("Re: Q4 budget review"));
        QCOMPARE(c.findChild<QLineEdit *>(QStringLiteral("fieldFrom"))->text(), QStringLiteral("Demo User <demo.user@example.com>"));
        c.findChild<QTextEdit *>(QStringLiteral("composeBody"))->textCursor().insertText(QStringLiteral("Looks good."));
        QSignalSpy sent(&c, &ComposeWindow::sent);
        c.send();
        QTRY_COMPARE_WITH_TIMEOUT(sent.size(), 1, 10000);
        QVERIFY(c.isSent());
        QCOMPARE(L.g.lastSendPath, QStringLiteral("simple"));

        const Parsed p = parse(L.g.lastRaw);
        QCOMPARE(p.headers.value("in-reply-to"), parent.messageIdHeader);
        QCOMPARE(p.headers.value("references"), rootMsgId + QLatin1Char(' ') + parent.messageIdHeader);
        QCOMPARE(decodeWords(p.headers.value("subject")), QStringLiteral("Re: Q4 budget review"));
        // Gmail kept it in the thread (the mock checks references + subject like Gmail does).
        QCOMPARE(sent.at(0).at(1).toString(), parent.threadId);
        const QString sentId = sent.at(0).at(0).toString();
        QCOMPARE(L.g.messages().value(sentId).threadId, parent.threadId);

        // And it shows up in Sent after the next sync.
        QTRY_VERIFY_WITH_TIMEOUT(L.session->cache()->contains(sentId), 15000);
        QVERIFY(L.session->cache()->message(sentId).labels.contains(QStringLiteral("SENT")));
        QVERIFY(L.session->cache()->count(QStringLiteral("SENT")) >= 1);
        QVERIFY(!log.all().contains(QLatin1String(MockGoogle::kAccessPrefix)));
    }

    void threadIdAloneDoesNotThread()
    {
        // Why the headers matter: a threadId without References starts a new thread.
        Live L;
        const QString root = L.seed(QStringLiteral("Lunch?"));
        QVERIFY(L.start());
        OutgoingMessage m;
        m.from = L.session->fromHeader();
        m.to = QStringLiteral("priya.raman@example.com");
        m.subject = QStringLiteral("Re: Lunch?");
        m.text = QStringLiteral("Sure");
        Sender::Result res;
        bool done = false;
        L.session->sender()->send(MimeBuilder::build(m), L.g.messages().value(root).threadId,
                                  [&](const Sender::Result &r) { res = r; done = true; });
        QTRY_VERIFY(done);
        QVERIFY(res.ok);
        QVERIFY(res.threadId != L.g.messages().value(root).threadId);
    }

    void replyAllAndForwardFromMainWindow()
    {
        Live L;
        MockGoogle::Message m;
        m.from = QStringLiteral("Priya Raman <priya.raman@example.com>");
        m.to = QStringLiteral("Demo User <demo.user@example.com>, Hannah Lindqvist <h.lindqvist@example.com>");
        m.cc = QStringLiteral("Omar Haddad <omar.haddad@example.net>");
        m.subject = QStringLiteral("Site estimate");
        m.text = QStringLiteral("Estimate attached.");
        m.labels = {QStringLiteral("INBOX")};
        m.attachments = {QStringLiteral("site-estimate.pdf")};
        m.attachmentData = {QByteArray("%PDF-1.4 site estimate ") + noise(3000)};
        const QString id = L.g.addMessage(m, true);
        QVERIFY(L.start());

        MainWindow w;
        w.setSession(L.session.get());
        // ready() already fired; attach by hand.
        QMetaObject::invokeMethod(L.session.get(), "ready");
        w.show();
        QTRY_VERIFY(w.isLive());
        auto *list = w.findChild<QTreeView *>(QStringLiteral("messageList"));
        QTRY_VERIFY(list->model()->rowCount() >= 1);
        QAction *replyAll = w.findChild<QAction *>(QStringLiteral("actionReplyAll"));
        QVERIFY(!replyAll->isEnabled()); // nothing selected yet
        list->setCurrentIndex(list->model()->index(0, 0));
        QTRY_COMPARE(w.shownMessageId(), id);
        QVERIFY(replyAll->isEnabled());

        replyAll->trigger();
        QTRY_COMPARE(w.composers().size(), 1);
        ComposeWindow *ra = w.composers().first();
        ra->setConfirmOnClose(false);
        QTRY_COMPARE(ra->findChild<QLineEdit *>(QStringLiteral("fieldSubject"))->text(), QStringLiteral("Re: Site estimate"));
        QCOMPARE(ra->findChild<QLineEdit *>(QStringLiteral("fieldTo"))->text(),
                 QStringLiteral("Priya Raman <priya.raman@example.com>, Hannah Lindqvist <h.lindqvist@example.com>"));
        QCOMPARE(ra->findChild<QLineEdit *>(QStringLiteral("fieldCc"))->text(), QStringLiteral("Omar Haddad <omar.haddad@example.net>"));
        QCOMPARE(ra->message().inReplyTo, QStringLiteral("<%1@mock.example>").arg(id));
        QCOMPARE(ra->threadId(), L.g.messages().value(id).threadId);
        ra->close();

        w.findChild<QAction *>(QStringLiteral("actionForward"))->trigger();
        QTRY_COMPARE(w.composers().size(), 1);
        ComposeWindow *fw = w.composers().first();
        fw->setConfirmOnClose(false);
        QTRY_COMPARE(fw->findChild<QLineEdit *>(QStringLiteral("fieldSubject"))->text(), QStringLiteral("Fwd: Site estimate"));
        QTRY_COMPARE_WITH_TIMEOUT(fw->attachments().size(), 1, 10000); // fetched via attachments.get
        QCOMPARE(fw->attachments().first().name, QStringLiteral("site-estimate.pdf"));
        QCOMPARE(fw->attachments().first().data, m.attachmentData.first());
        fw->findChild<QLineEdit *>(QStringLiteral("fieldTo"))->setText(QStringLiteral("dana.whitfield@example.org"));
        QSignalSpy sent(fw, &ComposeWindow::sent);
        fw->send();
        QTRY_COMPARE_WITH_TIMEOUT(sent.size(), 1, 10000);
        QVERIFY(L.g.lastRaw.contains("filename=\"site-estimate.pdf\""));
        QVERIFY(L.g.lastRaw.contains("---------- Forwarded message ---------") ||
                L.g.lastRaw.contains("Content-Transfer-Encoding: base64"));
        QVERIFY(parse(L.g.lastRaw).headers.value("in-reply-to").isEmpty());
    }

    // ---- uploads ---------------------------------------------------------------------

    void transportBySize()
    {
        QCOMPARE(Sender::transportFor(1000), Sender::Transport::Simple);
        QCOMPARE(Sender::transportFor(limits::kSimpleUploadMaxBytes - 1), Sender::Transport::Simple);
        QCOMPARE(Sender::transportFor(limits::kSimpleUploadMaxBytes), Sender::Transport::Resumable);
        QCOMPARE(Sender::transportFor(limits::kSendLimitBytes), Sender::Transport::Resumable);
        QCOMPARE(Sender::transportFor(limits::kSendLimitBytes + 1), Sender::Transport::None);
    }

    void bigMessageUsesResumableUploadAndResumes()
    {
        LogCapture log;
        Live L;
        QVERIFY(L.start());
        OutgoingMessage m;
        m.from = L.session->fromHeader();
        m.to = QStringLiteral("priya.raman@example.com");
        m.subject = QStringLiteral("Scans");
        m.text = QStringLiteral("8 MB of scans attached.");
        m.attachments = {{QStringLiteral("scans.bin"), QStringLiteral("application/octet-stream"), noise(8'000'000)}};
        const QByteArray mime = MimeBuilder::build(m);
        QVERIFY(mime.size() > limits::kSimpleUploadMaxBytes);
        QVERIFY(mime.size() < limits::kSendLimitBytes);

        // The first PUT is cut off after 3 MB; Sender asks how much arrived and sends the rest.
        L.g.failUploadAfterBytes = 3'000'000;
        const int before = int(L.g.requests.size());
        QSignalSpy progress(L.session->sender(), &Sender::progress);
        Sender::Result res;
        bool done = false;
        L.session->sender()->send(mime, {}, [&](const Sender::Result &r) { res = r; done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 30000);
        QVERIFY2(res.ok, qPrintable(res.err.message));
        QCOMPARE(res.transport, Sender::Transport::Resumable);
        QCOMPARE(res.resumes, 1);
        QCOMPARE(L.g.lastSendPath, QStringLiteral("resumable"));
        QCOMPARE(L.g.uploadSessions, 1);
        QCOMPARE(L.g.statusQueries, 1);
        QCOMPARE(L.g.uploadPuts, 2);
        QCOMPARE(L.g.lastRaw.size(), mime.size());
        QVERIFY(L.g.lastRaw == mime); // byte-for-byte, despite the interruption
        QVERIFY(!progress.isEmpty());
        // POST session, PUT (cut), PUT status, PUT rest; no simple /messages/send.
        const QStringList reqs = L.g.requests.mid(before);
        QVERIFY(reqs.contains(QStringLiteral("POST /upload/gmail/v1/users/me/messages/send")));
        QVERIFY(!reqs.contains(QStringLiteral("POST /gmail/v1/users/me/messages/send")));
        QVERIFY(!log.all().contains(QLatin1String(MockGoogle::kAccessPrefix)));
        // Sent appears after the next sync.
        L.session->syncSoon();
        QTRY_VERIFY_WITH_TIMEOUT(L.session->cache()->contains(res.messageId), 15000);
    }

    void overLimitIsBlockedBeforeAnyRequest()
    {
        Live L;
        QVERIFY(L.start());
        OutgoingMessage m;
        m.from = L.session->fromHeader();
        m.to = QStringLiteral("priya.raman@example.com");
        m.subject = QStringLiteral("Video");
        m.text = QStringLiteral("19 MB raw is about 26 MB once base64-encoded.");
        m.attachments = {{QStringLiteral("video.mp4"), {}, noise(19'000'000)}};
        const QByteArray mime = MimeBuilder::build(m);
        QVERIFY(mime.size() > limits::kSendLimitBytes);
        const int before = int(L.g.requests.size());
        Sender::Result res;
        bool done = false;
        L.session->sender()->send(mime, {}, [&](const Sender::Result &r) { res = r; done = true; });
        QVERIFY(done); // synchronous: nothing went out
        QVERIFY(res.blocked);
        QVERIFY(!res.ok);
        QTest::qWait(100);
        // (The background sync may poll history meanwhile; nothing may upload.)
        for (const QString &r : L.g.requests.mid(before)) {
            QVERIFY2(!r.contains(QLatin1String("/send")) && !r.startsWith(QLatin1String("POST /upload")), qPrintable(r));
        }
        QCOMPARE(L.g.sendCalls, 0);

        // The compose window blocks too: Send disabled, send() refuses, banner shows.
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("video.mp4"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(m.attachments.first().data);
        f.close();
        ComposeWindow::setZipPolicy(ComposeWindow::ZipPolicy::Never);
        ComposeWindow c;
        c.setConfirmOnClose(false);
        c.setSession(L.session.get());
        c.findChild<QLineEdit *>(QStringLiteral("fieldTo"))->setText(QStringLiteral("priya.raman@example.com"));
        c.addFiles({path});
        QCOMPARE(c.sizeLevel(), limits::SizeLevel::Blocked);
        QVERIFY(!c.findChild<QAction *>(QStringLiteral("actionSend"))->isEnabled());
        c.send();
        QTest::qWait(100);
        for (const QString &r : L.g.requests.mid(before)) {
            QVERIFY2(!r.contains(QLatin1String("/send")) && !r.startsWith(QLatin1String("POST /upload")) &&
                         !r.contains(QLatin1String("/drafts")),
                     qPrintable(r));
        }
        QCOMPARE(L.g.sendCalls, 0);
        QVERIFY(!c.isSent());
        QVERIFY(c.findChild<QWidget *>(QStringLiteral("composeBanner"))->isVisibleTo(&c));
        QVERIFY(!c.findChild<QPushButton *>(QStringLiteral("bannerZip"))->isVisibleTo(&c)); // policy Never
    }

    void zipOfferAsksThenZipsAndReverts()
    {
        QTemporaryDir dir;
        QStringList paths;
        QList<QByteArray> contents;
        for (int i = 0; i < 2; ++i) {
            const QString p = dir.filePath(QStringLiteral("log-%1.txt").arg(i));
            QFile f(p);
            QVERIFY(f.open(QIODevice::WriteOnly));
            contents << prose(10'000'000);
            f.write(contents.last());
            paths << p;
        }
        ComposeWindow c;
        c.setConfirmOnClose(false);
        int asked = 0;
        bool rememberNext = false;
        ComposeWindow::ZipAnswer answer = ComposeWindow::ZipAnswer::Cancel;
        c.setZipPrompt([&](const zip::Probe &probe, bool *remember) {
            ++asked;
            [&] { QVERIFY(probe.worthwhile); QVERIFY(probe.estimatedZipBytes < probe.rawBytes / 4); }();
            *remember = rememberNext;
            return answer;
        });
        c.addFiles(paths); // 20 MB raw -> ~27 MB encoded: blocked, offer queued
        QCOMPARE(c.sizeLevel(), limits::SizeLevel::Blocked);
        QTRY_COMPARE(asked, 1); // asked, not zipped behind the user's back
        QVERIFY(!c.isZipped());
        QCOMPARE(c.attachments().size(), 2);
        QVERIFY(c.findChild<QPushButton *>(QStringLiteral("bannerZip"))->isVisibleTo(&c));

        answer = ComposeWindow::ZipAnswer::Zip;
        QVERIFY(c.offerZip());
        QCOMPARE(asked, 2);
        QVERIFY(c.isZipped());
        QCOMPARE(c.attachments().size(), 1);
        QCOMPARE(c.attachments().first().name, QStringLiteral("attachments.zip"));
        QCOMPARE(c.sizeLevel(), limits::SizeLevel::Ok); // re-checked after zipping
        QVERIFY(c.findChild<QAction *>(QStringLiteral("actionSend"))->isEnabled());
        const QByteArray z = c.attachments().first().data;
        QString err;
        QCOMPARE(zip::listArchive(z, &err), (QStringList{QStringLiteral("log-0.txt"), QStringLiteral("log-1.txt")}));
        QVERIFY2(err.isEmpty(), qPrintable(err));
        QVERIFY(zip::extract(z, QStringLiteral("log-1.txt")) == contents[1]);
        QVERIFY(c.buildMime().contains("filename=\"attachments.zip\""));

        c.revertZip();
        QVERIFY(!c.isZipped());
        QCOMPARE(c.attachments().size(), 2);
        QCOMPARE(c.sizeLevel(), limits::SizeLevel::Blocked);

        // "Remember my choice" -> Always: next time it zips without asking.
        rememberNext = true;
        QVERIFY(c.offerZip());
        QCOMPARE(ComposeWindow::zipPolicy(), ComposeWindow::ZipPolicy::Always);
        c.revertZip();
        QVERIFY(c.offerZip());
        QCOMPARE(asked, 3);
        c.revertZip();
        ComposeWindow::setZipPolicy(ComposeWindow::ZipPolicy::Never);
        QVERIFY(!c.offerZip());
        QCOMPARE(asked, 3);
    }

    void zipOfferDeclinesWhenItWouldNotFit()
    {
        ComposeWindow c;
        c.setConfirmOnClose(false);
        int asked = 0;
        c.setZipPrompt([&](const zip::Probe &, bool *) { ++asked; return ComposeWindow::ZipAnswer::Zip; });
        ComposeWindow::Attachment a;
        a.name = QStringLiteral("random.bin");
        a.data = noise(20'000'000);
        a.bytes = a.data.size();
        c.setAttachments({a});
        QCOMPARE(c.sizeLevel(), limits::SizeLevel::Blocked);
        QVERIFY(!c.offerZip());
        QCOMPARE(asked, 0);
        QVERIFY(c.lastError().contains(QStringLiteral("won't help")));
        QVERIFY(zip::isAlreadyCompressed(QStringLiteral("x.JPG"), {}));
        QVERIFY(!zip::isAlreadyCompressed(QStringLiteral("x.csv"), QStringLiteral("text/csv")));
    }

    // ---- drafts ------------------------------------------------------------------------

    void draftsCreateUpdateAndDeleteOnSend()
    {
        Live L;
        QVERIFY(L.start());
        ComposeWindow c;
        c.setConfirmOnClose(false);
        c.setSession(L.session.get());
        c.findChild<QLineEdit *>(QStringLiteral("fieldTo"))->setText(QStringLiteral("priya.raman@example.com"));
        c.findChild<QLineEdit *>(QStringLiteral("fieldSubject"))->setText(QStringLiteral("Draft one"));
        c.setBodyText(QStringLiteral("<p>first</p>"));
        QSignalSpy saved(&c, &ComposeWindow::draftSaved);
        c.saveDraft();
        QTRY_COMPARE_WITH_TIMEOUT(saved.size(), 1, 10000);
        const QString draftId = c.draftId();
        QVERIFY(!draftId.isEmpty());
        QCOMPARE(L.g.drafts().size(), 1);
        QVERIFY(L.g.messages().value(L.g.drafts().value(draftId).messageId).labels.contains(QStringLiteral("DRAFT")));

        c.findChild<QLineEdit *>(QStringLiteral("fieldSubject"))->setText(QStringLiteral("Draft two"));
        c.saveDraft();
        QTRY_COMPARE_WITH_TIMEOUT(saved.size(), 2, 10000);
        QCOMPARE(c.draftId(), draftId); // drafts.update, same draft
        QCOMPARE(L.g.drafts().size(), 1);
        QCOMPARE(L.g.messages().value(L.g.drafts().value(draftId).messageId).subject, QStringLiteral("Draft two"));
        QVERIFY(L.g.requests.contains(QStringLiteral("PUT /gmail/v1/users/me/drafts/") + draftId));

        QSignalSpy sent(&c, &ComposeWindow::sent);
        c.send();
        QTRY_COMPARE_WITH_TIMEOUT(sent.size(), 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(L.g.drafts().isEmpty(), 10000); // sent -> draft deleted
        QVERIFY(L.g.requests.contains(QStringLiteral("DELETE /gmail/v1/users/me/drafts/") + draftId));
    }

    void bigDraftUsesResumableUpload()
    {
        Live L;
        QVERIFY(L.start());
        OutgoingMessage m;
        m.from = L.session->fromHeader();
        m.to = QStringLiteral("a@example.com");
        m.text = QStringLiteral("big draft");
        m.attachments = {{QStringLiteral("big.bin"), {}, noise(6'000'000)}};
        Sender::Result res;
        bool done = false;
        L.session->sender()->saveDraft(MimeBuilder::build(m), {}, {}, [&](const Sender::Result &r) { res = r; done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 30000);
        QVERIFY2(res.ok, qPrintable(res.err.message));
        QCOMPARE(res.transport, Sender::Transport::Resumable);
        QVERIFY(!res.draftId.isEmpty());
        QVERIFY(L.g.requests.contains(QStringLiteral("POST /upload/gmail/v1/users/me/drafts")));
        QCOMPARE(L.g.drafts().size(), 1);
    }

    // ---- signatures ----------------------------------------------------------------------

    void signaturesPlainAndHtml()
    {
        QTemporaryDir dir;
        QSettings s(dir.filePath(QStringLiteral("sig.ini")), QSettings::IniFormat);
        SignatureStore store(&s);
        Signature work{QStringLiteral("Work"), QStringLiteral("<p><b>Demo User</b><br>Field Operations</p>"), {}};
        Signature home{QStringLiteral("Personal"), {}, QStringLiteral("Demo\nsent from zmail")};
        store.setAll({work, home});
        store.setDefaultName(QStringLiteral("Work"));
        QCOMPARE(store.all().size(), 2);
        QCOMPARE(store.find(QStringLiteral("Personal")).plain(), QStringLiteral("Demo\nsent from zmail"));
        QVERIFY(store.find(QStringLiteral("Work")).plain().contains(QStringLiteral("Demo User")));
        QCOMPARE(SignatureStore::plainBlock(home), QStringLiteral("-- \nDemo\nsent from zmail"));

        ComposeWindow c;
        c.setConfirmOnClose(false);
        c.setSignatureStore(&store);
        QCOMPARE(c.signatureName(), QStringLiteral("Work")); // the default
        c.findChild<QLineEdit *>(QStringLiteral("fieldTo"))->setText(QStringLiteral("a@example.com"));
        ComposeDraft d;
        d.to = QStringLiteral("a@example.com");
        d.subject = QStringLiteral("Re: x");
        d.quotedText = QStringLiteral("On Sat, Priya wrote:\n> original");
        d.quotedHtml = richtext::quoteHtml(QStringLiteral("<p>original</p>"), QStringLiteral("On Sat, Priya wrote:"));
        c.setDraft(d);
        auto *body = c.findChild<QTextEdit *>(QStringLiteral("composeBody"));
        body->textCursor().insertText(QStringLiteral("My reply"));

        OutgoingMessage m = c.message();
        QVERIFY2(m.html.contains(QStringLiteral("Demo User")), qPrintable(m.html));
        QVERIFY(m.html.contains(QStringLiteral("font-weight:700")) || m.html.contains(QStringLiteral("font-weight:600")) ||
                m.html.contains(QStringLiteral("<b>")));
        // Order: reply, then "-- " + signature, then the quote.
        const qsizetype reply = m.text.indexOf(QStringLiteral("My reply"));
        const qsizetype sig = m.text.indexOf(QStringLiteral("-- \nDemo User"));
        const qsizetype quote = m.text.indexOf(QStringLiteral("> original"));
        QVERIFY2(reply >= 0 && sig > reply && quote > sig, qPrintable(m.text));

        // Switching signatures replaces it in place.
        c.setSignature(QStringLiteral("Personal"));
        m = c.message();
        QVERIFY(!m.text.contains(QStringLiteral("Field Operations")));
        QVERIFY2(m.text.contains(QStringLiteral("-- \nDemo\nsent from zmail")), qPrintable(m.text));
        QCOMPARE(m.text.count(QStringLiteral("-- \n")), 1);
        QVERIFY(m.text.indexOf(QStringLiteral("sent from zmail")) < m.text.indexOf(QStringLiteral("> original")));

        // Plain format keeps the reply, the signature and the quote.
        c.setFormat(ComposeWindow::Format::Plain);
        m = c.message();
        QVERIFY(m.html.isEmpty());
        QVERIFY2(m.text.contains(QStringLiteral("My reply")), qPrintable(m.text));
        QVERIFY(m.text.contains(QStringLiteral("-- \nDemo\nsent from zmail")));
        QVERIFY(m.text.contains(QStringLiteral("> original")));
        c.setSignature({});
        QVERIFY(!c.message().text.contains(QStringLiteral("-- \n")));
    }

    void signatureIsInTheSentMimeForNewReplyAndForward()
    {
        // 0.3.0: a New message in the default format went out without the
        // default signature (only Reply/Forward and format changes added it).
        // Uses the compose window's own SignatureStore, as the app does.
        Live L;
        const QString root = L.seed(QStringLiteral("Crew schedule"));
        QVERIFY(L.start());
        const CachedMessage original = L.fetch(root);

        const struct { ComposeWindow::Format format; const char *name; } formats[] = {
            {ComposeWindow::Format::Html, "html"},
            {ComposeWindow::Format::Plain, "plain"},
            {ComposeWindow::Format::Markdown, "markdown"},
        };
        for (const auto &f : formats) {
            for (const char *kind : {"new", "reply", "forward"}) {
                QSettings().clear();
                {
                    SignatureStore store;
                    store.setAll({Signature{QStringLiteral("Work"),
                                            QStringLiteral("<p><b>Demo User</b><br>Field Operations</p>"),
                                            QStringLiteral("Demo User\nField Operations")}});
                    store.setDefaultName(QStringLiteral("Work"));
                }
                QSettings().setValue(QStringLiteral("compose/format"), int(f.format));
                const QString what = QStringLiteral("%1/%2").arg(QLatin1String(f.name), QLatin1String(kind));

                ComposeWindow c;
                c.setConfirmOnClose(false);
                c.setSession(L.session.get());
                QCOMPARE(c.format(), f.format);
                QCOMPARE(c.signatureName(), QStringLiteral("Work"));
                if (QByteArray(kind) == "new") {
                    c.findChild<QLineEdit *>(QStringLiteral("fieldTo"))->setText(QStringLiteral("priya.raman@example.com"));
                    c.findChild<QLineEdit *>(QStringLiteral("fieldSubject"))->setText(QStringLiteral("Signature check"));
                } else {
                    const auto k = QByteArray(kind) == "reply" ? ReplyBuilder::Kind::Reply : ReplyBuilder::Kind::Forward;
                    ComposeDraft d = ReplyBuilder::make(k, original, L.session->account());
                    if (d.to.isEmpty()) {
                        d.to = QStringLiteral("priya.raman@example.com");
                    }
                    c.setDraft(d);
                }
                auto *body = c.findChild<QTextEdit *>(QStringLiteral("composeBody"));
                // Typed where the cursor is: above the signature (and the quote).
                body->textCursor().insertText(QStringLiteral("Body for %1").arg(what));
                QSignalSpy sent(&c, &ComposeWindow::sent);
                c.send();
                QTRY_COMPARE_WITH_TIMEOUT(sent.size(), 1, 10000);

                const auto parts = leafTexts(L.g.lastRaw);
                const QString plain = parts.value(QStringLiteral("text/plain"));
                QVERIFY2(plain.contains(QStringLiteral("Body for ") + what), qPrintable(what + QLatin1Char('\n') + plain));
                QVERIFY2(plain.contains(QStringLiteral("\n-- \r\n")) || plain.contains(QStringLiteral("\n-- \n")),
                         qPrintable(what + QStringLiteral(": no signature delimiter\n") + plain));
                QVERIFY2(plain.contains(QStringLiteral("Demo User")) && plain.contains(QStringLiteral("Field Operations")),
                         qPrintable(what + QStringLiteral(": no signature in text/plain\n") + plain));
                QVERIFY(plain.indexOf(QStringLiteral("Body for")) < plain.indexOf(QStringLiteral("Field Operations")));
                if (f.format != ComposeWindow::Format::Plain) {
                    const QString html = parts.value(QStringLiteral("text/html"));
                    QVERIFY2(html.contains(QStringLiteral("Field Operations")),
                             qPrintable(what + QStringLiteral(": no signature in text/html\n") + html.left(2000)));
                }
            }
        }
    }

    // ---- spell check ------------------------------------------------------------------------

    void spellCheckSharesZwriterDictionary()
    {
        QVERIFY(SpellChecker::userDictionaryPath().endsWith(QStringLiteral("user-dictionary.txt")));
        qunsetenv("ZMAIL_USER_DICTIONARY");
        QVERIFY(SpellChecker::userDictionaryPath().endsWith(QStringLiteral("/sbj-ee/zwriter/user-dictionary.txt")));
        qputenv("ZMAIL_USER_DICTIONARY", QFile::encodeName(m_tmp.filePath(QStringLiteral("user-dictionary.txt"))));

        SpellChecker sc;
        if (!sc.isAvailable()) {
            QSKIP("Hunspell or the en_US dictionary isn't installed");
        }
        QVERIFY(sc.isCorrect(QStringLiteral("the")));
        QVERIFY(!sc.isCorrect(QStringLiteral("teh")));
        QVERIFY(sc.suggestions(QStringLiteral("teh")).contains(QStringLiteral("the")));
        QVERIFY(sc.isCorrect(QStringLiteral("x86"))); // not a word
        QVERIFY(!sc.isCorrect(QStringLiteral("zmailish")));
        sc.addToUserDictionary(QStringLiteral("zmailish"));
        QVERIFY(sc.isCorrect(QStringLiteral("zmailish")));
        QFile f(SpellChecker::userDictionaryPath());
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(f.readAll().split('\n').contains("zmailish"));
        SpellChecker again; // a new instance (zwriter, or the next zmail run) reads it
        QVERIFY(again.isCorrect(QStringLiteral("zmailish")));
        sc.ignoreWord(QStringLiteral("blorpt"));
        QVERIFY(sc.isCorrect(QStringLiteral("Blorpt")));
    }

private:
    QTemporaryDir m_tmp;
};

QTEST_MAIN(TstSend)
#include "tst_send.moc"
