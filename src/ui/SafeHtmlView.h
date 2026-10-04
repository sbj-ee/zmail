#pragma once

#include <QHash>
#include <QImage>
#include <QSet>
#include <QTextBrowser>

class QNetworkAccessManager;

namespace zmail::ui {

// Message viewer for untrusted mail HTML. QTextBrowser has no JavaScript
// engine at all; on top of that this view
//  - never loads anything from disk, and nothing from the network unless the
//    user pressed "Load images" for this message (setRemoteImagesAllowed);
//    by default only data: URIs are rendered,
//  - never follows links itself: a click shows the real destination and asks.
// A QtWebEngine-based viewer (full CSS, still JS off) is a later milestone.
class SafeHtmlView : public QTextBrowser
{
    Q_OBJECT

public:
    explicit SafeHtmlView(QWidget *parent = nullptr);

    // Reduce an email's HTML to something safe to hand to setHtml():
    // drops <script>, <iframe>, <object>, <embed>, <form>, <link>, <meta>,
    // event-handler attributes and javascript: URLs; keeps the <body> content.
    // Remote (http/https/protocol-relative) and cid: images are counted in
    // *blockedImages; they're removed unless keepRemoteImages is true (then
    // http(s) ones stay so loadResource can fetch them on request).
    static QString sanitize(const QString &html, int *blockedImages = nullptr, bool keepRemoteImages = false);

    int blockedLoads() const { return m_blocked; }
    void resetBlocked() { m_blocked = 0; }

    // Per message: fetch http(s) images (no cookies, no auth, 15 s timeout,
    // 10 MB each). Off by default; the viewer's "Load images" turns it on.
    void setRemoteImagesAllowed(bool allowed);
    bool remoteImagesAllowed() const { return m_allowRemote; }
    int remoteFetches() const { return m_fetchesStarted; }

    QVariant loadResource(int type, const QUrl &name) override;

signals:
    void linkActivated(const QUrl &url);
    // A remote image finished downloading; re-render to show it.
    void remoteImageArrived();

private:
    void fetch(const QUrl &url);

    int m_blocked = 0;
    bool m_allowRemote = false;
    int m_fetchesStarted = 0;
    QNetworkAccessManager *m_nam = nullptr;
    QHash<QUrl, QImage> m_images;
    QSet<QUrl> m_pending;
};

} // namespace zmail::ui
