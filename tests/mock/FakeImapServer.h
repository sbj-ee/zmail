#pragma once
// In-process Gmail-flavoured IMAP server for tests and screenshots: LOGIN,
// CAPABILITY, LIST (special-use), STATUS, SELECT (CONDSTORE), FETCH / UID FETCH
// (X-GM-MSGID, X-GM-THRID, X-GM-LABELS, CHANGEDSINCE, BODY.PEEK[...]),
// UID SEARCH (UID ranges, X-GM-MSGID), IDLE / DONE, NOOP, LOGOUT.
// Plain TCP on localhost. Never linked into zmail. Passwords are fakes.
#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QTcpServer>

class QTcpSocket;

namespace zmail::test {

class FakeImapServer : public QObject
{
    Q_OBJECT

public:
    struct Msg
    {
        quint64 msgid = 0;
        quint64 thrid = 0;
        QStringList labels;        // Gmail labels: "\\Inbox", "\\Sent", "\\Important", "Receipts", ...
        QSet<QString> flags;       // "\\Seen", "\\Flagged", ...
        qint64 modseq = 0;
        QByteArray raw;
        QString internalDate;      // "12-Oct-2026 09:15:00 -0500"
        QHash<QString, qint64> uid; // folder -> UID
        bool gone = false;
    };

    explicit FakeImapServer(QObject *parent = nullptr);
    ~FakeImapServer() override;

    quint16 port() const { return m_server.serverPort(); }
    QString user = QStringLiteral("alex.morgan@gmail.com");
    QString password = QStringLiteral("abcdefghijklmnop"); // made up
    bool idleCapable = true;
    bool condstoreCapable = true;
    QStringList userFolders;            // extra empty labels to list
    QList<QByteArray> commands;         // every command line after LOGIN (tags stripped)
    int logins = 0;
    bool refuseChanges = false;
    QList<QByteArray> appended;
    // Gmail's limits, for the backoff tests.
    QList<QByteArray> byeGreetings;     // next connections are greeted with these and dropped
    QList<QByteArray> loginRefusals;    // next LOGINs answer these ("NO [ALERT] Too many ...")
    int throttleCommands = 0;           // next commands answer NO [THROTTLED]
    int connectAttempts = 0;
    int refusedLogins = 0;         // every APPEND's message bytes          // STORE / MOVE answer NO (to test rollback)

    // New message; notifies IDLE sessions (* n EXISTS). Returns its X-GM-MSGID.
    quint64 deliver(const QByteArray &raw, const QStringList &labels = {QStringLiteral("\\Inbox")},
                    const QSet<QString> &flags = {}, const QString &internalDate = {});
    // Changes that bump MODSEQ and notify IDLE sessions.
    void setFlags(quint64 msgid, const QSet<QString> &flags);
    void setLabels(quint64 msgid, const QStringList &labels);
    void expunge(quint64 msgid);
    void dropConnections();
    void bumpUidValidity(const QString &folder);
    bool anyIdling() const;
    Msg *find(quint64 msgid);

    void seedDemo(int count);
    static QByteArray makeMessage(const QString &fromName, const QString &fromAddr, const QString &subject,
                                  const QString &text, const QString &html = {}, const QDateTime &date = {});

private:
    struct Folder
    {
        QString name;
        QStringList attrs;
        QString label; // the Gmail label it shows; "" for All Mail; "*flag" for Starred
        qint64 uidValidity = 1;
        qint64 uidNext = 1;
    };
    struct Session
    {
        QPointer<QTcpSocket> socket;
        QByteArray buffer;
        bool authed = false;
        QString selected;
        QByteArray idleTag;
        QByteArray literalLine;   // a command waiting for its literal
        qint64 literalNeed = -1;
        QByteArray literal;
    };
    QList<Folder> folders() const;
    bool inFolder(const Msg &m, const Folder &f) const;
    QList<Msg *> members(const QString &folder);  // assigns UIDs, in UID order
    void handle(Session *s, const QByteArray &line, const QByteArray &literal = {});
    void reply(Session *s, const QByteArray &data);
    QByteArray fetchLine(const QString &folder, int seq, Msg *m, const QList<QByteArray> &items, bool withModseq);
    void notify(const std::function<QByteArray(const QString &folder, Session *)> &what);
    bool rawMatches(const Msg &m, const QString &query) const;
    void moveTo(Msg *m, const QString &from, const QString &to, bool move);
    QList<QPair<qint64, qint64>> parseSet(const QByteArray &set, qint64 star) const;

    QTcpServer m_server;
    QList<Msg> m_msgs;
    QList<Session *> m_sessions;
    QHash<QString, qint64> m_uidValidity;
    QHash<QString, qint64> m_uidNext;
    qint64 m_modseq = 100;
    quint64 m_nextId = 0x18f3a0c2b4d50000ULL;
};

} // namespace zmail::test
