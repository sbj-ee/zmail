#include "MimeBuilder.h"

#include "Limits.h"
#include "MessageParser.h"
#include "version.hpp"

#include <QLocale>
#include <QMimeDatabase>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTimeZone>
#include <QUrl>

namespace zmail {

namespace {
bool isAscii(const QString &s)
{
    for (const QChar c : s) {
        if (c.unicode() > 126 || (c.unicode() < 32 && c != QLatin1Char('\t'))) {
            return false;
        }
    }
    return true;
}

bool isAsciiBody(const QString &s)
{
    for (const QChar c : s) {
        const ushort u = c.unicode();
        if (u > 126 || (u < 32 && u != '\t' && u != '\n' && u != '\r')) {
            return false;
        }
    }
    return true;
}

QByteArray boundary()
{
    const quint64 a = QRandomGenerator::global()->generate64();
    const quint64 b = QRandomGenerator::global()->generate64();
    return "=_zmail_" + QByteArray::number(a, 16) + QByteArray::number(b, 16);
}

QByteArray crlf(const QString &text)
{
    QString t = text;
    t.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    t.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    QByteArray u = t.toUtf8();
    u.replace("\n", "\r\n");
    return u;
}

// A text body part: 7bit if it can be, else base64.
QByteArray textPart(const QByteArray &subtype, const QString &body)
{
    const QByteArray bytes = crlf(body);
    bool sevenBit = isAsciiBody(body);
    if (sevenBit) {
        for (const QByteArray &line : bytes.split('\n')) {
            if (line.size() > 900) {
                sevenBit = false;
                break;
            }
        }
    }
    QByteArray out = "Content-Type: text/" + subtype + "; charset=UTF-8\r\n";
    if (sevenBit) {
        out += "Content-Transfer-Encoding: 7bit\r\n\r\n" + bytes;
        if (!out.endsWith("\r\n")) {
            out += "\r\n";
        }
    } else {
        out += "Content-Transfer-Encoding: base64\r\n\r\n" + MimeBuilder::base64Lines(bytes);
    }
    return out;
}

QByteArray quoteParam(const QString &v)
{
    QString q = v;
    q.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    q.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return '"' + q.toLatin1() + '"';
}

QByteArray attachmentPart(const OutgoingAttachment &a)
{
    const QString name = a.fileName.isEmpty() ? QStringLiteral("attachment") : a.fileName;
    const QString type = a.mimeType.isEmpty() ? MimeBuilder::guessMimeType(name, a.data) : a.mimeType;
    QByteArray out;
    if (isAscii(name)) {
        out += "Content-Type: " + type.toLatin1() + "; name=" + quoteParam(name) + "\r\n";
        out += "Content-Disposition: attachment; filename=" + quoteParam(name) + "\r\n";
    } else {
        // RFC 2231 for modern clients, an RFC 2047 name= for older ones.
        out += MimeBuilder::foldHeader("Content-Type", type.toLatin1() + "; name=\"" +
                                                           MimeBuilder::encodeHeaderText(name) + "\"");
        out += MimeBuilder::foldHeader("Content-Disposition",
                                       "attachment; filename*=UTF-8''" + QUrl::toPercentEncoding(name));
    }
    out += "Content-Transfer-Encoding: base64\r\n\r\n";
    out += MimeBuilder::base64Lines(a.data);
    return out;
}

QByteArray multipart(const QByteArray &subtype, const QList<QByteArray> &parts)
{
    const QByteArray b = boundary();
    QByteArray out = "Content-Type: multipart/" + subtype + ";\r\n boundary=\"" + b + "\"\r\n\r\n";
    for (const QByteArray &p : parts) {
        out += "--" + b + "\r\n" + p;
        if (!out.endsWith("\r\n")) {
            out += "\r\n";
        }
    }
    out += "--" + b + "--\r\n";
    return out;
}
} // namespace

QByteArray MimeBuilder::base64Lines(const QByteArray &data)
{
    const QByteArray b64 = data.toBase64();
    QByteArray out;
    out.reserve(b64.size() + b64.size() / limits::kBase64LineLength * 2 + 2);
    for (qsizetype i = 0; i < b64.size(); i += limits::kBase64LineLength) {
        out += b64.mid(i, limits::kBase64LineLength);
        out += "\r\n";
    }
    return out;
}

QByteArray MimeBuilder::encodeHeaderText(const QString &text)
{
    if (isAscii(text)) {
        return text.toLatin1();
    }
    // RFC 2047 B-encoding, never splitting a UTF-8 sequence: at most 39 bytes
    // of UTF-8 per word (52 base64 chars, 64 with the =?UTF-8?B? ?= wrapper),
    // so "Subject: " plus one word still fits a 78-column line.
    QList<QByteArray> words;
    QByteArray chunk;
    for (int i = 0; i < text.size(); ++i) {
        QString ch(text.at(i));
        if (text.at(i).isHighSurrogate() && i + 1 < text.size()) {
            ch += text.at(++i);
        }
        const QByteArray u = ch.toUtf8();
        if (chunk.size() + u.size() > 39) {
            words << chunk;
            chunk.clear();
        }
        chunk += u;
    }
    if (!chunk.isEmpty()) {
        words << chunk;
    }
    QByteArray out;
    for (const QByteArray &w : words) {
        if (!out.isEmpty()) {
            out += ' '; // whitespace between encoded-words is ignored; foldHeader folds here
        }
        out += "=?UTF-8?B?" + w.toBase64() + "?=";
    }
    return out;
}

QStringList MimeBuilder::splitAddresses(const QString &list)
{
    QStringList out;
    QString cur;
    bool quoted = false;
    int angle = 0;
    for (int i = 0; i < list.size(); ++i) {
        const QChar c = list.at(i);
        if (c == QLatin1Char('\\') && quoted && i + 1 < list.size()) {
            cur += c;
            cur += list.at(++i);
            continue;
        }
        if (c == QLatin1Char('"')) {
            quoted = !quoted;
        } else if (!quoted && c == QLatin1Char('<')) {
            ++angle;
        } else if (!quoted && c == QLatin1Char('>')) {
            angle = std::max(0, angle - 1);
        }
        if ((c == QLatin1Char(',') || c == QLatin1Char(';')) && !quoted && angle == 0) {
            if (!cur.trimmed().isEmpty()) {
                out << cur.trimmed();
            }
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.trimmed().isEmpty()) {
        out << cur.trimmed();
    }
    return out;
}

QByteArray MimeBuilder::encodeAddressList(const QString &list)
{
    QList<QByteArray> parts;
    for (const QString &a : splitAddresses(list)) {
        const auto na = MessageParser::splitAddress(a);
        const QString name = na.first == na.second ? QString() : na.first;
        const QByteArray addr = na.second.toLatin1(); // IDN/EAI addresses aren't supported yet
        if (name.isEmpty()) {
            parts << addr;
        } else if (!isAscii(name)) {
            parts << encodeHeaderText(name) + " <" + addr + ">";
        } else if (name.contains(QRegularExpression(QStringLiteral("[()<>\\[\\]:;@\\\\,.\"]")))) {
            parts << quoteParam(name) + " <" + addr + ">";
        } else {
            parts << name.toLatin1() + " <" + addr + ">";
        }
    }
    QByteArray out;
    for (const QByteArray &p : parts) {
        if (!out.isEmpty()) {
            out += ", ";
        }
        out += p;
    }
    return out;
}

QByteArray MimeBuilder::foldHeader(const QByteArray &name, const QByteArray &value)
{
    // Fold at whitespace so no line exceeds 78 characters where possible.
    QByteArray out = name + ':';
    int col = int(out.size());
    bool first = true;
    for (const QByteArray &tok : value.split(' ')) {
        if (!first && col + 1 + tok.size() > 78) {
            out += "\r\n";
            col = 0;
        }
        out += ' ' + tok;
        col += 1 + int(tok.size());
        first = false;
    }
    return out + "\r\n";
}

QString MimeBuilder::guessMimeType(const QString &fileName, const QByteArray &data)
{
    static QMimeDatabase db;
    QMimeType t = db.mimeTypeForFileNameAndData(fileName, data.left(4096));
    return t.isValid() ? t.name() : QStringLiteral("application/octet-stream");
}

QString MimeBuilder::makeMessageId(const QString &fromAddress)
{
    QString domain = fromAddress.section(QLatin1Char('@'), 1).trimmed();
    if (domain.isEmpty() || !isAscii(domain)) {
        domain = QStringLiteral("zmail.invalid");
    }
    const quint64 r = QRandomGenerator::global()->generate64();
    return QStringLiteral("<zmail.%1.%2@%3>")
        .arg(QDateTime::currentMSecsSinceEpoch(), 0, 36)
        .arg(r, 0, 36)
        .arg(domain);
}

QByteArray MimeBuilder::build(const OutgoingMessage &m)
{
    QByteArray h;
    const QDateTime date = m.date.isValid() ? m.date : QDateTime::currentDateTime();
    h += "Date: " + date.toString(Qt::RFC2822Date).toLatin1() + "\r\n";
    h += foldHeader("From", encodeAddressList(m.from));
    if (!m.to.trimmed().isEmpty()) h += foldHeader("To", encodeAddressList(m.to));
    if (!m.cc.trimmed().isEmpty()) h += foldHeader("Cc", encodeAddressList(m.cc));
    // Gmail's messages.send delivers to Bcc recipients and strips the header.
    if (!m.bcc.trimmed().isEmpty()) h += foldHeader("Bcc", encodeAddressList(m.bcc));
    h += foldHeader("Subject", encodeHeaderText(m.subject));
    const QString from = MessageParser::splitAddress(m.from).second;
    h += "Message-ID: " + (m.messageId.isEmpty() ? makeMessageId(from) : m.messageId).toLatin1() + "\r\n";
    if (!m.inReplyTo.isEmpty()) {
        h += "In-Reply-To: " + m.inReplyTo.toLatin1() + "\r\n";
    }
    if (!m.references.isEmpty()) {
        h += foldHeader("References", m.references.join(QLatin1Char(' ')).toLatin1());
    }
    if (m.priority == OutgoingMessage::Priority::High) {
        h += "X-Priority: 1 (Highest)\r\nImportance: High\r\n";
    } else if (m.priority == OutgoingMessage::Priority::Low) {
        h += "X-Priority: 5 (Lowest)\r\nImportance: Low\r\n";
    }
    h += "MIME-Version: 1.0\r\n";
    h += "User-Agent: zmail/" + QByteArray(zmail::kVersionString) + "\r\n";

    QByteArray body = m.html.isEmpty()
                          ? textPart("plain", m.text)
                          : multipart("alternative", {textPart("plain", m.text), textPart("html", m.html)});
    if (!m.attachments.isEmpty()) {
        QList<QByteArray> parts{body};
        for (const OutgoingAttachment &a : m.attachments) {
            parts << attachmentPart(a);
        }
        body = multipart("mixed", parts);
    }
    return h + body;
}

} // namespace zmail
