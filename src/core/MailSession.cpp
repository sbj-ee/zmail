#include "MailSession.h"

#include "Sender.h"

#include "GmailClient.h"
#include "Log.h"
#include "MailCache.h"
#include "ContactStore.h"
#include "PeopleClient.h"
#include "SyncEngine.h"
#include "TokenStore.h"

#include <QNetworkAccessManager>
#include <QJsonArray>
#include <QSettings>

namespace zmail {

namespace {
const QString kAccountKey = QStringLiteral("account/email");
}

MailSession::MailSession(SessionOptions opts, QObject *parent)
    : QObject(parent)
    , m_opts(std::move(opts))
    , m_clientPath(ClientConfig::defaultPath())
    , m_nam(new QNetworkAccessManager(this))
{
    if (!m_opts.store) {
        m_ownedStore = std::make_unique<KeychainTokenStore>();
        m_opts.store = m_ownedStore.get();
    }
    buildAuth();
}

MailSession::~MailSession()
{
    stopAccount();
}

MailSession *MailSession::createDefault(QObject *parent)
{
    SessionOptions o;
    o.client = ClientConfig::load();
    if (o.client.permissionsTooOpen) {
        qCWarning(lcAuth) << "OAuth client file" << ClientConfig::defaultPath()
                          << "is readable by other users; it should be mode 0600";
    }
    return new MailSession(std::move(o), parent);
}

void MailSession::buildAuth()
{
    stopAccount();
    m_auth.reset();
    if (m_opts.client.status != ClientConfig::LoadStatus::Ok) {
        setState(State::NeedsClient);
        return;
    }
    m_auth = std::make_unique<AuthManager>(m_opts.client.config, m_opts.store, m_nam);
    if (m_opts.browserOpener) {
        m_auth->setBrowserOpener(m_opts.browserOpener);
    }
    if (m_opts.revokeUri.isValid()) {
        m_auth->setRevokeUri(m_opts.revokeUri);
    }
    connect(m_auth.get(), &AuthManager::signedIn, this, [this](const QString &account) {
        if (m_opts.rememberAccount) {
            QSettings().setValue(kAccountKey, account);
        }
        startAccount(account);
    });
    connect(m_auth.get(), &AuthManager::signInFailed, this, [this](const QString &reason) {
        setState(State::SignedOut);
        emit signInFailed(reason);
    });
    connect(m_auth.get(), &AuthManager::reauthRequired, this, [this](const QString &reason) {
        stopAccount();
        setState(State::SignedOut);
        emit reauthRequired(reason);
    });
    setState(State::SignedOut);
}

void MailSession::reloadClient()
{
    setClient(ClientConfig::load(m_clientPath));
}

void MailSession::setClient(const ClientConfig::LoadResult &client)
{
    m_opts.client = client;
    buildAuth();
    emit clientChanged();
}

QString MailSession::account() const
{
    return m_auth ? m_auth->account() : QString();
}

void MailSession::setState(State s)
{
    if (s != m_state) {
        m_state = s;
        emit stateChanged(s);
    }
}

void MailSession::restoreSaved()
{
    if (!m_auth || !m_opts.rememberAccount) {
        return;
    }
    const QString account = QSettings().value(kAccountKey).toString();
    if (account.isEmpty()) {
        return;
    }
    setState(State::Restoring);
    m_auth->restore(account, [this, account](bool ok, const QString &err) {
        if (!ok) {
            qCInfo(lcAuth) << "No saved sign-in for" << account << (err.isEmpty() ? QString() : err);
            setState(State::SignedOut);
            return;
        }
        startAccount(account);
    });
}

void MailSession::signIn()
{
    if (!m_auth) {
        return;
    }
    setState(State::SigningIn);
    m_auth->startSignIn();
}

void MailSession::cancelSignIn()
{
    if (m_auth) {
        m_auth->cancelSignIn();
    }
    if (m_state == State::SigningIn) {
        setState(State::SignedOut);
    }
}

void MailSession::signOut()
{
    stopAccount();
    if (m_auth) {
        m_auth->signOut();
    }
    if (m_opts.rememberAccount) {
        QSettings().remove(kAccountKey);
    }
    setState(m_auth ? State::SignedOut : State::NeedsClient);
}

void MailSession::startAccount(const QString &account)
{
    stopAccount();
    m_cache = std::make_unique<MailCache>();
    const QString path = m_opts.cachePathOverride.isEmpty() ? MailCache::defaultPath(account) : m_opts.cachePathOverride;
    if (!m_cache->open(path)) {
        // Nothing works without it (every write would silently do nothing):
        // stop here and say why. The saved sign-in is kept for a retry.
        const QString error = m_cache->lastError();
        qCWarning(lcSync) << "Couldn't open the mail cache:" << error;
        stopAccount();
        setState(State::SignedOut);
        emit signInFailed(tr("zmail couldn't open its mail cache at %1 (%2). Check that the folder exists, "
                             "is writable and has free space, then sign in again.")
                              .arg(path, error.isEmpty() ? tr("unknown error") : error));
        return;
    }
    m_contacts = std::make_unique<ContactStore>();
    const QString cpath = m_opts.contactsPathOverride.isEmpty() ? ContactStore::defaultPath(account)
                                                                : m_opts.contactsPathOverride;
    if (!m_contacts->open(cpath)) {
        qCWarning(lcSync) << "Couldn't open the contacts cache:" << m_contacts->lastError();
    }
    m_people = std::make_unique<PeopleClient>(m_auth.get(), m_nam);
    m_contactsSync = std::make_unique<ContactsSync>(m_people.get(), m_contacts.get());
    m_api = std::make_unique<GmailClient>(m_auth.get(), m_nam,
                                          m_opts.apiBase.isValid() ? m_opts.apiBase : GmailClient::defaultBaseUrl());
    m_api->setBackoffBaseMs(m_opts.backoffBaseMs);
    m_sync = std::make_unique<SyncEngine>(m_api.get(), m_cache.get());
    m_sync->setPollInterval(m_opts.pollIntervalMs);
    m_sync->setInitialCount(m_opts.initialCount);
    m_sender = std::make_unique<Sender>(m_api.get());
    m_displayName.clear();
    m_api->listSendAs([this](const QJsonObject &json, const ApiError &err) {
        if (err.isError) {
            return;
        }
        for (const auto &v : json.value(QStringLiteral("sendAs")).toArray()) {
            const QJsonObject o = v.toObject();
            if (o.value(QStringLiteral("isDefault")).toBool() || o.value(QStringLiteral("isPrimary")).toBool()) {
                m_displayName = o.value(QStringLiteral("displayName")).toString();
                emit identityChanged();
                return;
            }
        }
    });
    setState(State::SignedIn);
    emit ready();
    m_sync->start();
}

QString MailSession::fromHeader() const
{
    const QString addr = account();
    if (m_displayName.isEmpty() || addr.isEmpty()) {
        return addr;
    }
    return QStringLiteral("%1 <%2>").arg(m_displayName, addr);
}

void MailSession::syncSoon()
{
    if (m_sync) {
        m_sync->pollNow(true);
    }
}

void MailSession::stopAccount()
{
    if (m_sync) {
        m_sync->stop();
    }
    m_sync.reset();
    m_sender.reset();
    m_contactsSync.reset();
    m_people.reset();
    m_contacts.reset();
    m_api.reset();
    m_cache.reset();
}

void MailSession::enableContactsSync()
{
    if (!m_auth || !m_contactsSync) {
        return;
    }
    if (m_auth->hasContactScopes()) {
        m_contactsSync->sync();
        return;
    }
    // Incremental consent; sync once the new scopes are granted.
    connect(m_auth.get(), &AuthManager::signedIn, this, [this](const QString &) {
        if (m_auth->hasContactScopes() && m_contactsSync) {
            m_contactsSync->sync();
        }
    }, Qt::SingleShotConnection);
    m_auth->requestContactScopes();
}

} // namespace zmail
