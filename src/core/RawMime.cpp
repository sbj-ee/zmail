#include "RawMime.h"

#include <QRegularExpression>
#include <QStringDecoder>
#include <QUrl>

namespace zmail::RawMime {

namespace {

constexpr int kMaxDepth = 20;

QString decodeCharset(const QByteArray &bytes, const QByteArray &charset)
{
    const QByteArray name = charset.isEmpty() ? QByteArray("utf-8") : charset.trimmed();
    QStringDecoder dec(name.constData());
    if (!dec.isValid()) {
        dec = QStringDecoder(QStringDecoder::Latin1);
    }
    return dec.decode(bytes);
}

QString headerValue(const QJsonArray &headers, const QString &name)
{
    for (const auto &h : headers) {
        const QJsonObject o = h.toObject();
        if (o.value(QStringLiteral("name")).toString().compare(name, Qt::CaseInsensitive) == 0) {
            return o.value(QStringLiteral("value")).toString();
        }
    }
    return {};
}

QJsonObject partFrom(const QByteArray &raw, const QString &defaultType, int depth)
{
    QJsonObject part;
    int split = raw.indexOf("\r\n\r\n");
    int sepLen = 4;
    const int lfSplit = raw.indexOf("\n\n");
    if (split < 0 || (lfSplit >= 0 && lfSplit < split)) {
        split = lfSplit;
        sepLen = 2;
    }
    if (raw.startsWith("\r\n")) {
        split = 0;
        sepLen = 2;
    } else if (raw.startsWith('\n')) {
        split = 0;
        sepLen = 1;
    }
    const QByteArray head = split < 0 ? raw : raw.left(split);
    const QByteArray body = split < 0 ? QByteArray() : raw.mid(split + sepLen);
    const QJsonArray headers = parseHeaders(head);
    part.insert(QStringLiteral("headers"), headers);

    const QString ctype = headerValue(headers, QStringLiteral("Content-Type"));
    QString mime = ctype.section(QLatin1Char(';'), 0, 0).trimmed().toLower();
    if (mime.isEmpty() || !mime.contains(QLatin1Char('/'))) {
        mime = defaultType;
    }
    part.insert(QStringLiteral("mimeType"), mime);
    QString filename = headerParam(headerValue(headers, QStringLiteral("Content-Disposition")), QStringLiteral("filename"));
    if (filename.isEmpty()) {
        filename = headerParam(ctype, QStringLiteral("name"));
    }
    part.insert(QStringLiteral("filename"), filename);

    if (mime.startsWith(QLatin1String("multipart/")) && depth < kMaxDepth) {
        const QByteArray boundary = headerParam(ctype, QStringLiteral("boundary")).toUtf8();
        QJsonArray parts;
        if (!boundary.isEmpty()) {
            const QByteArray delim = "--" + boundary;
            const QString childDefault = mime == QLatin1String("multipart/digest") ? QStringLiteral("message/rfc822")
                                                                                   : QStringLiteral("text/plain");
            int pos = body.indexOf(delim);
            while (pos >= 0) {
                int start = pos + delim.size();
                if (body.mid(start, 2) == "--") {
                    break; // closing delimiter
                }
                const int eol = body.indexOf('\n', start);
                if (eol < 0) {
                    break;
                }
                start = eol + 1;
                int next = body.indexOf("\n" + delim, start);
                if (next < 0) {
                    next = body.size();
                }
                QByteArray chunk = body.mid(start, next - start);
                if (chunk.endsWith('\r')) {
                    chunk.chop(1);
                }
                parts.append(partFrom(chunk, childDefault, depth + 1));
                pos = next < body.size() ? next + 1 : -1;
            }
        }
        part.insert(QStringLiteral("parts"), parts);
        part.insert(QStringLiteral("body"), QJsonObject{{QStringLiteral("size"), 0}});
        return part;
    }

    const QString cte = headerValue(headers, QStringLiteral("Content-Transfer-Encoding")).trimmed().toLower();
    QByteArray data;
    if (cte == QLatin1String("base64")) {
        data = QByteArray::fromBase64(body, QByteArray::IgnoreBase64DecodingErrors);
    } else if (cte == QLatin1String("quoted-printable")) {
        data = decodeQuotedPrintable(body);
    } else {
        data = body;
    }
    QJsonObject b;
    b.insert(QStringLiteral("size"), data.size());
    b.insert(QStringLiteral("data"),
             QString::fromLatin1(data.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)));
    part.insert(QStringLiteral("body"), b);
    return part;
}

} // namespace

QByteArray decodeQuotedPrintable(const QByteArray &in, bool headerQ)
{
    QByteArray out;
    out.reserve(in.size());
    for (int i = 0; i < in.size(); ++i) {
        const char c = in.at(i);
        if (headerQ && c == '_') {
            out.append(' ');
        } else if (c == '=') {
            if (i + 1 < in.size() && (in.at(i + 1) == '\n' || in.at(i + 1) == '\r')) {
                i += (in.at(i + 1) == '\r' && i + 2 < in.size() && in.at(i + 2) == '\n') ? 2 : 1; // soft break
            } else if (i + 2 < in.size()) {
                bool ok = false;
                const int v = in.mid(i + 1, 2).toInt(&ok, 16);
                if (ok) {
                    out.append(char(v));
                    i += 2;
                } else {
                    out.append(c);
                }
            } else {
                out.append(c);
            }
        } else {
            out.append(c);
        }
    }
    return out;
}

QString decodeEncodedWords(const QByteArray &value)
{
    static const QRegularExpression re(QStringLiteral(R"(=\?([^?\s]+)\?([bBqQ])\?([^?\s]*)\?=)"));
    const QString v = QString::fromLatin1(value);
    QString out;
    int last = 0;
    bool prevWasWord = false;
    auto it = re.globalMatch(v);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString between = v.mid(last, m.capturedStart() - last);
        // Whitespace between two encoded-words is dropped (RFC 2047 6.2).
        if (!(prevWasWord && between.trimmed().isEmpty())) {
            out += decodeCharset(between.toLatin1(), "utf-8");
        }
        QByteArray charset = m.captured(1).toLatin1();
        charset = charset.left(charset.indexOf('*') >= 0 ? charset.indexOf('*') : charset.size());
        const QByteArray text = m.captured(3).toLatin1();
        const QByteArray bytes = m.captured(2).compare(QLatin1String("b"), Qt::CaseInsensitive) == 0
            ? QByteArray::fromBase64(text)
            : decodeQuotedPrintable(text, true);
        out += decodeCharset(bytes, charset);
        last = m.capturedEnd();
        prevWasWord = true;
    }
    out += decodeCharset(v.mid(last).toLatin1(), "utf-8");
    return out;
}

QJsonArray parseHeaders(const QByteArray &headerBlock)
{
    QJsonArray out;
    QByteArray name;
    QByteArray value;
    auto flush = [&] {
        if (!name.isEmpty()) {
            out.append(QJsonObject{{QStringLiteral("name"), QString::fromLatin1(name.trimmed())},
                                   {QStringLiteral("value"), decodeEncodedWords(value.trimmed())}});
        }
        name.clear();
        value.clear();
    };
    for (QByteArray line : headerBlock.split('\n')) {
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        if (line.isEmpty()) {
            continue;
        }
        if ((line.at(0) == ' ' || line.at(0) == '\t') && !name.isEmpty()) {
            value += ' ' + line.trimmed(); // unfold
            continue;
        }
        const int colon = line.indexOf(':');
        if (colon <= 0) {
            continue;
        }
        flush();
        name = line.left(colon);
        value = line.mid(colon + 1);
    }
    flush();
    return out;
}

QString headerParam(const QString &headerValue, const QString &param)
{
    // RFC 2231 extended form first: name*=charset'lang'percent-encoded
    const QRegularExpression ext(QStringLiteral(R"(;\s*%1\*=\s*([^';]*)'[^']*'([^;\s]+))").arg(QRegularExpression::escape(param)),
                                 QRegularExpression::CaseInsensitiveOption);
    if (const auto m = ext.match(headerValue); m.hasMatch()) {
        const QByteArray bytes = QByteArray::fromPercentEncoding(m.captured(2).toLatin1());
        return decodeCharset(bytes, m.captured(1).toLatin1());
    }
    const QRegularExpression re(QStringLiteral(R"re(;\s*%1\s*=\s*(?:"((?:[^"\\]|\\.)*)"|([^;\s]+)))re").arg(QRegularExpression::escape(param)),
                                QRegularExpression::CaseInsensitiveOption);
    const auto m = re.match(headerValue);
    if (!m.hasMatch()) {
        return {};
    }
    QString v = m.captured(1).isNull() ? m.captured(2) : m.captured(1);
    v.replace(QStringLiteral("\\\""), QStringLiteral("\""));
    return v;
}

QJsonObject toGmailPayload(const QByteArray &rfc822)
{
    return partFrom(rfc822, QStringLiteral("text/plain"), 0);
}

} // namespace zmail::RawMime
