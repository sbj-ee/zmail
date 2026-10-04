#pragma once

#include <QDialog>

class QLabel;
class QPushButton;
class QStackedWidget;
class QLineEdit;

namespace zmail {
class MailSession;
}

namespace zmail::ui {

// First-run "Connect your Gmail" dialog:
//  page 0  no client file yet: the Google Cloud steps + "Choose client file…"
//  page 1  client ready: "Sign in with Google" (+ permissions warning/fix)
//  page 2  waiting for the browser (shows the URL in case it didn't open)
// Accepts once the session is signed in.
class ConnectDialog : public QDialog
{
    Q_OBJECT

public:
    enum Page { SetupPage = 0, SignInPage = 1, WaitingPage = 2 };

    explicit ConnectDialog(zmail::MailSession *session, QWidget *parent = nullptr);

    Page page() const;
    // Shown above the sign-in button (e.g. "Google no longer accepts...").
    void setNotice(const QString &text, bool error = true);
    // What "Choose client file…" does after the file picker; public for tests.
    bool installClientFile(const QString &path);

    static QString setupStepsHtml();

private:
    void refresh();
    void chooseClientFile();
    QWidget *buildSetupPage();
    QWidget *buildSignInPage();
    QWidget *buildWaitingPage();

    zmail::MailSession *m_session;
    QStackedWidget *m_stack = nullptr;
    QLabel *m_setupError = nullptr;
    QLabel *m_clientLabel = nullptr;
    QLabel *m_permWarning = nullptr;
    QPushButton *m_fixPerms = nullptr;
    QLabel *m_notice = nullptr;
    QPushButton *m_signIn = nullptr;
    QLineEdit *m_authUrl = nullptr;
};

} // namespace zmail::ui
