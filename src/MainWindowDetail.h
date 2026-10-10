#pragma once

// Helpers shared by the MainWindow*.cpp files; not part of MainWindow's interface.

#include "core/MailCache.h"
#include "ui/MessageView.h"

namespace zmail::ui::detail {

// What the viewer shows for a cached message: `loading` while its body is
// being fetched, `error` if that failed.
ViewMessage liveViewMessage(const zmail::CachedMessage &c, bool loading, const QString &error);

// The full message still has to come from Gmail: no body yet, or one cached
// before zmail read what a message says about unsubscribing and invitations
// (the cached body is shown meanwhile).
inline bool needsFetch(const zmail::CachedMessage &c)
{
    return !c.hasBody || c.extrasVersion < zmail::MailCache::kExtrasVersion;
}

} // namespace zmail::ui::detail
