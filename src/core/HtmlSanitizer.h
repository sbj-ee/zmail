#pragma once

#include <QImage>
#include <QString>
#include <QUrl>
#include <QVariant>

namespace zmail::html {

// Reduce untrusted mail HTML to something safe to hand to setHtml() (the
// viewer, and the quote in a reply/forward). See SafeHtmlView::sanitize().
// Resource attributes (img src, background, CSS url()) keep only an allowed
// scheme: data:image/* (not SVG), http(s)/protocol-relative (remote images,
// governed by the viewer's remote-image setting) and cid:. Anything else,
// including bare absolute or relative paths, file:, qrc: and entity-encoded
// variants, is dropped: an <img> with such a src is removed.
QString sanitize(const QString &html, int *blockedImages = nullptr, bool keepRemoteImages = false);

// What a document resource request may get. QTextDocument reads the file
// from disk itself when loadResource() returns a null QVariant, so blocked
// resources must get this 1x1 transparent image instead.
QImage blockedResource();
// A data:image/... URL decoded to an image (blockedResource() if it isn't
// one, is SVG, or doesn't decode). Never touches the disk.
QImage dataImage(const QUrl &url);

} // namespace zmail::html
