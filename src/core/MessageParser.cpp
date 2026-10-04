#include "MessageParser.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QTextDocumentFragment>

namespace zmail::MessageParser {

namespace {
QString header(const QJsonArray &headers, const QString &name)
{
    for (const auto &h : headers) {
        const QJsonObject o = h.toObject();
        if (o.value(QStringLiteral("name")).toString().compare(name, Qt::CaseInsensitive) == 0) {
            return o.value(QStringLiteral("value")).toString();
        }
    }
    return {};
}

QString charsetOf(const QJsonArray &headers)
{
    static const QRegularExpression re(QStringLiteral("charset\\s*=\\s*\"?([^\";\\s]+)"),
                                       QRegularExpression::CaseInsensitiveOption);
    const auto m = re.match(header(headers, QStringLiteral("Content-Type")));
    return m.hasMatch() ? m.captured(1) : QStringLiteral("utf-8");
}

QString decodeText(const QByteArray &raw, const QString &charset)
{
    QStringDecoder dec(charset.toLatin1().constData());
    if (!dec.isValid()) {
        dec = QStringDecoder(QStringDecoder::Utf8);
    }
    return dec.decode(raw);
}

void walk(const QJsonObject &part, Body &out, int depth)
{
    if (depth > 32) {
        return;
    }
    const QString mime = part.value(QStringLiteral("mimeType")).toString().toLower();
    const QString filename = part.value(QStringLiteral("filename")).toString();
    const QJsonObject body = part.value(QStringLiteral("body")).toObject();
    const QJsonArray headers = part.value(QStringLiteral("headers")).toArray();
    const bool attachment =
        !filename.isEmpty() || header(headers, QStringLiteral("Content-Disposition")).startsWith(QLatin1String("attachment"), Qt::CaseInsensitive);

    if (mime.startsWith(QLatin1String("multipart/"))) {
        for (const auto &p : part.value(QStringLiteral("parts")).toArray()) {
            walk(p.toObject(), out, depth + 1);
        }
        return;
    }
    if (attachment) {
        out.attachments.append(filename.isEmpty() ? QStringLiteral("(unnamed)") : filename);
        AttachmentRef ref;
        ref.fileName = out.attachments.last();
        ref.mimeType = mime;
        ref.attachmentId = body.value(QStringLiteral("attachmentId")).toString();
        ref.size = body.value(QStringLiteral("size")).toInteger();
        if (ref.attachmentId.isEmpty()) {
            ref.inlineData = decodeBase64Url(body.value(QStringLiteral("data")).toString());
        }
        out.attachmentRefs.append(ref);
        return;
    }
    const QString data = body.value(QStringLiteral("data")).toString();
    if (data.isEmpty()) {
        return;
    }
    if (mime == QLatin1String("text/plain") && out.text.isEmpty()) {
        out.text = decodeText(decodeBase64Url(data), charsetOf(headers));
    } else if (mime == QLatin1String("text/html") && out.html.isEmpty()) {
        out.html = decodeText(decodeBase64Url(data), charsetOf(headers));
    }
}
} // namespace

QByteArray decodeBase64Url(const QString &data)
{
    return QByteArray::fromBase64(data.toLatin1(), QByteArray::Base64UrlEncoding);
}

QPair<QString, QString> splitAddress(const QString &h)
{
    static const QRegularExpression re(QStringLiteral("^\\s*\"?([^\"<]*?)\"?\\s*<([^>]+)>"));
    const auto m = re.match(h);
    if (m.hasMatch()) {
        QString name = m.captured(1).trimmed();
        const QString addr = m.captured(2).trimmed();
        return {name.isEmpty() ? addr : name, addr};
    }
    const QString addr = h.trimmed();
    return {addr, addr};
}

CachedMessage fromMetadata(const QJsonObject &msg)
{
    CachedMessage m;
    m.id = msg.value(QStringLiteral("id")).toString();
    m.threadId = msg.value(QStringLiteral("threadId")).toString();
    m.historyId = msg.value(QStringLiteral("historyId")).toString().toLongLong();
    m.internalDateMs = msg.value(QStringLiteral("internalDate")).toString().toLongLong();
    m.snippet = QTextDocumentFragment::fromHtml(msg.value(QStringLiteral("snippet")).toString()).toPlainText();
    m.size = msg.value(QStringLiteral("sizeEstimate")).toInteger();
    for (const auto &l : msg.value(QStringLiteral("labelIds")).toArray()) {
        m.labels.append(l.toString());
    }
    const QJsonObject payload = msg.value(QStringLiteral("payload")).toObject();
    const QJsonArray headers = payload.value(QStringLiteral("headers")).toArray();
    const auto from = splitAddress(header(headers, QStringLiteral("From")));
    m.fromName = from.first;
    m.fromAddr = from.second;
    m.to = header(headers, QStringLiteral("To"));
    m.cc = header(headers, QStringLiteral("Cc"));
    m.replyTo = header(headers, QStringLiteral("Reply-To"));
    m.messageIdHeader = header(headers, QStringLiteral("Message-ID")).trimmed();
    m.references = header(headers, QStringLiteral("References")).simplified();
    m.subject = header(headers, QStringLiteral("Subject"));
    if (m.internalDateMs == 0) {
        m.internalDateMs = QDateTime::fromString(header(headers, QStringLiteral("Date")), Qt::RFC2822Date).toMSecsSinceEpoch();
    }
    const QString ctype = header(headers, QStringLiteral("Content-Type")).toLower();
    m.hasAttachment = ctype.startsWith(QLatin1String("multipart/mixed"));
    return m;
}

Body bodyFromFull(const QJsonObject &msg)
{
    Body b;
    walk(msg.value(QStringLiteral("payload")).toObject(), b, 0);
    return b;
}

} // namespace zmail::MessageParser
