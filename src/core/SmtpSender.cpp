#include "SmtpSender.h"

#include "ImapSyncEngine.h"
#include "Limits.h"
#include "Log.h"

#include <QPointer>
#include <QRegularExpression>
#include <QSslSocket>
#include <QTimer>

namespace zmail {

namespace {

// Header block, unfolded: name (lower case) -> values.
QList<QPair<QByteArray, QByteArray>> headers(const QByteArray &mime)
{
    int end = mime.indexOf("\r\n\r\n");
    if (end < 0) {
        end = mime.indexOf("\n\n");
    }
    const QByteArray head = end < 0 ? mime : mime.left(end);
    QList<QPair<QByteArray, QByteArray>> out;
    for (QByteArray line : head.split('\n')) {
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        if (!line.isEmpty() && (line.at(0) == ' ' || line.at(0) == '\t') && !out.isEmpty()) {
            out.last().second += ' ' + line.trimmed();
            continue;
        }
        const int colon = line.indexOf(':');
        if (colon > 0) {
            out.append({line.left(colon).trimmed().toLower(), line.mid(colon + 1).trimmed()});
        }
    }
    return out;
}

// "A <a@x>, \"B, Jr\" <b@y>, c@z" -> a@x, b@y, c@z
QStringList addresses(const QByteArray &value)
{
    QStringList out;
    QByteArray cur;
    bool quoted = false;
    int angle = 0;
    auto flush = [&] {
        const QString part = QString::fromUtf8(cur).trimmed();
        cur.clear();
        if (part.isEmpty()) {
            return;
        }
        const int lt = part.lastIndexOf(QLatin1Char('<'));
        const int gt = part.lastIndexOf(QLatin1Char('>'));
        const QString addr = lt >= 0 && gt > lt ? part.mid(lt + 1, gt - lt - 1).trimmed() : part;
        if (addr.contains(QLatin1Char('@'))) {
            out.append(addr);
        }
    };
    for (const char c : value) {
        if (c == '"') {
            quoted = !quoted;
        } else if (!quoted && c == '<') {
            ++angle;
        } else if (!quoted && c == '>') {
            angle = std::max(0, angle - 1);
        } else if (!quoted && angle == 0 && (c == ',' || c == ';')) {
            flush();
            continue;
        }
        cur += c;
    }
    flush();
    return out;
}

// One SMTP conversation. Deletes itself when done.
class SmtpJob : public QObject
{
public:
    enum class Step { Greeting, Ehlo, Auth, MailFrom, Rcpt, Data, Body, Quit, Done };

    SmtpJob(Sender *owner, const AppPasswordAccount &account, const QString &password, bool plaintext, int timeoutMs,
            QStringList rcpts, QByteArray payload, QString threadId, Sender::ResultCb cb)
        : QObject(owner)
        , m_owner(owner)
        , m_account(account)
        , m_auth(QByteArray('\0' + account.email.toUtf8() + '\0' + password.toUtf8()).toBase64())
        , m_rcpts(std::move(rcpts))
        , m_payload(std::move(payload))
        , m_threadId(std::move(threadId))
        , m_cb(std::move(cb))
    {
        m_timer.setSingleShot(true);
        m_timer.setInterval(timeoutMs);
        connect(&m_timer, &QTimer::timeout, this, [this] { fail(tr("The mail server stopped answering.")); });
        connect(&m_socket, &QSslSocket::readyRead, this, &SmtpJob::onRead);
        connect(&m_socket, &QSslSocket::bytesWritten, this, [this](qint64 n) {
            if (m_step == Step::Body) {
                m_written += n;
                emit m_owner->progress(std::min(m_written, qint64(m_payload.size())), m_payload.size());
                m_timer.start();
            }
        });
        connect(&m_socket, &QSslSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
            if (m_step != Step::Done && m_step != Step::Quit) {
                fail(m_socket.errorString());
            }
        });
        connect(&m_socket, &QSslSocket::disconnected, this, [this] {
            if (m_step != Step::Done && m_step != Step::Quit) {
                fail(tr("The mail server closed the connection."));
            }
        });
        m_timer.start();
        if (plaintext) {
            m_socket.connectToHost(account.smtpHost, quint16(account.smtpPort));
        } else {
            m_socket.connectToHostEncrypted(account.smtpHost, quint16(account.smtpPort));
        }
    }

private:
    void onRead()
    {
        m_buffer += m_socket.readAll();
        int nl;
        while ((nl = m_buffer.indexOf("\r\n")) >= 0) {
            const QByteArray line = m_buffer.left(nl);
            m_buffer.remove(0, nl + 2);
            m_reply += (m_reply.isEmpty() ? QByteArray() : QByteArray("\n")) + line.mid(4);
            if (line.size() >= 4 && line.at(3) == '-') {
                continue; // multi-line reply goes on
            }
            const int code = line.left(3).toInt();
            const QByteArray text = std::exchange(m_reply, {});
            step(code, text);
            if (m_step == Step::Done) {
                return;
            }
        }
    }

    void write(const QByteArray &line)
    {
        m_socket.write(line + "\r\n");
        m_timer.start();
    }

    void step(int code, const QByteArray &text)
    {
        switch (m_step) {
        case Step::Greeting:
            if (code != 220) {
                return fail(text);
            }
            m_step = Step::Ehlo;
            write("EHLO zmail");
            return;
        case Step::Ehlo:
            if (code != 250) {
                return fail(text);
            }
            m_step = Step::Auth;
            write("AUTH PLAIN " + m_auth); // never logged
            return;
        case Step::Auth:
            m_auth.fill('\0');
            m_auth.clear();
            if (code != 235) {
                return fail(tr("Gmail didn't accept the app password (%1). Check it under Settings, or make a new one.")
                                .arg(QString::fromUtf8(text).left(200)));
            }
            m_step = Step::MailFrom;
            write("MAIL FROM:<" + m_account.email.toUtf8() + "> SIZE=" + QByteArray::number(m_payload.size()));
            return;
        case Step::MailFrom:
            if (code != 250) {
                return fail(text);
            }
            m_step = Step::Rcpt;
            write("RCPT TO:<" + m_rcpts.at(m_rcptIndex).toUtf8() + ">");
            return;
        case Step::Rcpt:
            if (code != 250 && code != 251) {
                return fail(tr("%1 was refused: %2").arg(m_rcpts.at(m_rcptIndex), QString::fromUtf8(text).left(200)));
            }
            if (++m_rcptIndex < m_rcpts.size()) {
                write("RCPT TO:<" + m_rcpts.at(m_rcptIndex).toUtf8() + ">");
                return;
            }
            m_step = Step::Data;
            write("DATA");
            return;
        case Step::Data:
            if (code != 354) {
                return fail(text);
            }
            m_step = Step::Body;
            m_socket.write(m_payload);
            m_timer.start();
            return;
        case Step::Body: {
            if (code != 250) {
                return fail(text);
            }
            m_step = Step::Quit;
            write("QUIT");
            Sender::Result r;
            r.ok = true;
            r.threadId = m_threadId;
            r.transport = Sender::Transport::Simple;
            finish(r);
            return;
        }
        case Step::Quit:
        case Step::Done:
            return;
        }
    }

    void fail(const QString &why)
    {
        if (m_step == Step::Done) {
            return;
        }
        Sender::Result r;
        r.err.isError = true;
        r.err.reason = QStringLiteral("smtp");
        r.err.message = why.isEmpty() ? tr("Sending failed.") : why;
        finish(r);
    }
    void fail(const QByteArray &text) { fail(QString::fromUtf8(text).left(300)); }

    void finish(const Sender::Result &r)
    {
        const bool quitting = m_step == Step::Quit;
        m_step = Step::Done;
        m_timer.stop();
        m_auth.fill('\0');
        const auto cb = std::exchange(m_cb, nullptr);
        if (quitting) {
            m_socket.disconnectFromHost();
        } else {
            m_socket.abort();
        }
        deleteLater();
        if (cb) {
            cb(r);
        }
    }

    Sender *m_owner;
    AppPasswordAccount m_account;
    QByteArray m_auth;
    QStringList m_rcpts;
    int m_rcptIndex = 0;
    QByteArray m_payload;
    QString m_threadId;
    Sender::ResultCb m_cb;
    QSslSocket m_socket;
    QTimer m_timer;
    QByteArray m_buffer;
    QByteArray m_reply;
    qint64 m_written = 0;
    Step m_step = Step::Greeting;
};

Sender::Result failure(const QString &message, const QString &reason = QStringLiteral("smtp"))
{
    Sender::Result r;
    r.err.isError = true;
    r.err.reason = reason;
    r.err.message = message;
    return r;
}

} // namespace

SmtpSender::SmtpSender(const AppPasswordAccount &account, PasswordProvider password, ImapSyncEngine *imap,
                       QObject *parent)
    : Sender(nullptr, parent)
    , m_account(account)
    , m_password(std::move(password))
    , m_imap(imap)
{
}

QStringList SmtpSender::envelopeRecipients(const QByteArray &mime)
{
    QStringList out;
    for (const auto &h : headers(mime)) {
        if (h.first == "to" || h.first == "cc" || h.first == "bcc") {
            for (const QString &a : addresses(h.second)) {
                bool dup = false;
                for (const QString &o : out) {
                    dup = dup || o.compare(a, Qt::CaseInsensitive) == 0;
                }
                if (!dup) {
                    out.append(a);
                }
            }
        }
    }
    return out;
}

QByteArray SmtpSender::dataPayload(const QByteArray &mime)
{
    QByteArray out;
    out.reserve(mime.size() + mime.size() / 50 + 8);
    bool inHead = true;
    bool skipping = false; // inside a (folded) Bcc header
    const QList<QByteArray> lines = mime.split('\n');
    for (int i = 0; i < lines.size(); ++i) {
        QByteArray line = lines.at(i);
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        if (i == lines.size() - 1 && line.isEmpty()) {
            break; // the mime's own trailing newline
        }
        if (inHead) {
            if (line.isEmpty()) {
                inHead = false;
                skipping = false;
            } else if (line.at(0) == ' ' || line.at(0) == '\t') {
                if (skipping) {
                    continue;
                }
            } else {
                skipping = line.left(4).toLower() == "bcc:";
                if (skipping) {
                    continue;
                }
            }
        }
        if (line.startsWith('.')) {
            out += '.';
        }
        out += line + "\r\n";
    }
    return out + ".\r\n";
}

void SmtpSender::send(const QByteArray &mime, const QString &threadId, ResultCb cb)
{
    if (mime.size() > limits::kSendLimitBytes) {
        Result r = failure(tr("This message is too large for Gmail (over 25 MB). Remove an attachment or share it "
                              "as a link instead."),
                           QStringLiteral("messageTooLarge"));
        r.blocked = true;
        QTimer::singleShot(0, this, [cb, r] { cb(r); });
        return;
    }
    const QStringList rcpts = envelopeRecipients(mime);
    if (rcpts.isEmpty()) {
        QTimer::singleShot(0, this, [cb] { cb(failure(tr("The message has no recipients."))); });
        return;
    }
    QByteArray payload = dataPayload(mime);
    QPointer<SmtpSender> self(this);
    m_password([self, rcpts, payload, threadId, cb](bool ok, const QString &password, const QString &error) {
        if (!self) {
            return;
        }
        if (!ok || password.isEmpty()) {
            cb(failure(error.isEmpty() ? tr("The app password isn't in the system keyring.") : error));
            return;
        }
        QPointer<ImapSyncEngine> imap = self->m_imap;
        new SmtpJob(self, self->m_account, password, self->m_plaintext, self->m_timeoutMs, rcpts, payload, threadId,
                    [cb, imap](const Result &r) {
                        if (r.ok && imap) {
                            // Gmail filed it in Sent Mail: read that folder now.
                            imap->ensureLabel(QStringLiteral("SENT"));
                            imap->pollNow(true);
                        }
                        cb(r);
                    });
    });
}

void SmtpSender::saveDraft(const QByteArray &mime, const QString &threadId, const QString &existingDraftId,
                           ResultCb cb)
{
    if (!m_imap) {
        QTimer::singleShot(0, this, [cb] { cb(failure(tr("Not connected to the mail server."))); });
        return;
    }
    m_imap->appendDraft(mime, existingDraftId,
                        [cb, threadId](bool ok, const QString &id, const QString &thread, const QString &error) {
                            if (!ok) {
                                cb(failure(error));
                                return;
                            }
                            Result r;
                            r.ok = true;
                            r.draftId = id;
                            r.messageId = id;
                            r.threadId = thread.isEmpty() ? threadId : thread;
                            r.transport = Transport::Simple;
                            cb(r);
                        });
}

void SmtpSender::deleteDraft(const QString &draftId, ResultCb cb)
{
    if (!m_imap) {
        QTimer::singleShot(0, this, [cb] { cb(failure(tr("Not connected to the mail server."))); });
        return;
    }
    m_imap->deleteDraft(draftId, [cb](bool ok, const QString &error) {
        if (!ok) {
            cb(failure(error));
            return;
        }
        Result r;
        r.ok = true;
        cb(r);
    });
}

} // namespace zmail
