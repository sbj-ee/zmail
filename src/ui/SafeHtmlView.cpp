#include "SafeHtmlView.h"

#include <QDesktopServices>
#include <QMessageBox>
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

QVariant SafeHtmlView::loadResource(int type, const QUrl &name)
{
    if (name.scheme() == QLatin1String("data") && type == QTextDocument::ImageResource) {
        return QTextBrowser::loadResource(type, name);
    }
    ++m_blocked; // http(s), file:, cid:, qrc: ... nothing leaves or reads the machine
    return {};
}

QString SafeHtmlView::sanitize(const QString &html, int *blockedImages)
{
    QString s = html;
    using RE = QRegularExpression;
    const auto opts = RE::CaseInsensitiveOption | RE::DotMatchesEverythingOption;
    static const RE body(QStringLiteral("<body[^>]*>(.*)</body>"), opts);
    const auto m = body.match(s);
    if (m.hasMatch()) {
        s = m.captured(1);
    }
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
    // Drop remote/cid images outright (no broken-image boxes; nothing fetched).
    static const RE remoteImgTag(QStringLiteral("<img\\b[^>]*\\bsrc\\s*=\\s*[\"']?\\s*(https?:|//|cid:)[^>]*>"), opts);
    s.remove(remoteImgTag);
    return s;
}

} // namespace zmail::ui
