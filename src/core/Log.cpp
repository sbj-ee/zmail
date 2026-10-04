#include "Log.h"

Q_LOGGING_CATEGORY(lcAuth, "zmail.auth", QtInfoMsg)
Q_LOGGING_CATEGORY(lcSync, "zmail.sync", QtInfoMsg)
Q_LOGGING_CATEGORY(lcGmail, "zmail.gmail", QtInfoMsg)

namespace zmail {
QString redact(const QString &secret)
{
    if (secret.isEmpty()) {
        return QStringLiteral("(empty)");
    }
    return secret.left(4) + QStringLiteral("\u2026(%1 chars)").arg(secret.size());
}
} // namespace zmail
