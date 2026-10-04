#include "RichText.h"

#include <QTextBlock>
#include <QTextDocument>
#include <QTextList>

namespace zmail::richtext {

QString toPlainText(const QTextDocument *doc)
{
    QStringList lines;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        QString line;
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (!f.isValid()) {
                continue;
            }
            const QString t = f.text();
            const QString href = f.charFormat().anchorHref();
            if (!href.isEmpty() && href != t && QStringLiteral("mailto:") + t != href) {
                line += t + QStringLiteral(" <") + href + QLatin1Char('>');
            } else {
                line += t;
            }
        }
        line.replace(QChar::LineSeparator, QLatin1Char('\n'));
        line.replace(QChar::Nbsp, QLatin1Char(' '));
        if (QTextList *list = b.textList()) {
            const int indent = std::max(0, list->format().indent() - 1);
            const QString pad(indent * 2, QLatin1Char(' '));
            const auto style = list->format().style();
            const bool numbered = style == QTextListFormat::ListDecimal || style == QTextListFormat::ListLowerAlpha ||
                                  style == QTextListFormat::ListUpperAlpha || style == QTextListFormat::ListLowerRoman ||
                                  style == QTextListFormat::ListUpperRoman;
            line = pad + (numbered ? QString::number(list->itemNumber(b) + 1) + QStringLiteral(". ") : QStringLiteral("- ")) +
                   line;
        }
        const int quoteLevel = b.blockFormat().property(QTextFormat::BlockQuoteLevel).toInt();
        if (quoteLevel > 0) {
            QStringList ql = line.split(QLatin1Char('\n'));
            for (QString &l : ql) {
                l = QString(QStringLiteral("> ")).repeated(quoteLevel) + l;
            }
            line = ql.join(QLatin1Char('\n'));
        }
        lines << line;
    }
    QString out = lines.join(QLatin1Char('\n'));
    while (out.endsWith(QLatin1Char('\n'))) {
        out.chop(1);
    }
    return out + QLatin1Char('\n');
}

QString htmlToPlainText(const QString &html)
{
    QTextDocument d;
    d.setHtml(html);
    return toPlainText(&d);
}

QString quotePlain(const QString &text)
{
    QStringList lines = text.split(QLatin1Char('\n'));
    while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) {
        lines.removeLast();
    }
    for (QString &l : lines) {
        l = l.startsWith(QLatin1Char('>')) ? QStringLiteral(">") + l : QStringLiteral("> ") + l;
    }
    return lines.join(QLatin1Char('\n'));
}

QString quoteHtml(const QString &html, const QString &attribution)
{
    return QStringLiteral("<p>%1</p><blockquote style=\"margin:0 0 0 4px;padding-left:10px;border-left:3px solid "
                          "#ccc;color:#555\">%2</blockquote>")
        .arg(attribution.toHtmlEscaped(), html);
}

} // namespace zmail::richtext
