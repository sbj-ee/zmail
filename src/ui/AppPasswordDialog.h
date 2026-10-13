#pragma once

#include "core/AppPasswordAccount.h"

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QGroupBox;

namespace zmail {
class ConnectionTester;
class TokenStore;
}

namespace zmail::ui {

// "Gmail (app password)" account: address + 16-letter app password, with the
// IMAP/SMTP servers under "Servers". Test logs in to both; Save is enabled
// only after a passing Test of exactly what's on screen. Save puts the app
// password in the system keyring (TokenStore) and the rest in QSettings.
// No plaintext fallback: if the keyring write fails, nothing is saved.
class AppPasswordDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AppPasswordDialog(zmail::TokenStore *store, QWidget *parent = nullptr);
    ~AppPasswordDialog() override;

    zmail::AppPasswordAccount account() const;
    zmail::ConnectionTester *tester() const { return m_tester; }
    QString statusText() const;

    void runTest();  // what Test does
    void save();     // what Save does

    static QString instructionsHtml();

signals:
    void saved(const zmail::AppPasswordAccount &account);

private:
    void invalidate();
    void setStatus(const QString &text, int kind); // 0 neutral, 1 ok, 2 error

    zmail::TokenStore *m_store;
    zmail::ConnectionTester *m_tester;
    QLineEdit *m_displayName = nullptr;
    QLineEdit *m_email = nullptr;
    QLineEdit *m_password = nullptr;
    QGroupBox *m_servers = nullptr;
    QLineEdit *m_imapHost = nullptr;
    QSpinBox *m_imapPort = nullptr;
    QLineEdit *m_smtpHost = nullptr;
    QSpinBox *m_smtpPort = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_test = nullptr;
    QPushButton *m_save = nullptr;
    bool m_tested = false;
};

} // namespace zmail::ui
