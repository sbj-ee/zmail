#pragma once

#include <QString>
#include <QUrl>

namespace zmail {

// What a message's List-Unsubscribe header (RFC 2369) offers, and whether
// List-Unsubscribe-Post (RFC 8058) makes the https one a one-click POST.
struct UnsubscribeInfo
{
    enum class Method {
        None,
        OneClick, // POST "List-Unsubscribe=One-Click" to `https`: nothing to open, nothing to fill in
        Mail,     // send the message `mailto` describes
        Web,      // open `https` in the browser; the sender's page takes it from there
    };

    QUrl https;
    QUrl mailto;
    bool oneClick = false;

    Method method() const;
    bool available() const { return method() != Method::None; }
    // Who hears about it: the host of the URL, or the address mailed.
    QString target() const;
};

namespace Unsubscribe {

// Only https URLs (never http, never an IP literal in a private range) and
// mailto: links with a recipient are kept; anything else in the header is
// ignored. One-click needs both headers, as RFC 8058 says.
UnsubscribeInfo parse(const QString &listUnsubscribe, const QString &listUnsubscribePost);

// The body RFC 8058 has the client POST.
inline constexpr char kOneClickBody[] = "List-Unsubscribe=One-Click";

// The tests' mock server is plain http on 127.0.0.1; nothing else turns this on.
void setLoopbackAllowedForTests(bool on);

} // namespace Unsubscribe
} // namespace zmail
