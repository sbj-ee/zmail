#include "RemoteImages.h"

#include <QRegularExpression>
#include <QSettings>

#include <algorithm>

namespace zmail::ui::RemoteImages {

namespace {
const QString kModeKey = QStringLiteral("privacy/remoteImages");
const QString kSendersKey = QStringLiteral("privacy/remoteImageSenders");
const QString kTrackersKey = QStringLiteral("privacy/blockTrackers");
const QString kHttpKey = QStringLiteral("privacy/allowHttpImages");

QStringList normalised(const QStringList &in)
{
    QStringList out;
    for (const QString &s : in) {
        const QString a = senderAddress(s);
        if (!a.isEmpty() && !out.contains(a)) {
            out << a;
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}
} // namespace

QString modeKey(RemoteImageMode m)
{
    switch (m) {
    case RemoteImageMode::Ask:
        return QStringLiteral("ask");
    case RemoteImageMode::Never:
        return QStringLiteral("never");
    case RemoteImageMode::Always:
        break;
    }
    return QStringLiteral("always");
}

RemoteImageMode modeFromKey(const QString &key)
{
    const QString k = key.trimmed().toLower();
    if (k == QLatin1String("ask")) {
        return RemoteImageMode::Ask;
    }
    if (k == QLatin1String("never")) {
        return RemoteImageMode::Never;
    }
    if (k == QLatin1String("always")) {
        return RemoteImageMode::Always;
    }
    return RemoteImageMode::Ask;
}

RemoteImageMode mode()
{
    // Default Ask (security review of 0.3.x; was Always). A saved choice,
    // including "always", is kept as it is.
    return modeFromKey(QSettings().value(kModeKey, QStringLiteral("ask")).toString());
}

bool allowInsecureHttp()
{
    return QSettings().value(kHttpKey, false).toBool();
}

void setMode(RemoteImageMode m)
{
    QSettings().setValue(kModeKey, modeKey(m));
}

bool blockTrackers()
{
    return QSettings().value(kTrackersKey, true).toBool();
}

void setBlockTrackers(bool on)
{
    QSettings().setValue(kTrackersKey, on);
}

QString senderAddress(const QString &from)
{
    static const QRegularExpression angle(QStringLiteral("<\\s*([^<>\\s]+@[^<>\\s]+)\\s*>"));
    const auto m = angle.match(from);
    QString a = m.hasMatch() ? m.captured(1) : from.trimmed();
    if (a.startsWith(QLatin1String("mailto:"), Qt::CaseInsensitive)) {
        a = a.mid(7);
    }
    static const QRegularExpression bare(QStringLiteral("^[^@\\s<>\"']+@[^@\\s<>\"']+$"));
    return bare.match(a).hasMatch() ? a.toLower() : QString();
}

QStringList allowedSenders()
{
    return normalised(QSettings().value(kSendersKey).toStringList());
}

void setAllowedSenders(const QStringList &senders)
{
    const QStringList n = normalised(senders);
    if (n.isEmpty()) {
        QSettings().remove(kSendersKey);
    } else {
        QSettings().setValue(kSendersKey, n);
    }
}

void allowSender(const QString &from)
{
    const QString a = senderAddress(from);
    if (a.isEmpty()) {
        return;
    }
    QStringList l = allowedSenders();
    l << a;
    setAllowedSenders(l);
}

bool isSenderAllowed(const QString &from)
{
    const QString a = senderAddress(from);
    return !a.isEmpty() && allowedSenders().contains(a);
}

bool shouldLoadFor(const QString &from)
{
    switch (mode()) {
    case RemoteImageMode::Always:
        return true;
    case RemoteImageMode::Ask:
        return isSenderAllowed(from);
    case RemoteImageMode::Never:
        break;
    }
    return false;
}

bool isTrackerUrl(const QUrl &url)
{
    const QString host = url.host().toLower();
    // Read-receipt / open-tracking services whose images are only pixels.
    static const QStringList hosts{
        QStringLiteral("mailtrack.io"),       QStringLiteral("mailfoogae.appspot.com"), // Streak
        QStringLiteral("t.yesware.com"),      QStringLiteral("app.yesware.com"),
        QStringLiteral("t.sidekickopen.com"), QStringLiteral("t.signauxun.com"),
        QStringLiteral("app.bananatag.com"),  QStringLiteral("getnotify.com"),
        QStringLiteral("app.mixmax.com"),     QStringLiteral("track.mixmax.com"),
        QStringLiteral("r.superhuman.com"),   QStringLiteral("pixel.mailtrack.io"),
        QStringLiteral("email.mixpanel.com"), QStringLiteral("open.convertkit-mail.com"),
    };
    for (const QString &h : hosts) {
        if (host == h || host.endsWith(QLatin1Char('.') + h)) {
            return true;
        }
    }
    // Open-tracking endpoints of the big senders (Mailchimp, SendGrid,
    // Mandrill, Mailgun, HubSpot...).
    const QString path = url.path().toLower();
    static const QRegularExpression paths(QStringLiteral(
        "(^|/)(track/open(\\.php)?|wf/open|trk/open|o\\.gif|open\\.gif|open\\.aspx|pixel\\.gif|beacon\\.gif)(/|$)"));
    if (paths.match(path).hasMatch()) {
        return true;
    }
    return false;
}

bool isTrackerImgTag(const QString &tag)
{
    using RE = QRegularExpression;
    const auto ci = RE::CaseInsensitiveOption;
    static const RE src(QStringLiteral("\\bsrc\\s*=\\s*([\"'])?\\s*([^\"'\\s>]+)"), ci);
    const auto s = src.match(tag);
    if (s.hasMatch() && isTrackerUrl(QUrl(s.captured(2)))) {
        return true;
    }
    static const RE dimAttr(QStringLiteral("\\b(width|height)\\s*=\\s*[\"']?\\s*([0-9.]+)\\s*(px)?\\s*[\"']?(?=[\\s/>])"), ci);
    for (auto it = dimAttr.globalMatch(tag); it.hasNext();) {
        if (it.next().captured(2).toDouble() <= 2.0) {
            return true;
        }
    }
    static const RE dimStyle(QStringLiteral("(^|[;\\s\"'])(width|height)\\s*:\\s*([0-9.]+)\\s*px"), ci);
    static const RE style(QStringLiteral("\\bstyle\\s*=\\s*(\"[^\"]*\"|'[^']*')"), ci);
    const auto st = style.match(tag);
    if (st.hasMatch()) {
        for (auto it = dimStyle.globalMatch(st.captured(1)); it.hasNext();) {
            if (it.next().captured(3).toDouble() <= 2.0) {
                return true;
            }
        }
    }
    return false;
}

} // namespace zmail::ui::RemoteImages
