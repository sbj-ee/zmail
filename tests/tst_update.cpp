// Auto-update: semver comparison, release JSON fixtures,
// SHA256SUMS verification, the once-a-day / skip rules, and the whole flow
// (check -> dialog -> download -> verify -> install -> restart) against a mock
// GitHub served from a local QTcpServer. Nothing here touches the network or apt.
// Ported from sbj-ee/zterminal's tests/tst_update.cpp.
#include "MainWindow.hpp"
#include "MinisignTestSigner.h"
#include "mock/FakeGitHub.h"
#include "core/Update.h"
#include "core/UpdateChecker.h"
#include "ui/ComposeWindow.h"
#include "ui/UpdateDialog.h"
#include "ui/UpdateManager.h"
#include "version.hpp"

#include <QAction>
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QMessageBox>
#include <QNetworkProxy>
#include <QSettings>
#include <QProgressDialog>
#include <QScopeGuard>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QtGlobal>
#include <QTextBrowser>

using namespace zmail;

namespace {

QByteArray fixture(const QString &name, const QString &base = QStringLiteral("http://127.0.0.1:1"))
{
    QFile f(QStringLiteral(ZMAIL_TEST_DATA "/update/") + name);
    if (!f.open(QIODevice::ReadOnly)) {
        qFatal("missing fixture %s", qPrintable(name));
    }
    return f.readAll().replace("@BASE@", base.toUtf8());
}

QByteArray sha256Hex(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}

template <typename T>
T *findTop(const char *name)
{
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (w->isVisible() && w->objectName() == QLatin1String(name)) {
            if (auto *t = qobject_cast<T *>(w)) {
                return t;
            }
        }
    }
    return nullptr;
}

void closeAllTop()
{
    for (const char *n : {"updateDialog", "updateMessage", "updateRestart", "updateProgress"}) {
        while (QWidget *w = findTop<QWidget>(n)) {
            w->close();
            QCoreApplication::processEvents();
        }
    }
}

} // namespace

class TstUpdate : public QObject
{
    Q_OBJECT
    const QByteArray debBytes = QByteArray("!<arch>\nfake zmail package\n").repeated(128);
    MinisignTestKey signKey;  // the "release key" for these tests
    MinisignTestKey otherKey; // someone else's

    // Serve the 0.10.0 release with a correct (or wrong) SHA256SUMS, signed
    // by the release key (or by another key).
    void serveRelease(MockGitHub &gh, bool goodSum = true, bool goodSignature = true)
    {
        gh.routes[QStringLiteral("/latest")].body = fixture(QStringLiteral("release-0.10.0.json"), gh.base());
        gh.routes[QStringLiteral("/download/v0.10.0/zmail_0.10.0_amd64.deb")].body = debBytes;
        const QByteArray sum = goodSum ? sha256Hex(debBytes) : sha256Hex("something else");
        gh.routes[QStringLiteral("/download/v0.10.0/SHA256SUMS")].body =
            sum + "  zmail_0.10.0_amd64.deb\n" + QByteArray(64, 'a') + "  other.tar.gz\n";
        const QByteArray &sums = gh.routes[QStringLiteral("/download/v0.10.0/SHA256SUMS")].body;
        gh.routes[QStringLiteral("/download/v0.10.0/SHA256SUMS.minisig")].body =
            goodSignature ? signKey.sign(sums) : otherKey.sign(sums);
    }

    static QString platformPackageName() { return QStringLiteral("zmail_0.10.0_amd64.deb"); }

    UpdateManager &freshManager(MockGitHub &gh)
    {
        closeAllTop();
        UpdateManager &m = UpdateManager::instance();
        m.resetForTests();
        m.setCurrentVersionForTests(QStringLiteral("0.9.0"));
        m.checker()->setApiUrl(QUrl(gh.url(QStringLiteral("/latest"))));
        m.setInstallerForTests({}, {});
        m.setRestartHandlerForTests({});
        QFile::remove(UpdateState::filePath());
        return m;
    }

private slots:
    void initTestCase()
    {
        QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
        QSettings().remove(QStringLiteral("updates/checkOnStartup"));
        setUpdateSigningPublicKeyForTests(signKey.publicKeyBase64());
        setAssetHostCheckForTests(false); // the mock server is http://127.0.0.1; assetUrlsMustBeGitHub turns it back on
    }

    void assetUrlsMustBeGitHub()
    {
        setAssetHostCheckForTests(true);
        const auto restore = qScopeGuard([] { setAssetHostCheckForTests(false); });
        const QString base = QStringLiteral("https://github.com/sbj-ee/zmail/releases/download/v1.0.1/");
        QVERIFY(isTrustedAssetUrl(base + QStringLiteral("SHA256SUMS")));
        for (const char *bad : {"http://github.com/sbj-ee/zmail/releases/download/v1.0.1/a",
                                "https://evil.example/sbj-ee/zmail/releases/download/v1.0.1/a",
                                "https://github.com.evil.example/sbj-ee/zmail/releases/download/v1.0.1/a",
                                "https://user@github.com/sbj-ee/zmail/releases/download/v1.0.1/a",
                                "https://github.com:8443/sbj-ee/zmail/releases/download/v1.0.1/a",
                                "https://github.com/other/zmail/releases/download/v1.0.1/a",
                                "https://github.com/sbj-ee/zmail/releases/download/../../../x",
                                "https://github.com/sbj-ee/zmail/releases/download/v1/a b", ""}) {
            QVERIFY2(!isTrustedAssetUrl(QString::fromLatin1(bad)), bad);
        }
        QVERIFY(isTrustedAssetRedirect(QUrl(QStringLiteral("https://objects.githubusercontent.com/x"))));
        QVERIFY(isTrustedAssetRedirect(QUrl(QStringLiteral("https://github.com/x"))));
        QVERIFY(!isTrustedAssetRedirect(QUrl(QStringLiteral("http://objects.githubusercontent.com/x"))));
        QVERIFY(!isTrustedAssetRedirect(QUrl(QStringLiteral("https://githubusercontent.com.evil.example/x"))));
        QVERIFY(!isTrustedAssetRedirect(QUrl(QStringLiteral("https://evilgithubusercontent.com/x"))));

        // A release whose assets are somewhere else is refused before anything is fetched.
        MockGitHub gh;
        serveRelease(gh);
        const auto rel = ReleaseInfo::fromJson(fixture(QStringLiteral("release-0.10.0.json"), gh.base()));
        QVERIFY(rel);
        UpdateDownloader d;
        QSignalSpy done(&d, &UpdateDownloader::finished);
        QTemporaryDir dir;
        d.start(*rel, dir.path());
        QVERIFY(done.wait(5000));
        QVERIFY(!done.takeFirst().at(0).toBool());
        QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("SHA256SUMS"))));
    }

    void oversizedAssetIsCutOff()
    {
        MockGitHub gh;
        serveRelease(gh);
        gh.routes[QStringLiteral("/download/v0.10.0/SHA256SUMS")].body = QByteArray(200 * 1024, 'x');
        const auto rel = ReleaseInfo::fromJson(fixture(QStringLiteral("release-0.10.0.json"), gh.base()));
        QVERIFY(rel);
        UpdateDownloader d;
        QSignalSpy done(&d, &UpdateDownloader::finished);
        QTemporaryDir dir;
        d.start(*rel, dir.path());
        QVERIFY(done.wait(5000));
        const auto args = done.takeFirst();
        QVERIFY(!args.at(0).toBool());
        QVERIFY2(args.at(2).toString().contains(QLatin1String("larger")), qPrintable(args.at(2).toString()));
    }

    void semverCompare_data()
    {
        QTest::addColumn<QString>("a");
        QTest::addColumn<QString>("b");
        QTest::addColumn<int>("cmp");
        QTest::newRow("equal") << "1.2.3" << "1.2.3" << 0;
        QTest::newRow("leading v") << "v0.10.0" << "0.10.0" << 0;
        QTest::newRow("numeric minor, not string") << "0.10.0" << "0.9.0" << 1;
        QTest::newRow("patch") << "0.9.1" << "0.9.0" << 1;
        QTest::newRow("major") << "1.0.0" << "0.99.99" << 1;
        QTest::newRow("short form") << "1.2" << "1.2.0" << 0;
        QTest::newRow("prerelease below release") << "1.0.0-rc1" << "1.0.0" << -1;
        QTest::newRow("prerelease above previous") << "1.0.0-rc.1" << "0.10.0" << 1;
        QTest::newRow("rc.2 > rc.1") << "1.0.0-rc.2" << "1.0.0-rc.1" << 1;
        QTest::newRow("rc.10 > rc.9 numerically") << "1.0.0-rc.10" << "1.0.0-rc.9" << 1;
        QTest::newRow("alpha < beta") << "1.0.0-alpha" << "1.0.0-beta" << -1;
        QTest::newRow("numeric before alnum") << "1.0.0-1" << "1.0.0-alpha" << -1;
        QTest::newRow("longer prerelease wins") << "1.0.0-alpha.1" << "1.0.0-alpha" << 1;
        QTest::newRow("build metadata ignored") << "1.0.0+build.5" << "1.0.0" << 0;
        QTest::newRow("debian tilde") << "1.0.0~rc1" << "1.0.0-rc1" << 0;
    }
    void semverCompare()
    {
        QFETCH(QString, a);
        QFETCH(QString, b);
        QFETCH(int, cmp);
        const SemVer va = SemVer::parse(a);
        const SemVer vb = SemVer::parse(b);
        QVERIFY(va.valid);
        QVERIFY(vb.valid);
        QCOMPARE(SemVer::compare(va, vb), cmp);
        QCOMPARE(SemVer::compare(vb, va), -cmp);
        QCOMPARE(isNewerVersion(a, b), cmp > 0);
    }

    void semverInvalid()
    {
        for (const char *bad : {"", "v", "latest", "1.2.3.4", "1.x.0", "1.0.0-", "1..0", "-1.0.0", "1.0.0-rc..1"}) {
            QVERIFY2(!SemVer::parse(QString::fromLatin1(bad)).valid, bad);
        }
        QVERIFY(!isNewerVersion(QStringLiteral("nightly"), QStringLiteral("0.9.0")));
        QCOMPARE(SemVer::parse(QStringLiteral("v1.0.0-rc.1+abc")).toString(), QStringLiteral("1.0.0-rc.1"));
        // The running build parses (the update check compares against it).
        QVERIFY(SemVer::parse(QString::fromLatin1(kVersionString)).valid);
    }

    void releaseJson()
    {
        const auto r = ReleaseInfo::fromJson(fixture(QStringLiteral("release-0.10.0.json")));
        QVERIFY(r);
        QCOMPARE(r->tag, QStringLiteral("v0.10.0"));
        QCOMPARE(r->versionString(), QStringLiteral("0.10.0"));
        QCOMPARE(r->name, QStringLiteral("zmail 0.10.0"));
        QVERIFY(r->notes.contains(QStringLiteral("Auto-update")));
        QCOMPARE(r->htmlUrl, QStringLiteral("https://github.com/sbj-ee/zmail/releases/tag/v0.10.0"));
        QVERIFY(r->publishedAt.isValid());
        QCOMPARE(r->assets.size(), 3);
        QVERIFY(r->signatureAsset());
        QCOMPARE(r->signatureAsset()->name, QStringLiteral("SHA256SUMS.minisig"));
        QVERIFY(r->debAsset());
        QCOMPARE(r->debAsset()->name, QStringLiteral("zmail_0.10.0_amd64.deb"));
        QCOMPARE(r->debAsset()->size, qint64(4096));
        QVERIFY(r->checksumAsset());
        QVERIFY(r->packageAsset());
        QCOMPARE(r->packageAsset()->name, platformPackageName());

        // Missing .deb for amd64 (an arm64 one doesn't count).
        const auto noDeb = ReleaseInfo::fromJson(fixture(QStringLiteral("release-no-deb.json")));
        QVERIFY(noDeb);
        QVERIFY(!noDeb->debAsset());
        QVERIFY(noDeb->checksumAsset());

        // Prerelease with the Debian '~' spelling in the package name.
        const auto rc = ReleaseInfo::fromJson(fixture(QStringLiteral("release-rc.json")));
        QVERIFY(rc);
        QVERIFY(rc->prerelease);
        QVERIFY(rc->debAsset());
        QCOMPARE(rc->debAsset()->name, QStringLiteral("zmail_1.0.0~rc.1_amd64.deb"));

        QString err;
        QVERIFY(!ReleaseInfo::fromJson("<html>rate limited</html>", &err));
        QVERIFY(err.contains(QStringLiteral("could not read")));
        QVERIFY(!ReleaseInfo::fromJson(R"({"message":"Not Found"})", &err));
        QVERIFY(err.contains(QStringLiteral("No published")));
        QVERIFY(!ReleaseInfo::fromJson(R"({"tag_name":"nightly"})", &err));
        QVERIFY(err.contains(QStringLiteral("nightly")));
    }

    // Found by fuzzing: an out-of-range asset size was cast from double (UB).
    void hostileAssetSizes()
    {
        const QByteArray json =
            "{\"tag_name\":\"v9.9.9\",\"assets\":["
            "{\"name\":\"a\",\"browser_download_url\":\"https://x/a\",\"size\":1e300},"
            "{\"name\":\"b\",\"browser_download_url\":\"https://x/b\",\"size\":-5},"
            "{\"name\":\"c\",\"browser_download_url\":\"https://x/c\",\"size\":4096}]}";
        const auto r = ReleaseInfo::fromJson(json);
        QVERIFY(r);
        QCOMPARE(r->assets.size(), 3);
        QCOMPARE(r->assets.at(0).size, qint64(0));
        QCOMPARE(r->assets.at(1).size, qint64(0));
        QCOMPARE(r->assets.at(2).size, qint64(4096));
    }

    void checksum()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("zmail_0.10.0_amd64.deb"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(debBytes);
        f.close();
        const QByteArray good = sha256Hex(debBytes);
        QCOMPARE(sha256OfFile(path).toLatin1(), good);

        // sha256sum's text and binary ("*") forms, upper-case hex, "./" prefix.
        for (const QByteArray &sums : {good + "  zmail_0.10.0_amd64.deb\n", good + " *zmail_0.10.0_amd64.deb\n",
                                        good.toUpper() + "  ./zmail_0.10.0_amd64.deb\r\n",
                                        QByteArray(64, 'b') + "  other\n" + good + "  zmail_0.10.0_amd64.deb"}) {
            const ChecksumResult r = verifyChecksum(path, sums, QStringLiteral("zmail_0.10.0_amd64.deb"));
            QVERIFY2(r.ok(), sums.constData());
        }
        const ChecksumResult bad = verifyChecksum(path, sha256Hex("tampered") + "  zmail_0.10.0_amd64.deb\n",
                                                  QStringLiteral("zmail_0.10.0_amd64.deb"));
        QCOMPARE(bad.status, ChecksumResult::Status::Mismatch);
        QVERIFY(bad.message().contains(QStringLiteral("does not match")));
        QVERIFY(bad.message().contains(QString::fromLatin1(good)));

        const ChecksumResult notListed =
            verifyChecksum(path, good + "  zmail_0.9.0_amd64.deb\n", QStringLiteral("zmail_0.10.0_amd64.deb"));
        QCOMPARE(notListed.status, ChecksumResult::Status::NotListed);
        QCOMPARE(verifyChecksum(path, "", QStringLiteral("zmail_0.10.0_amd64.deb")).status,
                 ChecksumResult::Status::BadFile);
        // A short/garbled hash isn't a match for anything.
        QCOMPARE(verifyChecksum(path, good.left(63) + "  zmail_0.10.0_amd64.deb\n",
                                QStringLiteral("zmail_0.10.0_amd64.deb"))
                     .status,
                 ChecksumResult::Status::NotListed);
        QCOMPARE(verifyChecksum(dir.filePath(QStringLiteral("gone.deb")), good + "  gone.deb\n", QStringLiteral("gone.deb"))
                     .status,
                 ChecksumResult::Status::Unreadable);
    }

    void dailyLimit()
    {
        const QDateTime now = QDateTime::fromString(QStringLiteral("2026-10-04T12:00:00Z"), Qt::ISODate);
        UpdateState st;
        QVERIFY(UpdatePolicy::shouldCheckAtStartup(true, st, now)); // never checked
        QVERIFY(!UpdatePolicy::shouldCheckAtStartup(false, st, now)); // turned off in Settings
        st.lastCheck = now.addSecs(-3600);
        QVERIFY(!UpdatePolicy::shouldCheckAtStartup(true, st, now)); // an hour ago
        st.lastCheck = now.addSecs(-(24 * 3600 - 1));
        QVERIFY(!UpdatePolicy::shouldCheckAtStartup(true, st, now));
        st.lastCheck = now.addSecs(-24 * 3600);
        QVERIFY(UpdatePolicy::shouldCheckAtStartup(true, st, now)); // a day
        st.lastCheck = now.addDays(3);
        QVERIFY(UpdatePolicy::shouldCheckAtStartup(true, st, now)); // clock went backwards
    }

    void skipLogic()
    {
        auto rel = [](const char *tag) {
            ReleaseInfo r;
            r.tag = QString::fromLatin1(tag);
            r.version = SemVer::parse(r.tag);
            return r;
        };
        UpdateState none;
        const QString cur = QStringLiteral("0.9.0");
        QVERIFY(UpdatePolicy::shouldOffer(rel("v0.10.0"), cur, none, false));
        QVERIFY(!UpdatePolicy::shouldOffer(rel("v0.9.0"), cur, none, true));  // same
        QVERIFY(!UpdatePolicy::shouldOffer(rel("v0.8.0"), cur, none, true));  // older
        QVERIFY(!UpdatePolicy::shouldOffer(rel("v1.0.0-rc1"), QStringLiteral("1.0.0"), none, true));

        UpdateState skipped;
        skipped.skippedVersion = QStringLiteral("v0.10.0");
        QVERIFY(!UpdatePolicy::shouldOffer(rel("v0.10.0"), cur, skipped, false)); // quiet at startup
        QVERIFY(UpdatePolicy::shouldOffer(rel("v0.10.0"), cur, skipped, true));   // still shown when asked
        QVERIFY(UpdatePolicy::shouldOffer(rel("v0.10.1"), cur, skipped, false));  // newer than the skipped one

        ReleaseInfo draft = rel("v0.10.0");
        draft.draft = true;
        QVERIFY(!UpdatePolicy::shouldOffer(draft, cur, none, true));

        // Persisted next to (not in) the QSettings file.
        QVERIFY(UpdateState::filePath().endsWith(QStringLiteral("/zmail/update-state.ini")));
        UpdateState st;
        st.lastCheck = QDateTime::fromString(QStringLiteral("2026-10-04T12:00:00Z"), Qt::ISODate);
        st.skippedVersion = QStringLiteral("v0.10.0");
        st.save();
        const UpdateState back = UpdateState::load();
        QCOMPARE(back.lastCheck, st.lastCheck);
        QCOMPARE(back.skippedVersion, st.skippedVersion);
        QFile::remove(UpdateState::filePath());
    }

    void checkerAgainstMock()
    {
        MockGitHub gh;
        serveRelease(gh);
        UpdateChecker c;
        c.setApiUrl(QUrl(gh.url(QStringLiteral("/latest"))));
        QSignalSpy done(&c, &UpdateChecker::finished);
        c.check();
        QVERIFY(done.wait(5000));
        auto r = done.takeFirst().at(0).value<UpdateChecker::Result>();
        QCOMPARE(r.outcome, UpdateChecker::Outcome::Release);
        QCOMPARE(r.release.tag, QStringLiteral("v0.10.0"));
        QVERIFY(gh.lastUserAgent.startsWith("zmail/"));
        QCOMPARE(gh.lastAccept, QByteArray("application/vnd.github+json"));
        QVERIFY(UpdateChecker::defaultApiUrl() == QStringLiteral("https://api.github.com/repos/sbj-ee/zmail/releases/latest"));
    }

    void checkerFailures()
    {
        MockGitHub gh;
        UpdateChecker c;
        c.setTimeoutMs(400);
        QSignalSpy done(&c, &UpdateChecker::finished);
        auto run = [&](const QString &path) {
            c.setApiUrl(QUrl(gh.url(path)));
            c.check();
            if (!done.wait(5000)) {
                qFatal("no result for %s", qPrintable(path));
            }
            return done.takeFirst().at(0).value<UpdateChecker::Result>();
        };

        // 404: no release published yet.
        auto r = run(QStringLiteral("/none"));
        QCOMPARE(r.outcome, UpdateChecker::Outcome::NoRelease);
        QVERIFY(r.error.contains(QStringLiteral("No published")));

        // Rate limited (GitHub: 403 + x-ratelimit-remaining: 0); the reset time is shown.
        const qint64 reset = QDateTime::currentSecsSinceEpoch() + 1800;
        gh.routes[QStringLiteral("/limited")] = {403, R"({"message":"API rate limit exceeded"})",
                                                 {{"x-ratelimit-remaining", "0"},
                                                  {"x-ratelimit-reset", QByteArray::number(reset)}}};
        r = run(QStringLiteral("/limited"));
        QCOMPARE(r.outcome, UpdateChecker::Outcome::Error);
        QVERIFY(r.rateLimited);
        QVERIFY2(r.error.contains(QStringLiteral("rate limit")), qPrintable(r.error));
        QVERIFY(r.error.contains(QDateTime::fromSecsSinceEpoch(reset).toString(QStringLiteral("HH:mm"))));
        gh.routes[QStringLiteral("/429")] = {429, "", {}};
        r = run(QStringLiteral("/429"));
        QVERIFY(r.rateLimited);
        // A 403 that isn't the rate limit.
        gh.routes[QStringLiteral("/forbidden")] = {403, "", {{"x-ratelimit-remaining", "42"}}};
        r = run(QStringLiteral("/forbidden"));
        QVERIFY(!r.rateLimited);
        QVERIFY(r.error.contains(QStringLiteral("403")));
        gh.routes[QStringLiteral("/500")] = {500, "", {}};
        QVERIFY(run(QStringLiteral("/500")).error.contains(QStringLiteral("HTTP 500")));

        // Garbage instead of JSON.
        gh.routes[QStringLiteral("/garbage")] = {200, "<html>captive portal</html>", {}};
        r = run(QStringLiteral("/garbage"));
        QCOMPARE(r.outcome, UpdateChecker::Outcome::Error);
        QVERIFY(r.error.contains(QStringLiteral("could not read")));

        // Server never answers: times out.
        gh.routes[QStringLiteral("/hang")].hang = true;
        r = run(QStringLiteral("/hang"));
        QCOMPARE(r.outcome, UpdateChecker::Outcome::Error);
        QVERIFY2(r.error.contains(QStringLiteral("timed out")), qPrintable(r.error));

        // No network: nothing listening.
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost));
        const quint16 deadPort = probe.serverPort();
        probe.close();
        c.setApiUrl(QUrl(QStringLiteral("http://127.0.0.1:%1/latest").arg(deadPort)));
        c.check();
        QVERIFY(done.wait(5000));
        r = done.takeFirst().at(0).value<UpdateChecker::Result>();
        QCOMPARE(r.outcome, UpdateChecker::Outcome::Error);
        QVERIFY2(r.error.contains(QStringLiteral("Couldn't reach GitHub")), qPrintable(r.error));
    }

    void downloaderVerifies()
    {
        MockGitHub gh;
        serveRelease(gh);
        const auto rel = ReleaseInfo::fromJson(fixture(QStringLiteral("release-0.10.0.json"), gh.base()));
        QVERIFY(rel);
        UpdateDownloader d;
        QSignalSpy done(&d, &UpdateDownloader::finished);
        QSignalSpy prog(&d, &UpdateDownloader::progress);

        QTemporaryDir dir;
        d.start(*rel, dir.path());
        QVERIFY(done.wait(5000));
        auto args = done.takeFirst();
        QVERIFY2(args.at(0).toBool(), qPrintable(args.at(2).toString()));
        const QString deb = args.at(1).toString();
        QCOMPARE(deb, dir.filePath(platformPackageName()));
        QCOMPARE(sha256OfFile(deb).toLatin1(), sha256Hex(debBytes));
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral("SHA256SUMS"))));
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral("SHA256SUMS.minisig"))));
        QCOMPARE(d.verifiedSha256().toLatin1(), sha256Hex(debBytes));
        QVERIFY(!prog.isEmpty());

        // Item 5: SHA256SUMS signed by another key: refused before the package
        // is even requested.
        {
            serveRelease(gh, /*goodSum=*/true, /*goodSignature=*/false);
            gh.requests.clear();
            QTemporaryDir d3;
            d.start(*rel, d3.path());
            QVERIFY(done.wait(5000));
            args = done.takeFirst();
            QVERIFY(!args.at(0).toBool());
            QVERIFY2(args.at(2).toString().contains(QStringLiteral("signature is not valid")), qPrintable(args.at(2).toString()));
            for (const QString &req : gh.requests) {
                QVERIFY2(!req.contains(platformPackageName()), qPrintable(req));
            }
            QVERIFY(!QFile::exists(d3.filePath(platformPackageName())));
        }
        // A SHA256SUMS swapped after signing (attacker rewrote the hashes).
        {
            serveRelease(gh);
            gh.routes[QStringLiteral("/download/v0.10.0/SHA256SUMS")].body.replace("other.tar.gz", "other.tar.gx");
            QTemporaryDir d4;
            d.start(*rel, d4.path());
            QVERIFY(done.wait(5000));
            args = done.takeFirst();
            QVERIFY(!args.at(0).toBool());
            QVERIFY(args.at(2).toString().contains(QStringLiteral("does not match the file")));
        }
        // Unsigned release (no SHA256SUMS.minisig asset): refused, nothing fetched.
        {
            QByteArray json = fixture(QStringLiteral("release-0.10.0.json"), gh.base());
            json.replace("\"SHA256SUMS.minisig\"", "\"SHA256SUMS.asc\"");
            const auto unsigned_ = ReleaseInfo::fromJson(json);
            QVERIFY(unsigned_ && !unsigned_->signatureAsset());
            const int before = gh.requests.size();
            QTemporaryDir d5;
            d.start(*unsigned_, d5.path());
            QVERIFY(done.wait(5000));
            args = done.takeFirst();
            QVERIFY(!args.at(0).toBool());
            QVERIFY(args.at(2).toString().contains(QStringLiteral("SHA256SUMS.minisig")));
            QCOMPARE(gh.requests.size(), before);
        }
        // A build without a signing key never installs.
        {
            setUpdateSigningPublicKeyForTests(QString());
            const bool builtInKey = !updateSigningPublicKey().isEmpty();
            serveRelease(gh);
            const int before = gh.requests.size();
            QTemporaryDir d6;
            d.start(*rel, d6.path());
            QVERIFY(done.wait(5000));
            args = done.takeFirst();
            setUpdateSigningPublicKeyForTests(signKey.publicKeyBase64());
            if (!builtInKey) {
                QVERIFY(!args.at(0).toBool());
                QVERIFY(args.at(2).toString().contains(QStringLiteral("no update-signing key")));
                QCOMPARE(gh.requests.size(), before);
            } else {
                QVERIFY(!args.at(0).toBool()); // the real key didn't sign this test release
            }
        }
        serveRelease(gh);

        // Mismatch: refused, and the unverified package is deleted.
        serveRelease(gh, /*goodSum=*/false);
        QTemporaryDir dir2;
        d.start(*rel, dir2.path());
        QVERIFY(done.wait(5000));
        args = done.takeFirst();
        QVERIFY(!args.at(0).toBool());
        QVERIFY(args.at(2).toString().contains(QStringLiteral("does not match")));
        QVERIFY(!QFile::exists(dir2.filePath(platformPackageName())));

        // SHA256SUMS vanished from the server (404).
        gh.routes.erase(QStringLiteral("/download/v0.10.0/SHA256SUMS"));
        d.start(*rel, dir2.path());
        QVERIFY(done.wait(5000));
        args = done.takeFirst();
        QVERIFY(!args.at(0).toBool());
        QVERIFY2(args.at(2).toString().contains(QStringLiteral("SHA256SUMS is no longer")), qPrintable(args.at(2).toString()));

        // Asset missing from the release JSON: refused before any download.
        const auto noDeb = ReleaseInfo::fromJson(fixture(QStringLiteral("release-no-deb.json"), gh.base()));
        const int before = gh.requests.size();
        d.start(*noDeb, dir2.path());
        QVERIFY(done.wait(5000));
        args = done.takeFirst();
        QVERIFY(!args.at(0).toBool());
        QVERIFY(args.at(2).toString().contains(QStringLiteral("missing zmail_0.11.0_amd64.deb")));
        QCOMPARE(gh.requests.size(), before);
    }

    void dialogButtons()
    {
        const auto rel = ReleaseInfo::fromJson(fixture(QStringLiteral("release-0.10.0.json")));
        UpdateDialog dlg(*rel, QStringLiteral("0.9.0"));
        QVERIFY(dlg.findChild<QLabel *>(QStringLiteral("updateHeading"))->text().contains(QStringLiteral("v0.10.0")));
        QVERIFY(dlg.findChild<QLabel *>(QStringLiteral("updateSubheading"))->text().contains(QStringLiteral("You have 0.9.0")));
        QVERIFY(dlg.notes()->toPlainText().contains(QStringLiteral("Auto-update")));
        QVERIFY(dlg.installButton()->isEnabled());
        QVERIFY(dlg.installButton()->isDefault());
        dlg.open();
        dlg.skipButton()->click();
        QCOMPARE(dlg.choice(), UpdateDialog::Skip);
        QCOMPARE(dlg.result(), int(UpdateDialog::Skip));
        dlg.open();
        dlg.laterButton()->click();
        QCOMPARE(dlg.result(), int(UpdateDialog::Later));
        dlg.open();
        dlg.installButton()->click();
        QCOMPARE(dlg.result(), int(UpdateDialog::Install));

        // No amd64 .deb: Install is disabled and says why.
        const auto noDeb = ReleaseInfo::fromJson(fixture(QStringLiteral("release-no-deb.json")));
        UpdateDialog d2(*noDeb, QStringLiteral("0.9.0"));
        QVERIFY(!d2.installButton()->isEnabled());
        QVERIFY(d2.findChild<QLabel *>(QStringLiteral("updateInstallNote"))->text().contains(QStringLiteral("can't be")));
    }

    void installCommandLine()
    {
        QCOMPARE(UpdateManager::installCommand(QStringLiteral("/tmp/zmail-install-x/zmail_0.10.0_amd64.deb")),
                 (QStringList{"/usr/bin/pkexec", "/usr/bin/apt", "install", "-y",
                              "/tmp/zmail-install-x/zmail_0.10.0_amd64.deb"}));
        // Relative names become absolute (apt must see a file, not a package name).
        QVERIFY(QFileInfo(UpdateManager::installCommand(QStringLiteral("z.deb")).last()).isAbsolute());
    }

    // The installer gets a private copy, re-hashed right before it runs.
    void stagingRechecksHash()
    {
        QTemporaryDir dl;
        const QString pkg = dl.filePath(QStringLiteral("zmail_0.10.0_amd64.deb"));
        {
            QFile f(pkg);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(debBytes);
        }
        const QString sha = QString::fromLatin1(sha256Hex(debBytes));
        QTemporaryDir into(QDir::tempPath() + QStringLiteral("/zmail-install-XXXXXX"));
        QString err;
        const QString staged = UpdateManager::stageVerifiedPackage(pkg, sha, into, &err);
        QVERIFY2(!staged.isEmpty(), qPrintable(err));
        QVERIFY(QFileInfo(staged).isAbsolute());
        QVERIFY(staged.startsWith(into.path()));
        QCOMPARE(QFileInfo(into.path()).permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther
                                                         | QFileDevice::WriteGroup | QFileDevice::WriteOther
                                                         | QFileDevice::ExeGroup | QFileDevice::ExeOther),
                 QFileDevice::Permissions());
        // Swapped after verification: refused, no copy left.
        {
            QFile f(pkg);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write("evil");
        }
        QTemporaryDir into2;
        QVERIFY(UpdateManager::stageVerifiedPackage(pkg, sha, into2, &err).isEmpty());
        QVERIFY(err.contains(QStringLiteral("changed after it was verified")));
        QVERIFY(QDir(into2.path()).isEmpty());
        QVERIFY(UpdateManager::stageVerifiedPackage(pkg, QString(), into2, &err).isEmpty());
    }

    void releasePageUrlIsChecked()
    {
        QVERIFY(isTrustedReleasePageUrl(QStringLiteral("https://github.com/sbj-ee/zmail/releases/tag/v1.0.0")));
        QVERIFY(!isTrustedReleasePageUrl(QStringLiteral("http://github.com/sbj-ee/zmail/releases/tag/v1.0.0")));
        QVERIFY(!isTrustedReleasePageUrl(QStringLiteral("https://github.com.evil/sbj-ee/zmail/releases/x")));
        QVERIFY(!isTrustedReleasePageUrl(QStringLiteral("https://evil@github.com/sbj-ee/zmail/releases/x")));
        QVERIFY(!isTrustedReleasePageUrl(QStringLiteral("https://github.com/other/zmail/releases/x")));
        QVERIFY(!isTrustedReleasePageUrl(QStringLiteral("file:///etc/passwd")));
        QVERIFY(!isTrustedReleasePageUrl(QString()));
        // An untrusted html_url gets no link in the dialog.
        QByteArray json = fixture(QStringLiteral("release-0.10.0.json"));
        json.replace("https://github.com/sbj-ee/zmail/releases/tag/v0.10.0", "file:///etc/passwd");
        const auto rel = ReleaseInfo::fromJson(json);
        QVERIFY(rel);
        UpdateDialog dlg(*rel, QStringLiteral("0.9.0"));
        QVERIFY(!dlg.findChild<QLabel *>(QStringLiteral("updateSubheading"))->text().contains(QStringLiteral("href")));
    }

    // Unlike zterminal's debug-only override, zmail has none: the environment
    // can't point the updater anywhere but GitHub.
    void noUrlOverrideFromEnvironment()
    {
        qputenv("ZMAIL_UPDATE_URL", "http://127.0.0.1:1/evil");
        UpdateChecker c;
        QCOMPARE(c.apiUrl(), QUrl(UpdateChecker::defaultApiUrl()));
        qunsetenv("ZMAIL_UPDATE_URL");
        QCOMPARE(UpdateChecker::defaultApiUrl(),
                 QStringLiteral("https://api.github.com/repos/sbj-ee/zmail/releases/latest"));
    }

    // Help > Check for Updates when up to date / failing reports it; Skip is remembered.
    void manualCheckAndSkip()
    {
        MockGitHub gh;
        serveRelease(gh);
        UpdateManager &m = freshManager(gh);
        QSignalSpy checked(&m, &UpdateManager::checkFinished);

        m.setCurrentVersionForTests(QStringLiteral("0.10.0"));
        m.checkNow(nullptr);
        QVERIFY(checked.wait(5000));
        QCOMPARE(checked.takeFirst().at(0).toBool(), false);
        QTRY_VERIFY(findTop<QMessageBox>("updateMessage"));
        QVERIFY(findTop<QMessageBox>("updateMessage")->text().contains(QStringLiteral("latest version")));
        closeAllTop();
        QVERIFY(!QFile::exists(UpdateState::filePath())); // manual checks don't move the daily clock

        m.setCurrentVersionForTests(QStringLiteral("0.9.0"));
        m.checkNow(nullptr);
        QVERIFY(checked.wait(5000));
        QCOMPARE(checked.takeFirst().at(0).toBool(), true);
        QTRY_VERIFY(findTop<UpdateDialog>("updateDialog"));
        findTop<UpdateDialog>("updateDialog")->skipButton()->click();
        QTRY_COMPARE(m.stage(), UpdateManager::Stage::Idle);
        QCOMPARE(UpdateState::load().skippedVersion, QStringLiteral("v0.10.0"));

        // Startup check: quiet for the skipped version, and records the check time.
        QVERIFY(m.startupCheckIfDue(nullptr));
        QVERIFY(checked.wait(5000));
        QCOMPARE(checked.takeFirst().at(0).toBool(), false);
        QTest::qWait(50);
        QVERIFY(!findTop<UpdateDialog>("updateDialog"));
        QVERIFY(!findTop<QMessageBox>("updateMessage"));
        QVERIFY(UpdateState::load().lastCheck.isValid());
        // ...and not again the same day.
        QVERIFY(!m.startupCheckIfDue(nullptr));

        UpdateState again = UpdateState::load();
        QCOMPARE(again.skippedVersion, QStringLiteral("v0.10.0"));

        // A manual check still offers the skipped version.
        m.checkNow(nullptr);
        QVERIFY(checked.wait(5000));
        QTRY_VERIFY(findTop<UpdateDialog>("updateDialog"));
        findTop<UpdateDialog>("updateDialog")->laterButton()->click();
        QTRY_COMPARE(m.stage(), UpdateManager::Stage::Idle);
        QCOMPARE(UpdateState::load().skippedVersion, QStringLiteral("v0.10.0")); // Later doesn't skip

        // Errors are reported for manual checks only.
        gh.routes[QStringLiteral("/latest")] = {403, "", {{"x-ratelimit-remaining", "0"}}};
        m.checkNow(nullptr);
        QVERIFY(checked.wait(5000));
        QTRY_VERIFY(findTop<QMessageBox>("updateMessage"));
        QVERIFY(findTop<QMessageBox>("updateMessage")->text().contains(QStringLiteral("rate limit")));
        closeAllTop();
        QFile::remove(UpdateState::filePath());
        QVERIFY(m.startupCheckIfDue(nullptr));
        QVERIFY(checked.wait(5000));
        QTest::qWait(50);
        QVERIFY(!findTop<QMessageBox>("updateMessage"));
        QVERIFY(UpdateState::load().lastCheck.isValid()); // a failed attempt still counts for the day

        // Turned off (Help > Check for Updates at Startup): no startup check at all.
        QFile::remove(UpdateState::filePath());
        UpdateManager::setCheckOnStartup(false);
        QVERIFY(!UpdateManager::checkOnStartupEnabled());
        const int before = gh.requests.size();
        QVERIFY(!m.startupCheckIfDue(nullptr));
        QTest::qWait(50);
        QCOMPARE(gh.requests.size(), before);
        QSettings().remove(QStringLiteral("updates/checkOnStartup"));
        QVERIFY(UpdateManager::checkOnStartupEnabled()); // on by default
    }

    // Install: download -> verify -> (mock) pkexec apt install -> restart offer,
    // warning about live sessions.
    void installFlow()
    {
        MockGitHub gh;
        serveRelease(gh);
        UpdateManager &m = freshManager(gh);
        QTemporaryDir out;
        const QString marker = out.filePath(QStringLiteral("installed"));
        // "$0" is the staged package's absolute path (appended); cwd is its private dir.
        m.setInstallerForTests(QStringLiteral("/bin/sh"),
                               {QStringLiteral("-c"),
                                QStringLiteral("test -f \"$0\" && pwd > '%1' && echo \"$0\" >> '%1' && "
                                               "ls -ld . | cut -c1-10 >> '%1'")
                                    .arg(marker)});
        int restarts = 0;
        m.setRestartHandlerForTests([&restarts]() { ++restarts; });
        QSignalSpy installed(&m, &UpdateManager::installFinished);

        // A main window with a message being written, for the restart warning.
        MainWindow w;
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        ComposeWindow *draft = w.openCompose();
        QVERIFY(draft && draft->isVisible());
        QCOMPARE(UpdateManager::openComposeCount(), 1);
        QVERIFY(w.findChild<QAction *>(QStringLiteral("actionCheckForUpdates"))->isEnabled());

        w.findChild<QAction *>(QStringLiteral("actionCheckForUpdates"))->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(findTop<UpdateDialog>("updateDialog"), 5000);
        findTop<UpdateDialog>("updateDialog")->installButton()->click();
        QVERIFY(installed.wait(10000));
        auto args = installed.takeFirst();
        QVERIFY2(args.at(0).toBool(), qPrintable(args.at(1).toString()));
        QFile mk(marker);
        QVERIFY(mk.open(QIODevice::ReadOnly));
        const QStringList lines = QString::fromUtf8(mk.readAll()).trimmed().split(QLatin1Char('\n'));
        QVERIFY2(lines.value(1).startsWith(QLatin1Char('/')), qPrintable(lines.value(1)));
        QVERIFY(lines.value(1).endsWith(QLatin1Char('/') + platformPackageName()));
        QVERIFY(lines.value(0).contains(QStringLiteral("zmail-install-")));
        QCOMPARE(lines.value(2), QStringLiteral("drwx------"));
        QTRY_VERIFY(findTop<QMessageBox>("updateRestart"));
        QMessageBox *box = findTop<QMessageBox>("updateRestart");
        QVERIFY2(box->text().contains(QStringLiteral("1 message being written")), qPrintable(box->text()));
        QCOMPARE(box->defaultButton()->objectName(), QStringLiteral("restartLater"));
        box->findChild<QPushButton *>(QStringLiteral("restartNow"))->click();
        QCOMPARE(restarts, 1);
        QTRY_VERIFY(!QDir(lines.value(0)).exists()); // download dir removed after a successful install

        // Checksum mismatch: refused with a message; the installer never runs.
        QFile::remove(marker);
        serveRelease(gh, /*goodSum=*/false);
        m.checkNow(&w);
        QTRY_VERIFY_WITH_TIMEOUT(findTop<UpdateDialog>("updateDialog"), 5000);
        findTop<UpdateDialog>("updateDialog")->installButton()->click();
        QVERIFY(installed.wait(10000));
        args = installed.takeFirst();
        QVERIFY(!args.at(0).toBool());
        QVERIFY(args.at(1).toString().contains(QStringLiteral("does not match")));
        QTRY_VERIFY(findTop<QMessageBox>("updateMessage"));
        QVERIFY(!QFile::exists(marker));
        closeAllTop();

        // apt fails (or the polkit prompt is dismissed): falls back, keeps the verified .deb.
        serveRelease(gh);
        m.setInstallerForTests(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), QStringLiteral("exit 126")});
        m.checkNow(&w);
        QTRY_VERIFY_WITH_TIMEOUT(findTop<UpdateDialog>("updateDialog"), 5000);
        findTop<UpdateDialog>("updateDialog")->installButton()->click();
        QVERIFY(installed.wait(10000));
        args = installed.takeFirst();
        QVERIFY(!args.at(0).toBool());
        const QString msg = args.at(1).toString();
        QVERIFY2(msg.contains(QStringLiteral("password prompt was dismissed")), qPrintable(msg));
        const QString kept = msg.section(QStringLiteral("The verified package is at:\n"), 1).section(QLatin1Char('\n'), 0, 0);
        QVERIFY2(kept.endsWith(platformPackageName()), qPrintable(kept));
        QVERIFY(QFile::exists(kept));
        QDir(QFileInfo(kept).absolutePath()).removeRecursively();
        closeAllTop();

        // Cancelling the download from the progress dialog.
        gh.routes[QStringLiteral("/download/v0.10.0/zmail_0.10.0_amd64.deb")].hang = true;
        m.checkNow(&w);
        QTRY_VERIFY_WITH_TIMEOUT(findTop<UpdateDialog>("updateDialog"), 5000);
        findTop<UpdateDialog>("updateDialog")->installButton()->click();
        QTRY_VERIFY(findTop<QProgressDialog>("updateProgress"));
        QTRY_COMPARE(m.stage(), UpdateManager::Stage::Downloading);
        findTop<QProgressDialog>("updateProgress")->findChild<QPushButton *>()->click(); // "Cancel"
        QTRY_COMPARE(installed.count(), 1); // abort() reports synchronously
        QVERIFY(installed.takeFirst().at(1).toString().contains(QStringLiteral("cancelled")));
        QCOMPARE(m.stage(), UpdateManager::Stage::Idle);
        QVERIFY(!findTop<QMessageBox>("updateMessage"));
        draft->close();
        QCOMPARE(UpdateManager::openComposeCount(), 0);
    }

    // "Restart Now" closes the window the way Quit does, so a window that
    // won't close (an unsaved message asks first) keeps zmail running and
    // nothing is relaunched; otherwise the relaunch happens.
    void restartRespectsOpenWindows()
    {
        MockGitHub gh;
        serveRelease(gh);
        UpdateManager &m = freshManager(gh);
        int relaunches = 0;
        m.setRelaunchHandlerForTests([&relaunches]() { ++relaunches; });

        MainWindow w;
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        struct Veto : QObject {
            bool eventFilter(QObject *, QEvent *e) override
            {
                if (e->type() == QEvent::Close) {
                    e->ignore(); // what a "Cancel" at "Save draft?" does
                    return true;
                }
                return false;
            }
        } veto;
        w.installEventFilter(&veto);
        m.checkNow(&w);
        QTRY_VERIFY_WITH_TIMEOUT(findTop<UpdateDialog>("updateDialog"), 5000);
        findTop<UpdateDialog>("updateDialog")->laterButton()->click();
        // (The manager only needs a release to name in its messages.)
        m.restartForTests();
        QCOMPARE(relaunches, 0);
        QTRY_VERIFY(findTop<QMessageBox>("updateMessage"));
        QVERIFY(findTop<QMessageBox>("updateMessage")->text().contains(QStringLiteral("did not restart")));
        QVERIFY(w.isVisible());
        closeAllTop();

        w.removeEventFilter(&veto);
        m.restartForTests();
        QCOMPARE(relaunches, 1);
        QVERIFY(!w.isVisible());
    }
};

QTEST_MAIN(TstUpdate)
#include "tst_update.moc"
