// Message viewer: layout of builder-made HTML mail, light page, header block,
// blocked remote images, zoom, splitter/layout memory, message windows.
#include "MainWindow.hpp"
#include "ui/ComposeWindow.h"
#include "ui/HtmlFit.h"
#include "ui/MessageView.h"
#include "ui/MessageWindow.h"
#include "ui/SafeHtmlView.h"
#include "ui/Theme.h"

#include <QAbstractTextDocumentLayout>
#include <QAction>
#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextBlock>
#include <QTextDocument>
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
                    pending->remove(0, end + 4);
                    s->write("HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nConnection: close\r\nContent-Length: " +
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

    void remoteImagesBlockedUntilAsked()
    {
        ImageServer server;
        const QString base = QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort());
        ViewMessage m;
        m.id = QStringLiteral("img");
        m.subject = QStringLiteral("pics");
        m.bodyHtml = QStringLiteral("<p>Hi</p><img src='%1logo.png' width='40'><img src='%1open.gif' width='1' height='1'>"
                                    "<img src='cid:part1'>")
                         .arg(base);
        MessageView v;
        v.resize(600, 400);
        v.show();
        v.setMessage(m);
        auto *bar = v.findChild<QWidget *>(QStringLiteral("remoteImagesBar"));
        QVERIFY(bar && !bar->isHidden());
        QCOMPARE(v.blockedImages(), 3);
        QTest::qWait(200);
        QVERIFY(server.paths.isEmpty());
        QCOMPARE(v.body()->remoteFetches(), 0);

        v.findChild<QPushButton *>(QStringLiteral("loadImagesButton"))->click();
        QVERIFY(v.imagesLoaded());
        QVERIFY(bar->isHidden());
        QTRY_COMPARE_WITH_TIMEOUT(server.paths.size(), 2, 5000); // cid: parts are never fetched
        QTest::qWait(300);
        QCOMPARE(server.paths.size(), 2); // each image once, re-renders use the cache
        QVERIFY(server.paths.contains(QStringLiteral("/logo.png")));
        QTRY_VERIFY(!v.body()->loadResource(QTextDocument::ImageResource, QUrl(base + QStringLiteral("logo.png")))
                         .value<QImage>()
                         .isNull());

        // Next message: blocked again.
        m.id = QStringLiteral("img2");
        v.setMessage(m);
        QVERIFY(!v.imagesLoaded());
        QVERIFY(!bar->isHidden());
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
};

QTEST_MAIN(TstViewer)
#include "tst_viewer.moc"
