#include "ui/UpdateManager.h"

#include <QtGlobal>

#include "MainWindow.hpp"
#include "ui/UpdateDialog.h"
#include "ui/ComposeWindow.h"
#include "version.hpp"

#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

namespace zmail {

namespace {
const QString kInstalledBinary = QStringLiteral("/usr/bin/zmail");
// Absolute paths: never whatever "pkexec"/"apt" $PATH finds first.
const QString kPkexec = QStringLiteral("/usr/bin/pkexec");
const QString kApt = QStringLiteral("/usr/bin/apt");
constexpr const char kCheckOnStartupKey[] = "updates/checkOnStartup";
} // namespace

UpdateManager &UpdateManager::instance()
{
    static UpdateManager *m = new UpdateManager(qApp);
    return *m;
}

UpdateManager::UpdateManager(QObject *parent)
    : QObject(parent)
    , m_checker(new UpdateChecker(this))
    , m_downloader(new UpdateDownloader(this))
    , m_currentVersion(QString::fromLatin1(kVersionString))
{
    connect(m_checker, &UpdateChecker::finished, this, &UpdateManager::onCheckFinished);
    connect(m_downloader, &UpdateDownloader::finished, this, &UpdateManager::onDownloadFinished);
    connect(m_downloader, &UpdateDownloader::progress, this, [this](const QString &name, qint64 got, qint64 total) {
        if (!m_progress) {
            return;
        }
        m_progress->setLabelText(QStringLiteral("Downloading %1\u2026").arg(name));
        if (total > 0) {
            m_progress->setMaximum(1000);
            m_progress->setValue(int(got * 1000 / total));
        } else {
            m_progress->setMaximum(0); // busy
        }
    });
}

void UpdateManager::setStage(Stage s)
{
    if (m_stage != s) {
        m_stage = s;
        emit stageChanged(s);
    }
}

void UpdateManager::setInstallerForTests(const QString &program, const QStringList &args)
{
    m_testProgram = program;
    m_testArgs = args;
}

void UpdateManager::resetForTests()
{
    m_checker->abort();
    m_downloader->cancel();
    m_tmp.reset();
    m_installDir.reset();
    m_release = {};
    m_debPath.clear();
    m_lastMessage.clear();
    m_startupScheduled = false;
    m_stage = Stage::Idle;
}

bool UpdateManager::checkOnStartupEnabled()
{
    return QSettings().value(QLatin1String(kCheckOnStartupKey), true).toBool(); // on unless turned off
}

void UpdateManager::setCheckOnStartup(bool enabled)
{
    QSettings().setValue(QLatin1String(kCheckOnStartupKey), enabled);
}

QStringList UpdateManager::installCommand(const QString &debPath)
{
    // An absolute path makes apt treat the argument as a file, not a package name.
    return {kPkexec, kApt, QStringLiteral("install"), QStringLiteral("-y"), QFileInfo(debPath).absoluteFilePath()};
}

QString UpdateManager::stageVerifiedPackage(const QString &path, const QString &sha256, QTemporaryDir &into,
                                            QString *error)
{
    auto fail = [error](const QString &why) {
        if (error) {
            *error = why;
        }
        return QString();
    };
    if (!into.isValid()) {
        return fail(QStringLiteral("Couldn't create a private folder for the package."));
    }
    // QTemporaryDir creates it 0700; make sure.
    QFile::setPermissions(into.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    const QString dest = QDir(into.path()).filePath(QFileInfo(path).fileName());
    if (!QFile::copy(path, dest)) {
        return fail(QStringLiteral("Couldn't copy the package to %1.").arg(into.path()));
    }
    QFile::setPermissions(dest, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup
                                    | QFileDevice::ReadOther);
    if (sha256.isEmpty() || sha256OfFile(dest) != sha256.toLower()) {
        QFile::remove(dest);
        return fail(QStringLiteral("The package changed after it was verified, so it was not installed."));
    }
    return QFileInfo(dest).absoluteFilePath();
}

int UpdateManager::openComposeCount()
{
    int open = 0;
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (auto *mw = qobject_cast<MainWindow *>(w)) {
            for (const ComposeWindow *c : mw->composers()) {
                open += c && c->isVisible() ? 1 : 0;
            }
        }
    }
    return open;
}

void UpdateManager::checkNow(QWidget *parent)
{
    startCheck(parent, true);
}

void UpdateManager::scheduleStartupCheck(QWidget *parent, int delayMs)
{
    if (m_startupScheduled) {
        return; // once per process, however many windows open
    }
    m_startupScheduled = true;
    QPointer<QWidget> p(parent);
    QTimer::singleShot(delayMs, this, [this, p]() {
        if (p) {
            startupCheckIfDue(p);
        }
    });
}

bool UpdateManager::startupCheckIfDue(QWidget *parent)
{
    const UpdateState state = UpdateState::load();
    if (!UpdatePolicy::shouldCheckAtStartup(checkOnStartupEnabled(), state,
                                            QDateTime::currentDateTimeUtc())) {
        return false;
    }
    startCheck(parent, false);
    return true;
}

void UpdateManager::startCheck(QWidget *parent, bool manual)
{
    if (m_stage != Stage::Idle && m_stage != Stage::Done) {
        if (manual && m_stage == Stage::Checking) {
            m_manual = true; // a quiet startup check becomes a reported one
            m_parent = parent;
        }
        return;
    }
    m_parent = parent;
    m_manual = manual;
    setStage(Stage::Checking);
    m_checker->check();
}

void UpdateManager::onCheckFinished(const UpdateChecker::Result &r)
{
    if (!m_manual) {
        // Count the attempt even if it failed, so a flaky network or a rate
        // limit doesn't turn into a request at every startup.
        UpdateState st = UpdateState::load();
        st.lastCheck = QDateTime::currentDateTimeUtc();
        st.save();
    }
    setStage(Stage::Idle);
    if (r.outcome != UpdateChecker::Outcome::Release) {
        m_lastMessage = r.error;
        if (m_manual) {
            message(QStringLiteral("Check for Updates"), r.error, r.outcome == UpdateChecker::Outcome::Error);
        }
        emit checkFinished(false);
        return;
    }
    m_release = r.release;
    if (!UpdatePolicy::shouldOffer(m_release, m_currentVersion, UpdateState::load(), m_manual)) {
        m_lastMessage = QStringLiteral("You're running the latest version of zmail (%1).").arg(m_currentVersion);
        if (m_manual) {
            if (isNewerVersion(m_currentVersion, m_release.tag)) {
                m_lastMessage = QStringLiteral("You're running zmail %1, newer than the latest release (%2).")
                                    .arg(m_currentVersion, m_release.tag);
            }
            message(QStringLiteral("Check for Updates"), m_lastMessage, false);
        }
        emit checkFinished(false);
        return;
    }
    offer();
    emit checkFinished(true);
}

void UpdateManager::offer()
{
    setStage(Stage::Offering);
    auto *dlg = new UpdateDialog(m_release, m_currentVersion, m_parent);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &QDialog::finished, this, [this](int result) {
        if (result == UpdateDialog::Skip) {
            UpdateState st = UpdateState::load();
            st.skippedVersion = m_release.tag;
            st.save();
            setStage(Stage::Idle);
        } else if (result == UpdateDialog::Install) {
            startDownload();
        } else {
            setStage(Stage::Idle);
        }
    });
    dlg->open();
}

void UpdateManager::startDownload()
{
    m_tmp = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/zmail-update-XXXXXX"));
    if (!m_tmp->isValid()) {
        message(QStringLiteral("Update"), QStringLiteral("Couldn't create a temporary folder for the download."), true);
        setStage(Stage::Idle);
        return;
    }
    setStage(Stage::Downloading);
    m_progress = new QProgressDialog(QStringLiteral("Downloading\u2026"), QStringLiteral("Cancel"), 0, 0, m_parent);
    m_progress->setObjectName(QStringLiteral("updateProgress"));
    m_progress->setWindowTitle(QStringLiteral("Updating zmail"));
    m_progress->setAttribute(Qt::WA_DeleteOnClose);
    m_progress->setAutoClose(false);
    m_progress->setAutoReset(false);
    m_progress->setMinimumDuration(0);
    m_progress->setMinimumWidth(420); // room for "Downloading zmail_0.10.0_amd64.deb…"
    connect(m_progress, &QProgressDialog::canceled, m_downloader, &UpdateDownloader::cancel);
    m_progress->open();
    m_downloader->start(m_release, m_tmp->path());
}

void UpdateManager::onDownloadFinished(bool ok, const QString &debPath, const QString &error)
{
    if (m_progress) {
        m_progress->disconnect(m_downloader);
        m_progress->close();
    }
    if (!ok) {
        m_tmp.reset();
        setStage(Stage::Idle);
        m_lastMessage = error;
        if (error != QLatin1String("Download cancelled.")) {
            message(QStringLiteral("Update Not Installed"), error, true);
        }
        emit installFinished(false, error);
        return;
    }
    m_debPath = debPath;
    runInstaller();
}

void UpdateManager::runInstaller()
{
    const QString pkgName = QFileInfo(m_debPath).fileName();
    // Hand the installer a private copy, re-verified against the signed hash
    // at the last moment.
    m_installDir = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/zmail-install-XXXXXX"));
    QString stageError;
    const QString staged = stageVerifiedPackage(m_debPath, m_downloader->verifiedSha256(), *m_installDir, &stageError);
    if (staged.isEmpty()) {
        m_installDir.reset();
        m_tmp.reset();
        setStage(Stage::Idle);
        m_lastMessage = stageError;
        message(QStringLiteral("Update Not Installed"), stageError, true);
        emit installFinished(false, stageError);
        return;
    }
    QString program;
    QStringList args;
    if (!m_testProgram.isEmpty()) {
        program = m_testProgram;
        args = m_testArgs;
        args << staged;
    } else {
        // Only replace a packaged install; a build-tree or /usr/local binary
        // wouldn't be the one apt updates.
        if (QCoreApplication::applicationFilePath() != kInstalledBinary) {
            fallback(QStringLiteral("This zmail (%1) wasn't installed from the .deb, so the package can't be "
                                    "installed in its place automatically.")
                         .arg(QCoreApplication::applicationFilePath()));
            return;
        }
        const QStringList cmd = installCommand(staged);
        if (!QFileInfo(cmd.at(0)).isExecutable() || !QFileInfo(cmd.at(1)).isExecutable()) {
            fallback(QStringLiteral("pkexec or apt isn't available, so the package can't be installed from here."));
            return;
        }
        program = cmd.at(0);
        args = cmd.mid(1);
    }
    setStage(Stage::Installing);
    m_progress = new QProgressDialog(
        QStringLiteral("Installing %1\u2026\nEnter your password in the system prompt.").arg(pkgName), QString(), 0, 0,
        m_parent);
    m_progress->setObjectName(QStringLiteral("updateProgress"));
    m_progress->setWindowTitle(QStringLiteral("Updating zmail"));
    m_progress->setAttribute(Qt::WA_DeleteOnClose);
    m_progress->setMinimumDuration(0);
    m_progress->setMinimumWidth(420);
    m_progress->setCancelButton(nullptr);
    m_progress->open();

    m_installer = new QProcess(this);
    m_installer->setWorkingDirectory(m_installDir->path());
    m_installer->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_installer, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
        onInstallerFinished(code, st == QProcess::CrashExit);
    });
    connect(m_installer, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            onInstallerFinished(127, false);
        }
    });
    m_installer->start(program, args); // argv list, no shell
}

void UpdateManager::onInstallerFinished(int exitCode, bool crashed)
{
    if (!m_installer) {
        return;
    }
    const QString output = QString::fromLocal8Bit(m_installer->readAll()).trimmed();
    m_installer->deleteLater();
    m_installer = nullptr;
    if (m_progress) {
        m_progress->close();
    }
    if (crashed || exitCode != 0) {
        QString why;
        if (exitCode == 126) {
            why = QStringLiteral("The password prompt was dismissed, so nothing was installed.");
        } else if (exitCode == 127) {
            why = QStringLiteral("Not authorised to install packages (pkexec exit 127).");
        } else {
            why = QStringLiteral("apt couldn't install the package (exit %1).").arg(exitCode);
            if (!output.isEmpty()) {
                why += QLatin1Char('\n') + output.right(600);
            }
        }
        fallback(why);
        return;
    }
    setStage(Stage::Done);
    m_lastMessage = QStringLiteral("zmail %1 was installed.").arg(m_release.tag);
    m_tmp.reset(); // remove the downloaded package
    m_installDir.reset();
    emit installFinished(true, m_lastMessage);
    offerRestart();
}

void UpdateManager::offerRestart()
{
    const int composing = openComposeCount();
    QString text = QStringLiteral("zmail %1 was installed. Restart zmail now to use it?").arg(m_release.tag);
    if (composing > 0) {
        text += QStringLiteral("\n\nYou have %1 open. zmail will ask whether to save %2 before it restarts.")
                    .arg(composing == 1 ? QStringLiteral("1 message being written")
                                        : QStringLiteral("%1 messages being written").arg(composing),
                         composing == 1 ? QStringLiteral("it") : QStringLiteral("them"));
    }
    auto *box = new QMessageBox(composing > 0 ? QMessageBox::Warning : QMessageBox::Question,
                                QStringLiteral("Restart zmail"), text, QMessageBox::NoButton, m_parent);
    box->setObjectName(QStringLiteral("updateRestart"));
    box->setAttribute(Qt::WA_DeleteOnClose);
    QPushButton *restartBtn = box->addButton(QStringLiteral("&Restart Now"), QMessageBox::AcceptRole);
    restartBtn->setObjectName(QStringLiteral("restartNow"));
    QPushButton *laterBtn = box->addButton(QStringLiteral("&Later"), QMessageBox::RejectRole);
    laterBtn->setObjectName(QStringLiteral("restartLater"));
    box->setDefaultButton(composing > 0 ? laterBtn : restartBtn);
    connect(box, &QMessageBox::buttonClicked, this, [this, restartBtn](QAbstractButton *b) {
        if (b == restartBtn) {
            restart();
        }
    });
    box->open();
}

void UpdateManager::restart()
{
    if (m_restartHandler) {
        m_restartHandler();
        return;
    }
    // Close the way Quit does, so every message still being written gets its
    // "Save draft?" first. If one is kept open (Cancel, or Save, which quits
    // by itself once the draft is stored), zmail stays up and nothing starts.
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (auto *mw = qobject_cast<MainWindow *>(w); mw && !mw->close()) {
            message(QStringLiteral("Restart zmail"),
                    QStringLiteral("zmail did not restart because a message is still open. Quit and reopen zmail "
                                   "when you are ready to use %1.")
                        .arg(m_release.tag),
                    false);
            return;
        }
    }
    if (m_relaunchHandler) {
        m_relaunchHandler();
        return;
    }
    // A new zmail two seconds from now, once this one has let go of the mail
    // cache. A fixed command line: nothing from the network is in it.
    QProcess::startDetached(QStringLiteral("/bin/sh"),
                            {QStringLiteral("-c"), QStringLiteral("sleep 2; exec \"$0\""), kInstalledBinary});
    QCoreApplication::quit();
}

void UpdateManager::fallback(const QString &why)
{
    setStage(Stage::Idle);
    if (m_tmp) {
        m_tmp->setAutoRemove(false); // keep the verified package for a manual install
    }
    const QString text =
        QStringLiteral("%1\n\nThe verified package is at:\n%2\n\nInstall it with:\nsudo apt install %2\n\n"
                       "Opening the release page.")
            .arg(why, m_debPath);
    m_lastMessage = text;
    if (isTrustedReleasePageUrl(m_release.htmlUrl) && m_testProgram.isEmpty()) {
        QDesktopServices::openUrl(QUrl(m_release.htmlUrl));
    }
    message(QStringLiteral("Update Not Installed"), text, true);
    emit installFinished(false, text);
}

void UpdateManager::message(const QString &title, const QString &text, bool warning)
{
    m_lastMessage = text;
    auto *box = new QMessageBox(warning ? QMessageBox::Warning : QMessageBox::Information, title, text,
                                QMessageBox::Ok, m_parent);
    box->setObjectName(QStringLiteral("updateMessage"));
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setTextInteractionFlags(Qt::TextSelectableByMouse);
    box->open();
}

} // namespace zmail
