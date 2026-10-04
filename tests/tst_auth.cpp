// OAuth against the in-process mock Google: PKCE S256 + state + loopback,
// refresh, invalid_grant -> re-auth, revoke, scope checks, client file
// handling, and that no secret ever shows up in the logs.
#include "LogCapture.h"
#include "core/AuthManager.h"
#include "core/ClientConfig.h"
#include "core/Pkce.h"
#include "core/TokenStore.h"
#include "mock/MockGoogle.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUrlQuery>

#include <sys/stat.h>

using namespace zmail;
using zmail::test::MockGoogle;

namespace {
// Plays the part of the system browser: follows Google's 302 back to the
// loopback listener, exactly as a real browser would after consent.
struct FakeBrowser
{
    QNetworkAccessManager nam;
    QUrl opened;
    bool tamperState = false;
    bool operator()(const QUrl &url)
    {
        opened = url;
        QUrl u = url;
        if (tamperState) {
            QUrlQuery q(u);
            q.removeQueryItem(QStringLiteral("state"));
            q.addQueryItem(QStringLiteral("state"), QStringLiteral("forged"));
            u.setQuery(q);
        }
        QNetworkReply *r = nam.get(QNetworkRequest(u));
        QObject::connect(r, &QNetworkReply::finished, r, &QObject::deleteLater);
        return true;
    }
};
} // namespace

class TstAuth : public QObject
{
    Q_OBJECT

    LogCapture *m_log = nullptr;

    void signIn(MockGoogle &g, AuthManager &auth, FakeBrowser &browser)
    {
        auth.setBrowserOpener([&browser](const QUrl &u) { return browser(u); });
        QSignalSpy ok(&auth, &AuthManager::signedIn);
        QSignalSpy failed(&auth, &AuthManager::signInFailed);
        auth.startSignIn();
        QTRY_VERIFY_WITH_TIMEOUT(ok.count() + failed.count() > 0, 10000);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(ok.first().first().toString(), g.email);
    }

private slots:
    void initTestCase() { m_log = new LogCapture; }

    void pkceChallengeIsS256()
    {
        // RFC 7636 appendix B test vector.
        QCOMPARE(pkce::challengeS256("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"),
                 QByteArray("E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"));
        const QByteArray v = pkce::makeVerifier();
        QVERIFY(v.size() >= 43 && v.size() <= 128);
        QVERIFY(QRegularExpression(QStringLiteral("^[A-Za-z0-9._~-]+$")).match(QString::fromLatin1(v)).hasMatch());
        QVERIFY(pkce::makeVerifier() != v);
        QVERIFY(pkce::makeState() != pkce::makeState());
    }

    void fullPkceLoopbackFlow()
    {
        MockGoogle g;
        QVERIFY(g.listen());
        MemoryTokenStore store;
        QNetworkAccessManager nam;
        AuthManager auth(g.clientConfig(), &store, &nam);
        FakeBrowser browser;
        signIn(g, auth, browser);

        const QUrlQuery q(browser.opened);
        QCOMPARE(q.queryItemValue(QStringLiteral("code_challenge_method")), QStringLiteral("S256"));
        QCOMPARE(q.queryItemValue(QStringLiteral("scope"), QUrl::FullyDecoded),
                 QStringLiteral("https://www.googleapis.com/auth/gmail.modify openid email"));
        QCOMPARE(q.queryItemValue(QStringLiteral("access_type")), QStringLiteral("offline"));
        const QUrl redirect(q.queryItemValue(QStringLiteral("redirect_uri"), QUrl::FullyDecoded));
        QCOMPARE(redirect.host(), QStringLiteral("127.0.0.1"));
        QVERIFY(redirect.port() > 1024);
        QVERIFY(!q.queryItemValue(QStringLiteral("state")).isEmpty());
        QVERIFY(g.pkceVerified); // mock checked SHA256(code_verifier) == code_challenge
        QCOMPARE(store.values.value(QStringLiteral("refresh-token:") + g.email), QString::fromLatin1(MockGoogle::kRefreshToken));

        QString token;
        auth.accessToken([&](const QString &t, const QString &) { token = t; });
        QVERIFY(token.startsWith(QLatin1String(MockGoogle::kAccessPrefix)));
        QCOMPARE(g.count(QStringLiteral("POST /token")), 1); // cached, no refresh yet
    }

    void randomPortEachTime()
    {
        MockGoogle g;
        QVERIFY(g.listen());
        MemoryTokenStore store;
        QNetworkAccessManager nam;
        AuthManager auth(g.clientConfig(), &store, &nam);
        QList<QUrl> urls;
        auth.setBrowserOpener([&](const QUrl &u) { urls << u; return true; });
        auth.startSignIn();
        auth.startSignIn();
        QCOMPARE(urls.size(), 2);
        const auto port = [](const QUrl &u) {
            return QUrl(QUrlQuery(u).queryItemValue(QStringLiteral("redirect_uri"), QUrl::FullyDecoded)).port();
        };
        QVERIFY(port(urls[0]) != port(urls[1]) || QUrlQuery(urls[0]).queryItemValue(QStringLiteral("state")) !=
                                                     QUrlQuery(urls[1]).queryItemValue(QStringLiteral("state")));
        auth.cancelSignIn();
    }

    void stateMismatchIsRejected()
    {
        MockGoogle g;
        QVERIFY(g.listen());
        MemoryTokenStore store;
        QNetworkAccessManager nam;
        AuthManager auth(g.clientConfig(), &store, &nam);
        FakeBrowser browser;
        browser.tamperState = true;
        auth.setBrowserOpener([&browser](const QUrl &u) { return browser(u); });
        QSignalSpy failed(&auth, &AuthManager::signInFailed);
        QSignalSpy ok(&auth, &AuthManager::signedIn);
        auth.startSignIn();
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
        QVERIFY(failed.first().first().toString().contains(QStringLiteral("state")));
        QCOMPARE(ok.count(), 0);
        QCOMPARE(g.count(QStringLiteral("POST /token")), 0); // code never exchanged
        QVERIFY(store.values.isEmpty());
    }

    void missingGmailScopeFails()
    {
        MockGoogle g;
        g.grantGmailScope = false; // user unticked the Gmail checkbox
        QVERIFY(g.listen());
        MemoryTokenStore store;
        QNetworkAccessManager nam;
        AuthManager auth(g.clientConfig(), &store, &nam);
        FakeBrowser browser;
        auth.setBrowserOpener([&browser](const QUrl &u) { return browser(u); });
        QSignalSpy failed(&auth, &AuthManager::signInFailed);
        auth.startSignIn();
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
        QVERIFY(failed.first().first().toString().contains(QStringLiteral("Gmail access")));
        QVERIFY(store.values.isEmpty());
    }

    void refreshWhenExpiredAndShareOneRequest()
    {
        MockGoogle g;
        g.accessTokenLifetime = 30; // < 60 s margin: every use refreshes
        QVERIFY(g.listen());
        MemoryTokenStore store;
        QNetworkAccessManager nam;
        AuthManager auth(g.clientConfig(), &store, &nam);
        FakeBrowser browser;
        signIn(g, auth, browser);
        QCOMPARE(g.tokensIssued, 1);
        QStringList got;
        for (int i = 0; i < 3; ++i) {
            auth.accessToken([&](const QString &t, const QString &) { got << t; });
        }
        QTRY_COMPARE(got.size(), 3);
        QCOMPARE(g.tokensIssued, 2); // three waiters, one refresh
        QCOMPARE(QSet<QString>(got.begin(), got.end()).size(), 1);
        QVERIFY(got.first().endsWith(QLatin1String("-2")));
    }

    void restoreFromKeyringThenRefresh()
    {
        MockGoogle g;
        QVERIFY(g.listen());
        MemoryTokenStore store;
        store.values.insert(QStringLiteral("refresh-token:") + g.email, QString::fromLatin1(MockGoogle::kRefreshToken));
        QNetworkAccessManager nam;
        AuthManager auth(g.clientConfig(), &store, &nam);
        bool restored = false;
        auth.restore(g.email, [&](bool ok, const QString &) { restored = ok; });
        QTRY_VERIFY(restored); // keyring reads are async
        QString token;
        auth.accessToken([&](const QString &t, const QString &) { token = t; });
        QTRY_VERIFY(!token.isEmpty());
        QCOMPARE(g.count(QStringLiteral("POST /token")), 1);
    }

    void invalidGrantAsksForSignInAgain()
    {
        MockGoogle g;
        QVERIFY(g.listen());
        MemoryTokenStore store;
        store.values.insert(QStringLiteral("refresh-token:") + g.email, QString::fromLatin1(MockGoogle::kRefreshToken));
        QNetworkAccessManager nam;
        AuthManager auth(g.clientConfig(), &store, &nam);
        bool restored = false;
        auth.restore(g.email, [&](bool ok, const QString &) { restored = ok; });
        QTRY_VERIFY(restored);
        g.revokeRefreshToken(); // user removed zmail at myaccount.google.com
        QSignalSpy reauth(&auth, &AuthManager::reauthRequired);
        QString token = QStringLiteral("unset"), error;
        auth.accessToken([&](const QString &t, const QString &e) { token = t; error = e; });
        QTRY_COMPARE(reauth.count(), 1);
        QVERIFY(token.isEmpty());
        QVERIFY(error.contains(QStringLiteral("sign in again")));
        QVERIFY(!auth.hasRefreshToken());
        QVERIFY(store.values.isEmpty()); // dead token removed from the keyring
    }

    void signOutRevokes()
    {
        MockGoogle g;
        QVERIFY(g.listen());
        MemoryTokenStore store;
        QNetworkAccessManager nam;
        AuthManager auth(g.clientConfig(), &store, &nam);
        auth.setRevokeUri(g.revokeUri());
        FakeBrowser browser;
        signIn(g, auth, browser);
        auth.signOut();
        QTRY_VERIFY(g.refreshRevoked());
        QVERIFY(store.values.isEmpty());
        QVERIFY(auth.account().isEmpty());
    }

    void clientFileParsing()
    {
        const QByteArray desktop = "{\"installed\":{\"client_id\":\"test-client-42.apps.example.test\",\"project_id\":\"zmail\",\"auth_uri\":\"https://accounts.google.com/o/oauth2/auth\",\"token_uri\":\"https://oauth2.googleapis.com/token\",\"auth_provider_x509_cert_url\":\"https://www.googleapis.com/oauth2/v1/certs\",\"client_secret\":\"test-secret-xyz\",\"redirect_uris\":[\"http://localhost\"]}}";
        auto r = ClientConfig::parse(desktop);
        QCOMPARE(r.status, ClientConfig::LoadStatus::Ok);
        QCOMPARE(r.config.clientId, QStringLiteral("test-client-42.apps.example.test"));
        QCOMPARE(r.config.authUri, QUrl(QStringLiteral("https://accounts.google.com/o/oauth2/v2/auth")));
        QVERIFY(!r.message.contains(QStringLiteral("test-secret")));

        QCOMPARE(ClientConfig::parse("{\"web\":{\"client_id\":\"x\",\"client_secret\":\"y\"}}").status,
                 ClientConfig::LoadStatus::NotDesktopClient);
        QCOMPARE(ClientConfig::parse("not json").status, ClientConfig::LoadStatus::BadJson);
        QCOMPARE(ClientConfig::parse("{\"installed\":{\"client_id\":\"x\",\"client_secret\":\"y\",\"token_uri\":\"https://evil.example/token\"}}").status, ClientConfig::LoadStatus::BadEndpoint);
    }

    void installCopiesWith0600AndWarnsWhenLoose()
    {
        QTemporaryDir dir;
        const QString src = dir.filePath(QStringLiteral("Downloads/client_secret_test-client-42.json"));
        QDir().mkpath(QFileInfo(src).absolutePath());
        QFile f(src);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"installed\":{\"client_id\":\"test-client-42.apps.example.test\",\"client_secret\":\"test-secret-xyz\",\"auth_uri\":\"https://accounts.google.com/o/oauth2/auth\",\"token_uri\":\"https://oauth2.googleapis.com/token\"}}");
        f.close();
        ::chmod(QFile::encodeName(src).constData(), 0644); // browsers save downloads world-readable
        const QString dest = dir.filePath(QStringLiteral("config/zmail/oauth-client.json"));
        QCOMPARE(ClientConfig::install(src, dest), QString());
        struct stat st{};
        QCOMPARE(::stat(QFile::encodeName(dest).constData(), &st), 0);
        QCOMPARE(int(st.st_mode & 0777), 0600);
        QCOMPARE(::stat(QFile::encodeName(QFileInfo(dest).absolutePath()).constData(), &st), 0);
        QCOMPARE(int(st.st_mode & 0777), 0700);
        auto r = ClientConfig::load(dest);
        QCOMPARE(r.status, ClientConfig::LoadStatus::Ok);
        QVERIFY(!r.permissionsTooOpen);

        ::chmod(QFile::encodeName(dest).constData(), 0644);
        r = ClientConfig::load(dest);
        QCOMPARE(r.status, ClientConfig::LoadStatus::Ok); // still usable, but warned
        QVERIFY(r.permissionsTooOpen);
        QVERIFY(ClientConfig::tightenPermissions(dest));
        QVERIFY(!ClientConfig::load(dest).permissionsTooOpen);

        QCOMPARE(ClientConfig::load(dir.filePath(QStringLiteral("nope.json"))).status, ClientConfig::LoadStatus::Missing);
        QFile bad(dir.filePath(QStringLiteral("web.json")));
        QVERIFY(bad.open(QIODevice::WriteOnly));
        bad.write("{\"web\":{\"client_id\":\"x\",\"client_secret\":\"y\"}}");
        bad.close();
        QVERIFY(!ClientConfig::install(bad.fileName(), dest).isEmpty()); // rejected, old file kept
        QCOMPARE(ClientConfig::load(dest).config.clientId, QStringLiteral("test-client-42.apps.example.test"));
    }

    void noSecretsInLogs()
    {
        // Runs last: everything above logged through LogCapture.
        const QString all = m_log->all();
        QVERIFY2(!all.isEmpty(), "log capture saw nothing; categories not enabled?");
        for (const char *secret : {MockGoogle::kClientSecret, MockGoogle::kRefreshToken, MockGoogle::kAccessPrefix,
                                   "test-secret-xyz", "4/mock-auth-code", "code_verifier"}) {
            QVERIFY2(!all.contains(QLatin1String(secret)), secret);
        }
        QVERIFY(all.contains(QStringLiteral("zmail.auth"))); // it did log, just not secrets
    }

    void cleanupTestCase() { delete m_log; }
};

QTEST_GUILESS_MAIN(TstAuth)
#include "tst_auth.moc"
