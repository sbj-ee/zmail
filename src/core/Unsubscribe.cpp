#include "Unsubscribe.h"

#include "AddressGuard.h"
#include "Mailto.h"

#include <QRegularExpression>

namespace zmail {

namespace {
constexpr int kMaxUrl = 2000;
bool g_loopbackForTests = false;

bool usableWebUrl(const QUrl &u)
{
    if (!u.isValid() || u.host().isEmpty() || !u.userInfo().isEmpty()) {
        return false;
    }
    const QString scheme = u.scheme().toLower();
    if (g_loopbackForTests && scheme == QLatin1String("http") && u.host() == QLatin1String("127.0.0.1")) {
        return true;
    }
    return scheme == QLatin1String("https") && !net::isBlockedLiteral(u.host()) &&
           u.host().compare(QLatin1String("localhost"), Qt::CaseInsensitive) != 0;
}
} // namespace

UnsubscribeInfo::Method UnsubscribeInfo::method() const
{
    if (oneClick && https.isValid()) {
        return Method::OneClick;
    }
    if (mailto.isValid()) {
        return Method::Mail;
    }
    return https.isValid() ? Method::Web : Method::None;
}

QString UnsubscribeInfo::target() const
{
    switch (method()) {
    case Method::OneClick:
    case Method::Web:
        return https.host();
    case Method::Mail:
        return Mailto::parse(mailto).to;
    case Method::None:
        break;
    }
    return {};
}

namespace Unsubscribe {

UnsubscribeInfo parse(const QString &listUnsubscribe, const QString &listUnsubscribePost)
{
    UnsubscribeInfo info;
    // "<https://example.com/u?x>, <mailto:leave@example.com?subject=unsubscribe>"
    static const QRegularExpression angle(QStringLiteral("<([^<>]*)>"));
    auto it = angle.globalMatch(listUnsubscribe);
    while (it.hasNext()) {
        QString raw = it.next().captured(1);
        raw.remove(QRegularExpression(QStringLiteral("\\s"))); // a folded header leaves spaces in a long URL
        if (raw.isEmpty() || raw.size() > kMaxUrl) {
            continue;
        }
        const QUrl url(raw, QUrl::StrictMode);
        if (Mailto::isMailto(url)) {
            if (!info.mailto.isValid() && !Mailto::parse(url).to.isEmpty()) {
                info.mailto = url;
            }
        } else if (!info.https.isValid() && usableWebUrl(url)) {
            info.https = url;
        }
    }
    QString post = listUnsubscribePost;
    post.remove(QRegularExpression(QStringLiteral("\\s")));
    info.oneClick = post.compare(QLatin1String(kOneClickBody), Qt::CaseInsensitive) == 0;
    return info;
}

void setLoopbackAllowedForTests(bool on)
{
    g_loopbackForTests = on;
}

} // namespace Unsubscribe
} // namespace zmail
