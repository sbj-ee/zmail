#include "SafeHtmlView.h"

#include "RemoteImages.h"
#include "core/HtmlSanitizer.h"

#include <QDesktopServices>
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
} // namespace

void SafeHtmlView::setRemoteImagesAllowed(bool allowed)
{
    m_allowRemote = allowed;
    if (!allowed) {
        m_images.clear();
        m_pending.clear();
    }
}

void SafeHtmlView::fetch(const QUrl &url)
{
    if (m_pending.contains(url) || m_pending.size() + m_images.size() >= kMaxRemoteImages) {
        return;
    }
    // RFC 2606 / 6761 names can't resolve: don't send them to DNS at all.
    const QString host = url.host().toLower();
    for (const char *tld : {".example", ".test", ".invalid", ".localhost"}) {
        if (host.endsWith(QLatin1String(tld)) || host == QLatin1String(tld + 1)) {
            QImage none(1, 1, QImage::Format_ARGB32);
            none.fill(Qt::transparent);
            m_images.insert(url, none);
            return;
        }
    }
    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
        m_nam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
    }
    m_pending.insert(url);
    ++m_fetchesStarted;
    QNetworkRequest req(url);
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
    connect(reply, &QNetworkReply::finished, this, [this, self, reply, url]() {
        reply->deleteLater();
        if (!self || !m_pending.remove(url) || !m_allowRemote) {
            return;
        }
        QImage img;
        if (reply->error() == QNetworkReply::NoError) {
            img.loadFromData(reply->read(kMaxImageBytes));
        }
        if (img.isNull()) {
            img = QImage(1, 1, QImage::Format_ARGB32);
            img.fill(Qt::transparent);
        }
        m_images.insert(url, img);
        emit remoteImageArrived();
    });
}

QVariant SafeHtmlView::loadResource(int type, const QUrl &name)
{
    // Never return a null QVariant: QTextDocument then reads the "resource"
    // from disk itself (bare paths, file:, /dev/zero ...).
    const QString scheme = name.scheme().toLower();
    if (type == QTextDocument::ImageResource && scheme == QLatin1String("data")) {
        return html::dataImage(name);
    }
    if (m_allowRemote && type == QTextDocument::ImageResource &&
        (scheme == QLatin1String("http") || scheme == QLatin1String("https"))) {
        const auto it = m_images.constFind(name);
        if (it != m_images.constEnd()) {
            return *it;
        }
        fetch(name);
        return html::blockedResource(); // until it arrives (remoteImageArrived re-renders)
    }
    ++m_blocked; // http(s), file:, cid:, qrc:, paths ... nothing leaves or reads the machine
    return html::blockedResource();
}

QString SafeHtmlView::sanitize(const QString &html, int *blockedImages, bool keepRemoteImages)
{
    return html::sanitize(html, blockedImages, keepRemoteImages);
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
