#include "Markdown.h"

#include <QHash>
#include <QRegularExpression>
#include <QTextDocumentFragment>

#include <md4c-html.h>

namespace zmail::markdown {

namespace {
void collect(const MD_CHAR *text, MD_SIZE size, void *userdata)
{
    static_cast<QByteArray *>(userdata)->append(text, qsizetype(size));
}
} // namespace

QString toHtml(const QString &source)
{
    const QByteArray in = source.toUtf8();
    QByteArray out;
    const unsigned flags = MD_FLAG_NOHTML | MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_PERMISSIVEAUTOLINKS;
    if (md_html(in.constData(), MD_SIZE(in.size()), &collect, &out, flags, 0) != 0) {
        return source.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>\n"));
    }
    return QString::fromUtf8(out);
}

QString toEmailHtml(const QString &source)
{
    QString html = toHtml(source);
    // Inline styles survive Gmail's and Outlook's CSS stripping.
    html.replace(QStringLiteral("<pre>"),
                 QStringLiteral("<pre style=\"background:#f6f8fa;padding:8px 10px;border-radius:4px;"
                                "font-family:monospace;white-space:pre-wrap\">"));
    html.replace(QStringLiteral("<code>"),
                 QStringLiteral("<code style=\"background:#f6f8fa;padding:1px 3px;border-radius:3px;"
                                "font-family:monospace\">"));
    html.replace(QStringLiteral("<blockquote>"),
                 QStringLiteral("<blockquote style=\"margin:0 0 0 4px;padding-left:10px;border-left:3px solid "
                                "#ccc;color:#555\">"));
    html.replace(QStringLiteral("<table>"),
                 QStringLiteral("<table style=\"border-collapse:collapse\" border=\"1\" cellpadding=\"4\">"));
    return QStringLiteral("<div style=\"font-family:sans-serif;font-size:14px\">") + html + QStringLiteral("</div>");
}

QString toPlainText(const QString &source)
{
    QString s = source;
    // Reference definitions: [ref]: url "title"
    QHash<QString, QString> refs;
    static const QRegularExpression def(QStringLiteral("^ {0,3}\\[([^\\]]+)\\]:\\s*<?(\\S+?)>?(\\s+[\"'(].*[\"')])?\\s*$"),
                                        QRegularExpression::MultilineOption);
    for (auto it = def.globalMatch(s); it.hasNext();) {
        const auto m = it.next();
        refs.insert(m.captured(1).toLower(), m.captured(2));
    }
    s.remove(def);
    static const QRegularExpression image(QStringLiteral("!\\[([^\\]]*)\\]\\([^)]*\\)"));
    s.replace(image, QStringLiteral("[image: \\1]"));
    static const QRegularExpression inlineLink(QStringLiteral("\\[([^\\]]+)\\]\\(<?([^)\\s>]+)>?(\\s+\"[^\"]*\")?\\)"));
    s.replace(inlineLink, QStringLiteral("\\1 <\\2>"));
    static const QRegularExpression refLink(QStringLiteral("\\[([^\\]]+)\\]\\[([^\\]]*)\\]"));
    QString out;
    qsizetype last = 0;
    for (auto it = refLink.globalMatch(s); it.hasNext();) {
        const auto m = it.next();
        const QString key = (m.captured(2).isEmpty() ? m.captured(1) : m.captured(2)).toLower();
        out += s.mid(last, m.capturedStart() - last);
        out += refs.contains(key) ? m.captured(1) + QStringLiteral(" <") + refs.value(key) + QLatin1Char('>')
                                  : m.captured(0);
        last = m.capturedEnd();
    }
    out += s.mid(last);
    // Decode HTML entities (&amp; &eacute; &#8212;) without touching other text.
    static const QRegularExpression entity(QStringLiteral("&(#[0-9]+|#x[0-9a-fA-F]+|[A-Za-z][A-Za-z0-9]{1,30});"));
    QString decoded;
    last = 0;
    for (auto it = entity.globalMatch(out); it.hasNext();) {
        const auto m = it.next();
        decoded += out.mid(last, m.capturedStart() - last);
        decoded += QTextDocumentFragment::fromHtml(m.captured(0)).toPlainText();
        last = m.capturedEnd();
    }
    decoded += out.mid(last);
    return decoded.trimmed() + QLatin1Char('\n');
}

} // namespace zmail::markdown
