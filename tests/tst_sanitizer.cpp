// Property tests for html::sanitize(): a corpus of hostile fragments plus
// seeded random combinations of them. Whatever goes in, what comes out must
// hold no active content, and rendering it must not ask for a single
// resource the viewer would have to block (file:, qrc:, paths, remote
// images while those are off): the sanitizer keeps them out of the document,
// SafeHtmlView::loadResource() is only the second line.
#include "core/HtmlSanitizer.h"
#include "ui/SafeHtmlView.h"
#include "ui/Theme.h"

#include <QAbstractTextDocumentLayout>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTest>
#include <QTextDocument>
#include <QTextDocumentFragment>

using namespace zmail;
using namespace zmail::ui;

namespace {
using RE = QRegularExpression;
const auto kOpts = RE::CaseInsensitiveOption | RE::DotMatchesEverythingOption;

// What an HTML parser makes of an attribute value: entities decoded, then
// (for a scheme check) whitespace and control characters dropped, lower case.
QString schemeView(const QString &rawValue)
{
    const QString decoded = QTextDocumentFragment::fromHtml(rawValue).toPlainText();
    QString out;
    for (const QChar c : decoded) {
        if (c.unicode() > 0x20 && c.unicode() != 0x7f) {
            out += c.toLower();
        }
    }
    return out;
}

// Empty if `out` is acceptable sanitizer output, else what is wrong with it.
// Reads tags and attributes itself (quotes may hold '>' and "on...=" text).
QString textualProblem(const QString &out, bool keptRemote)
{
    static const QStringList active{QStringLiteral("script"), QStringLiteral("style"),  QStringLiteral("iframe"),
                                    QStringLiteral("object"), QStringLiteral("embed"),  QStringLiteral("form"),
                                    QStringLiteral("input"),  QStringLiteral("link"),   QStringLiteral("meta"),
                                    QStringLiteral("base"),   QStringLiteral("frame"),  QStringLiteral("frameset"),
                                    QStringLiteral("applet"), QStringLiteral("svg"),    QStringLiteral("math")};
    static const RE tag(QStringLiteral("<([a-zA-Z][a-zA-Z0-9:-]*)((?:\"[^\"]*(?:\"|$)|'[^']*(?:'|$)|[^'\">])*)(>|$)"));
    static const RE attr(QStringLiteral("([^\\s\"'>/=]+)(?:\\s*=\\s*(\"[^\"]*\"|'[^']*'|[^\\s\"'>]+))?"));
    for (auto it = tag.globalMatch(out); it.hasNext();) {
        const auto m = it.next();
        const QString name = m.captured(1).toLower();
        if (active.contains(name)) {
            return QStringLiteral("an active <%1> tag survived").arg(name);
        }
        if (m.captured(3).isEmpty()) {
            return QStringLiteral("an unterminated <%1> tag survived").arg(name);
        }
        for (auto at = attr.globalMatch(m.captured(2)); at.hasNext();) {
            const auto a = at.next();
            const QString key = a.captured(1).toLower();
            QString value = a.captured(2);
            if (value.size() >= 2 && (value.front() == QLatin1Char('"') || value.front() == QLatin1Char('\''))) {
                value = value.mid(1, value.size() - 2);
            }
            const QString v = schemeView(value);
            if (key.size() > 2 && key.startsWith(QLatin1String("on"))) {
                return QStringLiteral("the event handler %1 survived").arg(key);
            }
            if ((key == QLatin1String("href") || key == QLatin1String("src") || key == QLatin1String("background")) &&
                (v.startsWith(QLatin1String("javascript:")) || v.startsWith(QLatin1String("vbscript:")) ||
                 v.startsWith(QLatin1String("file:")))) {
                return QStringLiteral("%1 kept a %2 URL").arg(key, v.left(11));
            }
            if (name == QLatin1String("img") && key == QLatin1String("src")) {
                const bool remote = v.startsWith(QLatin1String("http:")) || v.startsWith(QLatin1String("https:")) ||
                                    v.startsWith(QLatin1String("//"));
                if (v.startsWith(QLatin1String("cid:")) || (remote && !keptRemote)) {
                    return QStringLiteral("an image that may not load survived (%1)").arg(v.left(24));
                }
                if (!remote && !v.startsWith(QLatin1String("data:image/"))) {
                    return QStringLiteral("an image with a local or unknown source survived (%1)").arg(v.left(24));
                }
            }
        }
    }
    return {};
}

// Renders `html` in the viewer (remote images off) and returns how many
// resource requests had to be blocked at load time.
struct Rendered
{
    int blocked = 0;
    int fetches = 0;
};
Rendered render(SafeHtmlView &view, const QString &html)
{
    view.resetBlocked();
    const int fetchesBefore = view.remoteFetches();
    view.setHtml(html);
    view.document()->setTextWidth(600);
    (void)view.document()->documentLayout()->documentSize(); // lay out: images are requested here
    return {view.blockedLoads(), view.remoteFetches() - fetchesBefore};
}

const QStringList &hostileUrls()
{
    static const QStringList v{
        QStringLiteral("file:///etc/passwd"),
        QStringLiteral("FILE:///etc/passwd"),
        QStringLiteral("file:/etc/passwd"),
        QStringLiteral("  file:///etc/passwd"),
        QStringLiteral("fi\tle:///etc/passwd"),
        QStringLiteral("fi\nle:///etc/passwd"),
        QStringLiteral("&#102;ile:///etc/passwd"),
        QStringLiteral("&#x66;ile:///etc/passwd"),
        QStringLiteral("&#x66ile:///etc/passwd"),
        QStringLiteral("file&colon;///etc/passwd"),
        QStringLiteral("file&#58;///etc/passwd"),
        QStringLiteral("/etc/passwd"),
        QStringLiteral("/dev/zero"),
        QStringLiteral("../../secret.png"),
        QStringLiteral("secret.png"),
        QStringLiteral("./secret.png"),
        QStringLiteral("~/secret.png"),
        QStringLiteral("C:\\Users\\x\\secret.png"),
        QStringLiteral("\\\\host\\share\\x.png"),
        QStringLiteral("qrc:/icons/lucide/mail.svg"),
        QStringLiteral(":/icons/lucide/mail.svg"),
        QStringLiteral("javascript:alert(1)"),
        QStringLiteral("JaVaScRiPt:alert(1)"),
        QStringLiteral("jav&#x61;script:alert(1)"),
        QStringLiteral("java\tscript:alert(1)"),
        QStringLiteral("vbscript:msgbox(1)"),
        QStringLiteral("data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg'><image href='file:///etc/passwd'/></svg>"),
        QStringLiteral("data:text/html,<script>alert(1)</script>"),
        QStringLiteral("DATA:IMAGE/SVG+XML;base64,PHN2Zy8+"),
        QStringLiteral("http://tracker.example/p.gif"),
        QStringLiteral("https://tracker.example/p.gif?u=1"),
        QStringLiteral("HTTPS://TRACKER.EXAMPLE/p.gif"),
        QStringLiteral("//tracker.example/p.gif"),
        QStringLiteral("ht\ntps://tracker.example/p.gif"),
        QStringLiteral("&#104;ttps://tracker.example/p.gif"),
        QStringLiteral("cid:part1@example"),
        QStringLiteral("ftp://host.example/x.png"),
        QStringLiteral("about:blank"),
        QStringLiteral("blob:https://x.example/1"),
        QStringLiteral(""),
        QStringLiteral("x\" onerror=\"alert(1)"),
        QStringLiteral("x' onerror='alert(1)"),
        QStringLiteral("x> <img src=file:///etc/passwd"),
    };
    return v;
}

const QStringList &corpus()
{
    static const QStringList v{
        QStringLiteral("<script>alert(1)</script>"),
        QStringLiteral("<SCRIPT SRC=http://x.example/a.js></SCRIPT>"),
        QStringLiteral("<script>alert(1)"),
        QStringLiteral("<scr<script>ipt>alert(1)</scr</script>ipt>"),
        QStringLiteral("<script\n>alert(1)</script\n>"),
        QStringLiteral("<iframe src=file:///etc/passwd></iframe>"),
        QStringLiteral("<object data=file:///etc/passwd></object><embed src=file:///etc/passwd>"),
        QStringLiteral("<link rel=stylesheet href=file:///etc/passwd><meta http-equiv=refresh content='0;url=file:///x'>"),
        QStringLiteral("<base href=file:///etc/>"),
        QStringLiteral("<form action=https://x.example><input name=a><button>go</button></form>"),
        QStringLiteral("<img src=file:///etc/passwd>"),
        QStringLiteral("<img src='/etc/passwd'>"),
        QStringLiteral("<img\nsrc\n=\n\"file:///etc/passwd\"\n>"),
        QStringLiteral("<IMG SRC=secret.png>"),
        QStringLiteral("<img src=x onerror=alert(1)>"),
        QStringLiteral("<img alt='>' src=file:///etc/passwd>"),
        QStringLiteral("<img alt=\"a>b\" src=\"file:///etc/passwd\">"),
        QStringLiteral("<img src=\"file:///etc/passwd"),
        QStringLiteral("<img src=file:///etc/passwd"),
        QStringLiteral("<img src=https://tracker.example/p.gif width=1 height=1>"),
        QStringLiteral("<img/src=file:///etc/passwd>"),
        QStringLiteral("< img src=file:///etc/passwd>"),
        QStringLiteral("<\timg src=file:///etc/passwd>"),
        QStringLiteral("<img\fsrc=file:///etc/passwd>"),
        QStringLiteral("<img src=file:///etc/passwd\u0000>"),
        QStringLiteral("<span title=\"x> <img src=file:///etc/passwd y=\">t</span>"),
        QStringLiteral("<span title='x> <img src=file:///etc/passwd y='>t</span>"),
        QStringLiteral("<span <img src=file:///etc/passwd>>t</span>"),
        QStringLiteral("<p title=<img src=file:///etc/passwd>>t</p>"),
        QStringLiteral("<img src=data:image/png;base64,iVBORw0KGgo= src=file:///etc/passwd>"),
        QStringLiteral("<img src=file:///etc/passwd src=data:image/png;base64,iVBORw0KGgo=>"),
        QStringLiteral("<img src = file:///etc/passwd >"),
        QStringLiteral("<body background=file:///etc/passwd onload=alert(1)>hi</body>"),
        QStringLiteral("<table background='/etc/passwd'><tr><td background=secret.png>x</td></tr></table>"),
        QStringLiteral("<div style=\"background:url(file:///etc/passwd)\">x</div>"),
        QStringLiteral("<div style='background-image: URL( \"/etc/passwd\" )'>x</div>"),
        QStringLiteral("<div style=\"background:url(&#102;ile:///etc/passwd)\">x</div>"),
        QStringLiteral("<p style=\"list-style-image:url(secret.png)\">x</p>"),
        QStringLiteral("<a href=\"javascript:alert(1)\">x</a><a href='file:///etc/passwd'>y</a>"),
        QStringLiteral("<a href=jav&#x61;script:alert(1) onclick=alert(1)>x</a>"),
        QStringLiteral("<svg><image href=file:///etc/passwd /></svg>"),
        QStringLiteral("<math><mi xlink:href=file:///etc/passwd>x</mi></math>"),
        QStringLiteral("<style>body{background:url(file:///etc/passwd)}</style>"),
        QStringLiteral("<style>@import url(file:///etc/passwd);"),
        QStringLiteral("<!--<img src=file:///etc/passwd>-->"),
        QStringLiteral("<!DOCTYPE html><html><head><title>t</title></head><body><p>plain</p></body></html>"),
        QStringLiteral("<video poster=file:///etc/passwd src=file:///etc/passwd></video>"),
        QStringLiteral("<img srcset='file:///etc/passwd 2x' src='data:image/png;base64,iVBORw0KGgo='>"),
        QStringLiteral("<p>caf\u00e9 &amp; cr\u00e8me <b>bold</b> <i>it</i></p>"),
        QStringLiteral("<img src=\"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==\">"),
    };
    return v;
}

// A random fragment built from the pools above.
QString randomFragment(QRandomGenerator &rng)
{
    static const QStringList tags{QStringLiteral("img"),    QStringLiteral("IMG"),   QStringLiteral("div"),
                                  QStringLiteral("table"),  QStringLiteral("td"),    QStringLiteral("body"),
                                  QStringLiteral("a"),      QStringLiteral("p"),     QStringLiteral("span"),
                                  QStringLiteral("script"), QStringLiteral("style"), QStringLiteral("iframe"),
                                  QStringLiteral("svg"),    QStringLiteral("link"),  QStringLiteral("object"),
                                  QStringLiteral("embed"),  QStringLiteral("input"), QStringLiteral("video"),
                                  QStringLiteral("font"),   QStringLiteral("th"),    QStringLiteral("tr")};
    static const QStringList attrs{QStringLiteral("src"),    QStringLiteral("SRC"),    QStringLiteral("background"),
                                   QStringLiteral("href"),   QStringLiteral("style"),  QStringLiteral("onerror"),
                                   QStringLiteral("onload"), QStringLiteral("srcset"), QStringLiteral("poster"),
                                   QStringLiteral("data"),   QStringLiteral("alt"),    QStringLiteral("width"),
                                   QStringLiteral("class"),  QStringLiteral("title")};
    static const QStringList gaps{QStringLiteral(" "), QStringLiteral("\n"), QStringLiteral("\t"), QStringLiteral("  "),
                                  QStringLiteral("/"), QStringLiteral(" \r\n ")};
    auto pick = [&rng](const QStringList &l) { return l.at(rng.bounded(int(l.size()))); };

    QString out;
    const int parts = 1 + rng.bounded(4);
    for (int p = 0; p < parts; ++p) {
        if (rng.bounded(5) == 0) {
            out += pick(corpus());
            continue;
        }
        const QString tag = pick(tags);
        out += QLatin1Char('<') + tag;
        const int n = rng.bounded(4);
        for (int i = 0; i < n; ++i) {
            const QString name = pick(attrs);
            QString value = pick(hostileUrls());
            if (name.compare(QLatin1String("style"), Qt::CaseInsensitive) == 0) {
                value = QStringLiteral("background:url(%1);color:red").arg(value);
            }
            out += pick(gaps) + name;
            switch (rng.bounded(6)) {
            case 0: out += QLatin1Char('=') + value; break;                               // unquoted
            case 1: out += QStringLiteral("='") + value + QLatin1Char('\''); break;
            case 2: out += QStringLiteral(" = \"") + value + QLatin1Char('"'); break;
            case 3: out += QStringLiteral("=\"") + value; break;                          // unterminated quote
            case 4: break;                                                                // no value
            default: out += QStringLiteral("=\"") + value + QLatin1Char('"'); break;
            }
        }
        switch (rng.bounded(5)) {
        case 0: break; // tag never closed
        case 1: out += QStringLiteral("/>"); break;
        default: out += QLatin1Char('>'); break;
        }
        if (rng.bounded(2)) {
            out += QStringLiteral("text") + (rng.bounded(3) ? QStringLiteral("</%1>").arg(tag) : QString());
        }
    }
    return out;
}
} // namespace

class TstSanitizer : public QObject
{
    Q_OBJECT

private:
    // The whole contract, for one input, in both remote-image modes.
    static QString check(SafeHtmlView &view, const QString &input)
    {
        for (const bool keepRemote : {false, true}) {
            const QString out = html::sanitize(input, nullptr, keepRemote);
            const QString problem = textualProblem(out, keepRemote);
            if (!problem.isEmpty()) {
                return QStringLiteral("%1 (keepRemote=%2)\n  in:  %3\n  out: %4").arg(problem).arg(keepRemote).arg(input, out);
            }
            if (!keepRemote) {
                const Rendered r = render(view, out);
                if (r.fetches != 0) {
                    return QStringLiteral("a network fetch started\n  in:  %1\n  out: %2").arg(input, out);
                }
                if (r.blocked != 0) {
                    return QStringLiteral("%1 resource request(s) reached the viewer\n  in:  %2\n  out: %3")
                        .arg(r.blocked)
                        .arg(input, out);
                }
            }
        }
        return {};
    }

private slots:
    void initTestCase() { applyTheme(ThemeMode::Light); }

    void corpusIsNeutralised()
    {
        SafeHtmlView view;
        view.setRemoteImagesAllowed(false);
        for (const QString &input : corpus()) {
            const QString problem = check(view, input);
            QVERIFY2(problem.isEmpty(), qPrintable(problem));
        }
    }

    void everyHostileUrlInEveryResourceAttribute()
    {
        SafeHtmlView view;
        view.setRemoteImagesAllowed(false);
        const QStringList shapes{
            QStringLiteral("<img src=\"%1\">"),
            QStringLiteral("<img src='%1'>"),
            QStringLiteral("<img src=%1>"),
            QStringLiteral("<img alt=\"a>b\" src=\"%1\">"),
            QStringLiteral("<body background=\"%1\">x</body>"),
            QStringLiteral("<table background='%1'><tr><td background=\"%1\">x</td></tr></table>"),
            QStringLiteral("<div style=\"background:url(%1)\">x</div>"),
            QStringLiteral("<div style=\"background-image:url('%1')\">x</div>"),
            QStringLiteral("<a href=\"%1\"><img src=\"%1\"></a>"),
        };
        for (const QString &url : hostileUrls()) {
            for (const QString &shape : shapes) {
                const QString problem = check(view, shape.arg(url));
                QVERIFY2(problem.isEmpty(), qPrintable(problem));
            }
        }
    }

    // Seeded: a failure prints the input and reproduces on every run.
    void randomCombinations()
    {
        SafeHtmlView view;
        view.setRemoteImagesAllowed(false);
        // ZMAIL_FUZZ_CASES / ZMAIL_FUZZ_SEED: a longer or different run by hand.
        const int cases = qEnvironmentVariableIntValue("ZMAIL_FUZZ_CASES") > 0 ? qEnvironmentVariableIntValue("ZMAIL_FUZZ_CASES") : 4000;
        const quint32 seed = qEnvironmentVariableIsSet("ZMAIL_FUZZ_SEED") ? qEnvironmentVariable("ZMAIL_FUZZ_SEED").toUInt() : 20261006;
        QRandomGenerator rng(seed);
        for (int i = 0; i < cases; ++i) {
            const QString problem = check(view, randomFragment(rng));
            QVERIFY2(problem.isEmpty(), qPrintable(QStringLiteral("case %1: %2").arg(i).arg(problem)));
        }
    }

    // Ordinary mail keeps its content.
    void harmlessMarkupSurvives()
    {
        const QString in = QStringLiteral(
            "<p>Hello <b>there</b>, see <a href=\"https://example.org/x?a=1&amp;b=2\">this</a>.</p>"
            "<table><tr><td style=\"color:#333;padding:4px\">cell</td></tr></table>"
            "<img alt=\"dot\" src=\"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==\">");
        const QString out = html::sanitize(in);
        QVERIFY(out.contains(QStringLiteral("<b>there</b>")));
        QVERIFY(out.contains(QStringLiteral("href=\"https://example.org/x?a=1&amp;b=2\"")));
        QVERIFY(out.contains(QStringLiteral("color:#333")));
        QVERIFY(out.contains(QStringLiteral("data:image/png;base64,")));
        QCOMPARE(html::sanitize(out), out); // nothing left to remove
        // Words that merely look like an event handler in the text stay.
        const QString prose = QStringLiteral("<p>phase one = done, only = half; onward =&gt; next</p>");
        QCOMPARE(html::sanitize(prose), prose);
        // '>' inside a quoted attribute doesn't end the tag early.
        QCOMPARE(html::sanitize(QStringLiteral("<body title=\"a>b\" onload=\"x()\"><p>kept</p></body>")),
                 QStringLiteral("<p>kept</p>"));
        int blocked = 0;
        QCOMPARE(html::sanitize(QStringLiteral("<img alt=\"a>b\" src=\"https://tracker.example/p.gif\">ok"), &blocked),
                 QStringLiteral("ok"));
        QCOMPARE(blocked, 1);
        QCOMPARE(html::sanitize(QStringLiteral("<a href=\"jav&#x61;script:alert(1)\" onclick='x()'>t</a>")),
                 QStringLiteral("<a href=\"#\">t</a>"));
        QCOMPARE(html::sanitize(QStringLiteral("before<script>alert(1)")), QStringLiteral("before"));
    }

    // The regular expressions must stay linear on adversarial input.
    void pathologicalInputFinishesQuickly()
    {
        const QStringList inputs{
            QString(200'000, QLatin1Char('<')),
            QStringLiteral("<img ") + QString(100'000, QLatin1Char('"')),
            QStringLiteral("<script>") + QString(200'000, QLatin1Char('a')),
            QStringLiteral("<a ") + QStringLiteral("x=\"y\" ").repeated(20'000) + QLatin1Char('>'),
            QStringLiteral("<div style=\"") + QStringLiteral("url(").repeated(30'000) + QStringLiteral("\">"),
            QStringLiteral("<img src='") + QString(300'000, QLatin1Char('a')),
        };
        for (const QString &in : inputs) {
            QElapsedTimer t;
            t.start();
            const QString out = html::sanitize(in);
            QVERIFY2(t.elapsed() < 5000, qPrintable(QStringLiteral("%1 ms for input starting %2").arg(t.elapsed()).arg(in.left(20))));
            QVERIFY(textualProblem(out, false).isEmpty());
        }
    }
};

QTEST_MAIN(TstSanitizer)
#include "tst_sanitizer.moc"
