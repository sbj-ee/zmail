#pragma once

#include <QString>

class QTextDocument;

namespace zmail::richtext {

// text/plain alternative of an HTML-composed document: bullets become "- ",
// numbered lists "1. ", links "text <url>", block quotes "> ".
QString toPlainText(const QTextDocument *doc);
QString htmlToPlainText(const QString &html);

// Quote for a reply.
QString quotePlain(const QString &text);              // "> " per line
QString quoteHtml(const QString &html, const QString &attribution);

} // namespace zmail::richtext
