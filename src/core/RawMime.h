#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace zmail {

// RFC 5322 / MIME as IMAP hands it over, turned into the shape of a Gmail API
// message "payload" (mimeType, filename, headers[{name,value}], body{data,
// size}, parts[]) so MessageParser reads IMAP mail exactly as it reads API
// mail. Bodies come back transfer-decoded (base64 / quoted-printable) and
// base64url-encoded, as the API sends them; headers come back unfolded with
// RFC 2047 encoded-words decoded, as the API sends them.
namespace RawMime {

QJsonArray parseHeaders(const QByteArray &headerBlock);
QJsonObject toGmailPayload(const QByteArray &rfc822);
QString decodeEncodedWords(const QByteArray &value);
QByteArray decodeQuotedPrintable(const QByteArray &in, bool headerQ = false);
// A header parameter (charset, boundary, name, filename), RFC 2231 included.
QString headerParam(const QString &headerValue, const QString &param);

} // namespace RawMime
} // namespace zmail
