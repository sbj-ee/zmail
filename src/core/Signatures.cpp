#include "Signatures.h"

#include "HtmlSanitizer.h"
#include "RichText.h"

#include <QRegularExpression>
#include <QSettings>
#include <QTextDocument>
#include <QUrl>

namespace zmail {

namespace {
using RE = QRegularExpression;
constexpr auto kOpts = RE::CaseInsensitiveOption | RE::DotMatchesEverythingOption;

// Links: http(s) and mailto only; any other link is unwrapped (its text
// stays, without the link look). Qt's serializer writes <a href="...">
// with entities for quotes and never nests anchors, so one pattern sees
// them all.
QString filterLinks(const QString &html)
{
    static const RE anchor(QStringLiteral("<a\\b([^>]*)>(.*?)</a>"), kOpts);
    static const RE href(QStringLiteral("\\shref\\s*=\\s*\"([^\"]*)\""), kOpts);
    static const RE linkLook(QStringLiteral("\\s*(text-decoration:\\s*underline|color:#0000ff)\\s*;?"), kOpts);
    QString out;
    qsizetype last = 0;
    for (auto it = anchor.globalMatch(html); it.hasNext();) {
        const auto m = it.next();
        out += QStringView(html).mid(last, m.capturedStart() - last);
        last = m.capturedEnd();
        QString target = href.match(m.captured(1)).captured(1);
        target.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
        const QUrl u(target.trimmed(), QUrl::StrictMode);
        const QString scheme = u.scheme().toLower();
        if (u.isValid() && (scheme == QLatin1String("https") || scheme == QLatin1String("http") ||
                            scheme == QLatin1String("mailto"))) {
            out += QStringLiteral("<a href=\"") + href.match(m.captured(1)).captured(1) + QStringLiteral("\">") +
                   m.captured(2) + QStringLiteral("</a>");
        } else {
            static const RE bare(QStringLiteral("<span style=\"\\s*\">(.*?)</span>"), kOpts);
            out += QString(m.captured(2)).remove(linkLook).replace(bare, QStringLiteral("\\1"));
        }
    }
    out += QStringView(html).mid(last);
    return out;
}

// Style attributes: drop declarations that could load something.
QString filterStyle(const QString &style)
{
    QStringList keep;
    for (const QString &decl : style.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const QString d = decl.toLower();
        if (d.trimmed().startsWith(QLatin1String("-qt-")) || d.contains(QLatin1String("url")) || d.contains(QLatin1String("expression")) ||
            d.contains(QLatin1String("@import")) || d.contains(QLatin1Char('\\')) || d.contains(QLatin1String("image"))) {
            continue;
        }
        keep << decl;
    }
    return keep.join(QLatin1Char(';'));
}

// No resources of any kind in a signature.
QString dropResources(QString html)
{
    static const RE img(QStringLiteral("<img\\b[^>]*>"), kOpts);
    static const RE background(QStringLiteral("\\sbackground\\s*=\\s*(\"[^\"]*\"|'[^']*'|[^\\s>]+)"), kOpts);
    static const RE style(QStringLiteral("(\\sstyle\\s*=\\s*)(\"([^\"]*)\"|'([^']*)')"), kOpts);
    html.remove(img);
    html.remove(background);
    QString out;
    qsizetype last = 0;
    for (auto it = style.globalMatch(html); it.hasNext();) {
        const auto m = it.next();
        out += QStringView(html).mid(last, m.capturedStart() - last);
        last = m.capturedEnd();
        const QString value = m.captured(3).isNull() ? m.captured(4) : m.captured(3);
        QString clean = filterStyle(value);
        clean.replace(QLatin1Char('"'), QLatin1Char('\''));
        out += m.captured(1) + QLatin1Char('"') + clean + QLatin1Char('"');
    }
    out += QStringView(html).mid(last);
    return out;
}

QString bodyOf(const QString &doc)
{
    static const RE body(QStringLiteral("<body[^>]*>(.*)</body>"), kOpts);
    const auto m = body.match(doc);
    return m.hasMatch() ? m.captured(1) : doc;
}
} // namespace

QString sanitizeSignatureHtml(const QString &input)
{
    if (input.trimmed().isEmpty()) {
        return {};
    }
    // 1. The mail sanitizer (scripts, handlers, javascript:/file:, local
    //    and remote image references, <link>/<style>/<meta>...).
    QString s = dropResources(html::sanitize(input, nullptr, false));
    // 2. Qt's own model of it: obfuscated markup comes out normalized and
    //    only formatting Qt renders survives. setHtml()/toHtml() never load
    //    resources (only layout does), and there are none left anyway.
    QTextDocument doc;
    doc.setHtml(s);
    if (doc.toPlainText().trimmed().isEmpty()) {
        return {};
    }
    s = bodyOf(doc.toHtml());
    // 3. Once more on the normalized form.
    s = filterLinks(dropResources(html::sanitize(s, nullptr, false)));
    s.remove(QStringLiteral("<!--StartFragment-->"));
    s.remove(QStringLiteral("<!--EndFragment-->"));
    return s.trimmed();
}

QString signaturePlainFromHtml(const QString &html)
{
    QString t = richtext::htmlToPlainText(sanitizeSignatureHtml(html));
    while (t.endsWith(QLatin1Char('\n'))) {
        t.chop(1);
    }
    return t;
}

QString signatureHtmlFromPlain(const QString &text)
{
    return text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"));
}

QString Signature::plain() const
{
    return !text.isEmpty() ? text : signaturePlainFromHtml(html);
}

QString Signature::richHtml() const
{
    const QString clean = sanitizeSignatureHtml(html);
    return !clean.isEmpty() ? clean : signatureHtmlFromPlain(text);
}

SignatureStore::SignatureStore(QSettings *settings)
    : m_settings(settings)
{
    if (!m_settings) {
        m_settings = new QSettings();
        m_own = true;
    }
}

SignatureStore::~SignatureStore()
{
    if (m_own) {
        delete m_settings;
    }
}

QList<Signature> SignatureStore::all() const
{
    QList<Signature> out;
    const int n = m_settings->beginReadArray(QStringLiteral("signatures"));
    for (int i = 0; i < n; ++i) {
        m_settings->setArrayIndex(i);
        Signature s;
        s.name = m_settings->value(QStringLiteral("name")).toString();
        s.text = m_settings->value(QStringLiteral("text")).toString();
        s.html = m_settings->value(QStringLiteral("html")).toString();
        if (!s.name.isEmpty()) {
            out << s;
        }
    }
    m_settings->endArray();
    return out;
}

void SignatureStore::setAll(const QList<Signature> &sigs)
{
    m_settings->remove(QStringLiteral("signatures"));
    m_settings->beginWriteArray(QStringLiteral("signatures"), int(sigs.size()));
    for (int i = 0; i < sigs.size(); ++i) {
        m_settings->setArrayIndex(i);
        m_settings->setValue(QStringLiteral("name"), sigs[i].name);
        m_settings->setValue(QStringLiteral("text"), sigs[i].text);
        m_settings->setValue(QStringLiteral("html"), sanitizeSignatureHtml(sigs[i].html)); // stored sanitized
    }
    m_settings->endArray();
}

QString SignatureStore::defaultName() const
{
    return m_settings->value(QStringLiteral("signatureDefault")).toString();
}

void SignatureStore::setDefaultName(const QString &name)
{
    m_settings->setValue(QStringLiteral("signatureDefault"), name);
}

Signature SignatureStore::find(const QString &name) const
{
    for (const Signature &s : all()) {
        if (s.name == name) {
            return s;
        }
    }
    return {};
}

QString SignatureStore::plainBlock(const Signature &s)
{
    const QString p = s.plain();
    return p.isEmpty() ? QString() : QStringLiteral("-- \n") + p;
}

} // namespace zmail
