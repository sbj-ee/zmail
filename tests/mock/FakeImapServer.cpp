#include "FakeImapServer.h"

#include "core/ImapClient.h"

#include <QDateTime>
#include <QLocale>
#include <QTcpSocket>
#include <QTimeZone>

namespace zmail::test {

using zmail::ImapClient;
using zmail::ImapValue;

FakeImapServer::FakeImapServer(QObject *parent)
    : QObject(parent)
{
    if (!m_server.listen(QHostAddress::LocalHost)) {
        qFatal("FakeImapServer: listen failed");
    }
    connect(&m_server, &QTcpServer::newConnection, this, [this] {
        while (QTcpSocket *sock = m_server.nextPendingConnection()) {
            auto *s = new Session;
            s->socket = sock;
            m_sessions.append(s);
            sock->write("* OK Gimap ready for requests from 127.0.0.1\r\n");
            connect(sock, &QTcpSocket::readyRead, this, [this, s] {
                if (!s->socket) {
                    return;
                }
                s->buffer += s->socket->readAll();
                int nl;
                while (s->socket && (nl = s->buffer.indexOf("\r\n")) >= 0) {
                    const QByteArray line = s->buffer.left(nl);
                    s->buffer.remove(0, nl + 2);
                    handle(s, line);
                }
            });
            connect(sock, &QTcpSocket::disconnected, this, [this, s, sock] {
                m_sessions.removeAll(s);
                sock->deleteLater();
                delete s;
            });
        }
    });
}

FakeImapServer::~FakeImapServer()
{
    for (Session *s : std::as_const(m_sessions)) {
        if (s->socket) {
            s->socket->disconnect(this);
            s->socket->abort();
        }
        delete s;
    }
    m_sessions.clear();
}

QList<FakeImapServer::Folder> FakeImapServer::folders() const
{
    QList<Folder> out = {
        {QStringLiteral("INBOX"), {QStringLiteral("\\HasNoChildren")}, QStringLiteral("\\Inbox")},
        {QStringLiteral("[Gmail]"), {QStringLiteral("\\HasChildren"), QStringLiteral("\\Noselect")}, QStringLiteral("-")},
        {QStringLiteral("[Gmail]/All Mail"), {QStringLiteral("\\All"), QStringLiteral("\\HasNoChildren")}, QString()},
        {QStringLiteral("[Gmail]/Drafts"), {QStringLiteral("\\Drafts"), QStringLiteral("\\HasNoChildren")}, QStringLiteral("\\Draft")},
        {QStringLiteral("[Gmail]/Important"), {QStringLiteral("\\HasNoChildren"), QStringLiteral("\\Important")}, QStringLiteral("\\Important")},
        {QStringLiteral("[Gmail]/Sent Mail"), {QStringLiteral("\\HasNoChildren"), QStringLiteral("\\Sent")}, QStringLiteral("\\Sent")},
        {QStringLiteral("[Gmail]/Spam"), {QStringLiteral("\\HasNoChildren"), QStringLiteral("\\Junk")}, QStringLiteral("\\Spam")},
        {QStringLiteral("[Gmail]/Starred"), {QStringLiteral("\\Flagged"), QStringLiteral("\\HasNoChildren")}, QStringLiteral("*flag")},
        {QStringLiteral("[Gmail]/Trash"), {QStringLiteral("\\HasNoChildren"), QStringLiteral("\\Trash")}, QStringLiteral("\\Trash")},
    };
    QStringList user = userFolders;
    for (const Msg &m : m_msgs) {
        for (const QString &l : m.labels) {
            if (!l.startsWith(QLatin1Char('\\')) && !user.contains(l)) {
                user.append(l);
            }
        }
    }
    user.sort();
    for (const QString &u : user) {
        out.append({u, {QStringLiteral("\\HasNoChildren")}, u});
    }
    return out;
}

bool FakeImapServer::inFolder(const Msg &m, const Folder &f) const
{
    if (m.gone) {
        return false;
    }
    if (f.label.isEmpty()) {
        return !m.labels.contains(QStringLiteral("\\Trash")) && !m.labels.contains(QStringLiteral("\\Spam"));
    }
    if (f.label == QLatin1String("*flag")) {
        return m.flags.contains(QStringLiteral("\\Flagged"));
    }
    return m.labels.contains(f.label);
}

QList<FakeImapServer::Msg *> FakeImapServer::members(const QString &folder)
{
    Folder f;
    bool found = false;
    for (const Folder &c : folders()) {
        if (c.name == folder) {
            f = c;
            found = true;
        }
    }
    QList<Msg *> out;
    if (!found) {
        return out;
    }
    for (Msg &m : m_msgs) {
        if (!inFolder(m, f)) {
            continue;
        }
        if (!m.uid.contains(folder)) {
            m.uid.insert(folder, m_uidNext.value(folder, 1));
            m_uidNext.insert(folder, m.uid.value(folder) + 1);
        }
        out.append(&m);
    }
    std::sort(out.begin(), out.end(), [&folder](Msg *a, Msg *b) { return a->uid.value(folder) < b->uid.value(folder); });
    return out;
}

FakeImapServer::Msg *FakeImapServer::find(quint64 msgid)
{
    for (Msg &m : m_msgs) {
        if (m.msgid == msgid) {
            return &m;
        }
    }
    return nullptr;
}

QList<QPair<qint64, qint64>> FakeImapServer::parseSet(const QByteArray &set, qint64 star) const
{
    QList<QPair<qint64, qint64>> out;
    for (const QByteArray &part : set.split(',')) {
        const QList<QByteArray> ab = part.split(':');
        auto num = [star](const QByteArray &x) { return x == "*" ? star : x.toLongLong(); };
        qint64 a = num(ab.at(0));
        qint64 b = ab.size() > 1 ? num(ab.at(1)) : a;
        if (a > b) {
            std::swap(a, b);
        }
        out.append({a, b});
    }
    return out;
}

void FakeImapServer::reply(Session *s, const QByteArray &data)
{
    if (s->socket) {
        s->socket->write(data);
    }
}

QByteArray FakeImapServer::fetchLine(const QString &folder, int seq, Msg *m, const QList<QByteArray> &items, bool withModseq)
{
    QByteArray out = "* " + QByteArray::number(seq) + " FETCH (UID " + QByteArray::number(m->uid.value(folder));
    auto want = [&items](const char *x) {
        for (const QByteArray &i : items) {
            if (i.toUpper().startsWith(x)) {
                return true;
            }
        }
        return false;
    };
    if (want("X-GM-MSGID")) {
        out += " X-GM-MSGID " + QByteArray::number(m->msgid);
    }
    if (want("X-GM-THRID")) {
        out += " X-GM-THRID " + QByteArray::number(m->thrid);
    }
    if (want("X-GM-LABELS")) {
        QByteArray labels;
        for (const QString &l : m->labels) {
            // Like Gmail: the selected folder's own label is left out.
            if (folder == QLatin1String("INBOX") && l == QLatin1String("\\Inbox")) {
                continue;
            }
            labels += (labels.isEmpty() ? "" : " ") + ImapClient::quote(QString::fromLatin1(ImapClient::encodeMailboxName(l)));
        }
        out += " X-GM-LABELS (" + labels + ")";
    }
    if (want("FLAGS")) {
        QStringList f(m->flags.begin(), m->flags.end());
        f.sort();
        out += " FLAGS (" + f.join(QLatin1Char(' ')).toLatin1() + ")";
    }
    if (want("INTERNALDATE")) {
        out += " INTERNALDATE \"" + m->internalDate.toLatin1() + "\"";
    }
    if (want("RFC822.SIZE")) {
        out += " RFC822.SIZE " + QByteArray::number(m->raw.size());
    }
    if (withModseq) {
        out += " MODSEQ (" + QByteArray::number(m->modseq) + ")";
    }
    for (const QByteArray &i : items) {
        const QByteArray up = i.toUpper();
        if (up.startsWith("BODY.PEEK[HEADER.FIELDS")) {
            const int split = m->raw.indexOf("\r\n\r\n");
            const QByteArray head = m->raw.left(split < 0 ? m->raw.size() : split) + "\r\n\r\n";
            out += " BODY[HEADER.FIELDS " + i.mid(i.indexOf('(')).chopped(1) + "] {" + QByteArray::number(head.size())
                + "}\r\n" + head;
        } else if (up == "BODY.PEEK[]") {
            out += " BODY[] {" + QByteArray::number(m->raw.size()) + "}\r\n" + m->raw;
        }
    }
    return out + ")\r\n";
}

void FakeImapServer::handle(Session *s, const QByteArray &line)
{
    if (!s->idleTag.isEmpty()) {
        if (line.trimmed().toUpper() == "DONE") {
            reply(s, s->idleTag + " OK IDLE terminated (Success)\r\n");
            s->idleTag.clear();
        }
        return;
    }
    const int sp = line.indexOf(' ');
    const QByteArray tag = line.left(sp);
    int pos = sp + 1;
    QList<ImapValue> v;
    ImapClient::parseValues(line, pos, v);
    if (v.isEmpty()) {
        reply(s, tag + " BAD empty\r\n");
        return;
    }
    QByteArray cmd = v.at(0).text.toUpper();
    bool uid = false;
    if (cmd == "UID" && v.size() > 1) {
        uid = true;
        v.removeFirst();
        cmd = v.at(0).text.toUpper();
    }
    if (cmd != "LOGIN") {
        commands.append(line.mid(sp + 1));
    }
    if (cmd == "CAPABILITY") {
        QByteArray caps = "IMAP4rev1 UNSELECT X-GM-EXT-1 UIDPLUS MOVE";
        if (idleCapable) {
            caps += " IDLE";
        }
        if (condstoreCapable) {
            caps += " CONDSTORE";
        }
        reply(s, "* CAPABILITY " + caps + "\r\n" + tag + " OK Thats all she wrote!\r\n");
    } else if (cmd == "LOGIN") {
        if (v.size() >= 3 && v.at(1).str() == user && v.at(2).str() == password) {
            s->authed = true;
            ++logins;
            reply(s, tag + " OK " + user.toLatin1() + " authenticated (Success)\r\n");
        } else {
            reply(s, tag + " NO [AUTHENTICATIONFAILED] Invalid credentials (Failure)\r\n");
        }
    } else if (!s->authed) {
        reply(s, tag + " BAD not authenticated\r\n");
    } else if (cmd == "LOGOUT") {
        reply(s, "* BYE LOGOUT Requested\r\n" + tag + " OK 73 good day (Success)\r\n");
        s->socket->disconnectFromHost();
    } else if (cmd == "NOOP") {
        reply(s, tag + " OK Success\r\n");
    } else if (cmd == "IDLE") {
        s->idleTag = tag;
        reply(s, "+ idling\r\n");
    } else if (cmd == "LIST") {
        QByteArray out;
        for (const Folder &f : folders()) {
            out += "* LIST (" + f.attrs.join(QLatin1Char(' ')).toLatin1() + ") \"/\" "
                + ImapClient::quote(QString::fromLatin1(ImapClient::encodeMailboxName(f.name))) + "\r\n";
        }
        reply(s, out + tag + " OK Success\r\n");
    } else if (cmd == "STATUS" && v.size() >= 2) {
        const QString name = ImapClient::decodeMailboxName(v.at(1).text);
        const auto ms = members(name);
        int unseen = 0;
        for (const Msg *m : ms) {
            unseen += m->flags.contains(QStringLiteral("\\Seen")) ? 0 : 1;
        }
        reply(s, "* STATUS " + ImapClient::quote(QString::fromLatin1(v.at(1).text)) + " (MESSAGES "
                     + QByteArray::number(ms.size()) + " UNSEEN " + QByteArray::number(unseen) + ")\r\n" + tag
                     + " OK Success\r\n");
    } else if ((cmd == "SELECT" || cmd == "EXAMINE") && v.size() >= 2) {
        const QString name = ImapClient::decodeMailboxName(v.at(1).text);
        bool exists = false;
        for (const Folder &f : folders()) {
            exists = exists || (f.name == name && !f.attrs.contains(QStringLiteral("\\Noselect")));
        }
        if (!exists) {
            reply(s, tag + " NO [NONEXISTENT] Unknown Mailbox\r\n");
            return;
        }
        s->selected = name;
        const auto ms = members(name);
        reply(s, "* FLAGS (\\Answered \\Flagged \\Draft \\Deleted \\Seen)\r\n* " + QByteArray::number(ms.size())
                     + " EXISTS\r\n* 0 RECENT\r\n* OK [UIDVALIDITY " + QByteArray::number(m_uidValidity.value(name, 1))
                     + "] UIDs valid.\r\n* OK [UIDNEXT " + QByteArray::number(m_uidNext.value(name, 1))
                     + "] Predicted next UID.\r\n* OK [HIGHESTMODSEQ " + QByteArray::number(m_modseq) + "]\r\n" + tag
                     + " OK [READ-WRITE] " + name.toUtf8() + " selected. (Success)\r\n");
    } else if (cmd == "FETCH" && v.size() >= 3 && !s->selected.isEmpty()) {
        const auto ms = members(s->selected);
        QList<QByteArray> items;
        if (v.at(2).isList()) {
            for (const ImapValue &i : v.at(2).items) {
                items.append(i.text);
            }
        } else {
            items.append(v.at(2).text);
        }
        qint64 changedSince = -1;
        if (v.size() >= 4 && v.at(3).isList() && v.at(3).get("CHANGEDSINCE")) {
            changedSince = v.at(3).get("CHANGEDSINCE")->num();
        }
        QByteArray out;
        if (uid) {
            const qint64 maxUid = ms.isEmpty() ? 0 : ms.last()->uid.value(s->selected);
            const auto ranges = parseSet(v.at(1).text, maxUid);
            bool any = false;
            for (int i = 0; i < ms.size(); ++i) {
                const qint64 u = ms.at(i)->uid.value(s->selected);
                for (const auto &r : ranges) {
                    if (u >= r.first && u <= r.second && (changedSince < 0 || ms.at(i)->modseq > changedSince)) {
                        out += fetchLine(s->selected, i + 1, ms.at(i), items, changedSince >= 0);
                        any = true;
                        break;
                    }
                }
            }
            // "n:*" with n past the end: Gmail answers with the last message.
            if (!any && changedSince < 0 && v.at(1).text.endsWith(":*") && !ms.isEmpty()) {
                out += fetchLine(s->selected, int(ms.size()), ms.last(), items, false);
            }
        } else {
            for (const auto &r : parseSet(v.at(1).text, ms.size())) {
                for (qint64 i = r.first; i <= r.second && i <= ms.size(); ++i) {
                    if (i >= 1) {
                        out += fetchLine(s->selected, int(i), ms.at(i - 1), items, false);
                    }
                }
            }
        }
        reply(s, out + tag + " OK Success\r\n");
    } else if (cmd == "SEARCH" && v.size() >= 3 && !s->selected.isEmpty()) {
        const auto ms = members(s->selected);
        QByteArray hits;
        const QByteArray key = v.at(1).text.toUpper();
        if (key == "X-GM-MSGID") {
            for (const Msg *m : ms) {
                if (m->msgid == v.at(2).text.toULongLong()) {
                    hits += ' ' + QByteArray::number(m->uid.value(s->selected));
                }
            }
        } else if (key == "UID") {
            const qint64 maxUid = ms.isEmpty() ? 0 : ms.last()->uid.value(s->selected);
            const auto ranges = parseSet(v.at(2).text, maxUid);
            for (const Msg *m : ms) {
                const qint64 u = m->uid.value(s->selected);
                for (const auto &r : ranges) {
                    if (u >= r.first && u <= r.second) {
                        hits += ' ' + QByteArray::number(u);
                        break;
                    }
                }
            }
        }
        reply(s, "* SEARCH" + hits + "\r\n" + tag + " OK SEARCH completed (Success)\r\n");
    } else {
        reply(s, tag + " BAD Unknown command\r\n");
    }
}

void FakeImapServer::notify(const std::function<QByteArray(const QString &folder, Session *)> &what)
{
    for (Session *s : std::as_const(m_sessions)) {
        if (s->socket && !s->idleTag.isEmpty() && !s->selected.isEmpty()) {
            reply(s, what(s->selected, s));
        }
    }
}

bool FakeImapServer::anyIdling() const
{
    for (const Session *s : m_sessions) {
        if (s->socket && !s->idleTag.isEmpty()) {
            return true;
        }
    }
    return false;
}

quint64 FakeImapServer::deliver(const QByteArray &raw, const QStringList &labels, const QSet<QString> &flags,
                                const QString &internalDate)
{
    Msg m;
    m.msgid = ++m_nextId;
    m.thrid = m.msgid;
    m.labels = labels;
    m.flags = flags;
    m.modseq = ++m_modseq;
    m.raw = raw;
    m.internalDate = internalDate.isEmpty()
        ? QLocale::c().toString(QDateTime::currentDateTime(), QStringLiteral("dd-MMM-yyyy HH:mm:ss +0000"))
        : internalDate;
    m_msgs.append(m);
    notify([this](const QString &folder, Session *) {
        return "* " + QByteArray::number(members(folder).size()) + " EXISTS\r\n";
    });
    return m.msgid;
}

void FakeImapServer::setFlags(quint64 msgid, const QSet<QString> &flags)
{
    Msg *m = find(msgid);
    if (!m) {
        return;
    }
    m->flags = flags;
    m->modseq = ++m_modseq;
    notify([this, msgid](const QString &folder, Session *) -> QByteArray {
        const auto ms = members(folder);
        for (int i = 0; i < ms.size(); ++i) {
            if (ms.at(i)->msgid == msgid) {
                QStringList f(ms.at(i)->flags.begin(), ms.at(i)->flags.end());
                return "* " + QByteArray::number(i + 1) + " FETCH (FLAGS (" + f.join(QLatin1Char(' ')).toLatin1() + "))\r\n";
            }
        }
        return {};
    });
}

void FakeImapServer::setLabels(quint64 msgid, const QStringList &labels)
{
    Msg *m = find(msgid);
    if (!m) {
        return;
    }
    QHash<QString, int> seqBefore;
    for (const Folder &f : folders()) {
        const auto ms = members(f.name);
        for (int i = 0; i < ms.size(); ++i) {
            if (ms.at(i)->msgid == msgid) {
                seqBefore.insert(f.name, i + 1);
            }
        }
    }
    m = find(msgid);
    m->labels = labels;
    m->modseq = ++m_modseq;
    notify([this, seqBefore, msgid](const QString &folder, Session *) -> QByteArray {
        const auto ms = members(folder);
        bool still = false;
        for (const Msg *x : ms) {
            still = still || x->msgid == msgid;
        }
        if (seqBefore.contains(folder) && !still) {
            return "* " + QByteArray::number(seqBefore.value(folder)) + " EXPUNGE\r\n";
        }
        if (!seqBefore.contains(folder) && still) {
            return "* " + QByteArray::number(ms.size()) + " EXISTS\r\n";
        }
        return {};
    });
}

void FakeImapServer::expunge(quint64 msgid)
{
    if (Msg *m = find(msgid)) {
        m->labels = {QStringLiteral("\\Trash")};
        m->gone = true;
        m->modseq = ++m_modseq;
        notify([](const QString &, Session *) { return QByteArray("* 1 EXPUNGE\r\n"); });
    }
}

void FakeImapServer::dropConnections()
{
    const auto sessions = m_sessions;
    for (Session *s : sessions) {
        if (s->socket) {
            s->socket->abort();
        }
    }
}

void FakeImapServer::bumpUidValidity(const QString &folder)
{
    m_uidValidity.insert(folder, m_uidValidity.value(folder, 1) + 1);
    for (Msg &m : m_msgs) {
        m.uid.remove(folder);
    }
    m_uidNext.insert(folder, 1);
}

QByteArray FakeImapServer::makeMessage(const QString &fromName, const QString &fromAddr, const QString &subject,
                                       const QString &text, const QString &html, const QDateTime &dateIn)
{
    const QDateTime date = dateIn.isValid() ? dateIn : QDateTime::currentDateTime();
    QByteArray m;
    m += "From: " + fromName.toUtf8() + " <" + fromAddr.toUtf8() + ">\r\n";
    m += "To: Alex Morgan <alex.morgan@gmail.com>\r\n";
    // Non-ASCII subjects go out as RFC 2047 encoded-words, as real mail does.
    bool ascii = true;
    for (const QChar c : subject) {
        ascii = ascii && c.unicode() < 128;
    }
    m += "Subject: " + (ascii ? subject.toUtf8() : "=?UTF-8?B?" + subject.toUtf8().toBase64() + "?=") + "\r\n";
    m += "Date: " + QLocale::c().toString(date, QStringLiteral("ddd, dd MMM yyyy HH:mm:ss +0000")).toLatin1() + "\r\n";
    m += "Message-ID: <" + QByteArray::number(qHash(subject + fromAddr + date.toString()), 16) + "@example.com>\r\n";
    m += "MIME-Version: 1.0\r\n";
    if (html.isEmpty()) {
        m += "Content-Type: text/plain; charset=UTF-8\r\nContent-Transfer-Encoding: quoted-printable\r\n\r\n";
        QByteArray qp;
        for (const char c : text.toUtf8()) {
            const uchar u = uchar(c);
            if (u >= 128 || c == '=') {
                qp += '=' + QByteArray::number(u, 16).toUpper().rightJustified(2, '0');
            } else {
                qp += c;
            }
        }
        m += qp + "\r\n";
    } else {
        m += "Content-Type: multipart/alternative; boundary=\"zz-alt\"\r\n\r\n";
        m += "--zz-alt\r\nContent-Type: text/plain; charset=UTF-8\r\n\r\n" + text.toUtf8() + "\r\n";
        m += "--zz-alt\r\nContent-Type: text/html; charset=UTF-8\r\nContent-Transfer-Encoding: base64\r\n\r\n"
            + html.toUtf8().toBase64() + "\r\n--zz-alt--\r\n";
    }
    return m;
}

void FakeImapServer::seedDemo(int count)
{
    struct P { const char *name; const char *addr; const char *subject; const char *text; const char *label; };
    static const P people[] = {
        {"Priya Raman", "priya@northwind.example", "Q4 field schedule (draft)", "Here's the draft schedule for the field crews. Can you check the Tuesday rotations?", ""},
        {"GitHub", "noreply@github.example", "[sbj-ee/zmail] CI passed on main", "All checks have passed for the latest commit on main.", "Notifications"},
        {"Marcus Lee", "marcus.lee@example.org", "Lunch on Thursday?", "Are you free Thursday around noon? The new Thai place opened.", ""},
        {"Hydro Utility", "billing@hydro.example", "Your October statement is ready", "Your statement for October is now available. Amount due: $84.12.", "Receipts"},
        {"Dana Whitfield", "dana@whitfield.example", "Re: substation inspection photos", "Thanks, the photos came through. Two of them are blurry; could you resend?", "Projects/Substation"},
        {"Café Lumen", "hello@cafelumen.example", "Café Lumen: your loyalty reward", "You've earned a free drink. Show this email at the counter.", ""},
        {"Ops Calendar", "calendar@example.com", "Reminder: safety stand-down 10/15", "Reminder: the quarterly safety stand-down is on Thursday at 7:00.", ""},
        {"Jordan Kim", "jordan.kim@example.net", "Spare parts order", "The breaker parts are back-ordered until November. Want me to source elsewhere?", "Projects/Substation"},
    };
    const QDateTime now = QDateTime::currentDateTime();
    for (int i = 0; i < count; ++i) {
        const P &p = people[i % 8];
        const QDateTime when = now.addSecs(-qint64(count - i) * 3700);
        QStringList labels{QStringLiteral("\\Inbox")};
        if (*p.label) {
            labels.append(QString::fromUtf8(p.label));
        }
        if (i % 5 == 0) {
            labels.append(QStringLiteral("\\Important"));
        }
        QSet<QString> flags;
        if (i < count - 4) {
            flags.insert(QStringLiteral("\\Seen"));
        }
        if (i % 7 == 3) {
            flags.insert(QStringLiteral("\\Flagged"));
        }
        const QString html = i % 3 == 0 ? QStringLiteral("<p>%1</p><p style='color:#666'>&mdash; %2</p>")
                                               .arg(QString::fromUtf8(p.text).toHtmlEscaped(), QString::fromUtf8(p.name))
                                         : QString();
        deliver(makeMessage(QString::fromUtf8(p.name), QString::fromUtf8(p.addr), QString::fromUtf8(p.subject),
                            QString::fromUtf8(p.text), html, when),
                labels, flags, QLocale::c().toString(when.toUTC(), QStringLiteral("dd-MMM-yyyy HH:mm:ss +0000")));
    }
    deliver(makeMessage(QStringLiteral("Alex Morgan"), QStringLiteral("alex.morgan@gmail.com"),
                        QStringLiteral("Re: Lunch on Thursday?"), QStringLiteral("Thursday works. See you at noon.")),
            {QStringLiteral("\\Sent")}, {QStringLiteral("\\Seen")});
}

} // namespace zmail::test
