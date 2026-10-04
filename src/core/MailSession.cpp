#include "MailSession.h"

#include "GmailClient.h"
#include "Log.h"
#include "MailCache.h"
#include "SyncEngine.h"
#include "TokenStore.h"

#include <QNetworkAccessManager>
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
        qCWarning(lcSync) << "Couldn't open the mail cache:" << m_cache->lastError();
    }
    m_api = std::make_unique<GmailClient>(m_auth.get(), m_nam,
                                          m_opts.apiBase.isValid() ? m_opts.apiBase : GmailClient::defaultBaseUrl());
    m_api->setBackoffBaseMs(m_opts.backoffBaseMs);
    m_sync = std::make_unique<SyncEngine>(m_api.get(), m_cache.get());
    m_sync->setPollInterval(m_opts.pollIntervalMs);
    m_sync->setInitialCount(m_opts.initialCount);
    setState(State::SignedIn);
    emit ready();
    m_sync->start();
}

void MailSession::stopAccount()
{
    if (m_sync) {
        m_sync->stop();
    }
    m_sync.reset();
    m_api.reset();
    m_cache.reset();
}

} // namespace zmail
