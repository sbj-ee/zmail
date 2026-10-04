#pragma once

#include <QString>
#include <QUrl>

namespace zmail {

// OAuth client for a Google "Desktop app" client, read from the JSON that
// Google Cloud Console downloads (client_secret_<id>.apps.googleusercontent.com.json):
//   {"installed": {"client_id": "...", "client_secret": "...",
//                  "auth_uri": "https://accounts.google.com/o/oauth2/auth",
//                  "token_uri": "https://oauth2.googleapis.com/token", ...}}
// Google says a desktop client secret "is obviously not treated as a secret",
// but zmail still keeps it out of logs and in a 0600 file.
struct ClientConfig
{
    QString clientId;
    QString clientSecret;
    QUrl authUri = QUrl(QStringLiteral("https://accounts.google.com/o/oauth2/v2/auth"));
    QUrl tokenUri = QUrl(QStringLiteral("https://oauth2.googleapis.com/token"));

    bool isValid() const { return !clientId.isEmpty() && !clientSecret.isEmpty(); }

    enum class LoadStatus { Ok, Missing, Unreadable, BadJson, NotDesktopClient, BadEndpoint };
    struct LoadResult;

    // Default location: ~/.config/zmail/oauth-client.json, or $ZMAIL_OAUTH_CLIENT.
    static QString defaultPath();
    static LoadResult load(const QString &path = defaultPath());
    static LoadResult parse(const QByteArray &json);

    // Copy a downloaded client JSON to `dest` with mode 0600 (directory 0700),
    // after validating it. Returns an empty string on success, else an error.
    static QString install(const QString &source, const QString &dest = defaultPath());

    // chmod 0600.
    static bool tightenPermissions(const QString &path);
};

struct ClientConfig::LoadResult
{
    LoadStatus status = LoadStatus::Missing;
    ClientConfig config;
    bool permissionsTooOpen = false; // group/other can read: warn + offer to fix
    QString message;                 // user-facing; never contains the secret
};

} // namespace zmail
