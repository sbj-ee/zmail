#pragma once

#include <QString>
#include <QUrl>

namespace zmail {

// A mailto: link (RFC 6068) as fields for zmail's own compose window.
// Only to, cc, bcc, subject and body are used; attach=, attachment= and
// every other key are ignored, so a link can't attach a local file or set
// other headers. Header fields lose CR, LF and other control characters;
// the body is plain text with LF line ends. Long values are cut.
struct MailtoFields
{
    QString to, cc, bcc, subject, body;
    bool isEmpty() const { return to.isEmpty() && cc.isEmpty() && bcc.isEmpty() && subject.isEmpty() && body.isEmpty(); }
};

namespace Mailto {
bool isMailto(const QUrl &url);
MailtoFields parse(const QUrl &url);
} // namespace Mailto

} // namespace zmail
