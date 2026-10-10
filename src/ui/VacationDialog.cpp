#include "VacationDialog.h"

#include "core/AuthManager.h"
#include "core/MailSession.h"

#include <QCheckBox>
#include <QDateEdit>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace zmail::ui {

VacationDialog::VacationDialog(zmail::MailSession *session, QWidget *parent)
    : QDialog(parent)
    , m_session(session)
{
    setObjectName(QStringLiteral("vacationDialog"));
    setWindowTitle(tr("Vacation Responder"));
    auto *lay = new QVBoxLayout(this);

    auto *intro = new QLabel(tr("Gmail answers incoming mail for you while you are away, at most once every four days "
                                "to each person. It runs at Google, so it keeps working when zmail is closed."),
                             this);
    intro->setWordWrap(true);
    intro->setForegroundRole(QPalette::PlaceholderText);
    lay->addWidget(intro);

    m_enable = new QCheckBox(tr("&Send an automatic reply"), this);
    m_enable->setObjectName(QStringLiteral("vacationEnable"));
    lay->addWidget(m_enable);

    auto *form = new QFormLayout;
    m_subject = new QLineEdit(this);
    m_subject->setObjectName(QStringLiteral("vacationSubject"));
    m_subject->setPlaceholderText(tr("Out of the office"));
    m_subject->setMaxLength(250);
    form->addRow(tr("S&ubject:"), m_subject);
    m_text = new QPlainTextEdit(this);
    m_text->setObjectName(QStringLiteral("vacationText"));
    m_text->setPlaceholderText(tr("I'm away and will reply when I'm back."));
    m_text->setMinimumSize(420, 140);
    form->addRow(tr("&Message:"), m_text);

    const auto dateRow = [this](QCheckBox *&box, QDateEdit *&edit, const QString &label, const char *boxName,
                                const char *editName) {
        auto *row = new QHBoxLayout;
        box = new QCheckBox(label, this);
        box->setObjectName(QString::fromLatin1(boxName));
        edit = new QDateEdit(QDate::currentDate(), this);
        edit->setObjectName(QString::fromLatin1(editName));
        edit->setCalendarPopup(true);
        edit->setDisplayFormat(QStringLiteral("MMMM d, yyyy"));
        row->addWidget(box);
        row->addWidget(edit);
        row->addStretch(1);
        connect(box, &QCheckBox::toggled, this, &VacationDialog::updateEnabled);
        return row;
    };
    form->addRow(QString(), dateRow(m_hasFirst, m_first, tr("&First day:"), "vacationHasFirst", "vacationFirst"));
    form->addRow(QString(), dateRow(m_hasLast, m_last, tr("&Last day:"), "vacationHasLast", "vacationLast"));
    m_hasFirst->setToolTip(tr("Unticked, the reply starts as soon as it is saved"));
    m_hasLast->setToolTip(tr("Unticked, the reply goes on until you turn it off"));
    m_contactsOnly = new QCheckBox(tr("Only reply to people in my &Contacts"), this);
    m_contactsOnly->setObjectName(QStringLiteral("vacationContactsOnly"));
    form->addRow(QString(), m_contactsOnly);
    lay->addLayout(form);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("vacationStatus"));
    m_status->setWordWrap(true);
    lay->addWidget(m_status);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    lay->addWidget(m_buttons);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &VacationDialog::save);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_enable, &QCheckBox::toggled, this, &VacationDialog::updateEnabled);

    if (!m_session) {
        showStatus(tr("Sign in to Gmail to use the vacation responder."), true);
        setBusy(true);
        return;
    }
    setSettings(m_session->vacation());
    m_loaded = false;
    showStatus(tr("Asking Gmail for the current setting…"));
    setBusy(true);
    QPointer<VacationDialog> guard(this);
    m_session->loadVacation([guard](const zmail::VacationSettings &v, const QString &error) {
        if (!guard) {
            return;
        }
        guard->setSettings(v);
        guard->setBusy(false);
        if (error.isEmpty()) {
            guard->showStatus(tr("Now: %1").arg(v.summary()));
        } else {
            guard->showStatus(tr("Couldn't read the setting from Gmail: %1").arg(error), true);
        }
    });
}

zmail::VacationSettings VacationDialog::settings() const
{
    zmail::VacationSettings v;
    v.enabled = m_enable->isChecked();
    v.subject = m_subject->text().trimmed();
    v.text = m_text->toPlainText().trimmed();
    v.contactsOnly = m_contactsOnly->isChecked();
    v.domainOnly = m_domainOnly;
    v.firstDay = m_hasFirst->isChecked() ? m_first->date() : QDate();
    v.lastDay = m_hasLast->isChecked() ? m_last->date() : QDate();
    return v;
}

void VacationDialog::setSettings(const zmail::VacationSettings &v)
{
    m_loaded = true;
    m_enable->setChecked(v.enabled);
    m_subject->setText(v.subject);
    m_text->setPlainText(v.text);
    m_contactsOnly->setChecked(v.contactsOnly);
    m_domainOnly = v.domainOnly;
    m_hasFirst->setChecked(v.firstDay.isValid());
    m_first->setDate(v.firstDay.isValid() ? v.firstDay : QDate::currentDate());
    m_hasLast->setChecked(v.lastDay.isValid());
    m_last->setDate(v.lastDay.isValid() ? v.lastDay : QDate::currentDate().addDays(7));
    updateEnabled();
}

QString VacationDialog::statusText() const
{
    return m_status->text();
}

void VacationDialog::updateEnabled()
{
    const bool form = !m_saving && m_session;
    const bool on = form && m_enable->isChecked();
    m_enable->setEnabled(form);
    for (QWidget *w : {static_cast<QWidget *>(m_subject), static_cast<QWidget *>(m_text),
                       static_cast<QWidget *>(m_hasFirst), static_cast<QWidget *>(m_hasLast),
                       static_cast<QWidget *>(m_contactsOnly)}) {
        w->setEnabled(on);
    }
    m_first->setEnabled(on && m_hasFirst->isChecked());
    m_last->setEnabled(on && m_hasLast->isChecked());
}

void VacationDialog::setBusy(bool busy)
{
    m_saving = busy;
    m_buttons->button(QDialogButtonBox::Save)->setEnabled(!busy && m_session);
    updateEnabled();
}

void VacationDialog::showStatus(const QString &text, bool error)
{
    m_status->setText(text);
    m_status->setStyleSheet(error ? QStringLiteral("color:#b3261e") : QString());
}

void VacationDialog::save()
{
    if (!m_session || m_saving) {
        return;
    }
    const zmail::VacationSettings v = settings();
    if (v.enabled && v.subject.isEmpty() && v.text.isEmpty()) {
        showStatus(tr("Write a subject or a message for the reply."), true);
        return;
    }
    if (v.enabled && v.firstDay.isValid() && v.lastDay.isValid() && v.lastDay < v.firstDay) {
        showStatus(tr("The last day is before the first day."), true);
        return;
    }
    const bool needsConsent =
        m_session->auth() && !m_session->auth()->hasScopes(zmail::AuthManager::settingsScopes());
    showStatus(needsConsent ? tr("Waiting for you to allow zmail to change Gmail settings, in your browser…")
                            : tr("Saving to Gmail…"));
    setBusy(true);
    QPointer<VacationDialog> guard(this);
    m_session->saveVacation(v, [guard](const zmail::VacationSettings &stored, const QString &error) {
        if (!guard) {
            return;
        }
        guard->setBusy(false);
        if (!error.isEmpty()) {
            guard->showStatus(tr("Gmail didn't save it: %1").arg(error), true);
            return;
        }
        emit guard->saved(stored);
        guard->accept();
    });
}

} // namespace zmail::ui
