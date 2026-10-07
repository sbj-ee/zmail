// Message viewer: layout of builder-made HTML mail, light page, header block,
// blocked remote images, zoom, splitter/layout memory, message windows.
#include "MainWindow.hpp"
#include "ui/ComposeWindow.h"
#include "ui/HtmlFit.h"
#include "ui/MessageView.h"
#include "ui/MessageWindow.h"
#include "ui/PrivacyDialog.h"
#include "ui/RemoteImages.h"
#include "ui/SafeHtmlView.h"
#include "ui/Theme.h"

#include <QAbstractTextDocumentLayout>
#include <QAction>
#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QRadioButton>
#include <QCheckBox>
#include <QPushButton>
#include <QSettings>
#include <QFontInfo>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QSplitter>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextTable>
#include <QTreeView>
#include <QtTest>

using namespace zmail::ui;

namespace {

QString fixture(const char *name)
{
    QFile f(QStringLiteral(ZMAIL_FIXTURES "/") + QString::fromLatin1(name));
    if (!f.open(QIODevice::ReadOnly)) {
        qFatal("missing fixture %s", name);
    }
    return QString::fromUtf8(f.readAll());
}

ViewMessage fixtureMessage(const char *name)
{
    ViewMessage m;
    m.id = QString::fromLatin1(name);
    m.from = QStringLiteral("Example Sender <noreply@example.com>");
    m.subject = QString::fromLatin1(name);
    m.bodyHtml = fixture(name);
    return m;
}

// Smallest laid-out width among blocks whose text contains `needle`.
qreal minBlockWidth(QTextDocument *doc, const QString &needle)
{
    qreal w = 0;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        if (b.text().contains(needle)) {
            const qreal bw = doc->documentLayout()->blockBoundingRect(b).width();
            w = w == 0 ? bw : std::min(w, bw);
        }
    }
    return w;
}

ViewMessage vetMessage()
{
    ViewMessage m;
    m.id = QStringLiteral("vet-1");
    m.from = QStringLiteral("Maple Grove Animal Hospital <reminders@maplegrove-vet.example>");
    m.to = QStringLiteral("Jordan Example <jordan@example.com>");
    m.cc = QStringLiteral("Sam Example <sam@example.com>");
    m.subject = QStringLiteral("Biscuit is due for a visit");
    m.date = QDateTime(QDate(2026, 10, 2), QTime(11, 12), QTimeZone(QByteArrayLiteral("America/Chicago")));
    m.attachments = {QStringLiteral("vaccine-record.pdf"), QStringLiteral("map.png")};
    m.bodyHtml = fixture("unlayer-vet.html");
    return m;
}

// Width of the laid-out block whose text starts with `prefix` (0 = not found).
qreal blockWidth(QTextDocument *doc, const QString &prefix)
{
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        if (b.text().startsWith(prefix)) {
            return doc->documentLayout()->blockBoundingRect(b).width();
        }
    }
    return 0;
}

// Minimal HTTP server for "Load images": serves one PNG for any GET and
// records the paths asked for.
class ImageServer : public QTcpServer
{
public:
    QStringList paths;
    QList<QByteArray> requests; // raw request heads
    ImageServer()
    {
        QImage img(40, 20, QImage::Format_RGB32);
        img.fill(Qt::red);
        QBuffer buf(&m_png);
        buf.open(QIODevice::WriteOnly);
        img.save(&buf, "PNG");
        listen(QHostAddress::LocalHost);
        connect(this, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket *s = nextPendingConnection()) {
                auto *pending = new QByteArray;
                connect(s, &QTcpSocket::readyRead, s, [this, s, pending]() {
                    pending->append(s->readAll());
                    const int end = pending->indexOf("\r\n\r\n");
                    if (end < 0) {
                        return;
                    }
                    const QList<QByteArray> first = pending->left(pending->indexOf("\r\n")).split(' ');
                    paths << QString::fromLatin1(first.value(1));
                    requests << pending->left(end);
                    pending->remove(0, end + 4);
                    s->write("HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nSet-Cookie: track=me; Path=/\r\n"
                             "Connection: close\r\nContent-Length: " +
                             QByteArray::number(m_png.size()) + "\r\n\r\n" + m_png);
                    s->disconnectFromHost();
                });
                connect(s, &QTcpSocket::disconnected, s, [s, pending]() {
                    delete pending;
                    s->deleteLater();
                });
            }
        });
    }

private:
    QByteArray m_png;
};

// ImageServer is plain http on 127.0.0.1, which zmail refuses for mail
// images; let these tests through.
void serveImagesLocally()
{
    QSettings().setValue(QStringLiteral("privacy/allowHttpImages"), true);
    zmail::ui::SafeHtmlView::setLoopbackAllowedForTests(true);
}

} // namespace

class TstViewer : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QCoreApplication::setOrganizationName(QStringLiteral("zmail-tst-viewer"));
        QStandardPaths::setTestModeEnabled(true);
        applyTheme(ThemeMode::Light);
    }
    void init() { QSettings().clear(); }
    void cleanup()
    {
        zmail::ui::SafeHtmlView::setLoopbackAllowedForTests(false);
        for (QWidget *w : QApplication::topLevelWidgets()) {
            w->close();
        }
    }

    void builderMailFillsThePane()
    {
        // 0.2.0 showed Unlayer mail (fixed widths only inside <!--[if mso]>)
        // as a ~90 px column with words broken mid-word.
        MessageView v;
        v.resize(900, 700);
        v.show();
        QVERIFY(QTest::qWaitForWindowExposed(&v));
        v.setMessage(vetMessage());
        const int vw = v.body()->viewport()->width();
        const qreal para = blockWidth(v.body()->document(), QStringLiteral("Our records show"));
        QVERIFY2(para > 0.6 * vw, qPrintable(QStringLiteral("paragraph %1 px in a %2 px pane").arg(para).arg(vw)));
        // Two-column band stays side by side: both cells' text on one row.
        QTextDocument *doc = v.body()->document();
        qreal yBook = -1, yPhone = -2;
        for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
            const QRectF r = doc->documentLayout()->blockBoundingRect(b);
            if (b.text().startsWith(QStringLiteral("BOOK APPOINTMENT")) && yBook < 0) {
                yBook = r.top();
            } else if (b.text().contains(QStringLiteral("014-2290"))) {
                yPhone = r.top();
            }
        }
        QVERIFY(std::abs(yBook - yPhone) < 30);
        QVERIFY(doc->size().width() <= vw + 2); // no sideways scrolling

        // Narrow pane: still no overflow, paragraph reflows.
        v.resize(420, 700);
        QTRY_VERIFY(doc->size().width() <= v.body()->viewport()->width() + 2);
    }

    void fitHandlesFixedAndOversizedTables()
    {
        QTextDocument doc;
        doc.setHtml(QStringLiteral("<table width='300'><tr><td>small</td></tr></table>"
                                   "<table width='1200'><tr><td width='800'>a</td><td width='400'>b</td></tr></table>"
                                   "<table><tr><td>auto</td></tr></table>"));
        HtmlFit::Options o;
        o.availableWidth = 600;
        HtmlFit::fit(&doc, o);
        QList<QTextTable *> tables;
        for (QTextFrame *f : doc.rootFrame()->childFrames()) {
            if (auto *t = qobject_cast<QTextTable *>(f)) {
                tables << t;
            }
        }
        QCOMPARE(tables.size(), 3);
        QCOMPARE(tables[0]->format().width().type(), QTextLength::FixedLength); // fits: keeps its size
        QCOMPARE(tables[0]->format().width().rawValue(), 300.0);
        QCOMPARE(tables[1]->format().width().type(), QTextLength::PercentageLength);
        const auto cols = tables[1]->format().columnWidthConstraints();
        QCOMPARE(cols.size(), 2);
        QCOMPARE(cols[0].type(), QTextLength::PercentageLength);
        QVERIFY(qAbs(cols[0].rawValue() - 66.67) < 0.1); // proportional
        QCOMPARE(tables[2]->format().width().type(), QTextLength::PercentageLength);
        QCOMPARE(tables[2]->format().width().rawValue(), 100.0);
    }

    void prepareTurnsCssTablesAndBandsIntoTables()
    {
        const QString out = HtmlFit::prepare(QStringLiteral(
            "<!--[if mso]><table width='500'><![endif]-->"
            "<div style='display: table;width:100%'><div style='display: table-cell'>L</div>"
            "<div style='display: table-cell'>R</div></div>"
            "<div style='background-color: #00a0bd;;background-color:#11365c;'><p><span style='color: #ffffff'>"
            "<a href='https://x.example/'>Book</a></span></p></div>"
            "<div style='background-color: transparent'>plain</div>"));
        QVERIFY(!out.contains(QStringLiteral("mso")));
        QVERIFY(out.contains(QStringLiteral("<tr><td valign=\"top\">L</td><td valign=\"top\">R</td></tr></table>")));
        QVERIFY(out.contains(QStringLiteral("style=\"background-color:#11365c\"")));      // last value wins
        QVERIFY(out.contains(QStringLiteral("<a style=\"color:#ffffff\" href=")));         // link keeps span colour
        QVERIFY(out.contains(QStringLiteral("<div style='background-color: transparent'>plain</div>")));
    }

    void tableMailKeepsItsContentColumnReadable()
    {
        // 0.3.0 drew a brokerage confirmation as a narrow column, one letter
        // per line: the 10/15/10 px spacer columns were turned into
        // percentages of their own sum (100% of the table), the hidden
        // dark-mode twin was drawn too, and "margin: 0 auto" shifted cards.
        MessageView v;
        v.resize(760, 800);
        v.show();
        QVERIFY(QTest::qWaitForWindowExposed(&v));
        v.setMessage(fixtureMessage("brokerage-confirm.html"));
        QTextDocument *doc = v.body()->document();
        const QString text = doc->toPlainText();
        QVERIFY(text.contains(QStringLiteral("Account ending in: 4242")));
        QVERIFY(!text.contains(QStringLiteral("DARKTWIN")));   // display:none twin dropped
        QVERIFY(!text.contains(QChar(0x200c)));                // hidden preheader dropped
        const qreal info = minBlockWidth(doc, QStringLiteral("Account ending in"));
        QVERIFY2(info > 150, qPrintable(QStringLiteral("info box %1 px").arg(info)));
        const qreal para = minBlockWidth(doc, QStringLiteral("Hello Jordan"));
        QVERIFY2(para > 300, qPrintable(QStringLiteral("paragraph %1 px").arg(para)));
        QVERIFY(doc->size().width() <= v.body()->viewport()->width() + 2);

        // The 600 px card (width:100%; max-width:600px) is centred, not
        // stretched, in a wide pane.
        v.resize(1200, 800);
        QTRY_VERIFY(v.body()->viewport()->width() > 1100);
        v.setMessage(fixtureMessage("brokerage-confirm.html"));
        QTextTable *card = nullptr;
        for (QTextFrame *f : doc->rootFrame()->childFrames()) {
            auto *t = qobject_cast<QTextTable *>(f);
            if (t && t->format().width().type() == QTextLength::FixedLength) {
                card = t;
                break;
            }
        }
        QVERIFY(card);
        QCOMPARE(card->format().width().rawValue(), 600.0);
        QVERIFY(card->format().alignment() & Qt::AlignHCenter);

        // Narrow pane: words stay whole (no letter-per-line), no sideways scroll.
        v.resize(420, 800);
        QTRY_VERIFY(v.body()->viewport()->width() < 440);
        v.setMessage(fixtureMessage("brokerage-confirm.html"));
        QCOMPARE(v.body()->wordWrapMode(), QTextOption::WordWrap);
        QVERIFY(minBlockWidth(doc, QStringLiteral("Account ending in")) > 60);
        QVERIFY(doc->size().width() <= v.body()->viewport()->width() + 2);
    }

    void iconColumnBesideTextKeepsItsPixelWidth()
    {
        MessageView v;
        v.resize(760, 800);
        v.show();
        QVERIFY(QTest::qWaitForWindowExposed(&v));
        v.setMessage(fixtureMessage("retail-rx.html"));
        QTextDocument *doc = v.body()->document();
        QVERIFY(!doc->toPlainText().contains(QStringLiteral("prescription is expiring soon"))); // preheader
        QVERIFY(!HtmlFit::prepare(fixture("retail-rx.html")).contains(QStringLiteral("DARKLOGO")));
        const qreal copy = minBlockWidth(doc, QStringLiteral("You have no remaining refills"));
        QVERIFY2(copy > 280, qPrintable(QStringLiteral("text column %1 px").arg(copy)));

        // fit(): pixel spacers next to a column with no width stay pixels.
        QTextDocument d;
        d.setHtml(QStringLiteral("<table width='100%'><tr><td width='10'></td><td>content</td><td width='15'></td></tr></table>"
                                 "<table width='95%'><tr><td width='30'>i</td><td>text</td></tr></table>"));
        HtmlFit::Options o;
        o.availableWidth = 600;
        HtmlFit::fit(&d, o);
        const auto frames = d.rootFrame()->childFrames();
        auto *t0 = qobject_cast<QTextTable *>(frames.value(0));
        auto *t1 = qobject_cast<QTextTable *>(frames.value(1));
        QVERIFY(t0 && t1);
        const auto c0 = t0->format().columnWidthConstraints();
        QCOMPARE(c0.value(0).type(), QTextLength::FixedLength);
        QCOMPARE(c0.value(0).rawValue(), 10.0);
        QCOMPARE(c0.value(2).rawValue(), 15.0);
        QCOMPARE(t1->format().columnWidthConstraints().value(0).type(), QTextLength::FixedLength);
        QCOMPARE(t1->format().columnWidthConstraints().value(0).rawValue(), 30.0);
    }

    void concatenatedDocumentsAreNotBlank()
    {
        // A Meetup mail: a first document whose <body> holds only a pixel,
        // then the real mail with no <body>. 0.3.0 showed an empty page.
        MessageView v;
        v.resize(760, 600);
        v.setMessage(fixtureMessage("meetup-concat.html"));
        const QString text = v.body()->document()->toPlainText();
        QVERIFY(text.contains(QStringLiteral("Just scheduled: Practical Robotics Night")));
        QVERIFY(text.contains(QStringLiteral("Bring a laptop")));
        QVERIFY(!text.contains(QStringLiteral("Preheader nobody should see")));
        QVERIFY(!text.contains(QStringLiteral(".y{color")));
    }

    void emojiUseTheColourFont()
    {
        QVERIFY(isEmojiCodePoint(0x1F43E));            // paw prints
        QVERIFY(isEmojiCodePoint(0x1F4C5));            // calendar
        QVERIFY(isEmojiCodePoint(0x2B50));             // star
        QVERIFY(!isEmojiCodePoint(U'A'));
        QVERIFY(!isEmojiCodePoint(0x25CF));            // bullet stays text
        QVERIFY(!isEmojiCodePoint(0x2764));            // heart: text unless VS16
        QVERIFY(isEmojiCodePoint(0x2764, 0xFE0F));
        const QString emoji = emojiFamily();
        if (emoji.isEmpty()) {
            QSKIP("no colour emoji font installed");
        }
        // App font (message list, header, attachments label): real family
        // first, emoji font behind it.
        const QStringList fams = QApplication::font().families();
        QVERIFY(fams.size() >= 2);
        QCOMPARE(fams.last(), emoji);
        QVERIFY(fams.first() != emoji);
        QCOMPARE(fams.first(), QFontInfo(QApplication::font()).family());
        // Mail body: emoji inside an explicit font-family point at the
        // colour font; the text around them doesn't.
        QTextDocument d;
        d.setHtml(QStringLiteral("<p style='font-family: Arial, sans-serif'>Riley \U0001F43E is due</p>"));
        HtmlFit::Options o;
        o.availableWidth = 600;
        HtmlFit::fit(&d, o);
        QTextCursor c(&d);
        c.setPosition(7); // after "Riley " + first UTF-16 unit
        QVERIFY(c.charFormat().fontFamilies().toStringList().contains(emoji));
        c.setPosition(2);
        QVERIFY(!c.charFormat().fontFamilies().toStringList().contains(emoji));
    }

    void htmlMailIsOnALightPageEvenInDarkTheme()
    {
        applyTheme(ThemeMode::Dark);
        MessageView v;
        v.resize(800, 600);
        v.setMessage(vetMessage());
        const QPalette p = v.body()->viewport()->palette();
        QVERIFY(p.color(QPalette::Base).lightness() > 240);
        QVERIFY(p.color(QPalette::Text).lightness() < 60);
        QVERIFY(!v.darkMail());

        v.setDarkMail(true);
        QVERIFY(v.body()->viewport()->palette().color(QPalette::Base).lightness() < 60);
        QVERIFY(QSettings().value(QStringLiteral("viewer/darkMail")).toBool());
        // The mail's own white card is inverted too: no white slab on dark.
        bool sawLightBackground = false;
        std::function<void(QTextFrame *)> walk = [&](QTextFrame *f) {
            for (QTextFrame *c : f->childFrames()) {
                const QBrush b = c->frameFormat().background();
                if (b.style() != Qt::NoBrush && b.color().lightness() > 200) {
                    sawLightBackground = true;
                }
                walk(c);
            }
        };
        walk(v.body()->document()->rootFrame());
        QVERIFY(!sawLightBackground);
        applyTheme(ThemeMode::Light);
    }

    void headerBlockAndAttachments()
    {
        MessageView v;
        v.setMessage(vetMessage());
        const QString h = v.headerText();
        QVERIFY(h.contains(QStringLiteral("Biscuit is due for a visit")));
        QVERIFY(h.contains(QStringLiteral("From: Maple Grove Animal Hospital <reminders@maplegrove-vet.example>")));
        QVERIFY(h.contains(QStringLiteral("To: Jordan Example <jordan@example.com>")));
        QVERIFY(h.contains(QStringLiteral("Cc: Sam Example <sam@example.com>")));
        // Shown in the viewer's local zone (CT on Stephen's desktop, UTC in CI).
        const QDateTime local = vetMessage().date.toLocalTime();
        QVERIFY(h.contains(QStringLiteral("Date: ") +
                           QLocale(QLocale::English, QLocale::UnitedStates)
                               .toString(local, QStringLiteral("dddd, MMMM d, yyyy 'at' h:mm AP"))));
        auto *att = v.findChild<QLabel *>(QStringLiteral("messageAttachments"));
        QVERIFY(att && !att->isHidden());
        QVERIFY(att->text().contains(QStringLiteral("2 attachments")));
        QVERIFY(att->text().contains(QStringLiteral("vaccine-record.pdf")));

        ViewMessage plain;
        plain.id = QStringLiteral("p");
        plain.from = QStringLiteral("a@example.com");
        plain.subject = QStringLiteral("hi");
        plain.bodyText = QStringLiteral("hello");
        v.setMessage(plain);
        QVERIFY(!v.headerText().contains(QStringLiteral("Cc:")));
        QVERIFY(att->isHidden());
    }

    static ViewMessage imageMessage(const QString &base, const QString &id, const QString &from)
    {
        ViewMessage m;
        m.id = id;
        m.from = from;
        m.subject = QStringLiteral("pics");
        m.bodyHtml = QStringLiteral("<p>Hi</p><img src='%1logo.png' width='40'>"
                                    "<img src='%1open.gif' width='1' height='1'>"     // 1x1 pixel
                                    "<img src='%1wf/open?upn=abc' style='border:0'>" // SendGrid-style open tracker
                                    "<img src='cid:part1'>")
                         .arg(base);
        return m;
    }

    void remoteImagesAskByDefaultAlwaysLoads()
    {
        // No setting at all: Ask (security review; Always until then).
        QCOMPARE(RemoteImages::mode(), RemoteImageMode::Ask);
        QVERIFY(RemoteImages::blockTrackers());
        serveImagesLocally();
        ImageServer server;
        const QString base = QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort());
        MessageView v;
        v.resize(600, 400);
        v.show();
        v.setMessage(imageMessage(base, QStringLiteral("a0"), QStringLiteral("Shop <news@shop.example.com>")));
        auto *bar = v.findChild<QWidget *>(QStringLiteral("remoteImagesBar"));
        QVERIFY(!bar->isHidden());
        QVERIFY(!v.imagesLoaded());
        QTest::qWait(200);
        QVERIFY(server.paths.isEmpty());

        // Always (a saved choice, e.g. from before the default changed):
        // loads, trackers dropped.
        QSettings().setValue(QStringLiteral("privacy/remoteImages"), QStringLiteral("always"));
        v.setMessage(imageMessage(base, QStringLiteral("a"), QStringLiteral("Shop <news@shop.example.com>")));
        QVERIFY(bar->isHidden());
        QVERIFY(v.imagesLoaded());
        QCOMPARE(v.trackersBlocked(), 2);
        QTRY_COMPARE_WITH_TIMEOUT(server.paths.size(), 1, 5000);
        QTest::qWait(300);
        QCOMPARE(server.paths, QStringList{QStringLiteral("/logo.png")}); // no pixels, no cid:
        QTRY_VERIFY(!v.body()->loadResource(QTextDocument::ImageResource, QUrl(base + QStringLiteral("logo.png")))
                         .value<QImage>()
                         .isNull());

        // Safeguards: the server set a cookie; it is never sent back.
        ViewMessage m2 = imageMessage(base, QStringLiteral("b"), QStringLiteral("Shop <news@shop.example.com>"));
        m2.bodyHtml.replace(QStringLiteral("logo.png"), QStringLiteral("logo2.png"));
        v.setMessage(m2);
        QTRY_COMPARE_WITH_TIMEOUT(server.paths.size(), 2, 5000);
        for (const QByteArray &r : std::as_const(server.requests)) {
            QVERIFY2(!r.toLower().contains("\r\ncookie:"), r.constData());
            QVERIFY(!r.toLower().contains("authorization:"));
        }

        // Tracker blocking off: the pixels load too.
        RemoteImages::setBlockTrackers(false);
        server.paths.clear();
        v.setMessage(imageMessage(base, QStringLiteral("c"), QStringLiteral("x@shop.example.com")));
        QCOMPARE(v.trackersBlocked(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(server.paths.contains(QStringLiteral("/open.gif")) &&
                                     server.paths.contains(QStringLiteral("/wf/open?upn=abc")),
                                 5000);
    }

    void trackingPixelsAreRecognised()
    {
        using namespace RemoteImages;
        QVERIFY(isTrackerImgTag(QStringLiteral("<img src='https://a.example.com/x.png' width='1' height='1'>")));
        QVERIFY(isTrackerImgTag(QStringLiteral("<img src=\"https://a.example.com/x.png\" height=\"0\">")));
        QVERIFY(isTrackerImgTag(QStringLiteral("<img src='https://a.example.com/x.png' style='width:1px;height:1px'>")));
        QVERIFY(isTrackerImgTag(QStringLiteral("<img src='https://mailtrack.io/trace/mail/abc.png'>")));
        QVERIFY(isTrackerImgTag(QStringLiteral("<img src='https://x.list-manage.com/track/open.php?u=1'>")));
        QVERIFY(isTrackerImgTag(QStringLiteral("<img src='https://u123.ct.sendgrid.net/wf/open?upn=z'>")));
        QVERIFY(!isTrackerImgTag(QStringLiteral("<img src='https://cdn.example.com/hero.jpg' width='600'>")));
        QVERIFY(!isTrackerImgTag(QStringLiteral("<img src='https://cdn.example.com/icon.png' width='16' height='16'>")));
        QVERIFY(!isTrackerImgTag(QStringLiteral("<img src='https://cdn.example.com/open-house.png' width='120'>")));
        QVERIFY(!isTrackerImgTag(QStringLiteral("<img src='https://cdn.example.com/a.png' style='max-width:100%'>")));
    }

    void remoteImagesAskModeBarAndSenderList()
    {
        serveImagesLocally();
        RemoteImages::setMode(RemoteImageMode::Ask);
        ImageServer server;
        const QString base = QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort());
        const QString vet = QStringLiteral("Maple Grove Vet <Reminders@MapleGrove-Vet.example.com>");
        MessageView v;
        v.resize(700, 400);
        v.show();
        v.setMessage(imageMessage(base, QStringLiteral("img"), vet));
        auto *bar = v.findChild<QWidget *>(QStringLiteral("remoteImagesBar"));
        QVERIFY(bar && !bar->isHidden());
        auto *always = v.findChild<QPushButton *>(QStringLiteral("alwaysForSenderButton"));
        QVERIFY(always && !always->isHidden());
        QCOMPARE(v.blockedImages(), 4);
        QTest::qWait(200);
        QVERIFY(server.paths.isEmpty());
        QCOMPARE(v.body()->remoteFetches(), 0);

        // "Load images": this message only.
        v.findChild<QPushButton *>(QStringLiteral("loadImagesButton"))->click();
        QVERIFY(v.imagesLoaded());
        QVERIFY(bar->isHidden());
        QTRY_COMPARE_WITH_TIMEOUT(server.paths.size(), 1, 5000); // trackers and cid: never fetched
        QTest::qWait(300);
        QCOMPARE(server.paths.size(), 1); // each image once, re-renders use the cache
        QVERIFY(RemoteImages::allowedSenders().isEmpty());

        v.setMessage(imageMessage(base, QStringLiteral("img2"), vet)); // next message: blocked again
        QVERIFY(!v.imagesLoaded());
        QVERIFY(!bar->isHidden());

        // "Always for this sender": loads now and remembers the address.
        always->click();
        QVERIFY(v.imagesLoaded());
        QCOMPARE(RemoteImages::allowedSenders(), QStringList{QStringLiteral("reminders@maplegrove-vet.example.com")});
        v.setMessage(imageMessage(base, QStringLiteral("img3"), QStringLiteral("reminders@maplegrove-vet.example.com")));
        QVERIFY(v.imagesLoaded());
        QVERIFY(bar->isHidden());
        // Someone else still asks.
        v.setMessage(imageMessage(base, QStringLiteral("img4"), QStringLiteral("Other <other@example.com>")));
        QVERIFY(!v.imagesLoaded());
        QVERIFY(!bar->isHidden());

        // No usable address: no "Always for this sender".
        v.setMessage(imageMessage(base, QStringLiteral("img5"), QStringLiteral("Undisclosed")));
        QVERIFY(!bar->isHidden());
        QVERIFY(always->isHidden());
    }

    void remoteImagesNeverMode()
    {
        serveImagesLocally();
        ImageServer server;
        const QString base = QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort());
        RemoteImages::allowSender(QStringLiteral("friend@example.com")); // the list doesn't override Never
        RemoteImages::setMode(RemoteImageMode::Never);
        MessageView v;
        v.resize(600, 400);
        v.show();
        v.setMessage(imageMessage(base, QStringLiteral("n1"), QStringLiteral("friend@example.com")));
        QVERIFY(!v.imagesLoaded());
        QCOMPARE(v.blockedImages(), 4);
        QVERIFY(v.findChild<QWidget *>(QStringLiteral("remoteImagesBar"))->isHidden()); // no bar to click
        QTest::qWait(300);
        QVERIFY(server.paths.isEmpty());
        QVERIFY(!v.body()->toHtml().contains(QStringLiteral("logo.png")));

        // Switching an open message's policy (Settings > Privacy, OK).
        RemoteImages::setMode(RemoteImageMode::Always);
        v.reloadImagePolicy();
        QVERIFY(v.imagesLoaded());
        QTRY_COMPARE_WITH_TIMEOUT(server.paths.size(), 1, 5000);
        RemoteImages::setMode(RemoteImageMode::Never);
        v.reloadImagePolicy();
        QVERIFY(!v.imagesLoaded());
        QVERIFY(!v.body()->toHtml().contains(QStringLiteral("logo.png")));
    }

    void privacySettingsPage()
    {
        MainWindow w;
        QAction *act = w.findChild<QAction *>(QStringLiteral("actionPrivacy"));
        QVERIFY(act);
        QVERIFY(w.findChild<QMenu *>(QStringLiteral("menuSettings"))->actions().contains(act));

        RemoteImages::setAllowedSenders({QStringLiteral("b@example.com"), QStringLiteral("A <a@example.com>")});
        PrivacyDialog *dlg = w.showPrivacyDialog();
        QVERIFY(dlg->findChild<QRadioButton *>(QStringLiteral("remoteImagesAsk"))->isChecked()); // default
        QVERIFY(dlg->findChild<QCheckBox *>(QStringLiteral("blockTrackers"))->isChecked());
        QCOMPARE(dlg->senders(), (QStringList{QStringLiteral("a@example.com"), QStringLiteral("b@example.com")}));

        auto *edit = dlg->findChild<QLineEdit *>(QStringLiteral("addSenderEdit"));
        auto *add = dlg->findChild<QPushButton *>(QStringLiteral("addSenderButton"));
        edit->setText(QStringLiteral("not an address"));
        QVERIFY(!add->isEnabled());
        edit->setText(QStringLiteral("Pat Example <PAT@Example.com>"));
        QVERIFY(add->isEnabled());
        add->click();
        QCOMPARE(dlg->senders().size(), 3);
        QVERIFY(dlg->senders().contains(QStringLiteral("pat@example.com")));
        dlg->senderList()->setCurrentRow(0); // a@example.com
        dlg->findChild<QPushButton *>(QStringLiteral("removeSenderButton"))->click();
        QCOMPARE(dlg->senders(), (QStringList{QStringLiteral("b@example.com"), QStringLiteral("pat@example.com")}));
        dlg->findChild<QRadioButton *>(QStringLiteral("remoteImagesAlways"))->click();

        // Nothing is saved before OK.
        QCOMPARE(RemoteImages::mode(), RemoteImageMode::Ask);
        QVERIFY(!QSettings().contains(QStringLiteral("privacy/remoteImages")));
        dlg->save();
        QCOMPARE(RemoteImages::mode(), RemoteImageMode::Always);
        QCOMPARE(RemoteImages::allowedSenders(),
                 (QStringList{QStringLiteral("b@example.com"), QStringLiteral("pat@example.com")}));
        delete dlg;

        // Clear all, Never, OK.
        dlg = w.showPrivacyDialog();
        QVERIFY(dlg->findChild<QRadioButton *>(QStringLiteral("remoteImagesAlways"))->isChecked());
        dlg->findChild<QPushButton *>(QStringLiteral("clearSendersButton"))->click();
        QCOMPARE(dlg->senderList()->count(), 0);
        dlg->findChild<QRadioButton *>(QStringLiteral("remoteImagesNever"))->click();
        dlg->open();
        dlg->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        QCOMPARE(RemoteImages::mode(), RemoteImageMode::Never);
        QVERIFY(RemoteImages::allowedSenders().isEmpty());
        QVERIFY(!QSettings().contains(QStringLiteral("privacy/remoteImageSenders")));
    }

    void senderAddressesAreNormalised()
    {
        using RemoteImages::senderAddress;
        QCOMPARE(senderAddress(QStringLiteral("\"Doe, Jane\" <Jane.Doe@Example.COM>")), QStringLiteral("jane.doe@example.com"));
        QCOMPARE(senderAddress(QStringLiteral("  bob@example.org ")), QStringLiteral("bob@example.org"));
        QCOMPARE(senderAddress(QStringLiteral("mailto:x@y.example")), QStringLiteral("x@y.example"));
        QVERIFY(senderAddress(QStringLiteral("Undisclosed recipients")).isEmpty());
        QVERIFY(senderAddress(QString()).isEmpty());
        QCOMPARE(RemoteImages::modeFromKey(QStringLiteral("bogus")), RemoteImageMode::Ask);
    }

    void zoomShortcutsAndZoomToFit()
    {
        MainWindow w;
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        MessageView *v = w.messageView();
        QCOMPARE(v->zoom(), 1.0);
        auto *zin = w.findChild<QAction *>(QStringLiteral("actionZoomIn"));
        QVERIFY(zin->shortcuts().contains(QKeySequence(Qt::CTRL | Qt::Key_Equal)));
        QVERIFY(zin->shortcuts().contains(QKeySequence(QKeySequence::ZoomIn)));
        QCOMPARE(w.findChild<QAction *>(QStringLiteral("actionZoomReset"))->shortcut(),
                 QKeySequence(Qt::CTRL | Qt::Key_0));
        zin->trigger();
        zin->trigger();
        QCOMPARE(v->zoom(), 1.2);
        QCOMPARE(QSettings().value(QStringLiteral("viewer/zoom")).toDouble(), 1.2);
        w.findChild<QAction *>(QStringLiteral("actionZoomOut"))->trigger();
        QCOMPARE(v->zoom(), 1.1);
        w.findChild<QAction *>(QStringLiteral("actionZoomReset"))->trigger();
        QCOMPARE(v->zoom(), 1.0);

        // Explicit font sizes scale with zoom.
        ViewMessage m;
        m.id = QStringLiteral("z");
        m.bodyHtml = QStringLiteral("<p style='font-size:20px'>Big</p><pre>%1</pre>").arg(QString(120, QLatin1Char('x')));
        MessageView mv;
        mv.resize(600, 400);
        mv.show();
        mv.setMessage(m);
        // Unbreakable 120-char line: zoomed to fit rather than scrolled.
        QVERIFY(mv.effectiveZoom() < 1.0);
        QVERIFY(mv.body()->document()->size().width() <= mv.body()->viewport()->width() + 40);
    }

    void splitterAndPreviewLayoutAreRemembered()
    {
        {
            // Default split favours the message (about 35/65).
            MainWindow w;
            w.resize(1200, 900);
            w.show();
            QVERIFY(QTest::qWaitForWindowExposed(&w));
            const QList<int> sz = w.findChild<QSplitter *>(QStringLiteral("listPreviewSplitter"))->sizes();
            QVERIFY2(sz.value(1) > 1.6 * sz.value(0), qPrintable(QStringLiteral("%1/%2").arg(sz.value(0)).arg(sz.value(1))));
            w.close();
            QSettings().clear();
        }
        {
            MainWindow w;
            auto *split = w.findChild<QSplitter *>(QStringLiteral("listPreviewSplitter"));
            QCOMPARE(split->orientation(), Qt::Vertical);
            QVERIFY(!w.previewRight());
            w.setPreviewRight(true);
            QCOMPARE(split->orientation(), Qt::Horizontal);
            QVERIFY(w.findChild<QAction *>(QStringLiteral("actionPreviewRight"))->isChecked());
            split->setSizes({300, 700});
            w.close();
        }
        QVERIFY(QSettings().value(QStringLiteral("ui/previewRight")).toBool());
        MainWindow again;
        auto *split = again.findChild<QSplitter *>(QStringLiteral("listPreviewSplitter"));
        QCOMPARE(split->orientation(), Qt::Horizontal);
        QVERIFY(again.findChild<QAction *>(QStringLiteral("actionPreviewRight"))->isChecked());
        QVERIFY(!QSettings().value(QStringLiteral("ui/listSplitterRight")).toByteArray().isEmpty());
        again.setPreviewRight(false);
        QCOMPARE(split->orientation(), Qt::Vertical);
        QVERIFY(split->handleWidth() >= 4);
    }

    void doubleClickOpensMessageWindows()
    {
        MainWindow w;
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto *list = w.findChild<QTreeView *>(QStringLiteral("messageList"));
        const QString subject =
            list->model()->index(0, 7).data().toString(); // MessageListModel::Subject
        QSignalSpy dbl(list, &QAbstractItemView::doubleClicked);
        const QPoint at = list->visualRect(list->model()->index(0, 7)).center();
        // A real double-click: press/release, then the double-click event.
        QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, at);
        QTest::mouseDClick(list->viewport(), Qt::LeftButton, {}, at);
        QCOMPARE(dbl.count(), 1);
        QTRY_COMPARE(w.messageWindows().size(), 1);
        MessageWindow *mw = w.messageWindows().first();
        QCOMPARE(mw->windowTitle(), subject);
        QVERIFY(mw->view()->headerText().contains(QStringLiteral("From: ")));
        QVERIFY(mw->replyAction()->isEnabled());
        QVERIFY(!mw->deleteAction()->isEnabled()); // sample data: nothing to delete at Google
        QVERIFY(mw->replyAllAction()->isEnabled());
        QVERIFY(mw->forwardAction()->isEnabled());
        mw->forwardAction()->trigger(); // sample data: opens the composer
        QTRY_COMPARE(w.composers().size(), 1);
        w.composers().first()->setConfirmOnClose(false);

        QVERIFY(QTest::qWaitForWindowExposed(mw));
        MessageWindow *second = w.openMessageWindow(list->model()->index(1, 0));
        QVERIFY(second && second != mw);
        QCOMPARE(w.messageWindows().size(), 2);
        QVERIFY(second->pos() != mw->pos()); // cascaded

        mw->resize(700, 500);
        mw->close();
        QTRY_COMPARE(w.messageWindows().size(), 1);
        QVERIFY(!QSettings().value(QStringLiteral("messageWindow/geometry")).toByteArray().isEmpty());
        second->close();
    }

    void plainTextHasAComfortableMeasure()
    {
        MessageView v;
        v.resize(1400, 600);
        v.show();
        ViewMessage m;
        m.id = QStringLiteral("t");
        m.bodyText = QString(60, QLatin1Char('w')).replace(QLatin1Char('w'), QStringLiteral("word ")) +
                     QStringLiteral("\\nsee https://example.com/a?b=1&c=2.");
        v.setMessage(m);
        QCOMPARE(v.body()->lineWrapMode(), QTextEdit::FixedPixelWidth);
        QVERIFY(v.body()->lineWrapColumnOrWidth() < 1000);
        QVERIFY(v.body()->toHtml().contains(QStringLiteral("href=\"https://example.com/a?b=1&amp;c=2\"")));
    }

    // Newsletter mail nests tables 15-20 deep, and QTextDocument lays a
    // nested table's cells out several times per level. An <img> with no
    // width/height is measured on every one of those passes; handed over as
    // a QImage, each measurement copied the whole image into a new QPixmap,
    // and the window stopped responding for minutes (0.5.5).
    void deeplyNestedTablesWithAnImageRenderPromptly()
    {
        QImage img(600, 300, QImage::Format_ARGB32);
        img.fill(Qt::red);
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        img.save(&buf, "PNG");
        const QString src = QStringLiteral("data:image/png;base64,") + QString::fromLatin1(png.toBase64());
        QVERIFY(v_isPixmap(src));

        constexpr int depth = 24; // 2^24 cell layouts with no limit
        QString html;
        for (int i = 0; i < depth; ++i) {
            html += QStringLiteral("<table width=\"100%\"><tr><td>");
        }
        html += QStringLiteral("<img src=\"%1\"><p>Hello</p>").arg(src);
        for (int i = 0; i < depth; ++i) {
            html += QStringLiteral("</td></tr></table>");
        }
        MessageView v;
        v.resize(900, 600);
        v.show();
        ViewMessage m;
        m.id = QStringLiteral("nested");
        m.bodyHtml = html;
        QElapsedTimer timer;
        timer.start();
        v.setMessage(m);
        qInfo() << "nested tables rendered in" << timer.elapsed() << "ms";
        QVERIFY2(timer.elapsed() < 5000, qPrintable(QStringLiteral("%1 ms").arg(timer.elapsed())));
        QVERIFY(v.body()->toPlainText().contains(QStringLiteral("Hello")));
    }

    static int tableDepth(const QString &html)
    {
        static const QRegularExpression tag(QStringLiteral("<(/?)table\\b"), QRegularExpression::CaseInsensitiveOption);
        int depth = 0, deepest = 0;
        for (auto it = tag.globalMatch(html); it.hasNext();) {
            depth += it.next().capturedLength(1) > 0 ? -1 : 1;
            deepest = std::max(deepest, depth);
        }
        return deepest;
    }

    void tableNestingIsLimited()
    {
        const auto wrap = [](int n, const QString &open, const QString &inner) {
            QString h = inner;
            for (int i = 0; i < n; ++i) {
                h = open + h + QStringLiteral("</td></tr></table>");
            }
            return h;
        };
        const QString plain = QStringLiteral("<table><tr><td align=\"center\">");
        const QString card = QStringLiteral("<table width=\"600\"><tr><td>");
        const QString button = QStringLiteral("<table><tr><td bgcolor=\"#112233\">");
        const QString grid = QStringLiteral("<table><tr><td>left</td><td>right</td></tr></table>");

        // Shallow mail is left exactly as it was.
        const QString shallow = wrap(3, plain, grid);
        QCOMPARE(HtmlFit::limitTableDepth(shallow, 4), shallow);

        // Too deep: plain one-cell wrappers go first, outermost first; the
        // card, the button and the two-column grid survive.
        const QString deep = wrap(6, plain, wrap(1, card, wrap(1, button, grid)));
        const QString out = HtmlFit::limitTableDepth(deep, 4);
        QCOMPARE(tableDepth(deep), 9);
        QCOMPARE(tableDepth(out), 4);
        QVERIFY(out.contains(QStringLiteral("width=\"600\"")));
        QVERIFY(out.contains(QStringLiteral("bgcolor=\"#112233\"")));
        QVERIFY(out.contains(grid));
        QVERIFY(out.startsWith(QStringLiteral("<div><div align=\"center\">"))); // the cell's alignment is kept
        QCOMPARE(out.count(QStringLiteral("<div")), out.count(QStringLiteral("</div>")));

        // Nothing but wrappers with widths and colours: widths go before colours.
        const QString styled = wrap(3, card, wrap(3, button, QStringLiteral("x")));
        const QString thinned = HtmlFit::limitTableDepth(styled, 4);
        QCOMPARE(tableDepth(thinned), 4);
        QCOMPARE(thinned.count(QStringLiteral("bgcolor")), 3);
        QCOMPARE(thinned.count(QStringLiteral("width=\"600\"")), 1);

        // Nothing but grids: the innermost are flattened, cells stacked, and
        // a cell with no </td> is still closed.
        QString grids = QStringLiteral("<p>core</p>");
        for (int i = 0; i < 6; ++i) {
            grids = QStringLiteral("<table><tr><td>a<td>") + grids + QStringLiteral("</table>");
        }
        const QString flat = HtmlFit::limitTableDepth(grids, 4);
        QCOMPARE(tableDepth(flat), 4);
        QVERIFY(flat.contains(QStringLiteral("<div><div>a</div><div><p>core</p></div></div>")));

        // Only the path that is too deep is touched; a stray </table> is harmless.
        const QString mixed = QStringLiteral("</table>") + shallow + wrap(6, plain, QStringLiteral("x"));
        const QString fixed = HtmlFit::limitTableDepth(mixed, 4);
        QVERIFY(fixed.startsWith(QStringLiteral("</table>") + shallow));
        QCOMPARE(tableDepth(fixed.mid(8)), 4);

        // prepare() applies the limit, counting the tables it adds itself.
        QString divs = QStringLiteral("x");
        for (int i = 0; i < 20; ++i) {
            divs = QStringLiteral("<div style=\"background-color:#eeeeee\">") + divs + QStringLiteral("</div>");
        }
        QCOMPARE(tableDepth(HtmlFit::prepare(divs)), HtmlFit::kMaxTableDepth);
        QCOMPARE(tableDepth(HtmlFit::prepare(wrap(20, plain, grid))), HtmlFit::kMaxTableDepth);
    }

    // Nesting is not the only way to stall the window: a wide grid is slow
    // at any depth. The estimate sees both, prepare() thins tables until the
    // estimate fits, and the view says so and offers the full layout.
    void layoutBudgetBoundsWideAndNestedTables()
    {
        const auto grid = [](int cols, int rows, const QString &cell) {
            QString h = QStringLiteral("<table width=\"100%\">");
            for (int r = 0; r < rows; ++r) {
                h += QStringLiteral("<tr>");
                for (int c = 0; c < cols; ++c) {
                    h += QStringLiteral("<td>") + cell + QStringLiteral("</td>");
                }
                h += QStringLiteral("</tr>");
            }
            return h + QStringLiteral("</table>");
        };
        const QString small = grid(3, 4, QStringLiteral("x"));
        const QString wide = grid(40, 40, QStringLiteral("x"));
        const QString nested = grid(2, 2, grid(2, 2, grid(2, 2, grid(2, 2, grid(2, 2, QStringLiteral("x"))))));

        // The estimate: cells times columns, doubled per level of nesting.
        QCOMPARE(qRound(HtmlFit::layoutCost(QStringLiteral("<p>hello</p>"))), 0);
        QVERIFY(HtmlFit::layoutCost(small) < 100);
        QVERIFY(HtmlFit::layoutCost(wide) > HtmlFit::kLayoutBudget);
        QVERIFY(HtmlFit::layoutCost(nested) > HtmlFit::layoutCost(grid(2, 2, grid(2, 2, QStringLiteral("x")))) * 8);
        QVERIFY(HtmlFit::layoutCost(grid(1, 1, small)) > HtmlFit::layoutCost(small) * 1.9); // one level down: twice the work

        // No budget: only the standing depth limit. With one: under it.
        int depth = -1;
        QCOMPARE(HtmlFit::prepare(wide, 0, &depth), HtmlFit::prepare(wide));
        QCOMPARE(depth, HtmlFit::kMaxTableDepth);
        const QString fitted = HtmlFit::prepare(wide, HtmlFit::kLayoutBudget, &depth);
        QCOMPARE(depth, 0); // a flat grid: nothing to give up but the grid itself
        QVERIFY(HtmlFit::layoutCost(fitted) <= HtmlFit::kLayoutBudget);
        QCOMPARE(tableDepth(fitted), 0);
        HtmlFit::prepare(nested, 600, &depth);
        QVERIFY2(depth > 0 && depth < 5, qPrintable(QString::number(depth))); // the inner levels go, the outer stay
        // Ordinary mail is left alone.
        for (const char *name : {"retail-rx.html"}) {
            HtmlFit::prepare(fixture(name), HtmlFit::kLayoutBudget, &depth);
            QCOMPARE(depth, HtmlFit::kMaxTableDepth);
        }

        MessageView v;
        v.resize(900, 600);
        v.show();
        auto *bar = v.findChild<QWidget *>(QStringLiteral("simplifiedLayoutBar"));
        QVERIFY(bar && bar->isHidden());
        ViewMessage m;
        m.id = QStringLiteral("wide");
        m.bodyHtml = wide;
        QElapsedTimer timer;
        timer.start();
        v.setMessage(m);
        const qint64 simplified = timer.elapsed();
        QVERIFY2(simplified < 1500, qPrintable(QStringLiteral("%1 ms").arg(simplified)));
        QVERIFY(v.layoutSimplified());
        QVERIFY(!bar->isHidden());
        QVERIFY(v.body()->toPlainText().contains(QLatin1Char('x'))); // the content is all there
        QCOMPARE(v.body()->document()->rootFrame()->childFrames().size(), 0); // as blocks, not a table

        // On request, the layout as sent (slow, and the user was told so).
        v.findChild<QPushButton *>(QStringLiteral("fullLayoutButton"))->click();
        QVERIFY(!v.layoutSimplified());
        QVERIFY(bar->isHidden());
        QCOMPARE(v.body()->document()->rootFrame()->childFrames().size(), 1);
        // The next message starts simplified again; a plain one shows no bar.
        ViewMessage plain;
        plain.id = QStringLiteral("plain");
        plain.bodyHtml = small;
        v.setMessage(plain);
        QVERIFY(bar->isHidden());
        m.id = QStringLiteral("wide again");
        v.setMessage(m);
        QVERIFY(!bar->isHidden());
        v.clear();
        QVERIFY(bar->isHidden());
    }

private:
    // Images reach QTextDocument as pixmaps (shared, not copied per layout).
    static bool v_isPixmap(const QString &src)
    {
        SafeHtmlView view;
        return view.loadResource(QTextDocument::ImageResource, QUrl(src)).userType() == QMetaType::QPixmap;
    }
};

QTEST_MAIN(TstViewer)
#include "tst_viewer.moc"
