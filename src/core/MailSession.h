#pragma once

#include "AuthManager.h"
#include "ClientConfig.h"

#include <QObject>
#include <QUrl>
#include <memory>

class QNetworkAccessManager;

namespace zmail {

class GmailClient;
class MailCache;
class SyncEngine;
class TokenStore;

struct SessionOptions
{
    ClientConfig::LoadResult client;                 // from ClientConfig::load() (or a test config)
    TokenStore *store = nullptr;                     // KeychainTokenStore in the app
    QUrl apiBase;                                    // empty = Gmail
    QUrl revokeUri;                                  // empty = Google
    QString cachePathOverride;                       // ":memory:" in tests
    AuthManager::BrowserOpener browserOpener;        // empty = QDesktopServices
    bool rememberAccount = true;                     // QSettings "account/email"
    int pollIntervalMs = 30000;
    int initialCount = 500;
    int backoffBaseMs = 1000;
};

// One signed-in Gmail account: auth + API client + cache + sync.
class MailSession : public QObject
{
    Q_OBJECT

public:
    enum class State { NeedsClient, SignedOut, Restoring, SigningIn, SignedIn };
    Q_ENUM(State)

    explicit MailSession(SessionOptions opts, QObject *parent = nullptr);
    ~MailSession() override;

    // App default: ~/.config/zmail/oauth-client.json + system keyring.
    static MailSession *createDefault(QObject *parent = nullptr);

    State state() const { return m_state; }
    QString account() const;
    const ClientConfig::LoadResult &client() const { return m_opts.client; }
    QString clientPath() const { return m_clientPath; }
    void setClientPath(const QString &p) { m_clientPath = p; }

    // Re-read the client file (after "Choose client file…").
    void reloadClient();
    void setClient(const ClientConfig::LoadResult &client);

    void restoreSaved();   // silently resume the remembered account
    void signIn();
    void cancelSignIn();
    void signOut();

    AuthManager *auth() const { return m_auth.get(); }
    GmailClient *api() const { return m_api.get(); }
    MailCache *cache() const { return m_cache.get(); }
    SyncEngine *sync() const { return m_sync.get(); }
    QNetworkAccessManager *network() const { return m_nam; }

signals:
    void stateChanged(zmail::MailSession::State state);
    void signInFailed(const QString &reason);
    void reauthRequired(const QString &reason);
    void ready(); // cache open and sync started
    void clientChanged();

private:
    void buildAuth();
    void setState(State s);
    void startAccount(const QString &account);
    void stopAccount();

    SessionOptions m_opts;
    QString m_clientPath;
    QNetworkAccessManager *m_nam;
    std::unique_ptr<TokenStore> m_ownedStore;
    std::unique_ptr<AuthManager> m_auth;
    std::unique_ptr<GmailClient> m_api;
    std::unique_ptr<MailCache> m_cache;
    std::unique_ptr<SyncEngine> m_sync;
    State m_state = State::NeedsClient;
};

} // namespace zmail
