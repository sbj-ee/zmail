#include "AuthResults.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>

namespace zmail {

namespace {
// Header values are attacker-controlled: never scan more than this.
constexpr int kMaxHeader = 16 * 1024;

QString tr(const char *s)
{
    return QCoreApplication::translate("zmail::Auth", s);
}

// Drop RFC 5322 comments "(...)" (nested), which may hold anything.
QString stripComments(const QString &in)
{
    QString out;
    out.reserve(in.size());
    int depth = 0;
    bool quoted = false;
    for (int i = 0; i < in.size(); ++i) {
        const QChar c = in.at(i);
        if (c == QLatin1Char('\\') && i + 1 < in.size()) {
            if (depth == 0) {
                out += c;
                out += in.at(i + 1);
            }
            ++i;
            continue;
        }
        if (depth == 0 && c == QLatin1Char('"')) {
            quoted = !quoted;
        } else if (!quoted && c == QLatin1Char('(')) {
            ++depth;
            continue;
        } else if (!quoted && c == QLatin1Char(')') && depth > 0) {
            --depth;
            continue;
        }
        if (depth == 0) {
            out += c;
        }
    }
    return out;
}

QString domainOf(const QString &address)
{
    const int at = address.lastIndexOf(QLatin1Char('@'));
    return at < 0 ? QString() : address.mid(at + 1).trimmed().toLower();
}

bool sameOrSubdomain(const QString &domain, const QString &of)
{
    return !of.isEmpty() && (domain == of || domain.endsWith(QLatin1Char('.') + of));
}
} // namespace

bool AuthResults::anyPass() const
{
    const QString pass = QStringLiteral("pass");
    return spf == pass || dkim == pass || dmarc == pass;
}

bool AuthResults::dmarcFailed() const
{
    return dmarc == QLatin1String("fail");
}

namespace Auth {

QString topmostHeader(const QJsonArray &gmailHeaders)
{
    for (const auto &h : gmailHeaders) {
        const QJsonObject o = h.toObject();
        if (o.value(QStringLiteral("name")).toString().compare(QLatin1String("Authentication-Results"),
                                                                Qt::CaseInsensitive) == 0) {
            return o.value(QStringLiteral("value")).toString().left(kMaxHeader);
        }
    }
    return {};
}

AuthResults parse(const QString &headerValue)
{
    AuthResults r;
    const QString value = stripComments(headerValue.left(kMaxHeader));
    const QStringList parts = value.split(QLatin1Char(';'));
    if (parts.isEmpty()) {
        return r;
    }
    // authserv-id [version]
    const QString servId = parts.first().simplified().section(QLatin1Char(' '), 0, 0).toLower();
    r.trusted = servId == QLatin1String(kTrustedAuthServId);
    if (!r.trusted) {
        return r;
    }
    for (int i = 1; i < parts.size(); ++i) {
        const QString res = parts.at(i).simplified();
        const int eq = res.indexOf(QLatin1Char('='));
        if (eq <= 0) {
            continue;
        }
        // "dkim/1=pass" (method version) -> "dkim"
        const QString method = res.left(eq).section(QLatin1Char('/'), 0, 0).trimmed().toLower();
        const QString result = res.mid(eq + 1).section(QLatin1Char(' '), 0, 0).trimmed().toLower();
        if (result.isEmpty()) {
            continue;
        }
        if (method == QLatin1String("spf")) {
            if (r.spf.isEmpty()) {
                r.spf = result;
            }
        } else if (method == QLatin1String("dkim")) {
            // Several signatures: one that passes is enough.
            if (r.dkim.isEmpty() || result == QLatin1String("pass")) {
                r.dkim = result;
            }
        } else if (method == QLatin1String("dmarc")) {
            if (r.dmarc.isEmpty()) {
                r.dmarc = result;
            }
        }
    }
    return r;
}

bool displayNameMismatch(const QString &displayName, const QString &address)
{
    const QString name = displayName.left(1024).trimmed().toLower();
    const QString addr = address.trimmed().toLower();
    if (name.isEmpty() || addr.isEmpty()) {
        return false;
    }
    const QString realDomain = domainOf(addr);
    static const QRegularExpression emailRe(QStringLiteral(R"([a-z0-9._%+\-]+@([a-z0-9\-]+(?:\.[a-z0-9\-]+)+))"));
    bool sawEmail = false;
    for (auto it = emailRe.globalMatch(name); it.hasNext();) {
        const auto m = it.next();
        sawEmail = true;
        if (m.captured(0) != addr) {
            return true;
        }
    }
    if (sawEmail) {
        return false;
    }
    // A bare domain in the name ("paypal.com", "Support - bank.example").
    static const QRegularExpression domainRe(
        QStringLiteral(R"((?:^|[\s"'<(\[,:|-])((?:[a-z0-9](?:[a-z0-9\-]*[a-z0-9])?\.)+[a-z]{2,24})(?=$|[\s"'>)\],:|.-]))"));
    for (auto it = domainRe.globalMatch(name); it.hasNext();) {
        const QString d = it.next().captured(1);
        if (!sameOrSubdomain(realDomain, d)) {
            return true;
        }
    }
    return false;
}

QString warningHtml(const AuthResults &r, const QString &displayName, const QString &address)
{
    QStringList reasons;
    if (r.suspicious()) {
        if (r.dmarcFailed()) {
            reasons << tr("Gmail's DMARC check failed: this message may not really be from %1.")
                           .arg(domainOf(address).toHtmlEscaped());
        } else {
            reasons << tr("Gmail couldn't confirm the sender: SPF, DKIM and DMARC all did not pass.");
        }
    }
    if (displayNameMismatch(displayName, address)) {
        reasons << tr("The sender's name, \"%1\", looks like a different address than the one it came from (%2).")
                       .arg(displayName.left(200).toHtmlEscaped(), address.left(320).toHtmlEscaped());
    }
    if (reasons.isEmpty()) {
        return {};
    }
    QString html = QStringLiteral("<b>\u26a0 %1</b>").arg(tr("Check the sender before you trust this message."));
    for (const QString &s : reasons) {
        html += QStringLiteral("<br>\u2022 ") + s;
    }
    if (r.trusted) {
        html += QStringLiteral("<br><small>spf=%1 dkim=%2 dmarc=%3</small>")
                    .arg((r.spf.isEmpty() ? QStringLiteral("none") : r.spf).toHtmlEscaped(),
                         (r.dkim.isEmpty() ? QStringLiteral("none") : r.dkim).toHtmlEscaped(),
                         (r.dmarc.isEmpty() ? QStringLiteral("none") : r.dmarc).toHtmlEscaped());
    }
    return html;
}

} // namespace Auth
} // namespace zmail
