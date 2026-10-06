#include "Mailto.h"

#include <QStringList>

namespace zmail::Mailto {

namespace {
constexpr int kMaxHeader = 2000;
constexpr int kMaxBody = 100000;

// RFC 6068 percent-encodes everything; '+' is a literal plus, not a space.
QString decode(const QString &raw)
{
    return QUrl::fromPercentEncoding(raw.toUtf8());
}

QString headerValue(const QString &v)
{
    QString out;
    for (const QChar c : v) {
        const char16_t u = c.unicode();
        if (u < 0x20 || u == 0x7f || (u >= 0x80 && u < 0xa0) || u == 0x2028 || u == 0x2029) {
            continue;
        }
        out += c;
    }
    return out.trimmed().left(kMaxHeader);
}

QString bodyValue(const QString &v)
{
    QString out = v;
    out.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    out.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    QString clean;
    for (const QChar c : std::as_const(out)) {
        const char16_t u = c.unicode();
        if ((u < 0x20 && u != '\n' && u != '\t') || u == 0x7f) {
            continue;
        }
        clean += c;
    }
    return clean.left(kMaxBody);
}

void addAddresses(QString &field, const QString &value)
{
    const QString v = headerValue(value);
    if (v.isEmpty()) {
        return;
    }
    field = field.isEmpty() ? v : field + QStringLiteral(", ") + v;
    field = field.left(kMaxHeader);
}
} // namespace

bool isMailto(const QUrl &url)
{
    return url.scheme().compare(QLatin1String("mailto"), Qt::CaseInsensitive) == 0;
}

MailtoFields parse(const QUrl &url)
{
    MailtoFields f;
    if (!isMailto(url)) {
        return f;
    }
    // Work on the encoded form so %26 / %3D inside values don't split them.
    const QString encoded = url.toString(QUrl::FullyEncoded);
    QString rest = encoded.mid(encoded.indexOf(QLatin1Char(':')) + 1);
    const int hash = rest.indexOf(QLatin1Char('#'));
    if (hash >= 0) {
        rest.truncate(hash);
    }
    const int q = rest.indexOf(QLatin1Char('?'));
    addAddresses(f.to, decode(q < 0 ? rest : rest.left(q)));
    if (q < 0) {
        return f;
    }
    bool haveSubject = false, haveBody = false;
    for (const QString &pair : rest.mid(q + 1).split(QLatin1Char('&'), Qt::SkipEmptyParts)) {
        const int eq = pair.indexOf(QLatin1Char('='));
        const QString key = decode(eq < 0 ? pair : pair.left(eq)).trimmed().toLower();
        const QString value = eq < 0 ? QString() : decode(pair.mid(eq + 1));
        if (key == QLatin1String("to")) {
            addAddresses(f.to, value);
        } else if (key == QLatin1String("cc")) {
            addAddresses(f.cc, value);
        } else if (key == QLatin1String("bcc")) {
            addAddresses(f.bcc, value);
        } else if (key == QLatin1String("subject") && !haveSubject) {
            f.subject = headerValue(value);
            haveSubject = true;
        } else if (key == QLatin1String("body") && !haveBody) {
            f.body = bodyValue(value);
            haveBody = true;
        }
        // attach, attachment, from, in-reply-to, anything else: ignored.
    }
    return f;
}

} // namespace zmail::Mailto
