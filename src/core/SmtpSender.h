#pragma once

#include "AppPasswordAccount.h"
#include "Sender.h"

#include <QPointer>
#include <functional>

namespace zmail {

class ImapSyncEngine;

// Sender for "Gmail (app password)" accounts.
//   send       SMTP over implicit TLS (smtp.gmail.com:465): EHLO, AUTH PLAIN
//              with the app password, MAIL FROM, RCPT TO for every To / Cc /
//              Bcc address, DATA (dot-stuffed, the Bcc header left out).
//              Gmail files the message in Sent Mail itself.
//   saveDraft  IMAP APPEND to [Gmail]/Drafts through the account's
//   deleteDraft ImapSyncEngine (one connection, commands in order).
// The app password comes from the keyring for each send and is never logged
// or kept; the AUTH line is the only place it goes.
class SmtpSender : public Sender
{
    Q_OBJECT

public:
    using PasswordProvider =
        std::function<void(std::function<void(bool ok, const QString &password, const QString &error)>)>;

    SmtpSender(const AppPasswordAccount &account, PasswordProvider password, ImapSyncEngine *imap,
               QObject *parent = nullptr);

    void setPlaintextForTests(bool on) { m_plaintext = on; }
    void setTimeoutMs(int ms) { m_timeoutMs = ms; }
    // Temporary refusals before DATA are retried with backoff (ServerLimits).
    void setMaxRetries(int n) { m_maxRetries = n; }
    void setBackoffScaleForTests(double scale) { m_backoffScale = scale; }
    int retries() const { return m_retries; } // retries made so far

    void send(const QByteArray &mime, const QString &threadId, ResultCb cb) override;
    void saveDraft(const QByteArray &mime, const QString &threadId, const QString &existingDraftId,
                   ResultCb cb) override;
    void deleteDraft(const QString &draftId, ResultCb cb) override;

    // Public for tests.
    // Every address in To, Cc and Bcc (unfolded, de-duplicated, in order).
    static QStringList envelopeRecipients(const QByteArray &mime);
    // The message as sent in DATA: Bcc header removed, bare LF -> CRLF,
    // lines starting with "." doubled, ending in CRLF . CRLF.
    static QByteArray dataPayload(const QByteArray &mime);

signals:
    void retrying(int inMs, const QString &why);

private:
    void attempt(const QStringList &rcpts, const QByteArray &payload, const QString &threadId, int tries, ResultCb cb);

    int m_maxRetries = 2;
    double m_backoffScale = 1.0;
    int m_retries = 0;
    AppPasswordAccount m_account;
    PasswordProvider m_password;
    QPointer<ImapSyncEngine> m_imap;
    bool m_plaintext = false;
    int m_timeoutMs = 120000;
};

} // namespace zmail
