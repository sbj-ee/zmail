#pragma once

#include "AuthManager.h"
#include "ClientConfig.h"
#include "Vacation.h"

#include <QObject>
#include <QUrl>
#include <memory>

class QNetworkAccessManager;

namespace zmail {

class GmailClient;
class MailCache;
class ContactStore;
class PeopleClient;
class ContactsSync;
class Sender;
class SyncEngine;
class TokenStore;

struct SessionOptions
{
    ClientConfig::LoadResult client;                 // from ClientConfig::load() (or a test config)
    TokenStore *store = nullptr;                     // KeychainTokenStore in the app
    QUrl apiBase;                                    // empty = Gmail
    QUrl revokeUri;                                  // empty = Google
    QString cachePathOverride;                       // ":memory:" in tests
    QString contactsPathOverride;                    // ":memory:" in tests
    AuthManager::BrowserOpener browserOpener;        // empty = QDesktopServices
    bool rememberAccount = true;                     // QSettings "account/email"
    int pollIntervalMs = 15000; // how soon mail and changes made elsewhere show up
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
    ContactStore *contacts() const { return m_contacts.get(); }
    ContactsSync *contactsSync() const { return m_contactsSync.get(); }
    PeopleClient *people() const { return m_people.get(); }
    SyncEngine *sync() const { return m_sync.get(); }
    void enableContactsSync(); // incremental consent if needed, then sync

    // Gmail's vacation responder. loadVacation() asks Google; vacation() is
    // what it last said (read once when the account starts). saveVacation()
    // first sends the browser for Google's consent to change settings, the
    // first time, and reports what Google stored.
    using VacationCb = std::function<void(const VacationSettings &settings, const QString &error)>;
    void loadVacation(VacationCb cb);
    void saveVacation(const VacationSettings &settings, VacationCb cb);
    const VacationSettings &vacation() const { return m_vacation; }
    QNetworkAccessManager *network() const { return m_nam; }
    Sender *sender() const { return m_sender.get(); }

    // "Display Name <address>" from the default send-as identity
    // (users.settings.sendAs), or just the address until that arrives.
    QString fromHeader() const;
    // After a send: pull the new message into Sent right away.
    void syncSoon();

signals:
    void stateChanged(zmail::MailSession::State state);
    void signInFailed(const QString &reason);
    void reauthRequired(const QString &reason);
    void ready(); // cache open and sync started
    void clientChanged();
    void identityChanged();
    void vacationChanged();
    // Google's consent for something extra (Contacts) was refused or timed
    // out. The account stays signed in.
    void scopeRequestFailed(const QString &reason);

private:
    void buildAuth();
    void setState(State s);
    void startAccount(const QString &account);
    void stopAccount();
    // Incremental consent for optional scopes; done("") once they are
    // granted (at once if they already are), else done(why not).
    void requestScopes(const QStringList &scopes, std::function<void(const QString &error)> done);

    SessionOptions m_opts;
    QString m_clientPath;
    QNetworkAccessManager *m_nam;
    std::unique_ptr<TokenStore> m_ownedStore;
    std::unique_ptr<AuthManager> m_auth;
    std::unique_ptr<GmailClient> m_api;
    std::unique_ptr<MailCache> m_cache;
    std::unique_ptr<ContactStore> m_contacts;
    std::unique_ptr<PeopleClient> m_people;
    std::unique_ptr<ContactsSync> m_contactsSync;
    std::unique_ptr<SyncEngine> m_sync;
    std::unique_ptr<Sender> m_sender;
    QString m_displayName;
    VacationSettings m_vacation;
    QStringList m_scopesWanted;
    std::function<void(const QString &error)> m_scopesDone; // set while consent is in the browser
    State m_state = State::NeedsClient;
};

} // namespace zmail
