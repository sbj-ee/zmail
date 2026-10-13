#include "ImapClient.h"

#include <QSslSocket>
#include <QTimer>

namespace zmail {

namespace {
bool isAtomChar(char c)
{
    return c > ' ' && c != '(' && c != ')' && c != '{' && c != '"' && c != 0x7f;
}
} // namespace

const ImapValue *ImapValue::get(const QByteArray &key) const
{
    for (int i = 0; i + 1 < items.size(); i += 2) {
        if (items.at(i).type == Type::Atom && items.at(i).text.compare(key, Qt::CaseInsensitive) == 0) {
            return &items.at(i + 1);
        }
    }
    return nullptr;
}

QByteArray ImapUntagged::code() const
{
    for (const ImapValue &v : values) {
        if (v.type == ImapValue::Type::Atom && v.text.startsWith('[')) {
            QByteArray c = v.text.mid(1);
            if (c.endsWith(']')) {
                c.chop(1);
            }
            return c;
        }
    }
    return {};
}

ImapClient::ImapClient(QObject *parent)
    : QObject(parent)
    , m_timer(new QTimer(this))
{
    qRegisterMetaType<zmail::ImapUntagged>();
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, [this] { fail(tr("The IMAP server stopped answering.")); });
}

ImapClient::~ImapClient()
{
    m_greeting = nullptr;
    m_queue.clear();
    m_current = {};
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
        delete m_socket;
    }
}

bool ImapClient::isOpen() const
{
    return m_socket && m_socket->state() == QAbstractSocket::ConnectedState && !m_greeting;
}

bool ImapClient::hasCapability(const QByteArray &cap) const
{
    for (const QByteArray &c : m_caps) {
        if (c.compare(cap, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

QByteArray ImapClient::quote(const QString &s)
{
    QByteArray q = "\"";
    for (const char c : s.toUtf8()) {
        if (c == '\r' || c == '\n' || c == '\0') {
            continue;
        }
        if (c == '\\' || c == '"') {
            q.append('\\');
        }
        q.append(c);
    }
    q.append('"');
    return q;
}

QString ImapClient::decodeMailboxName(const QByteArray &in)
{
    QString out;
    for (int i = 0; i < in.size(); ++i) {
        if (in.at(i) != '&') {
            out.append(QLatin1Char(in.at(i)));
            continue;
        }
        const int end = in.indexOf('-', i + 1);
        if (end < 0) {
            out.append(QString::fromLatin1(in.mid(i)));
            break;
        }
        if (end == i + 1) {
            out.append(QLatin1Char('&'));
        } else {
            QByteArray b64 = in.mid(i + 1, end - i - 1);
            b64.replace(',', '/');
            const QByteArray utf16 = QByteArray::fromBase64(b64);
            for (int k = 0; k + 1 < utf16.size(); k += 2) {
                out.append(QChar(ushort((uchar(utf16.at(k)) << 8) | uchar(utf16.at(k + 1)))));
            }
        }
        i = end;
    }
    return out;
}

QByteArray ImapClient::encodeMailboxName(const QString &name)
{
    QByteArray out;
    QByteArray pending; // UTF-16BE run
    auto flush = [&] {
        if (pending.isEmpty()) {
            return;
        }
        QByteArray b64 = pending.toBase64(QByteArray::OmitTrailingEquals);
        b64.replace('/', ',');
        out += '&' + b64 + '-';
        pending.clear();
    };
    for (const QChar c : name) {
        const ushort u = c.unicode();
        if (u >= 0x20 && u <= 0x7e) {
            flush();
            out += u == '&' ? QByteArray("&-") : QByteArray(1, char(u));
        } else {
            pending.append(char(u >> 8));
            pending.append(char(u & 0xff));
        }
    }
    flush();
    return out;
}

bool ImapClient::parseValues(const QByteArray &d, int &pos, QList<ImapValue> &out, int depth)
{
    if (depth > 50) {
        return false;
    }
    while (pos < d.size()) {
        const char c = d.at(pos);
        if (c == ' ' || c == '\r' || c == '\n') {
            ++pos;
        } else if (c == ')') {
            return depth > 0;
        } else if (c == '(') {
            ++pos;
            ImapValue list;
            list.type = ImapValue::Type::List;
            if (!parseValues(d, pos, list.items, depth + 1)) {
                out.append(list);
                return false;
            }
            ++pos; // ')'
            out.append(list);
        } else if (c == '"') {
            ImapValue v;
            v.type = ImapValue::Type::String;
            ++pos;
            while (pos < d.size() && d.at(pos) != '"') {
                if (d.at(pos) == '\\' && pos + 1 < d.size()) {
                    ++pos;
                }
                v.text.append(d.at(pos++));
            }
            if (pos >= d.size()) {
                return false;
            }
            ++pos;
            out.append(v);
        } else if (c == '{') {
            const int close = d.indexOf('}', pos);
            if (close < 0) {
                return false;
            }
            QByteArray n = d.mid(pos + 1, close - pos - 1);
            if (n.endsWith('+')) {
                n.chop(1);
            }
            bool ok = false;
            const int len = n.toInt(&ok);
            int start = close + 1;
            if (d.mid(start, 2) == "\r\n") {
                start += 2;
            }
            if (!ok || len < 0 || start + len > d.size()) {
                return false;
            }
            ImapValue v;
            v.type = ImapValue::Type::String;
            v.text = d.mid(start, len);
            pos = start + len;
            out.append(v);
        } else {
            ImapValue v;
            v.type = ImapValue::Type::Atom;
            while (pos < d.size()) {
                const char a = d.at(pos);
                if (a == '[') {
                    // A section or response code, spaces and parentheses included.
                    const int close = d.indexOf(']', pos);
                    if (close < 0) {
                        v.text.append(d.mid(pos));
                        pos = d.size();
                        break;
                    }
                    v.text.append(d.mid(pos, close - pos + 1));
                    pos = close + 1;
                } else if (isAtomChar(a)) {
                    v.text.append(a);
                    ++pos;
                } else {
                    break;
                }
            }
            if (v.text.isEmpty()) {
                ++pos; // stray character
                continue;
            }
            if (v.text == "NIL") {
                v.type = ImapValue::Type::Nil;
                v.text.clear();
            }
            out.append(v);
        }
    }
    return depth == 0;
}

void ImapClient::open(const QString &host, int port, const QString &user, const QString &password,
                      std::function<void(bool ok, const QString &error)> done)
{
    close();
    auto *sock = new QSslSocket(this);
    m_socket = sock;
    m_buffer.clear();
    m_caps.clear();
    m_selected.clear();
    auto doneShared = std::make_shared<std::function<void(bool, const QString &)>>(std::move(done));
    auto finish = [doneShared](bool ok, const QString &err) {
        if (*doneShared) {
            auto d = std::exchange(*doneShared, nullptr);
            d(ok, err);
        }
    };
    const QByteArray login = "LOGIN " + quote(user) + ' ' + quote(password);
    m_greeting = [this, finish, login](const QByteArray &line) {
        m_greeting = nullptr;
        if (!line.startsWith("* OK") && !line.startsWith("* PREAUTH")) {
            finish(false, tr("Unexpected IMAP greeting."));
            fail(tr("Unexpected IMAP greeting."));
            return;
        }
        command(login, [this, finish](const Reply &r) {
            if (!r.ok) {
                const bool auth = r.text.contains(QLatin1String("AUTHENTICATIONFAILED"))
                    || r.text.contains(QLatin1String("Invalid credentials"));
                finish(false, auth ? tr("Google refused the app password. It may have been revoked: make a new "
                                        "one and enter it in the account dialog.")
                                   : tr("IMAP sign-in failed: %1").arg(r.text.left(200)));
                return;
            }
            command("CAPABILITY", [finish](const Reply &c) {
                finish(c.ok, c.ok ? QString() : tr("IMAP CAPABILITY failed: %1").arg(c.text.left(200)));
            });
        });
    };
    connect(sock, &QIODevice::readyRead, this, &ImapClient::onReadyRead);
    connect(sock, &QAbstractSocket::errorOccurred, this, [this, sock, finish](QAbstractSocket::SocketError) {
        const QString why = tr("IMAP connection error: %1").arg(sock->errorString());
        finish(false, why);
        fail(why);
    });
    connect(sock, &QAbstractSocket::disconnected, this, [this, finish] {
        finish(false, tr("The IMAP server closed the connection."));
        fail(tr("The IMAP server closed the connection."));
    });
    m_timer->start(m_timeoutMs);
    if (m_plaintext) {
        sock->connectToHost(host, quint16(port));
    } else {
        sock->connectToHostEncrypted(host, quint16(port));
    }
}

void ImapClient::close()
{
    m_greeting = nullptr;
    m_timer->stop();
    if (m_socket) {
        QSslSocket *s = m_socket;
        s->disconnect(this);
        if (s->state() == QAbstractSocket::ConnectedState) {
            s->write(m_idleState == IdleState::On ? "DONE\r\nzq LOGOUT\r\n" : "zq LOGOUT\r\n");
            s->disconnectFromHost();
        }
        connect(s, &QAbstractSocket::disconnected, s, &QObject::deleteLater);
        QTimer::singleShot(2000, s, [s] {
            s->abort();
            s->deleteLater();
        });
        m_socket = nullptr;
    }
    const auto queue = std::exchange(m_queue, {});
    const Pending current = std::exchange(m_current, {});
    m_inFlight = false;
    m_idleState = IdleState::Off;
    m_collected.clear();
    Reply lost;
    lost.text = tr("Disconnected.");
    if (current.cb) {
        current.cb(lost);
    }
    for (const Pending &p : queue) {
        if (p.cb) {
            p.cb(lost);
        }
    }
}

void ImapClient::fail(const QString &reason)
{
    if (!m_socket) {
        return;
    }
    QSslSocket *s = m_socket;
    m_socket = nullptr;
    s->disconnect(this);
    s->abort();
    s->deleteLater();
    m_greeting = nullptr;
    m_timer->stop();
    const auto queue = std::exchange(m_queue, {});
    const Pending current = std::exchange(m_current, {});
    m_inFlight = false;
    m_idleState = IdleState::Off;
    m_collected.clear();
    Reply lost;
    lost.text = reason;
    if (current.cb) {
        current.cb(lost);
    }
    for (const Pending &p : queue) {
        if (p.cb) {
            p.cb(lost);
        }
    }
    emit connectionLost(reason);
}

void ImapClient::command(const QByteArray &cmd, ReplyCb cb)
{
    if (!m_socket) {
        Reply lost;
        lost.text = tr("Not connected.");
        if (cb) {
            QTimer::singleShot(0, this, [cb, lost] { cb(lost); });
        }
        return;
    }
    m_queue.enqueue({cmd, std::move(cb)});
    if (m_idleState == IdleState::On) {
        m_idleState = IdleState::Stopping;
        m_socket->write("DONE\r\n");
        m_timer->start(m_timeoutMs);
        return;
    }
    sendNext();
}

void ImapClient::idle()
{
    if (!m_socket || m_greeting || m_inFlight || !m_queue.isEmpty() || m_idleState != IdleState::Off
        || !hasCapability("IDLE")) {
        return;
    }
    m_idleTag = "zi" + QByteArray::number(++m_tagCounter);
    m_idleState = IdleState::Starting;
    m_socket->write(m_idleTag + " IDLE\r\n");
    m_timer->start(m_timeoutMs);
}

void ImapClient::sendNext()
{
    if (!m_socket || m_inFlight || m_idleState != IdleState::Off || m_queue.isEmpty()) {
        return;
    }
    m_current = m_queue.dequeue();
    m_currentTag = "z" + QByteArray::number(++m_tagCounter);
    m_inFlight = true;
    m_collected.clear();
    m_socket->write(m_currentTag + ' ' + m_current.cmd + "\r\n");
    m_timer->start(m_timeoutMs);
}

bool ImapClient::takeResponse(QByteArray &out)
{
    int p = 0;
    for (;;) {
        const int crlf = m_buffer.indexOf("\r\n", p);
        if (crlf < 0) {
            return false;
        }
        const QByteArray line = m_buffer.mid(p, crlf - p);
        if (line.endsWith('}')) {
            const int open = line.lastIndexOf('{');
            QByteArray n = line.mid(open + 1, line.size() - open - 2);
            if (n.endsWith('+')) {
                n.chop(1);
            }
            bool ok = false;
            const qint64 len = n.toLongLong(&ok);
            if (open >= 0 && ok && len >= 0) {
                if (m_buffer.size() < crlf + 2 + len) {
                    return false;
                }
                p = int(crlf + 2 + len);
                continue;
            }
        }
        out = m_buffer.left(crlf + 2);
        m_buffer.remove(0, crlf + 2);
        return true;
    }
}

void ImapClient::onReadyRead()
{
    if (!m_socket) {
        return;
    }
    m_buffer += m_socket->readAll();
    QByteArray resp;
    QPointer<ImapClient> self(this);
    while (self && m_socket && takeResponse(resp)) {
        handleResponse(resp);
    }
}

void ImapClient::handleResponse(const QByteArray &respIn)
{
    QByteArray resp = respIn;
    if (resp.endsWith("\r\n")) {
        resp.chop(2);
    }
    if (m_greeting) {
        m_timer->stop();
        m_greeting(resp);
        return;
    }
    if (resp.startsWith('+')) {
        if (m_idleState == IdleState::Starting) {
            m_timer->stop(); // idling has no deadline; the engine re-IDLEs before Gmail's 29 min
            if (!m_queue.isEmpty()) {
                m_idleState = IdleState::Stopping;
                m_socket->write("DONE\r\n");
                m_timer->start(m_timeoutMs);
            } else {
                m_idleState = IdleState::On;
            }
        }
        return;
    }
    if (resp.startsWith("* ")) {
        ImapUntagged u;
        u.raw = resp.left(resp.indexOf("\r\n") >= 0 ? resp.indexOf("\r\n") : resp.size());
        int pos = 2;
        parseValues(resp, pos, u.values);
        if (!u.values.isEmpty()) {
            bool numeric = false;
            const qint64 n = u.values.at(0).text.toLongLong(&numeric);
            if (numeric && u.values.size() > 1) {
                u.number = n;
                u.keyword = u.values.at(1).text.toUpper();
            } else {
                u.keyword = u.values.at(0).text.toUpper();
            }
        }
        if (u.keyword == "CAPABILITY") {
            m_caps.clear();
            for (int i = 1; i < u.values.size(); ++i) {
                m_caps.append(u.values.at(i).text);
            }
        } else if (u.keyword == "OK" && u.code().startsWith("CAPABILITY ")) {
            m_caps = u.code().mid(11).split(' ');
        }
        if (m_inFlight) {
            m_collected.append(u);
            m_timer->start(m_timeoutMs); // a long FETCH is still progress
        }
        emit untagged(u);
        return;
    }
    const int sp = resp.indexOf(' ');
    const QByteArray tag = resp.left(sp);
    const QByteArray rest = sp < 0 ? QByteArray() : resp.mid(sp + 1);
    const int sp2 = rest.indexOf(' ');
    const QByteArray status = (sp2 < 0 ? rest : rest.left(sp2)).toUpper();
    const QString text = sp2 < 0 ? QString() : QString::fromUtf8(rest.mid(sp2 + 1));
    if (!m_idleTag.isEmpty() && tag == m_idleTag) {
        m_idleTag.clear();
        m_idleState = IdleState::Off;
        m_timer->stop();
        sendNext();
        return;
    }
    if (m_inFlight && tag == m_currentTag) {
        m_timer->stop();
        Reply r;
        r.status = status;
        r.ok = status == "OK";
        r.text = text;
        r.untagged = std::exchange(m_collected, {});
        const Pending done = std::exchange(m_current, {});
        m_inFlight = false;
        QPointer<ImapClient> self(this);
        if (done.cb) {
            done.cb(r);
        }
        if (self) {
            sendNext();
        }
    }
}

} // namespace zmail
