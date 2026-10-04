#include "SafeHtmlView.h"

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

} // namespace zmail::ui
