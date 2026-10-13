#include "ServerLimits.h"

#include <QCoreApplication>
#include <QRandomGenerator>

#include <algorithm>
#include <cmath>

namespace zmail::ServerLimits {

namespace {
bool has(const QString &text, const char *needle)
{
    return text.contains(QLatin1String(needle), Qt::CaseInsensitive);
}
QString tr(const char *s)
{
    return QCoreApplication::translate("ServerLimits", s);
}
} // namespace

Kind classifyImap(const QString &text)
{
    if (has(text, "AUTHENTICATIONFAILED") || has(text, "Invalid credentials") || has(text, "refused the app password")
        || has(text, "Application-specific password required") || has(text, "[AUTHORIZATIONFAILED]")) {
        return Kind::AuthFailed;
    }
    if (has(text, "Too many simultaneous connections") || has(text, "too many connections")) {
        return Kind::TooManyConnections;
    }
    if (has(text, "bandwidth") || has(text, "exceeded command") || has(text, "[THROTTLED]") || has(text, "[OVERQUOTA]")
        || has(text, "[LIMIT]")) {
        return Kind::Bandwidth;
    }
    if (has(text, "[UNAVAILABLE]") || has(text, "Temporary System") || has(text, "try again later")) {
        return Kind::Unavailable;
    }
    return Kind::None;
}

Kind classifySmtp(int code, const QString &text)
{
    if (code == 535 || code == 534 || has(text, "5.7.8") || has(text, "BadCredentials")) {
        return Kind::AuthFailed;
    }
    if (has(text, "5.4.5") || has(text, "sending limit")) {
        return Kind::SendingLimit;
    }
    if (has(text, "Too many login attempts") || has(text, "too many connections")
        || has(text, "Too many simultaneous")) {
        return Kind::TooManyConnections;
    }
    if (code == 421 || code == 450 || code == 451 || code == 452 || code == 454) {
        return Kind::Unavailable;
    }
    return Kind::None;
}

qint64 backoffMs(Kind kind, int attempt, double scale, bool jitter)
{
    qint64 base = 0;
    qint64 cap = 0;
    switch (kind) {
    case Kind::AuthFailed:
    case Kind::SendingLimit:
        return -1;
    case Kind::TooManyConnections:
        base = 60'000;
        cap = 15 * 60'000;
        break;
    case Kind::Bandwidth:
        base = 10 * 60'000;
        cap = 60 * 60'000;
        break;
    case Kind::None:
    case Kind::Unavailable:
        base = 2'000;
        cap = 5 * 60'000;
        break;
    }
    const int n = std::clamp(attempt, 0, 20);
    double ms = std::min(double(cap), double(base) * std::pow(2.0, n));
    if (jitter) {
        ms *= 0.8 + 0.4 * QRandomGenerator::global()->generateDouble();
    }
    return std::max<qint64>(1, qint64(ms * scale));
}

QString explain(Kind kind, const QString &server)
{
    switch (kind) {
    case Kind::AuthFailed:
        return tr("Google refused the app password for %1. It may have been revoked: make a new one and enter it in "
                  "the account dialog. zmail won't keep retrying, so Google doesn't lock the account.")
            .arg(server);
    case Kind::TooManyConnections:
        return tr("Gmail says there are too many connections to this account (it allows 15 for IMAP). Close other "
                  "mail apps or wait; zmail will try again in a few minutes.");
    case Kind::Bandwidth:
        return tr("Gmail is limiting this account (command or bandwidth limit). zmail will wait and try again; this "
                  "usually clears within an hour.");
    case Kind::Unavailable:
        return tr("%1 is temporarily unavailable; trying again shortly.").arg(server);
    case Kind::SendingLimit:
        return tr("Gmail's daily sending limit has been reached for this account. The message is kept; try again "
                  "tomorrow.");
    case Kind::None:
        break;
    }
    return {};
}

} // namespace zmail::ServerLimits
