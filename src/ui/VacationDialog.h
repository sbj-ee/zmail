#pragma once

#include "core/Vacation.h"

#include <QDialog>
#include <QPointer>

class QCheckBox;
class QDateEdit;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;

namespace zmail {
class MailSession;
}

namespace zmail::ui {

// Settings > Vacation Responder: Gmail's automatic reply while you are away.
// The dialog shows what Google has (asked for as it opens) and Save writes
// it back. Google answers from its side, so it works with zmail closed. The
// first Save sends the browser to Google for permission to change settings.
class VacationDialog : public QDialog
{
    Q_OBJECT

public:
    explicit VacationDialog(zmail::MailSession *session, QWidget *parent = nullptr);

    zmail::VacationSettings settings() const;
    void setSettings(const zmail::VacationSettings &v);
    bool loaded() const { return m_loaded; }
    bool saving() const { return m_saving; }
    QString statusText() const;

    // What the Save button does: checks the form, then asks Google. The
    // dialog closes once Google has stored it.
    void save();

signals:
    void saved(const zmail::VacationSettings &settings);

private:
    void setBusy(bool busy);
    void showStatus(const QString &text, bool error = false);
    void updateEnabled();

    QPointer<zmail::MailSession> m_session;
    QCheckBox *m_enable = nullptr;
    QLineEdit *m_subject = nullptr;
    QPlainTextEdit *m_text = nullptr;
    QCheckBox *m_hasFirst = nullptr;
    QDateEdit *m_first = nullptr;
    QCheckBox *m_hasLast = nullptr;
    QDateEdit *m_last = nullptr;
    QCheckBox *m_contactsOnly = nullptr;
    QLabel *m_status = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
    bool m_domainOnly = false; // kept as Google has it; zmail has no box for it
    bool m_loaded = false;
    bool m_saving = false;
};

} // namespace zmail::ui
