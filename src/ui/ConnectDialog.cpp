#include "ConnectDialog.h"

#include "core/AuthManager.h"
#include "core/ClientConfig.h"
#include "core/MailSession.h"
#include "ui/Icons.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

namespace zmail::ui {

namespace {
QString tildePath(const QString &p)
{
    const QString home = QDir::homePath();
    return p.startsWith(home) ? QStringLiteral("~") + p.mid(home.size()) : p;
}
QLabel *wrapLabel(const QString &html, QWidget *parent)
{
    auto *l = new QLabel(html, parent);
    l->setWordWrap(true);
    l->setTextFormat(Qt::RichText);
    l->setOpenExternalLinks(true);
    l->setTextInteractionFlags(Qt::TextBrowserInteraction);
    return l;
}
} // namespace

ConnectDialog::ConnectDialog(zmail::MailSession *session, QWidget *parent)
    : QDialog(parent)
    , m_session(session)
{
    setObjectName(QStringLiteral("connectDialog"));
    setWindowTitle(tr("Connect your Gmail"));
    setMinimumWidth(640);

    auto *lay = new QVBoxLayout(this);
    m_stack = new QStackedWidget(this);
    m_stack->addWidget(buildSetupPage());
    m_stack->addWidget(buildSignInPage());
    m_stack->addWidget(buildWaitingPage());
    lay->addWidget(m_stack);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)->setText(tr("Not Now"));
    connect(buttons, &QDialogButtonBox::rejected, this, [this] {
        m_session->cancelSignIn();
        reject();
    });
    lay->addWidget(buttons);

    connect(m_session, &MailSession::stateChanged, this, [this](MailSession::State s) {
        if (s == MailSession::State::SignedIn) {
            accept();
        } else {
            refresh();
        }
    });
    connect(m_session, &MailSession::clientChanged, this, &ConnectDialog::refresh);
    connect(m_session, &MailSession::signInFailed, this, [this](const QString &reason) {
        setNotice(reason);
        refresh();
    });
    refresh();
}

QString ConnectDialog::setupStepsHtml()
{
    return tr(
        "<p>zmail talks to Gmail with your own Google OAuth client. One-time setup (about 5 minutes):</p>"
        "<ol>"
        "<li>Open <a href='https://console.cloud.google.com/'>Google Cloud Console</a> and create (or pick) a "
        "project, e.g. <b>zmail</b>.</li>"
        "<li><b>APIs &amp; Services \u2192 Library</b>: search for <b>Gmail API</b> and click <b>Enable</b>.</li>"
        "<li>Open <a href='https://console.cloud.google.com/auth/overview'>Google Auth Platform</a> and click "
        "<b>Get started</b>: app name <b>zmail</b>, your email as the support and contact address.</li>"
        "<li><b>Audience</b>: user type <b>External</b>. Then click <b>Publish app</b> so the status is "
        "<b>In production</b>. It stays <i>unverified</i>; that's fine for your own account (Google shows a "
        "warning at sign-in, and refresh tokens don't expire after 7 days the way they do in Testing).</li>"
        "<li><b>Clients \u2192 Create client</b>: application type <b>Desktop app</b>, name <b>zmail</b>, "
        "<b>Create</b>, then <b>Download JSON</b> in the \u201cOAuth client created\u201d box right away (Google may "
        "not show the secret again). It saves "
        "<tt>client_secret_\u2026.apps.googleusercontent.com.json</tt> to <tt>~/Downloads</tt>.</li>"
        "<li>Click <b>Choose client file\u2026</b> below and pick that file. zmail copies it to "
        "<tt>~/.config/zmail/oauth-client.json</tt> (mode 0600).</li>"
        "</ol>");
}

QWidget *ConnectDialog::buildSetupPage()
{
    auto *w = new QWidget(this);
    w->setObjectName(QStringLiteral("setupPage"));
    auto *v = new QVBoxLayout(w);
    auto *title = new QLabel(tr("<h2>Connect your Gmail</h2>"), w);
    v->addWidget(title);
    v->addWidget(wrapLabel(setupStepsHtml(), w));
    m_setupError = new QLabel(w);
    m_setupError->setObjectName(QStringLiteral("setupError"));
    m_setupError->setWordWrap(true);
    m_setupError->setStyleSheet(QStringLiteral("color:#b3261e"));
    v->addWidget(m_setupError);
    auto *row = new QHBoxLayout;
    auto *console = new QPushButton(icon(QStringLiteral("external-link")), tr("Open Google Cloud Console"), w);
    console->setObjectName(QStringLiteral("openConsoleButton"));
    connect(console, &QPushButton::clicked, this,
            [] { QDesktopServices::openUrl(QUrl(QStringLiteral("https://console.cloud.google.com/auth/clients"))); });
    auto *choose = new QPushButton(icon(QStringLiteral("folder-open")), tr("Choose client file\u2026"), w);
    choose->setObjectName(QStringLiteral("chooseClientButton"));
    choose->setDefault(true);
    connect(choose, &QPushButton::clicked, this, &ConnectDialog::chooseClientFile);
    row->addWidget(console);
    row->addStretch();
    row->addWidget(choose);
    v->addLayout(row);
    return w;
}

QWidget *ConnectDialog::buildSignInPage()
{
    auto *w = new QWidget(this);
    w->setObjectName(QStringLiteral("signInPage"));
    auto *v = new QVBoxLayout(w);
    v->addWidget(new QLabel(tr("<h2>Sign in to Gmail</h2>"), w));
    m_clientLabel = wrapLabel({}, w);
    m_clientLabel->setObjectName(QStringLiteral("clientLabel"));
    v->addWidget(m_clientLabel);

    auto *permRow = new QHBoxLayout;
    m_permWarning = new QLabel(w);
    m_permWarning->setObjectName(QStringLiteral("permWarning"));
    m_permWarning->setWordWrap(true);
    m_permWarning->setStyleSheet(QStringLiteral("color:#b26a00"));
    m_fixPerms = new QPushButton(tr("Fix (chmod 600)"), w);
    m_fixPerms->setObjectName(QStringLiteral("fixPermsButton"));
    connect(m_fixPerms, &QPushButton::clicked, this, [this] {
        ClientConfig::tightenPermissions(m_session->clientPath());
        m_session->reloadClient();
        refresh();
    });
    permRow->addWidget(m_permWarning, 1);
    permRow->addWidget(m_fixPerms);
    v->addLayout(permRow);

    v->addWidget(wrapLabel(
        tr("<p>Click <b>Sign in with Google</b>. Your browser opens Google's sign-in page:</p>"
           "<ol>"
           "<li>Pick your Gmail account.</li>"
           "<li>Google says <i>\u201cGoogle hasn\u2019t verified this app\u201d</i> because this is your own "
           "unverified client. Click <b>Advanced</b>, then <b>Go to zmail (unsafe)</b>.</li>"
           "<li>Tick the box to let zmail <b>read, compose, and send email</b> (and see your email address), "
           "then <b>Continue</b>.</li>"
           "<li>When the browser says <i>zmail is signed in</i>, come back here.</li>"
           "</ol>"
           "<p><small>zmail asks only for <tt>gmail.modify</tt> plus <tt>openid email</tt>. It never deletes mail "
           "permanently. The refresh token is kept in your system keyring, never in a file.</small></p>"),
        w));
    m_notice = new QLabel(w);
    m_notice->setObjectName(QStringLiteral("notice"));
    m_notice->setWordWrap(true);
    m_notice->hide();
    v->addWidget(m_notice);

    auto *row = new QHBoxLayout;
    auto *other = new QPushButton(tr("Use a different client file\u2026"), w);
    other->setObjectName(QStringLiteral("otherClientButton"));
    other->setFlat(true);
    connect(other, &QPushButton::clicked, this, &ConnectDialog::chooseClientFile);
    m_signIn = new QPushButton(icon(QStringLiteral("log-in")), tr("Sign in with Google"), w);
    m_signIn->setObjectName(QStringLiteral("signInButton"));
    m_signIn->setDefault(true);
    m_signIn->setMinimumHeight(34);
    connect(m_signIn, &QPushButton::clicked, this, [this] {
        m_notice->hide();
        m_session->signIn();
    });
    row->addWidget(other);
    row->addStretch();
    row->addWidget(m_signIn);
    v->addLayout(row);
    v->addStretch();
    return w;
}

QWidget *ConnectDialog::buildWaitingPage()
{
    auto *w = new QWidget(this);
    w->setObjectName(QStringLiteral("waitingPage"));
    auto *v = new QVBoxLayout(w);
    v->addWidget(new QLabel(tr("<h2>Finish signing in in your browser\u2026</h2>"), w));
    v->addWidget(wrapLabel(tr("<p>zmail is waiting for Google to send you back to "
                              "<tt>http://127.0.0.1</tt>. This page closes by itself once you've allowed access. "
                              "Remember: <b>Advanced \u2192 Go to zmail (unsafe)</b> on the unverified-app screen.</p>"
                              "<p>If no browser window opened, copy this link into your browser:</p>"),
                           w));
    auto *row = new QHBoxLayout;
    m_authUrl = new QLineEdit(w);
    m_authUrl->setObjectName(QStringLiteral("authUrl"));
    m_authUrl->setReadOnly(true);
    auto *copy = new QPushButton(icon(QStringLiteral("copy")), tr("Copy"), w);
    connect(copy, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(m_authUrl->text()); });
    row->addWidget(m_authUrl, 1);
    row->addWidget(copy);
    v->addLayout(row);
    auto *cancel = new QPushButton(tr("Cancel Sign-in"), w);
    cancel->setObjectName(QStringLiteral("cancelSignInButton"));
    connect(cancel, &QPushButton::clicked, this, [this] { m_session->cancelSignIn(); });
    auto *crow = new QHBoxLayout;
    crow->addStretch();
    crow->addWidget(cancel);
    v->addLayout(crow);
    v->addStretch();
    return w;
}

ConnectDialog::Page ConnectDialog::page() const
{
    return Page(m_stack->currentIndex());
}

void ConnectDialog::setNotice(const QString &text, bool error)
{
    m_notice->setText(text);
    m_notice->setStyleSheet(error ? QStringLiteral("color:#b3261e; font-weight:bold") : QString());
    m_notice->setVisible(!text.isEmpty());
}

void ConnectDialog::refresh()
{
    const auto &client = m_session->client();
    const auto state = m_session->state();
    if (state == MailSession::State::NeedsClient) {
        m_stack->setCurrentIndex(SetupPage);
        m_setupError->setText(client.status == ClientConfig::LoadStatus::Missing ? QString() : client.message);
        m_setupError->setVisible(!m_setupError->text().isEmpty());
        return;
    }
    if (state == MailSession::State::SigningIn) {
        m_stack->setCurrentIndex(WaitingPage);
        if (m_session->auth()) {
            m_authUrl->setText(m_session->auth()->lastAuthUrl().toString());
            m_authUrl->setCursorPosition(0);
        }
        return;
    }
    m_stack->setCurrentIndex(SignInPage);
    QString id = client.config.clientId;
    if (id.size() > 24) {
        id = id.left(12) + QStringLiteral("\u2026") + id.mid(id.indexOf(QLatin1Char('.')));
    }
    m_clientLabel->setText(tr("<p>OAuth client <tt>%1</tt> (Desktop app) from <tt>%2</tt>.</p>")
                               .arg(id.toHtmlEscaped(), tildePath(m_session->clientPath()).toHtmlEscaped()));
    m_permWarning->setText(client.permissionsTooOpen
                               ? tr("\u26a0 %1 is readable by other users. zmail requires mode 0600 before signing in.")
                                     .arg(tildePath(m_session->clientPath()))
                               : QString());
    m_permWarning->setVisible(client.permissionsTooOpen);
    m_fixPerms->setVisible(client.permissionsTooOpen);
    // 0600 is required: sign-in stays disabled until the file is tightened.
    m_signIn->setEnabled(state != MailSession::State::Restoring && !client.permissionsTooOpen);
}

void ConnectDialog::chooseClientFile()
{
    QString start = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (start.isEmpty()) {
        start = QDir::homePath() + QStringLiteral("/Downloads");
    }
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Choose the Desktop app client JSON you downloaded"), start,
        tr("Google OAuth client (client_secret*.json *.json);;All files (*)"));
    if (!path.isEmpty()) {
        installClientFile(path);
    }
}

bool ConnectDialog::installClientFile(const QString &path)
{
    const QString err = ClientConfig::install(path, m_session->clientPath());
    if (!err.isEmpty()) {
        m_setupError->setText(err);
        m_setupError->show();
        if (m_stack->currentIndex() == SignInPage) {
            setNotice(err);
        }
        return false;
    }
    m_session->reloadClient();
    setNotice(tr("Client file saved to %1 (mode 0600). You can delete the copy in Downloads.")
                  .arg(tildePath(m_session->clientPath())),
              false);
    refresh();
    return true;
}

} // namespace zmail::ui
