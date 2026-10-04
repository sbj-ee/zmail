#pragma once

#include <QString>
#include <QStringList>
#include <QUrl>

namespace zmail::ui {

// Settings > Privacy > Remote images. Stored in QSettings:
//   privacy/remoteImages        "always" (default) | "ask" | "never"
//   privacy/remoteImageSenders  addresses whose mail loads images in Ask mode
//   privacy/blockTrackers       bool, default true: known tracking pixels are
//                               dropped even when images load
// Whatever the mode, fetches never send or store cookies or credentials, are
// capped in size and count, and cid:/file: images are never fetched
// (SafeHtmlView).
enum class RemoteImageMode { Always, Ask, Never };

namespace RemoteImages {

RemoteImageMode mode();
void setMode(RemoteImageMode m);
QString modeKey(RemoteImageMode m);              // "always" / "ask" / "never"
RemoteImageMode modeFromKey(const QString &key); // unknown -> Always

bool blockTrackers();
void setBlockTrackers(bool on);

// Sender allow list (Ask mode), lower-cased bare addresses, sorted.
QStringList allowedSenders();
void setAllowedSenders(const QStringList &senders);
void allowSender(const QString &from);
bool isSenderAllowed(const QString &from);

// "Name <Addr@Example.com>" -> "addr@example.com"; "" if there's no address.
QString senderAddress(const QString &from);

// Should this message's remote images load without asking?
bool shouldLoadFor(const QString &from);

// Known open-tracking pixels: an <img> whose width or height attribute or
// inline style is 0-2 px, or whose URL is on a known tracker host or path.
bool isTrackerUrl(const QUrl &url);
bool isTrackerImgTag(const QString &imgTag);

} // namespace RemoteImages
} // namespace zmail::ui
