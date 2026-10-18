#include "ui/UpdateDialog.h"

#include <QtGlobal>

#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

namespace zmail {

UpdateDialog::UpdateDialog(const ReleaseInfo &release, const QString &currentVersion, QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("updateDialog"));
    setWindowTitle(QStringLiteral("Update Available"));
    auto *layout = new QVBoxLayout(this);

    auto *heading = new QLabel(QStringLiteral("<h3>zmail %1 is available</h3>").arg(release.tag.toHtmlEscaped()));
    heading->setObjectName(QStringLiteral("updateHeading"));
    layout->addWidget(heading);
    QString sub = QStringLiteral("You have %1.").arg(currentVersion.toHtmlEscaped());
    if (release.publishedAt.isValid()) {
        sub += QStringLiteral(" Released %1.")
                   .arg(QLocale().toString(release.publishedAt.toLocalTime().date(), QLocale::ShortFormat));
    }
    if (release.prerelease) {
        sub += QStringLiteral(" <b>This is a pre-release.</b>");
    }
    if (isTrustedReleasePageUrl(release.htmlUrl)) {
        sub += QStringLiteral(" <a href=\"%1\">Release page</a>").arg(release.htmlUrl.toHtmlEscaped());
    }
    auto *subLabel = new QLabel(sub);
    subLabel->setObjectName(QStringLiteral("updateSubheading"));
    subLabel->setOpenExternalLinks(true);
    subLabel->setWordWrap(true);
    layout->addWidget(subLabel);

    m_notes = new QTextBrowser;
    m_notes->setObjectName(QStringLiteral("releaseNotes"));
    // Links in the notes open in the browser only if they are https.
    m_notes->setOpenLinks(false);
    connect(m_notes, &QTextBrowser::anchorClicked, this, [](const QUrl &url) {
        if (url.scheme() == QLatin1String("https")) {
            QDesktopServices::openUrl(url);
        }
    });
    m_notes->setMarkdown(release.notes.trimmed().isEmpty() ? QStringLiteral("*No release notes.*") : release.notes);
    m_notes->setMinimumSize(520, 240);
    layout->addWidget(m_notes, 1);

    const auto pkg = release.packageAsset();
    const bool installable = pkg && release.checksumAsset() && release.signatureAsset();
    auto *how = new QLabel;
    how->setObjectName(QStringLiteral("updateInstallNote"));
    how->setWordWrap(true);
    if (installable) {
        how->setText(QStringLiteral("<small>Install downloads %1 and the signed SHA256SUMS, checks the signature and the SHA256, then runs "
                                    "<tt>pkexec apt install</tt> (you'll be asked for your password).</small>")
                         .arg(pkg->name.toHtmlEscaped()));
    } else {
        how->setText(QStringLiteral("<small>This release has no %1 package with a signed SHA256SUMS file (SHA256SUMS.minisig), so it "
                                    "can't be installed from here. Download it from the release page.</small>")
                         .arg(QStringLiteral("zmail_%1_amd64.deb").arg(release.versionString()).toHtmlEscaped()));
    }
    layout->addWidget(how);

    auto *buttons = new QDialogButtonBox;
    m_install = buttons->addButton(QStringLiteral("&Install"), QDialogButtonBox::AcceptRole);
    m_install->setObjectName(QStringLiteral("install"));
    m_install->setEnabled(installable);
    m_later = buttons->addButton(QStringLiteral("&Later"), QDialogButtonBox::RejectRole);
    m_later->setObjectName(QStringLiteral("later"));
    m_skip = buttons->addButton(QStringLiteral("&Skip This Version"), QDialogButtonBox::DestructiveRole);
    m_skip->setObjectName(QStringLiteral("skip"));
    (installable ? m_install : m_later)->setDefault(true);
    layout->addWidget(buttons);

    connect(m_install, &QPushButton::clicked, this, [this]() {
        m_choice = Install;
        done(Install);
    });
    connect(m_later, &QPushButton::clicked, this, [this]() {
        m_choice = Later;
        done(Later);
    });
    connect(m_skip, &QPushButton::clicked, this, [this]() {
        m_choice = Skip;
        done(Skip);
    });
    resize(600, 440);
}

} // namespace zmail
