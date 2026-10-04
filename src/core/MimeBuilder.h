#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

namespace zmail {

struct OutgoingAttachment
{
    QString fileName;
    QString mimeType;   // "" = guessed from the name / content
    QByteArray data;
};

struct OutgoingMessage
{
    QString from;        // "Name <addr>"
    QString to, cc, bcc; // comma-separated address lists
    QString subject;
    QString text;        // text/plain body (always sent)
    QString html;        // text/html body; "" = text/plain only
    QList<OutgoingAttachment> attachments;
    // Threading (replies / forwards)
    QString inReplyTo;   // "<id@host>"
    QStringList references;
    QString threadId;    // Gmail threadId (not a header; passed to the API)
    enum class Priority { Normal, High, Low } priority = Priority::Normal;
    QDateTime date;      // invalid = now
    QString messageId;   // "" = generated
};

// RFC 5322 / 2045-2047 / 2231 message builder:
//  - text only            -> text/plain
//  - text + html          -> multipart/alternative
//  - + attachments        -> multipart/mixed (first part as above)
// Headers are folded at 78 columns, non-ASCII header text becomes UTF-8
// encoded-words, non-ASCII filenames use RFC 2231 (plus an encoded-word
// name= for old clients). Bodies are 7bit when they're short-line ASCII and
// base64 (76-column lines) otherwise; attachments are always base64. CRLF
// line endings throughout.
class MimeBuilder
{
public:
    static QByteArray build(const OutgoingMessage &m);

    // Building blocks (exposed for tests).
    static QByteArray encodeHeaderText(const QString &text);      // encoded-word if needed
    static QByteArray encodeAddressList(const QString &list);     // "A <a@x>, \"B, C\" <b@x>"
    static QStringList splitAddresses(const QString &list);       // respects quotes and <>
    static QByteArray base64Lines(const QByteArray &data);        // 76-col lines, CRLF
    static QByteArray foldHeader(const QByteArray &name, const QByteArray &value);
    static QString guessMimeType(const QString &fileName, const QByteArray &data);
    static QString makeMessageId(const QString &fromAddress);
};

} // namespace zmail
