// OAuth against the in-process mock Google: PKCE S256 + state + loopback,
// refresh, invalid_grant -> re-auth, revoke, scope checks, client file
// handling, and that no secret ever shows up in the logs.
#include "LogCapture.h"
#include "core/AuthManager.h"
#include "core/ClientConfig.h"
#include "core/LoopbackServer.h"
#include "core/Pkce.h"
#include "core/TokenStore.h"
#include "mock/MockGoogle.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QRegularExpression>
#include <QSet>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTcpSocket>
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

    void wrongOrMissingStateGets400AndKeepsListening()
    {
        // A forged or stray request to the loopback port used to end the
        // sign-in ("state mismatch"). Now it gets a 400 and is ignored; the
        // real redirect still completes the sign-in.
        MockGoogle g;
        QVERIFY(g.listen());
        MemoryTokenStore store;
        QNetworkAccessManager nam;
        AuthManager auth(g.clientConfig(), &store, &nam);
        QUrl opened;
        auth.setBrowserOpener([&](const QUrl &u) { opened = u; return true; });
        QSignalSpy failed(&auth, &AuthManager::signInFailed);
        QSignalSpy ok(&auth, &AuthManager::signedIn);
        auth.startSignIn();
        const QUrl redirect(QUrlQuery(opened).queryItemValue(QStringLiteral("redirect_uri"), QUrl::FullyDecoded));
        const QString state = QUrlQuery(opened).queryItemValue(QStringLiteral("state"), QUrl::FullyDecoded);
        QVERIFY(!state.isEmpty());

        auto get = [&](const QString &query) {
            QUrl u = redirect;
            u.setPath(QStringLiteral("/"));
            u.setQuery(query);
            QNetworkReply *r = nam.get(QNetworkRequest(u));
            QSignalSpy done(r, &QNetworkReply::finished);
            done.wait(5000);
            const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            r->deleteLater();
            return status;
        };
        QCOMPARE(get(QStringLiteral("code=stolen&state=forged")), 400);
        QCOMPARE(get(QStringLiteral("code=stolen")), 400);                       // missing state
        QCOMPARE(get(QStringLiteral("code=stolen&state=")), 400);                // empty state
        QCOMPARE(get(QStringLiteral("error=access_denied&state=forged")), 400);  // forged error
        QCOMPARE(get(QStringLiteral("code=stolen&state=") + state + QLatin1Char('x')), 400);
        QTest::qWait(100);
        QCOMPARE(failed.count(), 0);
        QVERIFY(auth.signInInProgress()); // still listening
        QCOMPARE(g.count(QStringLiteral("POST /token")), 0);

        // The real browser redirect still works.
        FakeBrowser browser;
        browser(opened);
        QTRY_COMPARE_WITH_TIMEOUT(ok.count(), 1, 10000);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(g.count(QStringLiteral("POST /token")), 1);

        // A forged state through the full browser flow: the code is never
        // exchanged.
        MockGoogle g2;
        QVERIFY(g2.listen());
        MemoryTokenStore store2;
        AuthManager auth2(g2.clientConfig(), &store2, &nam);
        FakeBrowser forged;
        forged.tamperState = true;
        auth2.setBrowserOpener([&forged](const QUrl &u) { return forged(u); });
        QSignalSpy ok2(&auth2, &AuthManager::signedIn);
        const int logStart = m_log->lines().size();
        auth2.startSignIn();
        auto rejectedLogged = [&]() {
            for (const QString &l : m_log->lines().mid(logStart)) {
                if (l.contains(QLatin1String("ignored a redirect with a missing or wrong state"))) {
                    return true;
                }
            }
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(rejectedLogged(), 5000); // the browser came back with the forged state
        QTest::qWait(200);
        QCOMPARE(ok2.count(), 0);
        QVERIFY(auth2.signInInProgress());
        QCOMPARE(g2.count(QStringLiteral("POST /token")), 0);
        QVERIFY(store2.values.isEmpty());
        auth2.cancelSignIn();
    }

    void badStateKeepsTheTimeout()
    {
        MockGoogle g;
        QVERIFY(g.listen());
        MemoryTokenStore store;
        QNetworkAccessManager nam;
        AuthManager auth(g.clientConfig(), &store, &nam);
        FakeBrowser forged;
        forged.tamperState = true;
        auth.setBrowserOpener([&forged](const QUrl &u) { return forged(u); });
        auth.setSignInTimeoutMs(800);
        QSignalSpy failed(&auth, &AuthManager::signInFailed);
        QElapsedTimer t;
        t.start();
        auth.startSignIn();
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 5000);
        QVERIFY(t.elapsed() >= 700); // ended by the timeout, not by the forged redirect
        QVERIFY2(failed.first().first().toString().contains(QLatin1String("timed out")),
                 qPrintable(failed.first().first().toString()));
        QVERIFY(!auth.signInInProgress());
    }

    void loopbackServerRejectsBadState()
    {
        LoopbackServer server;
        server.setExpectedState("s3cret");
        QVERIFY(server.listen());
        QSignalSpy got(&server, &LoopbackServer::callbackReceived);
        QSignalSpy rejected(&server, &LoopbackServer::callbackRejected);
        QNetworkAccessManager nam;
        auto get = [&](const QString &query) {
            QUrl u = server.redirectUri();
            u.setPath(QStringLiteral("/"));
            u.setQuery(query);
            QNetworkReply *r = nam.get(QNetworkRequest(u));
            QSignalSpy done(r, &QNetworkReply::finished);
            done.wait(5000);
            const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            r->deleteLater();
            return status;
        };
        QCOMPARE(get(QStringLiteral("code=x&state=nope")), 400);
        QCOMPARE(get(QStringLiteral("code=x")), 400);
        QCOMPARE(rejected.count(), 2);
        QCOMPARE(server.rejectedCallbacks(), 2);
        QCOMPARE(got.count(), 0);
        QCOMPARE(get(QStringLiteral("code=x&state=s3cret")), 200);
        QCOMPARE(got.count(), 1);
        QCOMPARE(got.first().at(0).toString(), QStringLiteral("x"));
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

    void signInTimeoutIsLongAndStopsListening()
    {
        // Default: 15 minutes (was 5; too short for 2FA on a phone or for
        // finding the browser again on Wayland).
        QCOMPARE(LoopbackServer::kDefaultTimeoutMs, 15 * 60 * 1000);
        QCOMPARE(LoopbackServer().timeoutMs(), 15 * 60 * 1000);
        MockGoogle g;
        QVERIFY(g.listen());
        MemoryTokenStore store;
        QNetworkAccessManager nam;
        AuthManager auth(g.clientConfig(), &store, &nam);
        QCOMPARE(auth.signInTimeoutMs(), LoopbackServer::kDefaultTimeoutMs);
        QCOMPARE(AuthManager::describeTimeout(auth.signInTimeoutMs()), QStringLiteral("15 minutes"));
        QCOMPARE(AuthManager::describeTimeout(60 * 1000), QStringLiteral("1 minute"));
        QCOMPARE(AuthManager::describeTimeout(300), QStringLiteral("1 second"));

        // A browser that never comes back.
        QUrl opened;
        auth.setBrowserOpener([&](const QUrl &u) { opened = u; return true; });
        auth.setSignInTimeoutMs(300);
        QSignalSpy failed(&auth, &AuthManager::signInFailed);
        const int logStart = m_log->lines().size();
        auth.startSignIn();
        QVERIFY(auth.signInInProgress());
        const QUrl redirect(QUrlQuery(opened).queryItemValue(QStringLiteral("redirect_uri"), QUrl::FullyDecoded));
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 5000);
        QVERIFY(!auth.signInInProgress());
        const QString reason = failed.first().first().toString();
        QVERIFY2(reason.contains(QLatin1String("within 1 second")), qPrintable(reason));
        QVERIFY(reason.contains(QLatin1String("Retry")));

        // The port really is closed now...
        QTcpSocket probe;
        probe.connectToHost(QHostAddress::LocalHost, quint16(redirect.port()));
        QVERIFY(!probe.waitForConnected(1000));
        // ...and the log says so, after the "waiting" line (which is no
        // longer the last word on the port).
        const QStringList log = m_log->lines().mid(logStart);
        const QString port = QString::number(redirect.port());
        int waiting = -1, stopped = -1;
        for (int i = 0; i < log.size(); ++i) {
            if (log[i].contains(QLatin1String("waiting for the browser redirect on port ") + port)) {
                waiting = i;
                QVERIFY2(log[i].contains(QLatin1String("for up to 1 second")), qPrintable(log[i]));
            }
            if (log[i].contains(QLatin1String("timed out after 1 second ; stopped listening on port ") + port)) {
                stopped = i;
            }
        }
        QVERIFY2(waiting >= 0 && stopped > waiting, qPrintable(log.join('\n')));
    }

    void cancelAndRedirectAreLogged()
    {
        MockGoogle g;
        QVERIFY(g.listen());
        MemoryTokenStore store;
        QNetworkAccessManager nam;
        AuthManager auth(g.clientConfig(), &store, &nam);
        auth.setBrowserOpener([](const QUrl &) { return true; });
        int from = m_log->lines().size();
        auth.startSignIn();
        auth.cancelSignIn();
        QVERIFY(m_log->lines().mid(from).join('\n').contains(QLatin1String("Sign-in: cancelled; stopped listening on port")));

        // Browser that can't be opened: the listener doesn't stay open.
        auth.setBrowserOpener([](const QUrl &) { return false; });
        QSignalSpy failed(&auth, &AuthManager::signInFailed);
        auth.startSignIn();
        QCOMPARE(failed.count(), 1);
        QVERIFY(!auth.signInInProgress());

        from = m_log->lines().size();
        FakeBrowser browser;
        signIn(g, auth, browser);
        QVERIFY(m_log->lines().mid(from).join('\n').contains(QLatin1String("browser redirect received; stopped listening on port")));
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
