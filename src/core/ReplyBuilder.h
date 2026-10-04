#pragma once

#include "MailCache.h"
#include "MimeBuilder.h"

namespace zmail {

// Pre-filled headers and quoted body for Reply / Reply All / Forward.
struct ComposeDraft
{
    QString to, cc, bcc, subject;
    QString quotedText;   // plain-text quote / forwarded block
    QString quotedHtml;   // HTML equivalent
    QString inReplyTo;
    QStringList references;
    QString threadId;
};

namespace ReplyBuilder {

enum class Kind { Reply, ReplyAll, Forward };

// `self` is the signed-in address (dropped from Reply All recipients).
ComposeDraft make(Kind kind, const CachedMessage &original, const QString &self);

QString replySubject(const QString &subject);   // "Re: x" (no "Re: Re:")
QString forwardSubject(const QString &subject); // "Fwd: x"
// References for a reply: the parent's References + its Message-ID,
// trimmed to the last 20 ids (RFC 5322 §3.6.4 allows trimming the middle).
QStringList replyReferences(const QString &parentReferences, const QString &parentMessageId);
QString attribution(const CachedMessage &original); // "On Sat, Oct 3, 2026 at 8:12 PM, Priya <p@x> wrote:"

} // namespace ReplyBuilder
} // namespace zmail
