#pragma once

#include "AppPasswordAccount.h"

#include <QObject>
#include <QPointer>
#include <functional>
#include <memory>

namespace zmail {

// The account dialog's "Test" button: log in to IMAP (LOGIN over implicit
// TLS, then LOGOUT) and SMTP (EHLO + AUTH PLAIN over implicit TLS, then QUIT)
// and report what each said. Nothing else is read or sent.
//
// The password is held only for the duration of the test, is never logged,
// and is scrubbed from any server text before that text reaches a message.
class ConnectionTester : public QObject
{
    Q_OBJECT

public:
    struct Result
    {
        bool imapOk = false;
        bool smtpOk = false;
        QString imapMessage;
        QString smtpMessage;
        bool ok() const { return imapOk && smtpOk; }
    };
    using ResultCb = std::function<void(const Result &)>;

    explicit ConnectionTester(QObject *parent = nullptr);
    ~ConnectionTester() override;

    void test(const AppPasswordAccount &account, const QString &appPassword, ResultCb cb);
    void cancel();
    bool running() const;

    void setTimeoutMs(int ms) { m_timeoutMs = ms; }
    // Tests only: talk to a local fake server without TLS.
    void setPlaintextForTests(bool on) { m_plaintext = on; }

    // IMAP quoted string (RFC 3501): backslash-escape \ and ". Refuses CR/LF/NUL.
    static bool imapQuote(const QString &s, QByteArray *out);
    static QByteArray smtpAuthPlain(const QString &user, const QString &password);

signals:
    void progress(const QString &status);

private:
    struct Run;
    void startImap();
    void startSmtp();
    void finish();

    std::shared_ptr<Run> m_run;
    int m_timeoutMs = 20000;
    bool m_plaintext = false;
};

} // namespace zmail
