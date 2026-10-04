#pragma once

#include <QString>

namespace zmail::markdown {

// CommonMark + tables, strikethrough and permissive autolinks via md4c.
// Raw HTML is disabled: "<script>" in the source stays visible text.
QString toHtml(const QString &source);
// A complete HTML body (zmail's minimal inline CSS for code and quotes).
QString toEmailHtml(const QString &source);
// The text/plain alternative: the source, lightly cleaned ("![alt](x)" ->
// "[image: alt]", "[text][ref]" -> "text <url>", entities decoded).
QString toPlainText(const QString &source);

} // namespace zmail::markdown
