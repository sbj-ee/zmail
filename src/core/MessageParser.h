#pragma once

#include "MailCache.h"

#include <QJsonObject>

namespace zmail {

// Turn Gmail API message resources into CachedMessage.
namespace MessageParser {

// format=metadata or format=full: headers, labels, snippet, size, date.
CachedMessage fromMetadata(const QJsonObject &msg);

struct AttachmentRef
{
    QString fileName;
    QString mimeType;
    QString attachmentId; // users.messages.attachments.get; "" = inline data
    QByteArray inlineData; // small parts Gmail returns inline
    qint64 size = 0;
};

struct Body
{
    QString text;
    QString html;
    QStringList attachments; // file names
    QList<AttachmentRef> attachmentRefs;
};
// format=full: walk the MIME tree (multipart/alternative, mixed, related),
// base64url-decode bodies and convert from the part's charset.
Body bodyFromFull(const QJsonObject &msg);

// "Ada Lovelace <ada@example.org>" -> {"Ada Lovelace", "ada@example.org"}
QPair<QString, QString> splitAddress(const QString &header);
QByteArray decodeBase64Url(const QString &data);

} // namespace MessageParser
} // namespace zmail
