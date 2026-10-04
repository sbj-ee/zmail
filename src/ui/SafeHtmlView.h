#pragma once

#include <QTextBrowser>

namespace zmail::ui {

// Message viewer for untrusted mail HTML. QTextBrowser has no JavaScript
// engine at all; on top of that this view
//  - never loads anything from the network or disk (remote images blocked;
//    only data: URIs are rendered),
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
    static QString sanitize(const QString &html, int *blockedImages = nullptr);

    int blockedLoads() const { return m_blocked; }
    void resetBlocked() { m_blocked = 0; }

    QVariant loadResource(int type, const QUrl &name) override;

signals:
    void linkActivated(const QUrl &url);

private:
    int m_blocked = 0;
};

} // namespace zmail::ui
