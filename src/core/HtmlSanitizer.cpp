#include "HtmlSanitizer.h"

#include <QRegularExpression>
#include <QSet>

namespace zmail::html {

namespace {
using RE = QRegularExpression;
constexpr auto kOpts = RE::CaseInsensitiveOption | RE::DotMatchesEverythingOption;

// What QTextDocument's HTML parser will see in an attribute value, for the
// scheme check only: character references decoded, whitespace and control
// characters dropped (they're ignored inside a scheme), lower case.
QString normalizedForCheck(const QString &raw)
{
    static const RE ref(QStringLiteral("&(#[xX][0-9a-fA-F]+|#[0-9]+|[a-zA-Z]+);?"));
    QString v;
    qsizetype last = 0;
    for (auto it = ref.globalMatch(raw); it.hasNext();) {
        const auto m = it.next();
        v += QStringView(raw).mid(last, m.capturedStart() - last);
        last = m.capturedEnd();
        const QString e = m.captured(1);
        if (e.startsWith(QLatin1Char('#'))) {
            bool ok = false;
            const uint cp = e.startsWith(QLatin1String("#x"), Qt::CaseInsensitive) ? e.mid(2).toUInt(&ok, 16) : e.mid(1).toUInt(&ok);
            v += ok && cp > 0 && cp < 0x110000 ? QString::fromUcs4(reinterpret_cast<const char32_t *>(&cp), 1) : QStringLiteral("?");
        } else {
            static const QHash<QString, QString> named{{QStringLiteral("colon"), QStringLiteral(":")},
                                                       {QStringLiteral("sol"), QStringLiteral("/")},
                                                       {QStringLiteral("bsol"), QStringLiteral("\\")},
                                                       {QStringLiteral("period"), QStringLiteral(".")},
                                                       {QStringLiteral("amp"), QStringLiteral("&")},
                                                       {QStringLiteral("tab"), QStringLiteral("\t")},
                                                       {QStringLiteral("newline"), QStringLiteral("\n")}};
            v += named.value(e.toLower(), QStringLiteral("?")); // unknown: never matches an allowed scheme
        }
    }
    v += QStringView(raw).mid(last);
    QString out;
    out.reserve(v.size());
    for (const QChar c : std::as_const(v)) {
        if (c.unicode() > 0x20 && c.unicode() != 0x7f) {
            out += c;
        }
    }
    return out.toLower();
}

enum class Res { DataImage, Remote, Cid, Blocked };

Res classify(const QString &raw)
{
    const QString v = normalizedForCheck(raw);
    if (v.startsWith(QLatin1String("data:image/")) && !v.startsWith(QLatin1String("data:image/svg"))) {
        return Res::DataImage;
    }
    if (v.startsWith(QLatin1String("https:")) || v.startsWith(QLatin1String("http:")) || v.startsWith(QLatin1String("//"))) {
        return Res::Remote;
    }
    if (v.startsWith(QLatin1String("cid:"))) {
        return Res::Cid;
    }
    return Res::Blocked; // bare or relative paths, file:, qrc:, empty, anything else
}

QString unquote(const QString &v)
{
    if (v.size() >= 2 && (v.front() == QLatin1Char('"') || v.front() == QLatin1Char('\'')) && v.back() == v.front()) {
        return v.mid(1, v.size() - 2);
    }
    return v;
}

// CSS url(...) in a style attribute: anything that isn't an allowed scheme
// becomes url() (nothing to load). Remote ones only while remote images are on.
QString filterCssUrls(const QString &style, bool keepRemote)
{
    static const RE url(QStringLiteral("url\\s*\\(\\s*(\"[^\"]*\"|'[^']*'|[^)]*)\\s*\\)"), kOpts);
    QString out;
    qsizetype last = 0;
    for (auto it = url.globalMatch(style); it.hasNext();) {
        const auto m = it.next();
        const Res r = classify(unquote(m.captured(1).trimmed()));
        out += QStringView(style).mid(last, m.capturedStart() - last);
        out += r == Res::DataImage || (r == Res::Remote && keepRemote) ? m.captured(0) : QStringLiteral("url()");
        last = m.capturedEnd();
    }
    out += QStringView(style).mid(last);
    return out;
}

// Elements that never reach the document, whatever their attributes.
bool isDroppedElement(const QString &lowerName)
{
    static const QSet<QString> dropped{
        QStringLiteral("script"),   QStringLiteral("style"),    QStringLiteral("iframe"), QStringLiteral("object"),
        QStringLiteral("embed"),    QStringLiteral("form"),     QStringLiteral("input"),  QStringLiteral("textarea"),
        QStringLiteral("select"),   QStringLiteral("button"),   QStringLiteral("link"),   QStringLiteral("meta"),
        QStringLiteral("base"),     QStringLiteral("frame"),    QStringLiteral("frameset"), QStringLiteral("applet"),
        QStringLiteral("noscript"), QStringLiteral("template"), QStringLiteral("svg"),    QStringLiteral("math"),
        QStringLiteral("head"),     QStringLiteral("title"),    QStringLiteral("html"),   QStringLiteral("body")};
    return dropped.contains(lowerName);
}

// key="value" in the one spelling every HTML parser reads the same way:
// double quotes, with the characters that could end the value or the tag
// (or start another) written as entities.
QString quoted(const QString &key, QString value)
{
    value.replace(QLatin1Char('"'), QStringLiteral("&quot;"));
    value.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    value.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    return key + QStringLiteral("=\"") + value + QLatin1Char('"');
}

bool isAttributeName(const QString &key)
{
    static const RE name(QStringLiteral("^[a-zA-Z_][a-zA-Z0-9_:.-]*$"));
    return name.match(key).hasMatch();
}

// One start tag, rebuilt from the attributes that parse, each written
// name="value" (so no parser reads the result differently from this one):
//  - a dropped element, or an <img> whose src may not load, goes entirely
//    (returns an empty string);
//  - event handlers (on*) and resource attributes with a blocked scheme go;
//  - a link to javascript:/vbscript:/file: points at "#".
// Remote and cid: images are counted in *remoteImages.
QString filterTag(const QString &name, const QString &attrs, bool keepRemote, int *remoteImages)
{
    static const RE attr(QStringLiteral("([^\\s\"'>/=]+)(\\s*=\\s*(\"[^\"]*\"|'[^']*'|[^\\s\"'>]+))?"));
    const QString lower = name.toLower();
    if (isDroppedElement(lower)) {
        return {};
    }
    const bool img = lower == QLatin1String("img");
    QString out = QLatin1Char('<') + name;
    bool hasSrc = false;
    for (auto it = attr.globalMatch(attrs); it.hasNext();) {
        const auto m = it.next();
        const QString key = m.captured(1).toLower();
        const QString value = unquote(m.captured(3));
        if (!isAttributeName(key)) {
            continue; // debris ("<link", "?", ...) another parser might read differently
        }
        if (key.size() > 2 && key.startsWith(QLatin1String("on"))) {
            continue; // event handler
        }
        if (key == QLatin1String("src") || key == QLatin1String("background")) {
            const Res r = classify(value);
            if (img && key == QLatin1String("src")) {
                if (r == Res::Remote || r == Res::Cid) {
                    ++*remoteImages;
                }
                // cid: parts aren't fetched yet, so those go either way.
                if (r == Res::Blocked || r == Res::Cid || (r == Res::Remote && !keepRemote)) {
                    return {};
                }
                if (r == Res::Remote && normalizedForCheck(value).startsWith(QLatin1String("//"))) {
                    out += QLatin1Char(' ') + quoted(m.captured(1), QStringLiteral("https:") + value.trimmed());
                    hasSrc = true;
                    continue;
                }
                if (hasSrc) {
                    continue; // a second src: the parser would use only one, don't guess which
                }
                hasSrc = true;
            } else if (r == Res::Blocked || r == Res::Cid || (r == Res::Remote && !keepRemote)) {
                continue; // attribute dropped
            }
        } else if (key == QLatin1String("href") || key == QLatin1String("xlink:href") || key == QLatin1String("action") ||
                   key == QLatin1String("formaction")) {
            const QString v = normalizedForCheck(value);
            if (v.startsWith(QLatin1String("javascript:")) || v.startsWith(QLatin1String("vbscript:")) ||
                v.startsWith(QLatin1String("file:"))) {
                out += QLatin1Char(' ') + quoted(m.captured(1), QStringLiteral("#"));
                continue;
            }
        } else if (key == QLatin1String("style") && m.capturedLength(3) > 0) {
            out += QLatin1Char(' ') + quoted(m.captured(1), filterCssUrls(value, keepRemote));
            continue;
        }
        out += QLatin1Char(' ') + (m.capturedLength(3) > 0 ? quoted(m.captured(1), value) : m.captured(1));
    }
    if (img && !hasSrc) {
        return {}; // nothing to show; the document would still ask for a resource named ""
    }
    if (attrs.trimmed().endsWith(QLatin1Char('/'))) {
        out += QStringLiteral(" /");
    }
    return out + QLatin1Char('>');
}

QString filterTags(const QString &s, bool keepRemote, int *remoteImages)
{
    // Quoted values may contain '>'; the tag ends at the first '>' outside quotes.
    // An unterminated quote runs to the end (as Qt's parser reads it).
    static const RE tag(QStringLiteral("<([a-zA-Z][a-zA-Z0-9:-]*)((?:\"[^\"]*(?:\"|$)|'[^']*(?:'|$)|[^'\">])*)(?:>|$)"));
    QString out;
    out.reserve(s.size());
    qsizetype last = 0;
    for (auto it = tag.globalMatch(s); it.hasNext();) {
        const auto m = it.next();
        out += QStringView(s).mid(last, m.capturedStart() - last);
        out += filterTag(m.captured(1), m.captured(2), keepRemote, remoteImages);
        last = m.capturedEnd();
    }
    out += QStringView(s).mid(last);
    return out;
}
} // namespace

QImage blockedResource()
{
    QImage img(1, 1, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    return img;
}

QImage dataImage(const QUrl &url)
{
    if (url.scheme().compare(QLatin1String("data"), Qt::CaseInsensitive) != 0) {
        return blockedResource();
    }
    const QByteArray enc = url.toEncoded();
    const qsizetype comma = enc.indexOf(',');
    if (comma < 0) {
        return blockedResource();
    }
    const QByteArray header = QByteArray::fromPercentEncoding(enc.mid(5, comma - 5)).toLower().trimmed();
    if (!header.startsWith("image/") || header.startsWith("image/svg")) {
        return blockedResource(); // SVG can reference other files
    }
    QByteArray payload = QByteArray::fromPercentEncoding(enc.mid(comma + 1));
    if (header.endsWith(";base64")) {
        payload = QByteArray::fromBase64(payload);
    }
    QImage img = QImage::fromData(payload);
    return img.isNull() ? blockedResource() : img;
}

QString sanitize(const QString &html, int *blockedImages, bool keepRemoteImages)
{
    QString s = html;
    // Don't crop to <body>...</body>: some senders (Meetup) concatenate two
    // documents, the first a body holding only a tracking pixel, the second
    // the real mail with no <body> at all, which then showed as blank. Head
    // sections go below; <html>/<body> tags are dropped with the others.
    static const RE prolog(QStringLiteral("<!DOCTYPE[^>]*>|<\\?xml[^>]*>"), kOpts);
    s.remove(prolog);
    // A '<' that doesn't start a tag, end tag or comment is text. Say so:
    // Qt's parser skips white space after it and would read "< img ..." as a tag.
    static const RE strayLt(QStringLiteral("<(?![a-zA-Z/!?])"));
    s.replace(strayLt, QStringLiteral("&lt;"));
    static const RE paired(
        QStringLiteral("<(script|style|iframe|object|embed|form|textarea|select|button|noscript|template|svg|math|head|title)\\b.*?</\\1\\s*>"),
        kOpts);
    s.remove(paired);
    // Stray end tags of the elements that are dropped (their start tags go in
    // filterTags(), which reads attributes the way a parser does).
    static const RE endTags(
        QStringLiteral("</(script|style|iframe|object|embed|form|input|textarea|select|button|link|meta|base|frame|frameset|"
                       "applet|noscript|template|svg|math|head|title|html|body)\\b[^>]*>"),
        kOpts);
    s.remove(endTags);
    // A <script> or <style> that is never closed swallows the rest, as it
    // would in a browser (its source must not show up as text).
    static const RE unclosed(QStringLiteral("<(script|style)\\b.*$"), kOpts);
    s.remove(unclosed);
    // Everything attribute-level happens per tag, quote-aware: event handlers,
    // javascript:/file: links, resources that may not load (local files
    // always; remote images unless they are on). loadResource blocks those
    // too; this keeps them out of the document entirely.
    int remoteImages = 0;
    s = filterTags(s, keepRemoteImages, &remoteImages);
    if (blockedImages) {
        *blockedImages = remoteImages;
    }
    return s;
}

} // namespace zmail::html
