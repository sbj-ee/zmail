#include "AuthManager.h"

#include "LoopbackServer.h"
#include "Log.h"
#include "Pkce.h"
#include "TokenStore.h"

#include <algorithm>
#include <utility>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>
#include <QSettings>

namespace zmail {

namespace {
QString keyFor(const QString &account)
{
    return QStringLiteral("refresh-token:%1").arg(account);
}

bool constantTimeEquals(const QByteArray &a, const QByteArray &b)
{
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (qsizetype i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}
} // namespace

QStringList AuthManager::scopes()
{
    return {QStringLiteral("https://www.googleapis.com/auth/gmail.modify"), QStringLiteral("openid"),
            QStringLiteral("email")};
}

QStringList AuthManager::contactScopes()
{
    return {QStringLiteral("https://www.googleapis.com/auth/contacts.readonly"),
            QStringLiteral("https://www.googleapis.com/auth/contacts.other.readonly")};
}

AuthManager::AuthManager(ClientConfig config, TokenStore *store, QNetworkAccessManager *nam, QObject *parent)
    : QObject(parent)
    , m_config(std::move(config))
    , m_store(store)
    , m_nam(nam)
    , m_open([](const QUrl &u) { return QDesktopServices::openUrl(u); })
{
}

AuthManager::~AuthManager() = default;

bool AuthManager::signInInProgress() const
{
    return !m_loopback.isNull();
}

void AuthManager::startSignIn(const QString &loginHint)
{
    cancelSignIn();
    if (!m_config.isValid()) {
        emit signInFailed(tr("No OAuth client is configured."));
        return;
    }
    m_loopback = new LoopbackServer(this);
    m_loopback->setTimeoutMs(signInTimeoutMs());
    if (!m_loopback->listen()) {
        delete m_loopback;
        emit signInFailed(tr("Couldn't open a local port for the sign-in redirect."));
        return;
    }
    m_listenPort = m_loopback->port();
    connect(m_loopback, &LoopbackServer::callbackReceived, this, &AuthManager::onCallback);
    connect(m_loopback, &LoopbackServer::callbackRejected, this, [this]() {
        qCWarning(lcAuth) << "Sign-in: ignored a redirect with a missing or wrong state; still listening on port"
                          << m_listenPort;
    });
    connect(m_loopback, &LoopbackServer::timedOut, this, [this]() {
        const int ms = m_loopback->timeoutMs();
        m_loopback->deleteLater();
        m_loopback = nullptr;
        qCWarning(lcAuth).noquote() << "Sign-in: timed out after" << describeTimeout(ms)
                                    << "; stopped listening on port" << m_listenPort;
        emit signInFailed(tr("Sign-in timed out: nothing came back from the browser within %1. "
                             "Choose Retry to open the Google sign-in page again.").arg(describeTimeout(ms)));
    });

    m_verifier = pkce::makeVerifier();
    m_state = pkce::makeState();
    m_loopback->setExpectedState(m_state); // anything else gets a 400; the listener keeps waiting
    m_redirect = m_loopback->redirectUri();

    QUrlQuery q;
    q.addQueryItem(QStringLiteral("client_id"), m_config.clientId);
    q.addQueryItem(QStringLiteral("redirect_uri"), m_redirect.toString());
    q.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
    QStringList want = scopes();
    if (m_requestingContacts) {
        want += contactScopes();
        q.addQueryItem(QStringLiteral("include_granted_scopes"), QStringLiteral("true"));
    }
    q.addQueryItem(QStringLiteral("scope"), want.join(QLatin1Char(' ')));
    q.addQueryItem(QStringLiteral("code_challenge"), QString::fromLatin1(pkce::challengeS256(m_verifier)));
    q.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("S256"));
    q.addQueryItem(QStringLiteral("state"), QString::fromLatin1(m_state));
    q.addQueryItem(QStringLiteral("access_type"), QStringLiteral("offline"));
    q.addQueryItem(QStringLiteral("prompt"), QStringLiteral("consent"));
    if (!loginHint.isEmpty()) {
        q.addQueryItem(QStringLiteral("login_hint"), loginHint);
    }
    QUrl url = m_config.authUri;
    url.setQuery(q);
    m_lastAuthUrl = url;
    qCInfo(lcAuth).noquote() << "Sign-in: waiting for the browser redirect on port" << m_listenPort << "(for up to"
                             << describeTimeout(m_loopback->timeoutMs()) + ')';
    emit signInStarted(url);
    m_requestingContacts = false;
    if (!m_open(url)) {
        cancelSignIn(); // nothing will come back; don't leave the port open
        emit signInFailed(tr("Couldn't open the web browser for Google sign-in."));
    }
}

void AuthManager::requestContactScopes()
{
    if (hasContactScopes()) {
        emit signedIn(m_account);
        return;
    }
    if (signInInProgress()) {
        return;
    }
    m_requestingContacts = true;
    startSignIn(m_account);
}

bool AuthManager::hasContactScopes() const
{
    if (m_grantedScopes.isEmpty()) {
        return false;
    }
    for (const QString &s : contactScopes()) {
        if (!m_grantedScopes.contains(s)) {
            return false;
        }
    }
    return true;
}

void AuthManager::cancelSignIn()
{
    if (m_loopback) {
        m_loopback->close();
        m_loopback->deleteLater();
        m_loopback = nullptr;
        qCInfo(lcAuth) << "Sign-in: cancelled; stopped listening on port" << m_listenPort;
    }
}

int AuthManager::signInTimeoutMs() const
{
    return m_signInTimeoutMs > 0 ? m_signInTimeoutMs : LoopbackServer::kDefaultTimeoutMs;
}

QString AuthManager::describeTimeout(int ms)
{
    // Spelled out: there's no English .qm to resolve "%n minute(s)".
    if (ms >= 60 * 1000) {
        const int n = (ms + 30 * 1000) / (60 * 1000);
        return n == 1 ? tr("1 minute") : tr("%1 minutes").arg(n);
    }
    const int n = std::max(1, (ms + 500) / 1000);
    return n == 1 ? tr("1 second") : tr("%1 seconds").arg(n);
}

void AuthManager::onCallback(const QString &code, const QString &state, const QString &error)
{
    if (m_loopback) {
        m_loopback->deleteLater();
        m_loopback = nullptr;
    }
    qCInfo(lcAuth) << "Sign-in: browser redirect received; stopped listening on port" << m_listenPort;
    if (!constantTimeEquals(state.toLatin1(), m_state)) {
        qCWarning(lcAuth) << "Sign-in: state mismatch; ignoring the redirect";
        emit signInFailed(tr("The sign-in response didn't match this request (state mismatch). Please try again."));
        return;
    }
    if (!error.isEmpty()) {
        emit signInFailed(error == QLatin1String("access_denied")
                              ? tr("Access wasn't granted in the browser.")
                              : tr("Google returned an error: %1").arg(error));
        return;
    }
    exchangeCode(code);
}

QNetworkReply *AuthManager::postForm(const QUrl &url, const QList<QPair<QString, QString>> &form)
{
    // application/x-www-form-urlencoded, encoding everything outside
    // RFC 3986 unreserved characters (so '+' and '/' in tokens survive).
    QByteArray body;
    for (const auto &kv : form) {
        if (!body.isEmpty()) {
            body += '&';
        }
        body += QUrl::toPercentEncoding(kv.first) + '=' + QUrl::toPercentEncoding(kv.second);
    }
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    req.setTransferTimeout(30000);
    return m_nam->post(req, body);
}

QString AuthManager::googleError(const QByteArray &body, QString *description)
{
    const QJsonObject o = QJsonDocument::fromJson(body).object();
    if (description) {
        *description = o.value(QStringLiteral("error_description")).toString();
    }
    return o.value(QStringLiteral("error")).toString();
}

QString AuthManager::emailFromIdToken(const QString &idToken)
{
    // Received directly from Google's token endpoint over TLS, so per OpenID
    // Connect Core §3.1.3.7 the signature check may be skipped; we only read it.
    const QStringList parts = idToken.split(QLatin1Char('.'));
    if (parts.size() < 2) {
        return {};
    }
    const QByteArray payload = QByteArray::fromBase64(parts[1].toLatin1(), QByteArray::Base64UrlEncoding);
    return QJsonDocument::fromJson(payload).object().value(QStringLiteral("email")).toString();
}

void AuthManager::exchangeCode(const QString &code)
{
    QNetworkReply *r = postForm(m_config.tokenUri, {{QStringLiteral("code"), code},
                                                    {QStringLiteral("client_id"), m_config.clientId},
                                                    {QStringLiteral("client_secret"), m_config.clientSecret},
                                                    {QStringLiteral("redirect_uri"), m_redirect.toString()},
                                                    {QStringLiteral("grant_type"), QStringLiteral("authorization_code")},
                                                    {QStringLiteral("code_verifier"), QString::fromLatin1(m_verifier)}});
    connect(r, &QNetworkReply::finished, this, [this, r]() {
        r->deleteLater();
        m_verifier.clear();
        const QByteArray body = r->readAll();
        const QJsonObject o = QJsonDocument::fromJson(body).object();
        if (r->error() != QNetworkReply::NoError || !o.contains(QStringLiteral("access_token"))) {
            QString desc;
            const QString err = googleError(body, &desc);
            qCWarning(lcAuth) << "Token exchange failed:" << (err.isEmpty() ? r->errorString() : err);
            emit signInFailed(tr("Google refused the sign-in (%1). %2")
                                  .arg(err.isEmpty() ? r->errorString() : err, desc));
            return;
        }
        const QString granted = o.value(QStringLiteral("scope")).toString();
        if (!granted.isEmpty()) {
            m_grantedScopes = granted.split(QLatin1Char(' '), Qt::SkipEmptyParts);
            QSettings().setValue(QStringLiteral("auth/grantedScopes"), m_grantedScopes);
        }
        if (!granted.isEmpty() && !granted.contains(QLatin1String("https://www.googleapis.com/auth/gmail.modify"))) {
            emit signInFailed(tr("Gmail access wasn't granted. On Google's consent screen, tick the box that "
                                 "lets zmail read, compose and send your email, then try again."));
            return;
        }
        const QString refreshToken = o.value(QStringLiteral("refresh_token")).toString();
        if (refreshToken.isEmpty()) {
            emit signInFailed(tr("Google didn't return a refresh token. Remove zmail under "
                                 "myaccount.google.com \u2192 Security \u2192 Third-party connections, then sign in again."));
            return;
        }
        const QString email = emailFromIdToken(o.value(QStringLiteral("id_token")).toString());
        if (email.isEmpty()) {
            emit signInFailed(tr("Google didn't say which account signed in."));
            return;
        }
        m_accessToken = o.value(QStringLiteral("access_token")).toString();
        m_accessExpiry = QDateTime::currentDateTimeUtc().addSecs(o.value(QStringLiteral("expires_in")).toInt(3600));
        m_refreshToken = refreshToken;
        m_account = email;
        qCInfo(lcAuth) << "Signed in as" << email << "; saving the refresh token to the keyring";
        m_store->write(keyFor(email), refreshToken, [this, email](bool ok, const QString &err) {
            if (!ok) {
                emit signInFailed(tr("Signed in, but the token couldn't be saved to the keyring: %1. "
                                     "zmail never stores tokens in plain files.")
                                      .arg(err));
                return;
            }
            emit signedIn(email);
        });
    });
}

void AuthManager::restore(const QString &account, std::function<void(bool, const QString &)> done)
{
    m_store->read(keyFor(account), [this, account, done](bool ok, const QString &value, const QString &err) {
        if (ok && !value.isEmpty()) {
            m_account = account;
            m_refreshToken = value;
            m_grantedScopes = QSettings().value(QStringLiteral("auth/grantedScopes")).toStringList();
            m_accessToken.clear();
            qCInfo(lcAuth) << "Restored saved sign-in for" << account;
        }
        done(ok && !value.isEmpty(), err);
    });
}

void AuthManager::accessToken(TokenCb cb)
{
    if (!m_accessToken.isEmpty() && QDateTime::currentDateTimeUtc().secsTo(m_accessExpiry) > 60) {
        cb(m_accessToken, {});
        return;
    }
    if (m_refreshToken.isEmpty()) {
        cb({}, tr("Not signed in."));
        return;
    }
    m_waiters.append(std::move(cb));
    if (!m_refreshing) {
        refresh();
    }
}

void AuthManager::invalidateAccessToken()
{
    m_accessToken.clear();
}

void AuthManager::refresh()
{
    m_refreshing = true;
    QNetworkReply *r = postForm(m_config.tokenUri, {{QStringLiteral("client_id"), m_config.clientId},
                                                    {QStringLiteral("client_secret"), m_config.clientSecret},
                                                    {QStringLiteral("refresh_token"), m_refreshToken},
                                                    {QStringLiteral("grant_type"), QStringLiteral("refresh_token")}});
    connect(r, &QNetworkReply::finished, this, [this, r]() {
        r->deleteLater();
        const QByteArray body = r->readAll();
        const QJsonObject o = QJsonDocument::fromJson(body).object();
        if (r->error() == QNetworkReply::NoError && o.contains(QStringLiteral("access_token"))) {
            m_accessToken = o.value(QStringLiteral("access_token")).toString();
            m_accessExpiry = QDateTime::currentDateTimeUtc().addSecs(o.value(QStringLiteral("expires_in")).toInt(3600));
            qCInfo(lcAuth) << "Access token refreshed";
            finishRefresh(true, {});
            return;
        }
        QString desc;
        const QString err = googleError(body, &desc);
        if (err == QLatin1String("invalid_grant") || err == QLatin1String("unauthorized_client")) {
            qCWarning(lcAuth) << "Refresh token rejected (" << err << "); sign-in required";
            const QString account = m_account;
            m_refreshToken.clear();
            m_accessToken.clear();
            m_store->remove(keyFor(account), [](bool, const QString &) {});
            const QString reason = tr("Google no longer accepts zmail's saved sign-in for %1 (it was revoked, "
                                      "expired, or your password changed). Please sign in again.")
                                       .arg(account);
            finishRefresh(false, reason);
            emit reauthRequired(reason);
            return;
        }
        qCWarning(lcAuth) << "Token refresh failed:" << (err.isEmpty() ? r->errorString() : err);
        finishRefresh(false, tr("Couldn't refresh the Google sign-in: %1").arg(err.isEmpty() ? r->errorString() : err));
    });
}

void AuthManager::finishRefresh(bool ok, const QString &error)
{
    m_refreshing = false;
    const QList<TokenCb> waiters = std::exchange(m_waiters, {});
    for (const TokenCb &cb : waiters) {
        cb(ok ? m_accessToken : QString(), ok ? QString() : error);
    }
}

void AuthManager::signOut()
{
    if (!m_refreshToken.isEmpty()) {
        QNetworkReply *r = postForm(m_revokeUri, {{QStringLiteral("token"), m_refreshToken}});
        connect(r, &QNetworkReply::finished, r, &QObject::deleteLater);
    }
    if (!m_account.isEmpty()) {
        m_store->remove(keyFor(m_account), [](bool, const QString &) {});
    }
    m_refreshToken.clear();
    m_accessToken.clear();
    m_account.clear();
}

} // namespace zmail
