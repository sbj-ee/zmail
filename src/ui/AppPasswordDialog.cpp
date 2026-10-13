#include "AppPasswordDialog.h"

#include "core/ConnectionTester.h"
#include "core/TokenStore.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>

namespace zmail::ui {

AppPasswordDialog::AppPasswordDialog(zmail::TokenStore *store, QWidget *parent)
    : QDialog(parent)
    , m_store(store)
    , m_tester(new zmail::ConnectionTester(this))
{
    setObjectName(QStringLiteral("appPasswordDialog"));
    setWindowTitle(tr("Gmail (app password)"));
    setMinimumWidth(560);
    const AppPasswordAccount defaults;

    auto *lay = new QVBoxLayout(this);
    lay->addWidget(new QLabel(tr("<h2>Connect Gmail with an app password</h2>"), this));
    auto *intro = new QLabel(instructionsHtml(), this);
    intro->setWordWrap(true);
    intro->setTextFormat(Qt::RichText);
    intro->setOpenExternalLinks(true);
    lay->addWidget(intro);

    auto *form = new QFormLayout;
    m_displayName = new QLineEdit(this);
    m_displayName->setObjectName(QStringLiteral("displayNameEdit"));
    m_displayName->setPlaceholderText(tr("Alex Morgan (optional)"));
    m_displayName->setToolTip(tr("Shown to recipients in the From line: \u201cAlex Morgan <you@gmail.com>\u201d."));
    form->addRow(tr("Your name:"), m_displayName);
    m_email = new QLineEdit(this);
    m_email->setObjectName(QStringLiteral("emailEdit"));
    m_email->setPlaceholderText(QStringLiteral("you@gmail.com"));
    form->addRow(tr("Gmail address:"), m_email);
    m_password = new QLineEdit(this);
    m_password->setObjectName(QStringLiteral("appPasswordEdit"));
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setPlaceholderText(QStringLiteral("xxxx xxxx xxxx xxxx"));
    m_password->setAttribute(Qt::WA_InputMethodEnabled, false);
    form->addRow(tr("App password:"), m_password);
    lay->addLayout(form);

    m_servers = new QGroupBox(tr("Servers"), this);
    m_servers->setObjectName(QStringLiteral("serversBox"));
    m_servers->setCheckable(true);
    m_servers->setChecked(false); // Gmail's defaults; tick to change
    auto *sf = new QFormLayout(m_servers);
    auto hostRow = [this, sf](const QString &label, QLineEdit *&host, QSpinBox *&port, const QString &h, int p,
                              const char *name) {
        host = new QLineEdit(h, m_servers);
        host->setObjectName(QString::fromLatin1(name) + QStringLiteral("Host"));
        port = new QSpinBox(m_servers);
        port->setObjectName(QString::fromLatin1(name) + QStringLiteral("Port"));
        port->setRange(1, 65535);
        port->setValue(p);
        auto *row = new QHBoxLayout;
        row->addWidget(host, 1);
        row->addWidget(port);
        sf->addRow(label, row);
    };
    hostRow(tr("IMAP (SSL/TLS):"), m_imapHost, m_imapPort, defaults.imapHost, defaults.imapPort, "imap");
    hostRow(tr("SMTP (SSL/TLS):"), m_smtpHost, m_smtpPort, defaults.smtpHost, defaults.smtpPort, "smtp");
    lay->addWidget(m_servers);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("testStatus"));
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    lay->addWidget(m_status);

    auto *buttons = new QDialogButtonBox(this);
    m_test = buttons->addButton(tr("Test"), QDialogButtonBox::ActionRole);
    m_test->setObjectName(QStringLiteral("testButton"));
    m_save = buttons->addButton(QDialogButtonBox::Save);
    m_save->setObjectName(QStringLiteral("saveButton"));
    m_save->setEnabled(false);
    buttons->addButton(QDialogButtonBox::Cancel);
    connect(m_test, &QPushButton::clicked, this, &AppPasswordDialog::runTest);
    connect(m_save, &QPushButton::clicked, this, &AppPasswordDialog::save);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(buttons);

    for (QLineEdit *e : {m_email, m_password, m_imapHost, m_smtpHost}) {
        connect(e, &QLineEdit::textEdited, this, &AppPasswordDialog::invalidate);
    }
    for (QSpinBox *s : {m_imapPort, m_smtpPort}) {
        connect(s, &QSpinBox::valueChanged, this, &AppPasswordDialog::invalidate);
    }
    connect(m_servers, &QGroupBox::toggled, this, &AppPasswordDialog::invalidate);
    connect(m_tester, &zmail::ConnectionTester::progress, this, [this](const QString &s) { setStatus(s, 0); });
    setStatus(tr("Enter your address and app password, then click Test."), 0);
}

AppPasswordDialog::~AppPasswordDialog()
{
    m_password->clear();
}

QString AppPasswordDialog::instructionsHtml()
{
    return tr("<p>No Google Cloud project or OAuth client needed: zmail signs in over IMAP and SMTP.</p>"
              "<ol>"
              "<li>Turn on <b>2-Step Verification</b> for your Google account (app passwords need it).</li>"
              "<li>Open <a href='https://myaccount.google.com/apppasswords'>myaccount.google.com/apppasswords</a>, "
              "name it <b>zmail</b> and click <b>Create</b>.</li>"
              "<li>Type or paste the 16-letter password here, click <b>Test</b>, then <b>Save</b>.</li>"
              "</ol>"
              "<p><small>The app password goes in your system keyring, never in a file or a log. "
              "Revoke it on the same Google page at any time.</small></p>");
}

AppPasswordAccount AppPasswordDialog::account() const
{
    AppPasswordAccount a;
    a.email = m_email->text().trimmed();
    a.displayName = m_displayName->text().trimmed();
    if (m_servers->isChecked()) {
        a.imapHost = m_imapHost->text().trimmed();
        a.imapPort = m_imapPort->value();
        a.smtpHost = m_smtpHost->text().trimmed();
        a.smtpPort = m_smtpPort->value();
    }
    return a;
}

QString AppPasswordDialog::statusText() const
{
    return m_status->text();
}

void AppPasswordDialog::setStatus(const QString &text, int kind)
{
    m_status->setText(text);
    m_status->setStyleSheet(kind == 1   ? QStringLiteral("color:#1e7d32; font-weight:bold")
                            : kind == 2 ? QStringLiteral("color:#b3261e; font-weight:bold")
                                        : QString());
}

void AppPasswordDialog::invalidate()
{
    m_tester->cancel();
    m_tested = false;
    m_save->setEnabled(false);
    m_test->setEnabled(true);
}

void AppPasswordDialog::runTest()
{
    invalidate();
    const AppPasswordAccount a = account();
    if (!AppPasswordAccount::looksLikeEmail(a.email)) {
        setStatus(tr("Enter your full Gmail address."), 2);
        return;
    }
    if (!a.isValid()) {
        setStatus(tr("Check the server names and ports."), 2);
        return;
    }
    QString err;
    const QString pw = AppPasswordAccount::normalizeAppPassword(m_password->text(), &err);
    if (pw.isEmpty()) {
        setStatus(err, 2);
        return;
    }
    m_test->setEnabled(false);
    QPointer<AppPasswordDialog> self(this);
    m_tester->test(a, pw, [self](const zmail::ConnectionTester::Result &r) {
        if (!self) {
            return;
        }
        self->m_test->setEnabled(true);
        self->m_tested = r.ok();
        self->m_save->setEnabled(r.ok());
        self->setStatus(r.imapMessage + QLatin1Char('\n') + r.smtpMessage, r.ok() ? 1 : 2);
    });
}

void AppPasswordDialog::save()
{
    if (!m_tested || !m_store) {
        return;
    }
    const AppPasswordAccount a = account();
    const QString pw = AppPasswordAccount::normalizeAppPassword(m_password->text());
    if (pw.isEmpty()) {
        invalidate();
        return;
    }
    m_save->setEnabled(false);
    m_test->setEnabled(false);
    setStatus(tr("Saving to the system keyring\u2026"), 0);
    QPointer<AppPasswordDialog> self(this);
    m_store->write(AppPasswordAccount::appPasswordKey(a.email), pw, [self, a](bool ok, const QString &error) {
        if (!self) {
            return;
        }
        if (!ok) {
            // No plaintext fallback: without a keyring there is nowhere safe to keep it.
            self->setStatus(tr("Couldn't save to the system keyring, so nothing was saved. %1").arg(error), 2);
            self->m_test->setEnabled(true);
            self->m_save->setEnabled(true);
            return;
        }
        QSettings s;
        a.save(s);
        s.sync();
        self->m_password->clear();
        emit self->saved(a);
        self->accept();
    });
}

} // namespace zmail::ui
