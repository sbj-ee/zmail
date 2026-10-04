#pragma once

#include <QLoggingCategory>
#include <QString>

// Logging categories. NEVER log client secrets, authorization codes, access
// tokens or refresh tokens; use zmail::redact() for anything token-shaped.
Q_DECLARE_LOGGING_CATEGORY(lcAuth)
Q_DECLARE_LOGGING_CATEGORY(lcSync)
Q_DECLARE_LOGGING_CATEGORY(lcGmail)

namespace zmail {
// "ya29.a0Af…" -> "ya29…(73 chars)": enough to tell tokens apart, useless to steal.
QString redact(const QString &secret);
} // namespace zmail
