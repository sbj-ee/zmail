#include "HtmlSanitizer.h"

#include <QRegularExpression>

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
// becomes url() (nothing to load).
QString filterCssUrls(const QString &style)
{
    static const RE url(QStringLiteral("url\\s*\\(\\s*(\"[^\"]*\"|'[^']*'|[^)]*)\\s*\\)"), kOpts);
    QString out;
    qsizetype last = 0;
    for (auto it = url.globalMatch(style); it.hasNext();) {
        const auto m = it.next();
        const Res r = classify(unquote(m.captured(1).trimmed()));
        out += QStringView(style).mid(last, m.capturedStart() - last);
        out += r == Res::DataImage || r == Res::Remote ? m.captured(0) : QStringLiteral("url()");
        last = m.capturedEnd();
    }
    out += QStringView(style).mid(last);
    return out;
}

// One start tag: drop resource attributes with a blocked scheme; an <img>
// whose src is blocked goes entirely (returns an empty string).
QString filterTag(const QString &name, const QString &attrs)
{
    static const RE attr(QStringLiteral("([^\\s\"'>/=]+)(\\s*=\\s*(\"[^\"]*\"|'[^']*'|[^\\s\"'>]+))?"));
    const bool img = name.compare(QLatin1String("img"), Qt::CaseInsensitive) == 0;
    QString out = QLatin1Char('<') + name;
    qsizetype last = 0;
    for (auto it = attr.globalMatch(attrs); it.hasNext();) {
        const auto m = it.next();
        const QString between = attrs.mid(last, m.capturedStart() - last);
        last = m.capturedEnd();
        const QString key = m.captured(1).toLower();
        const QString value = unquote(m.captured(3));
        if (key == QLatin1String("src") || key == QLatin1String("background")) {
            const Res r = classify(value);
            if (img && key == QLatin1String("src") && r == Res::Blocked) {
                return {};
            }
            if (r == Res::Blocked || (!img && r == Res::Cid)) {
                out += between;
                continue; // attribute dropped
            }
        } else if (key == QLatin1String("style") && m.capturedLength(3) > 0) {
            out += between + m.captured(1) + QStringLiteral("=\"") +
                   filterCssUrls(value).replace(QLatin1Char('"'), QStringLiteral("&quot;")) + QLatin1Char('"');
            continue;
        }
        out += between + m.captured(0);
    }
    out += attrs.mid(last);
    return out + QLatin1Char('>');
}

QString filterResources(const QString &s)
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
        out += filterTag(m.captured(1), m.captured(2));
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
    static const RE paired(
        QStringLiteral("<(script|style|iframe|object|embed|form|textarea|select|button|noscript|template|svg|math|head|title)\\b.*?</\\1\\s*>"),
        kOpts);
    s.remove(paired);
    static const RE single(QStringLiteral("<\\/?(script|iframe|object|embed|form|input|link|meta|base|frame|frameset|applet|html|body)\\b[^>]*>"), kOpts);
    s.remove(single);
    static const RE events(QStringLiteral("\\s+on[a-z]+\\s*=\\s*(\"[^\"]*\"|'[^']*'|[^\\s>]+)"), kOpts);
    s.remove(events);
    static const RE jsUrl(QStringLiteral("(href|src)\\s*=\\s*([\"']?)\\s*(javascript|vbscript|file):[^\"'>\\s]*\\2"), kOpts);
    s.replace(jsUrl, QStringLiteral("\\1=\"#\""));
    // Local files: only allowed schemes may load anything (loadResource
    // blocks the rest too; this keeps them out of the document entirely).
    s = filterResources(s);
    if (blockedImages) {
        static const RE remoteImg(QStringLiteral("<img\\b[^>]*\\bsrc\\s*=\\s*[\"']?\\s*(https?:|//|cid:)"), kOpts);
        int n = 0;
        auto it = remoteImg.globalMatch(s);
        while (it.hasNext()) {
            it.next();
            ++n;
        }
        *blockedImages = n;
    }
    if (keepRemoteImages) {
        // Protocol-relative URLs -> https; cid: parts aren't fetched yet.
        static const RE protoRel(QStringLiteral("(<img\\b[^>]*\\bsrc\\s*=\\s*[\"']?)\\s*//"), kOpts);
        s.replace(protoRel, QStringLiteral("\\1https://"));
        static const RE cidImgTag(QStringLiteral("<img\\b[^>]*\\bsrc\\s*=\\s*[\"']?\\s*cid:[^>]*>"), kOpts);
        s.remove(cidImgTag);
        return s;
    }
    // Drop remote/cid images outright (no broken-image boxes; nothing fetched).
    static const RE remoteImgTag(QStringLiteral("<img\\b[^>]*\\bsrc\\s*=\\s*[\"']?\\s*(https?:|//|cid:)[^>]*>"), kOpts);
    s.remove(remoteImgTag);
    return s;
}

} // namespace zmail::html
