#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QStringList>
#include <functional>

class QSslSocket;
class QTimer;

namespace zmail {

// One IMAP value (RFC 3501 response syntax): an atom (incl. numbers and
// flags such as \Seen), a string (quoted or literal: bytes), NIL, or a
// parenthesised list.
struct ImapValue
{
    enum class Type { Atom, String, Nil, List };
    Type type = Type::Nil;
    QByteArray text;            // Atom / String
    QList<ImapValue> items;     // List

    bool isList() const { return type == Type::List; }
    // Atom or string as text (NIL -> empty).
    QString str() const { return QString::fromUtf8(text); }
    qint64 num() const { return text.toLongLong(); }
    // In a FETCH attribute list: the value after `key` (case-insensitive).
    const ImapValue *get(const QByteArray &key) const;
};

// An untagged response: "* 12 FETCH (...)", "* LIST (...) "/" "INBOX"",
// "* OK [HIGHESTMODSEQ 99]", ... split into values.
struct ImapUntagged
{
    QList<ImapValue> values;
    QByteArray raw;              // first line, for diagnostics (never logged: may hold subjects)
    // "* 12 FETCH" -> number 12, keyword FETCH; "* LIST" -> keyword LIST.
    qint64 number = -1;
    QByteArray keyword;
    // Bracketed response code in an OK/NO: "[UIDVALIDITY 3]" -> "UIDVALIDITY 3".
    QByteArray code() const;
};

// Minimal async IMAP4rev1 client for Gmail: one connection, one command in
// flight at a time, literals in responses, IDLE (RFC 2177). The password is
// only ever sent as a quoted LOGIN argument and is never logged or kept.
class ImapClient : public QObject
{
    Q_OBJECT

public:
    struct Reply
    {
        bool ok = false;            // tagged OK
        QByteArray status;          // OK / NO / BAD / "" (connection lost)
        QString text;               // the tagged line's text
        QList<ImapUntagged> untagged;
    };
    using ReplyCb = std::function<void(const Reply &)>;

    explicit ImapClient(QObject *parent = nullptr);
    ~ImapClient() override;

    void setPlaintextForTests(bool on) { m_plaintext = on; }
    void setTimeoutMs(int ms) { m_timeoutMs = ms; }

    // Connect, read the greeting, LOGIN, then CAPABILITY. done(ok, error).
    void open(const QString &host, int port, const QString &user, const QString &password,
              std::function<void(bool ok, const QString &error)> done);
    void close();
    bool isOpen() const;
    bool hasCapability(const QByteArray &cap) const;

    // Queue a command (without tag / CRLF). Leaves IDLE first if idling.
    void command(const QByteArray &cmd, ReplyCb cb);
    // A command ending in one synchronizing literal (APPEND): sends
    // "cmd {n}", waits for the server's "+", then the bytes.
    void command(const QByteArray &cmd, const QByteArray &literal, ReplyCb cb);
    // IDLE until the next command(); untagged responses arrive via untagged().
    void idle();
    bool idling() const { return m_idleState != IdleState::Off; }
    QString selected() const { return m_selected; }
    void setSelected(const QString &mailbox) { m_selected = mailbox; }

    // Parse helpers, public for tests.
    static bool parseValues(const QByteArray &data, int &pos, QList<ImapValue> &out, int depth = 0);
    static QByteArray quote(const QString &s);              // "..." with \ and " escaped
    static QString decodeMailboxName(const QByteArray &utf7); // modified UTF-7 (RFC 3501 5.1.3)
    static QByteArray encodeMailboxName(const QString &name);

signals:
    void untagged(const zmail::ImapUntagged &response); // also during IDLE
    void connectionLost(const QString &reason);
    // A tagged NO saying Gmail is throttling the account ([THROTTLED], bandwidth limits).
    void throttled(const QString &text);

private:
    enum class IdleState { Off, Starting, On, Stopping };
    struct Pending
    {
        QByteArray cmd;
        ReplyCb cb;
        QByteArray literal;
        bool hasLiteral = false;
    };
    void onReadyRead();
    bool takeResponse(QByteArray &out); // one complete response (line + literals)
    void handleResponse(const QByteArray &resp);
    void submit(Pending p);
    void sendNext();
    void fail(const QString &reason);

    QPointer<QSslSocket> m_socket;
    QTimer *m_timer;
    QByteArray m_buffer;
    QQueue<Pending> m_queue;
    bool m_inFlight = false;
    QByteArray m_currentTag;
    Pending m_current;
    QList<ImapUntagged> m_collected;
    int m_tagCounter = 0;
    IdleState m_idleState = IdleState::Off;
    QByteArray m_idleTag;
    QList<QByteArray> m_caps;
    QString m_selected;
    QString m_bye; // the server's last BYE text, for the disconnect reason
    bool m_plaintext = false;
    int m_timeoutMs = 60000;
    std::function<void(const QByteArray &line)> m_greeting; // set until the greeting arrives
};

} // namespace zmail

Q_DECLARE_METATYPE(zmail::ImapUntagged)
