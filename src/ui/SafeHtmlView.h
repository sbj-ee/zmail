#pragma once

#include <QHash>
#include <QImage>
#include <QSet>
#include <QTextBrowser>

class QHostAddress;

class QNetworkAccessManager;

namespace zmail::ui {

// Message viewer for untrusted mail HTML. QTextBrowser has no JavaScript
// engine at all; on top of that this view
//  - never loads anything from disk, and nothing from the network unless
//    remote images are allowed for this message (setRemoteImagesAllowed,
//    per Settings > Privacy > Remote images or the "Load images" bar);
//    otherwise only data: URIs are rendered,
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
    // Resource references (src, background, CSS url()) keep only data:image/*
    // (not SVG), http(s) and cid:; bare paths, file:, qrc: etc. are dropped
    // (see html::sanitize). loadResource never returns a null QVariant, which
    // would make QTextDocument read the file from disk itself.
    static QString sanitize(const QString &html, int *blockedImages = nullptr, bool keepRemoteImages = false);
    // Remove known tracking pixels (RemoteImages::isTrackerImgTag) from
    // sanitized HTML whose remote images are kept; *count says how many.
    static QString dropTrackers(const QString &html, int *count = nullptr);

    int blockedLoads() const { return m_blocked; }
    void resetBlocked() { m_blocked = 0; }

    // Per message: fetch remote images (no cookies, no auth, 15 s timeout,
    // 10 MB each, 200 per message). Off until the viewer allows it (the
    // Remote images setting, or "Load images"). Reserved names (.example,
    // .test, .invalid, .localhost) are never looked up.
    // HTTPS only (plain http needs the hidden privacy/allowHttpImages), and
    // only to public addresses: the host is resolved first, every address is
    // checked (net::isBlockedAddress) and the request goes to the checked
    // address (Host header and TLS name stay the hostname), so DNS can't
    // swap in a private one between check and connect. Redirects are
    // followed by hand (at most 5), each hop checked the same way.
    void setRemoteImagesAllowed(bool allowed);
    // New message: forget the last one's images (the 200-image cap is per
    // message; in-flight replies for it are dropped).
    void clearRemoteImages()
    {
        m_images.clear();
        m_pending.clear();
    }
    bool remoteImagesAllowed() const { return m_allowRemote; }
    int remoteFetches() const { return m_fetchesStarted; } // requests sent
    int refusedFetches() const { return m_refused; }       // http, private address, redirect loop
    // The tests serve images from 127.0.0.1 and "localhost" (looked up for
    // real instead of being treated as a reserved name); nothing else turns
    // this on.
    static void setLoopbackAllowedForTests(bool on);

    QVariant loadResource(int type, const QUrl &name) override;

signals:
    void linkActivated(const QUrl &url);
    // A mailto: link was clicked: compose in zmail (Mailto::parse).
    void mailtoActivated(const QUrl &url);
    // A remote image finished downloading; re-render to show it.
    void remoteImageArrived();

private:
    void fetch(const QUrl &url);
    void fetchHop(const QUrl &url, const QUrl &target, int hops);
    void request(const QUrl &url, const QUrl &target, const QHostAddress &address, int hops);
    void refuse(const QUrl &url);

    int m_blocked = 0;
    bool m_allowRemote = false;
    int m_fetchesStarted = 0;
    int m_refused = 0;
    QNetworkAccessManager *m_nam = nullptr;
    QHash<QUrl, QImage> m_images;
    QSet<QUrl> m_pending;
};

} // namespace zmail::ui
