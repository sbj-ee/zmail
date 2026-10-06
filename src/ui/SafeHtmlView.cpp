#include "SafeHtmlView.h"

#include "core/AddressGuard.h"
#include "RemoteImages.h"

#include <QDesktopServices>
#include <QHostAddress>
#include <QHostInfo>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>

namespace zmail::ui {

SafeHtmlView::SafeHtmlView(QWidget *parent)
    : QTextBrowser(parent)
{
    setOpenLinks(false);
    setOpenExternalLinks(false);
    connect(this, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) {
        emit linkActivated(url);
        const QString scheme = url.scheme().toLower();
        if (scheme != QLatin1String("http") && scheme != QLatin1String("https") && scheme != QLatin1String("mailto")) {
            return;
        }
        const auto choice = QMessageBox::question(
            this, tr("Open link?"),
            tr("This link goes to:\n\n%1\n\nOpen it in your browser?").arg(url.toDisplayString()));
        if (choice == QMessageBox::Yes) {
            QDesktopServices::openUrl(url);
        }
    });
}

namespace {
constexpr qint64 kMaxImageBytes = 10 * 1024 * 1024;
constexpr int kMaxRemoteImages = 200;
constexpr int kMaxRedirects = 5;
bool s_loopbackForTests = false;

bool addressAllowed(const QHostAddress &a)
{
    if (s_loopbackForTests && a.isLoopback()) {
        return true;
    }
    return !net::isBlockedAddress(a);
}

QImage transparentPixel()
{
    QImage none(1, 1, QImage::Format_ARGB32);
    none.fill(Qt::transparent);
    return none;
}
} // namespace

void SafeHtmlView::setRemoteImagesAllowed(bool allowed)
{
    m_allowRemote = allowed;
    if (!allowed) {
        m_images.clear();
        m_pending.clear();
    }
}

void SafeHtmlView::setLoopbackAllowedForTests(bool on)
{
    s_loopbackForTests = on;
}

void SafeHtmlView::fetch(const QUrl &url)
{
    if (m_pending.contains(url) || m_pending.size() + m_images.size() >= kMaxRemoteImages) {
        return;
    }
    m_pending.insert(url);
    fetchHop(url, url, 0);
}

// Give up on url (the image a message asked for): it shows as nothing.
void SafeHtmlView::refuse(const QUrl &url)
{
    if (m_pending.remove(url)) {
        ++m_refused;
        m_images.insert(url, transparentPixel());
    }
}

// One hop of fetching url: target is url itself or where a redirect sent it.
void SafeHtmlView::fetchHop(const QUrl &url, const QUrl &target, int hops)
{
    const QString scheme = target.scheme().toLower();
    if (scheme != QLatin1String("https") && !(scheme == QLatin1String("http") && RemoteImages::allowInsecureHttp())) {
        refuse(url);
        return;
    }
    // RFC 2606 / 6761 names can't resolve: don't send them to DNS at all.
    const QString host = target.host().toLower();
    for (const char *tld : {".example", ".test", ".invalid", ".localhost"}) {
        if (s_loopbackForTests && host == QLatin1String("localhost")) {
            break; // the tests' local server, looked up like any other name
        }
        if (host.endsWith(QLatin1String(tld)) || host == QLatin1String(tld + 1)) {
            m_pending.remove(url);
            m_images.insert(url, transparentPixel());
            return;
        }
    }
    if (host.isEmpty()) {
        refuse(url);
        return;
    }
    QHostAddress literal;
    if (literal.setAddress(host)) {
        if (addressAllowed(literal)) {
            request(url, target, literal, hops);
        } else {
            refuse(url);
        }
        return;
    }
    QPointer<SafeHtmlView> self(this);
    QHostInfo::lookupHost(host, this, [this, self, url, target, hops](const QHostInfo &info) {
        if (!self || !m_pending.contains(url)) {
            return;
        }
        // Connect only to an address that passed the check.
        const QHostAddress pick = net::pickAllowed(info.addresses(), s_loopbackForTests);
        if (pick.isNull()) {
            refuse(url); // no address, or only private ones
            return;
        }
        request(url, target, pick, hops);
    });
}

void SafeHtmlView::request(const QUrl &url, const QUrl &target, const QHostAddress &address, int hops)
{
    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
        m_nam->setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
    }
    ++m_fetchesStarted;
    QUrl direct = target;
    direct.setHost(address.toString()); // the address we checked, not a second lookup
    QNetworkRequest req(direct);
    QByteArray hostHeader = target.host(QUrl::FullyEncoded).toLatin1();
    if (hostHeader.contains(':')) {
        hostHeader = '[' + hostHeader + ']'; // IPv6 literal
    }
    if (target.port() != -1) {
        hostHeader += ':' + QByteArray::number(target.port());
    }
    req.setRawHeader("Host", hostHeader);
    req.setPeerVerifyName(target.host()); // TLS SNI and certificate name
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    req.setTransferTimeout(15000);
    req.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    req.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    req.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    req.setHeader(QNetworkRequest::UserAgentHeader, QByteArrayLiteral("zmail"));
    QNetworkReply *reply = m_nam->get(req);
    QPointer<SafeHtmlView> self(this);
    connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 got, qint64) {
        if (got > kMaxImageBytes) {
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, self, reply, url, target, hops]() {
        reply->deleteLater();
        if (!self || !m_pending.contains(url)) {
            return;
        }
        if (!m_allowRemote) {
            m_pending.remove(url);
            return;
        }
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QUrl location = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
        if (status >= 300 && status < 400 && location.isValid()) {
            if (hops >= kMaxRedirects) {
                refuse(url);
            } else {
                fetchHop(url, target.resolved(location), hops + 1); // scheme and address checked again
            }
            return;
        }
        m_pending.remove(url);
        QImage img;
        if (reply->error() == QNetworkReply::NoError) {
            img.loadFromData(reply->read(kMaxImageBytes));
        }
        m_images.insert(url, img.isNull() ? transparentPixel() : img);
        emit remoteImageArrived();
    });
}

QVariant SafeHtmlView::loadResource(int type, const QUrl &name)
{
    if (name.scheme() == QLatin1String("data") && type == QTextDocument::ImageResource) {
        return QTextBrowser::loadResource(type, name);
    }
    const QString scheme = name.scheme().toLower();
    if (m_allowRemote && type == QTextDocument::ImageResource &&
        (scheme == QLatin1String("http") || scheme == QLatin1String("https"))) {
        const auto it = m_images.constFind(name);
        if (it != m_images.constEnd()) {
            return *it;
        }
        fetch(name);
        return {};
    }
    ++m_blocked; // http(s), file:, cid:, qrc: ... nothing leaves or reads the machine
    return {};
}

QString SafeHtmlView::sanitize(const QString &html, int *blockedImages, bool keepRemoteImages)
{
    QString s = html;
    using RE = QRegularExpression;
    const auto opts = RE::CaseInsensitiveOption | RE::DotMatchesEverythingOption;
    // Don't crop to <body>...</body>: some senders (Meetup) concatenate two
    // documents, the first a body holding only a tracking pixel, the second
    // the real mail with no <body> at all, which then showed as blank. Head
    // sections go below; <html>/<body> tags are dropped with the others.
    static const RE prolog(QStringLiteral("<!DOCTYPE[^>]*>|<\\?xml[^>]*>"), opts);
    s.remove(prolog);
    static const RE paired(
        QStringLiteral("<(script|style|iframe|object|embed|form|textarea|select|button|noscript|template|svg|math|head|title)\\b.*?</\\1\\s*>"),
        opts);
    s.remove(paired);
    static const RE single(QStringLiteral("<\\/?(script|iframe|object|embed|form|input|link|meta|base|frame|frameset|applet|html|body)\\b[^>]*>"), opts);
    s.remove(single);
    static const RE events(QStringLiteral("\\s+on[a-z]+\\s*=\\s*(\"[^\"]*\"|'[^']*'|[^\\s>]+)"), opts);
    s.remove(events);
    static const RE jsUrl(QStringLiteral("(href|src)\\s*=\\s*([\"']?)\\s*(javascript|vbscript|file):[^\"'>\\s]*\\2"), opts);
    s.replace(jsUrl, QStringLiteral("\\1=\"#\""));
    if (blockedImages) {
        static const RE remoteImg(QStringLiteral("<img\\b[^>]*\\bsrc\\s*=\\s*[\"']?\\s*(https?:|//|cid:)"), opts);
        int n = 0;
        auto it = remoteImg.globalMatch(s);
        while (it.hasNext()) {
            it.next();
            ++n;
        }
        *blockedImages = n;
    }
    if (keepRemoteImages) {
        // Protocol-relative URLs -> https; cid: parts aren't fetched yet.
        static const RE protoRel(QStringLiteral("(<img\\b[^>]*\\bsrc\\s*=\\s*[\"']?)\\s*//"), opts);
        s.replace(protoRel, QStringLiteral("\\1https://"));
        static const RE cidImgTag(QStringLiteral("<img\\b[^>]*\\bsrc\\s*=\\s*[\"']?\\s*cid:[^>]*>"), opts);
        s.remove(cidImgTag);
        return s;
    }
    // Drop remote/cid images outright (no broken-image boxes; nothing fetched).
    static const RE remoteImgTag(QStringLiteral("<img\\b[^>]*\\bsrc\\s*=\\s*[\"']?\\s*(https?:|//|cid:)[^>]*>"), opts);
    s.remove(remoteImgTag);
    return s;
}

QString SafeHtmlView::dropTrackers(const QString &s, int *count)
{
    using RE = QRegularExpression;
    static const RE remoteTag(QStringLiteral("<img\\b[^>]*\\bsrc\\s*=\\s*[\"']?\\s*https?:[^>]*>"),
                              RE::CaseInsensitiveOption | RE::DotMatchesEverythingOption);
    int n = 0;
    QString out;
    out.reserve(s.size());
    qsizetype last = 0;
    for (auto it = remoteTag.globalMatch(s); it.hasNext();) {
        const auto m = it.next();
        if (RemoteImages::isTrackerImgTag(m.captured(0))) {
            out += QStringView(s).mid(last, m.capturedStart() - last);
            last = m.capturedEnd();
            ++n;
        }
    }
    out += QStringView(s).mid(last);
    if (count) {
        *count = n;
    }
    return out;
}

} // namespace zmail::ui
