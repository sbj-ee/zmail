// Offscreen screenshots of the self-update flow against a local fake GitHub:
// the update dialog, the download progress, and the restart prompt (with a message
// still open). The release, its notes and the signing key are made up for the
// shot; nothing touches the network, apt, polkit or your real settings.
//   QT_QPA_PLATFORM=offscreen zmail_updateshot <outdir>
//     writes update-available.png, update-downloading.png, update-restart.png
#include "MainWindow.hpp"
#include "MinisignTestSigner.h"
#include "core/Update.h"
#include "mock/FakeGitHub.h"
#include "ui/ComposeWindow.h"
#include "ui/Theme.h"
#include "ui/UpdateDialog.h"
#include "ui/UpdateManager.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QMessageBox>
#include <QNetworkProxy>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <cstdio>

using namespace zmail;

namespace {
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

void shoot(QWidget *w, const QString &dir, const QString &name)
{
    QTest::qWait(250);
    const QString path = QDir(dir).filePath(name);
    w->grab().save(path);
    std::printf("%s\n", qPrintable(path));
}
} // namespace

int main(int argc, char *argv[])
{
    // Never the real ~/.config: settings and update-state live in a throwaway dir.
    QTemporaryDir cfg;
    qputenv("XDG_CONFIG_HOME", cfg.path().toUtf8());
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("zmail"));
    QApplication::setOrganizationName(QStringLiteral("sbj-ee"));
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
    if (argc < 2) {
        std::fprintf(stderr, "usage: zmail_updateshot <outdir>\n");
        return 2;
    }
    const QString out = QString::fromLocal8Bit(argv[1]);
    QDir().mkpath(out);
    ui::applyThemeId(QStringLiteral("light"));

    // A package big enough to show a progress bar, signed by a throwaway key.
    const QByteArray pkg = QByteArray("!<arch>\nsample zmail package\n").repeated(160000); // ~4.5 MB
    MinisignTestKey key;
    setUpdateSigningPublicKeyForTests(key.publicKeyBase64());
    const QByteArray sum = QCryptographicHash::hash(pkg, QCryptographicHash::Sha256).toHex();
    const QByteArray sums = sum + "  zmail_0.10.0_amd64.deb\n";

    MockGitHub gh;
    const QByteArray release = QByteArray(R"({
  "html_url": "https://github.com/sbj-ee/zmail/releases/tag/v0.10.0",
  "tag_name": "v0.10.0", "name": "zmail 0.10.0", "draft": false, "prerelease": false,
  "published_at": "2026-10-25T15:00:00Z",
  "body": "## New in 0.10.0\n\n- **Sample release notes.** This release and its notes are made up for the screenshot.\n- Notes are Markdown: lists, **bold**, `code`.\n- The update was downloaded, its signature and SHA-256 checked, then installed.\n",
  "assets": [
    {"name": "zmail_0.10.0_amd64.deb", "size": 4500000, "browser_download_url": "@BASE@/dl/zmail_0.10.0_amd64.deb"},
    {"name": "SHA256SUMS", "size": 90, "browser_download_url": "@BASE@/dl/SHA256SUMS"},
    {"name": "SHA256SUMS.minisig", "size": 300, "browser_download_url": "@BASE@/dl/SHA256SUMS.minisig"}
  ]})")
                                    .replace("@BASE@", gh.base().toUtf8());
    gh.routes[QStringLiteral("/latest")].body = release;
    gh.routes[QStringLiteral("/dl/zmail_0.10.0_amd64.deb")].body = pkg;
    gh.routes[QStringLiteral("/dl/SHA256SUMS")].body = sums;
    gh.routes[QStringLiteral("/dl/SHA256SUMS.minisig")].body = key.sign(sums, "zmail v0.10.0 SHA256SUMS");

    MainWindow::setDemoData(true);
    MainWindow w;
    w.resize(1100, 700);
    w.show();
    QTest::qWait(300);

    UpdateManager &m = UpdateManager::instance();
    m.setCurrentVersionForTests(QStringLiteral("0.9.0"));
    m.checker()->setApiUrl(QUrl(gh.url(QStringLiteral("/latest"))));
    m.setInstallerForTests(QStringLiteral("/bin/true"), {}); // stands in for pkexec apt install
    m.setRestartHandlerForTests([]() {});

    // 1. The update dialog.
    m.checkNow(&w);
    if (!QTest::qWaitFor([] { return findTop<UpdateDialog>("updateDialog") != nullptr; }, 10000)) {
        std::fprintf(stderr, "no update dialog\n");
        return 1;
    }
    auto *dlg = findTop<UpdateDialog>("updateDialog");
    shoot(dlg, out, QStringLiteral("update-available.png"));

    // 2. Downloading: the package stalls at about 60%.
    gh.routes[QStringLiteral("/dl/zmail_0.10.0_amd64.deb")].stallAfter = qint64(pkg.size()) * 6 / 10;
    dlg->installButton()->click();
    auto *progress = static_cast<QProgressDialog *>(nullptr);
    if (!QTest::qWaitFor(
            [&] {
                progress = findTop<QProgressDialog>("updateProgress");
                return progress && progress->labelText().contains(QLatin1String(".deb")) && progress->value() >= 500;
            },
            10000)) {
        std::fprintf(stderr, "no download progress (%s, %d)\n", progress ? qPrintable(progress->labelText()) : "no dialog",
                     progress ? progress->value() : -1);
        return 1;
    }
    shoot(progress, out, QStringLiteral("update-downloading.png"));
    progress->findChild<QPushButton *>()->click(); // Cancel
    QTest::qWait(300);

    // 3. The install finished: the restart prompt, with a message still being written.
    gh.routes[QStringLiteral("/dl/zmail_0.10.0_amd64.deb")].stallAfter = -1;
    ComposeWindow *draft = w.openCompose(true);
    draft->move(40, 40);
    QSignalSpy installed(&m, &UpdateManager::installFinished);
    m.checkNow(&w);
    if (!QTest::qWaitFor([] { return findTop<UpdateDialog>("updateDialog") != nullptr; }, 10000)) {
        return 1;
    }
    findTop<UpdateDialog>("updateDialog")->installButton()->click();
    if (!installed.wait(15000) || !installed.takeFirst().at(0).toBool()
        || !QTest::qWaitFor([] { return findTop<QMessageBox>("updateRestart") != nullptr; }, 10000)) {
        std::fprintf(stderr, "no restart prompt\n");
        return 1;
    }
    shoot(findTop<QMessageBox>("updateRestart"), out, QStringLiteral("update-restart.png"));
    return 0;
}
