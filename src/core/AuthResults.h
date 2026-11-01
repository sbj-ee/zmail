#pragma once

#include <QJsonArray>
#include <QString>

namespace zmail {

// What the receiving server (Gmail) recorded about a message's sender in its
// Authentication-Results header (RFC 8601): SPF, DKIM and DMARC. Only the
// topmost Authentication-Results header is read, and only when its authserv-id
// is Gmail's own (mx.google.com): anything below it, or with another id, could
// have been written by the sender and proves nothing.
struct AuthResults
{
    bool trusted = false; // the topmost header is Gmail's
    QString spf;          // "pass", "fail", "softfail", "neutral", "none", ... ("" = not reported)
    QString dkim;         // "pass" when any DKIM signature passed, else the first result
    QString dmarc;

    bool anyPass() const;
    bool dmarcFailed() const;
    // Warn: Gmail checked the sender, and DMARC failed or nothing passed.
    bool suspicious() const { return trusted && (dmarcFailed() || !anyPass()); }
};

namespace Auth {

inline constexpr const char *kTrustedAuthServId = "mx.google.com";

// The value of the first (topmost) Authentication-Results header, "" = none.
QString topmostHeader(const QJsonArray &gmailHeaders);
// Parse one Authentication-Results value.
AuthResults parse(const QString &headerValue);
// The display name looks like an email address or a domain name that isn't
// the sender's real address or domain ("PayPal <x@evil.example>" is fine;
// "paypal.com <x@evil.example>" or "help@bank.example <x@evil.example>" isn't).
bool displayNameMismatch(const QString &displayName, const QString &address);
// The banner text (HTML, escaped) for a message, "" = nothing to warn about.
QString warningHtml(const AuthResults &r, const QString &displayName, const QString &address);

} // namespace Auth
} // namespace zmail
