#pragma once

// Helpers shared by the MainWindow*.cpp files; not part of MainWindow's interface.

#include "core/MailCache.h"
#include "ui/MessageView.h"

namespace zmail::ui::detail {

// What the viewer shows for a cached message: `loading` while its body is
// being fetched, `error` if that failed.
ViewMessage liveViewMessage(const zmail::CachedMessage &c, bool loading, const QString &error);

} // namespace zmail::ui::detail
