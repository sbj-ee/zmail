#include "ConnectionTester.h"

#include <QSslSocket>
#include <QTimer>

namespace zmail {

struct ConnectionTester::Run
{
    AppPasswordAccount account;
    QString password;
    ResultCb cb;
    Result result;
    QPointer<QSslSocket> socket;
    QTimer timer;
    QByteArray buffer;
    int step = 0;
    QStringList smtpLines;

    QString scrub(QString text) const
    {
        if (!password.isEmpty()) {
            text.replace(password, QStringLiteral("\u2022\u2022\u2022\u2022"), Qt::CaseInsensitive);
        }
        return text.trimmed();
    }
};

namespace {
// Let a socket flush its LOGOUT / QUIT, then go (or be cut off after 2 s).
void retire(QSslSocket *s, QObject *owner)
{
    s->disconnect(owner);
    QObject::connect(s, &QAbstractSocket::disconnected, s, &QObject::deleteLater);
    QTimer::singleShot(2000, s, [s] {
        s->abort();
        s->deleteLater();
    });
    if (s->state() == QAbstractSocket::UnconnectedState) {
        s->deleteLater();
    }
}

QString describeSocketError(QAbstractSocket *s, const QString &host, int port)
{
    return ConnectionTester::tr("Couldn't connect to %1:%2 (%3).").arg(host).arg(port).arg(s->errorString());
}
} // namespace

ConnectionTester::ConnectionTester(QObject *parent)
    : QObject(parent)
{
}

ConnectionTester::~ConnectionTester()
{
    cancel();
}

bool ConnectionTester::running() const
{
    return bool(m_run);
}

void ConnectionTester::cancel()
{
    if (!m_run) {
        return;
    }
    auto run = std::move(m_run);
    run->password.fill(QLatin1Char('\0'));
    run->timer.stop();
    if (run->socket) {
        run->socket->disconnect(this);
        run->socket->abort();
        run->socket->deleteLater();
    }
}

bool ConnectionTester::imapQuote(const QString &s, QByteArray *out)
{
    QByteArray q = "\"";
    for (const char c : s.toUtf8()) {
        if (c == '\r' || c == '\n' || c == '\0') {
            return false;
        }
        if (c == '\\' || c == '"') {
            q.append('\\');
        }
        q.append(c);
    }
    q.append('"');
    *out = q;
    return true;
}

QByteArray ConnectionTester::smtpAuthPlain(const QString &user, const QString &password)
{
    QByteArray raw;
    raw.append('\0');
    raw.append(user.toUtf8());
    raw.append('\0');
    raw.append(password.toUtf8());
    return raw.toBase64();
}

void ConnectionTester::test(const AppPasswordAccount &account, const QString &appPassword, ResultCb cb)
{
    cancel();
    m_run = std::make_shared<Run>();
    m_run->account = account;
    m_run->account.email = account.email.trimmed();
    m_run->password = appPassword;
    m_run->cb = std::move(cb);
    m_run->timer.setSingleShot(true);
    connect(&m_run->timer, &QTimer::timeout, this, [this] {
        if (!m_run) {
            return;
        }
        if (m_run->step < 100) {
            m_run->result.imapMessage = tr("IMAP: no answer from %1 within %2 s.")
                                            .arg(m_run->account.imapHost)
                                            .arg(m_timeoutMs / 1000);
            m_run->result.smtpMessage = tr("SMTP: not tried.");
            finish();
        } else {
            m_run->result.smtpMessage = tr("SMTP: no answer from %1 within %2 s.")
                                            .arg(m_run->account.smtpHost)
                                            .arg(m_timeoutMs / 1000);
            finish();
        }
    });
    startImap();
}

void ConnectionTester::startImap()
{
    auto run = m_run;
    QByteArray user;
    QByteArray pass;
    if (!imapQuote(run->account.email, &user) || !imapQuote(run->password, &pass)) {
        run->result.imapMessage = tr("IMAP: the address or password contains a line break.");
        run->result.smtpMessage = tr("SMTP: not tried.");
        finish();
        return;
    }
    emit progress(tr("Connecting to %1\u2026").arg(run->account.imapHost));
    auto *sock = new QSslSocket(this);
    run->socket = sock;
    run->step = 0;
    run->buffer.clear();
    run->timer.start(m_timeoutMs);

    connect(sock, &QAbstractSocket::errorOccurred, this, [this, run, sock](QAbstractSocket::SocketError) {
        if (m_run != run || run->step >= 100) {
            return;
        }
        if (run->step >= 2) { // LOGOUT done; Gmail closes the connection
            return;
        }
        run->result.imapMessage = QStringLiteral("IMAP: ")
            + describeSocketError(sock, run->account.imapHost, run->account.imapPort);
        run->result.smtpMessage = tr("SMTP: not tried.");
        finish();
    });
    connect(sock, &QIODevice::readyRead, this, [this, run, sock, user, pass] {
        if (m_run != run) {
            return;
        }
        run->buffer += sock->readAll();
        int nl;
        while ((nl = run->buffer.indexOf("\r\n")) >= 0) {
            const QByteArray line = run->buffer.left(nl);
            run->buffer.remove(0, nl + 2);
            const QString text = run->scrub(QString::fromUtf8(line));
            if (run->step == 0) {
                if (!line.startsWith("* OK")) {
                    run->result.imapMessage = tr("IMAP: unexpected greeting: %1").arg(text.left(200));
                    run->result.smtpMessage = tr("SMTP: not tried.");
                    finish();
                    return;
                }
                emit progress(tr("Signing in to %1\u2026").arg(run->account.imapHost));
                run->step = 1;
                sock->write("a1 LOGIN " + user + ' ' + pass + "\r\n");
            } else if (run->step == 1 && line.startsWith("a1 ")) {
                if (line.startsWith("a1 OK")) {
                    run->result.imapOk = true;
                    run->result.imapMessage = tr("IMAP: signed in to %1.").arg(run->account.imapHost);
                    run->step = 2;
                    sock->write("a2 LOGOUT\r\n");
                    sock->disconnectFromHost();
                    startSmtp();
                    return;
                }
                const bool auth = line.contains("AUTHENTICATIONFAILED") || line.contains("Invalid credentials");
                run->result.imapMessage = auth
                    ? tr("IMAP: Google refused the address or app password. Check both, and that 2-Step "
                         "Verification is on (app passwords need it).")
                    : tr("IMAP: sign-in failed: %1").arg(text.mid(3).left(200));
                run->result.smtpMessage = tr("SMTP: not tried.");
                finish();
                return;
            }
        }
    });
    if (m_plaintext) {
        sock->connectToHost(run->account.imapHost, quint16(run->account.imapPort));
    } else {
        sock->connectToHostEncrypted(run->account.imapHost, quint16(run->account.imapPort));
    }
}

void ConnectionTester::startSmtp()
{
    auto run = m_run;
    if (run->socket) {
        retire(run->socket, this);
        run->socket = nullptr;
    }
    emit progress(tr("Connecting to %1\u2026").arg(run->account.smtpHost));
    auto *sock = new QSslSocket(this);
    run->socket = sock;
    run->step = 100;
    run->buffer.clear();
    run->smtpLines.clear();
    run->timer.start(m_timeoutMs);
    const QByteArray auth = smtpAuthPlain(run->account.email, run->password);

    connect(sock, &QAbstractSocket::errorOccurred, this, [this, run, sock](QAbstractSocket::SocketError) {
        if (m_run != run || run->step >= 104) {
            return;
        }
        run->result.smtpMessage = QStringLiteral("SMTP: ")
            + describeSocketError(sock, run->account.smtpHost, run->account.smtpPort);
        finish();
    });
    connect(sock, &QIODevice::readyRead, this, [this, run, sock, auth] {
        if (m_run != run) {
            return;
        }
        run->buffer += sock->readAll();
        int nl;
        while ((nl = run->buffer.indexOf("\r\n")) >= 0) {
            const QByteArray line = run->buffer.left(nl);
            run->buffer.remove(0, nl + 2);
            if (line.size() >= 4 && line.at(3) == '-') {
                continue; // multi-line reply; act on its last line
            }
            const int code = line.left(3).toInt();
            const QString text = run->scrub(QString::fromUtf8(line.mid(4)));
            auto fail = [&](const QString &msg) {
                run->result.smtpMessage = msg;
                finish();
            };
            switch (run->step) {
            case 100:
                if (code != 220) {
                    return fail(tr("SMTP: unexpected greeting: %1").arg(text.left(200)));
                }
                run->step = 101;
                sock->write("EHLO zmail\r\n");
                break;
            case 101:
                if (code != 250) {
                    return fail(tr("SMTP: EHLO refused: %1").arg(text.left(200)));
                }
                emit progress(tr("Signing in to %1\u2026").arg(run->account.smtpHost));
                run->step = 102;
                sock->write("AUTH PLAIN " + auth + "\r\n");
                break;
            case 102:
                if (code == 235) {
                    run->result.smtpOk = true;
                    run->result.smtpMessage = tr("SMTP: signed in to %1.").arg(run->account.smtpHost);
                    run->step = 104;
                    sock->write("QUIT\r\n");
                    sock->disconnectFromHost();
                    finish();
                    return;
                }
                return fail(code == 535
                                ? tr("SMTP: Google refused the address or app password.")
                                : tr("SMTP: sign-in failed: %1 %2").arg(code).arg(text.left(200)));
            default:
                break;
            }
        }
    });
    if (m_plaintext) {
        sock->connectToHost(run->account.smtpHost, quint16(run->account.smtpPort));
    } else {
        sock->connectToHostEncrypted(run->account.smtpHost, quint16(run->account.smtpPort));
    }
}

void ConnectionTester::finish()
{
    auto run = m_run;
    if (!run) {
        return;
    }
    const Result result = run->result;
    const ResultCb cb = run->cb;
    // Let the socket flush LOGOUT / QUIT before it goes.
    if (run->socket) {
        retire(run->socket, this);
        run->socket = nullptr;
    }
    m_run.reset();
    run->password.fill(QLatin1Char('\0'));
    run->timer.stop();
    emit progress(result.ok() ? tr("Connected.") : tr("Test failed."));
    if (cb) {
        cb(result);
    }
}

} // namespace zmail
