#include "ReplyBuilder.h"

#include "HtmlSanitizer.h"
#include "MessageParser.h"
#include "RichText.h"

#include <QLocale>
#include <QRegularExpression>
#include <QSet>

namespace zmail::ReplyBuilder {

namespace {
QString stripPrefixes(const QString &subject, const QRegularExpression &re)
{
    QString s = subject.trimmed();
    while (true) {
        const auto m = re.match(s);
        if (!m.hasMatch()) {
            return s;
        }
        s = s.mid(m.capturedLength()).trimmed();
    }
}

QString formatPerson(const QString &name, const QString &addr)
{
    return name.isEmpty() || name == addr ? addr : QStringLiteral("%1 <%2>").arg(name, addr);
}
} // namespace

QString replySubject(const QString &subject)
{
    static const QRegularExpression re(QStringLiteral("^(re|aw|sv)\\s*(\\[\\d+\\])?\\s*:\\s*"),
                                       QRegularExpression::CaseInsensitiveOption);
    return QStringLiteral("Re: ") + stripPrefixes(subject, re);
}

QString forwardSubject(const QString &subject)
{
    static const QRegularExpression re(QStringLiteral("^(fwd?|fw)\\s*:\\s*"), QRegularExpression::CaseInsensitiveOption);
    return QStringLiteral("Fwd: ") + stripPrefixes(subject, re);
}

QStringList replyReferences(const QString &parentReferences, const QString &parentMessageId)
{
    static const QRegularExpression id(QStringLiteral("<[^<>\\s]+>"));
    QStringList refs;
    for (auto it = id.globalMatch(parentReferences); it.hasNext();) {
        const QString r = it.next().captured(0);
        if (!refs.contains(r)) {
            refs << r;
        }
    }
    const QString pid = parentMessageId.trimmed();
    if (!pid.isEmpty() && !refs.contains(pid)) {
        refs << pid;
    }
    if (refs.size() > 20) {
        refs = QStringList{refs.first()} + refs.mid(refs.size() - 19);
    }
    return refs;
}

QString attribution(const CachedMessage &o)
{
    const QString when = QLocale(QLocale::English, QLocale::UnitedStates)
                             .toString(o.date().toLocalTime(), QStringLiteral("ddd, MMM d, yyyy 'at' h:mm AP"));
    return QStringLiteral("On %1, %2 wrote:").arg(when, formatPerson(o.fromName, o.fromAddr));
}

ComposeDraft make(Kind kind, const CachedMessage &o, const QString &self)
{
    ComposeDraft d;
    d.threadId = o.threadId;
    const QString body = o.bodyText.isEmpty() ? o.snippet : o.bodyText;
    if (kind == Kind::Forward) {
        d.subject = forwardSubject(o.subject);
        QStringList hdr{QStringLiteral("---------- Forwarded message ---------"),
                        QStringLiteral("From: ") + formatPerson(o.fromName, o.fromAddr),
                        QStringLiteral("Date: ") + QLocale(QLocale::English, QLocale::UnitedStates)
                                                       .toString(o.date().toLocalTime(), QStringLiteral("ddd, MMM d, yyyy 'at' h:mm AP")),
                        QStringLiteral("Subject: ") + o.subject,
                        QStringLiteral("To: ") + o.to};
        if (!o.cc.isEmpty()) {
            hdr << QStringLiteral("Cc: ") + o.cc;
        }
        d.quotedText = hdr.join(QLatin1Char('\n')) + QStringLiteral("\n\n") + body;
        QString hh;
        for (const QString &l : hdr) {
            hh += l.toHtmlEscaped() + QStringLiteral("<br>");
        }
        d.quotedHtml = QStringLiteral("<p>") + hh + QStringLiteral("</p>") +
                       (o.bodyHtml.isEmpty() ? QStringLiteral("<p>") + body.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")) + QStringLiteral("</p>")
                                             : html::sanitize(o.bodyHtml, nullptr, true));
        // A forward starts a new thread but still points at the original.
        d.references = replyReferences(o.references, o.messageIdHeader);
        d.threadId.clear();
        return d;
    }

    d.subject = replySubject(o.subject);
    d.inReplyTo = o.messageIdHeader;
    d.references = replyReferences(o.references, o.messageIdHeader);
    const QString selfLc = self.trimmed().toLower();
    const bool fromMe = o.fromAddr.toLower() == selfLc;
    // Replying to my own sent message goes to its original recipients.
    const QString primary = fromMe ? o.to : (o.replyTo.isEmpty() ? formatPerson(o.fromName, o.fromAddr) : o.replyTo);
    QSet<QString> seen{selfLc};
    auto keep = [&seen](const QString &list) {
        QStringList out;
        for (const QString &a : MimeBuilder::splitAddresses(list)) {
            const QString addr = MessageParser::splitAddress(a).second.toLower();
            if (!addr.isEmpty() && !seen.contains(addr)) {
                seen.insert(addr);
                out << a;
            }
        }
        return out;
    };
    d.to = keep(primary).join(QStringLiteral(", "));
    if (kind == Kind::ReplyAll) {
        const QStringList moreTo = keep(fromMe ? QString() : o.to);
        if (!moreTo.isEmpty()) {
            d.to += (d.to.isEmpty() ? QString() : QStringLiteral(", ")) + moreTo.join(QStringLiteral(", "));
        }
        d.cc = keep(o.cc).join(QStringLiteral(", "));
    }
    const QString attr = attribution(o);
    d.quotedText = attr + QLatin1Char('\n') + richtext::quotePlain(body);
    d.quotedHtml = richtext::quoteHtml(
        o.bodyHtml.isEmpty() ? body.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"))
                             : html::sanitize(o.bodyHtml, nullptr, true), // untrusted: no local files, scripts ...
        attr);
    return d;
}

} // namespace zmail::ReplyBuilder
