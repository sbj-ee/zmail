#pragma once

#include "ClientConfig.h"

#include <QDateTime>
#include <QObject>
#include <QPointer>
#include <QUrl>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

namespace zmail {

class LoopbackServer;
class TokenStore;

// Google OAuth 2.0 for installed apps: loopback redirect + PKCE (S256) +
// state check, system browser. Refresh token lives in the TokenStore
// (keyring); the access token stays in memory only.
class AuthManager : public QObject
{
    Q_OBJECT

public:
    // Sign-in scopes (PLAN.md §2): gmail.modify + openid email.
    static QStringList scopes();
    // Added later via incremental consent when Contacts sync is enabled.
    static QStringList contactScopes();
    // ...and when the vacation responder is first changed (reading it needs
    // nothing more than the sign-in scopes).
    static QStringList settingsScopes();

    using BrowserOpener = std::function<bool(const QUrl &)>;
    using TokenCb = std::function<void(const QString &accessToken, const QString &error)>;

    AuthManager(ClientConfig config, TokenStore *store, QNetworkAccessManager *nam, QObject *parent = nullptr);
    ~AuthManager() override;

    void setBrowserOpener(BrowserOpener opener) { m_open = std::move(opener); }
    void setRevokeUri(const QUrl &u) { m_revokeUri = u; }
    const ClientConfig &config() const { return m_config; }

    QString account() const { return m_account; }
    bool hasRefreshToken() const { return !m_refreshToken.isEmpty(); }
    bool signInInProgress() const;

    // Interactive sign-in. Emits signedIn() or signInFailed().
    void startSignIn(const QString &loginHint = {});
    // Incremental consent for People API scopes (include_granted_scopes=true).
    // Existing Gmail grants are kept; user is only asked for Contacts.
    void requestContactScopes();
    bool hasContactScopes() const;
    // The same for any optional scopes: asks only for what is missing, on
    // top of everything already granted. Emits signedIn() straight away when
    // nothing is missing.
    void requestScopes(const QStringList &scopes);
    bool hasScopes(const QStringList &scopes) const;
    QStringList grantedScopes() const { return m_grantedScopes; }
    void cancelSignIn();
    // How long startSignIn() waits for the browser (0 = the default,
    // LoopbackServer::kDefaultTimeoutMs, 15 minutes). For tests.
    void setSignInTimeoutMs(int ms) { m_signInTimeoutMs = ms; }
    int signInTimeoutMs() const;
    // "15 minutes", "30 seconds": for the timeout message.
    static QString describeTimeout(int ms);
    // Load a saved refresh token for `account` from the keyring.
    void restore(const QString &account, std::function<void(bool ok, const QString &error)> done);
    // A valid access token, refreshing if it expires within 60 s.
    void accessToken(TokenCb cb);
    // After a 401 from the API: drop the cached access token.
    void invalidateAccessToken();
    // Forget the refresh token locally and revoke it at Google (best effort).
    void signOut();

    // Exposed for tests.
    QUrl lastAuthUrl() const { return m_lastAuthUrl; }

signals:
    void signInStarted(const QUrl &authUrl);
    void signedIn(const QString &account);
    void signInFailed(const QString &reason);
    // The refresh token was rejected (revoked, expired, password change).
    void reauthRequired(const QString &reason);

private:
    void onCallback(const QString &code, const QString &state, const QString &error);
    void exchangeCode(const QString &code);
    void refresh();
    void finishRefresh(bool ok, const QString &error);
    QNetworkReply *postForm(const QUrl &url, const QList<QPair<QString, QString>> &form);
    static QString emailFromIdToken(const QString &idToken);
    static QString googleError(const QByteArray &body, QString *description = nullptr);

    ClientConfig m_config;
    TokenStore *m_store = nullptr;
    QNetworkAccessManager *m_nam = nullptr;
    BrowserOpener m_open;
    QUrl m_revokeUri = QUrl(QStringLiteral("https://oauth2.googleapis.com/revoke"));
    QPointer<LoopbackServer> m_loopback;
    QByteArray m_verifier;
    QByteArray m_state;
    QUrl m_redirect;
    QUrl m_lastAuthUrl;
    int m_signInTimeoutMs = 0;
    quint16 m_listenPort = 0; // for the log once the listener is gone

    QString m_account;
    QString m_refreshToken;
    QString m_accessToken;
    QDateTime m_accessExpiry;
    QStringList m_grantedScopes;
    bool m_refreshing = false;
    QStringList m_extraScopes; // optional scopes the next startSignIn() asks for
    QList<TokenCb> m_waiters;
};

} // namespace zmail
