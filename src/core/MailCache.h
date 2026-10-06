#pragma once

#include <QDateTime>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <QVariant>

namespace zmail {

struct CachedLabel
{
    QString id;        // "INBOX", "Label_123"
    QString name;      // "INBOX", "Receipts/2026"
    QString type;      // "system" | "user"
    int unread = 0;
    int total = 0;
    QString color;     // background colour (#rrggbb) or ""
};

struct CachedMessage
{
    QString id;
    QString threadId;
    qint64 historyId = 0;
    qint64 internalDateMs = 0;
    QString fromName;
    QString fromAddr;
    QString to;
    QString cc;
    QString replyTo;
    QString messageIdHeader; // RFC 5322 Message-ID, for threading replies
    QString references;      // References header (space-separated ids)
    QString subject;
    QString snippet;
    qint64 size = 0;
    QStringList labels;
    bool hasAttachment = false;
    // Filled once the full message has been fetched.
    bool hasBody = false;
    QString bodyText;
    QString bodyHtml;
    QStringList attachments;

    bool unread() const { return labels.contains(QStringLiteral("UNREAD")); }
    QDateTime date() const { return QDateTime::fromMSecsSinceEpoch(internalDateMs); }
};

// Per-account SQLite cache (~/.local/share/zmail/<account>/zmail.db, dir 0700)
// with an FTS5 index over subject/from/to/snippet/body.
class MailCache
{
public:
    MailCache();
    ~MailCache();
    MailCache(const MailCache &) = delete;
    MailCache &operator=(const MailCache &) = delete;

    static QString defaultPath(const QString &account);

    bool open(const QString &path);
    bool isOpen() const;
    QString lastError() const { return m_error; }
    bool hasFts5() const { return m_fts5; }

    // Write transactions nest: only the outermost begin()/commit() pair reaches
    // SQLite, so a caller can batch calls that are themselves transactions.
    bool begin();
    bool commit();
    class Batch // scoped begin()/commit()
    {
    public:
        explicit Batch(MailCache &cache) : m_cache(cache) { m_cache.begin(); }
        ~Batch() { m_cache.commit(); }
        Batch(const Batch &) = delete;
        Batch &operator=(const Batch &) = delete;

    private:
        MailCache &m_cache;
    };
    // A PRAGMA's current value on the cache's connection (diagnostics, tests).
    QVariant pragma(const QString &name) const;

    QString meta(const QString &key) const;
    void setMeta(const QString &key, const QString &value);
    qint64 historyId() const { return meta(QStringLiteral("historyId")).toLongLong(); }
    void setHistoryId(qint64 id) { setMeta(QStringLiteral("historyId"), QString::number(id)); }

    void replaceLabels(const QList<CachedLabel> &labels);
    QList<CachedLabel> labels() const;

    // Insert or update metadata; keeps an already-fetched body.
    void upsert(const CachedMessage &m);
    void setBody(const QString &id, const QString &text, const QString &html, const QStringList &attachments);
    void setLabels(const QString &id, const QStringList &labels);
    void modifyLabels(const QString &id, const QStringList &add, const QStringList &remove);
    void remove(const QString &id);
    // Full resync: drop every cached message. keepSnoozed leaves the rows that
    // have a snooze (deleting them would cascade the wake time away with them).
    void clearMessages(bool keepSnoozed = false);
    bool contains(const QString &id) const;

    CachedMessage message(const QString &id) const;
    // message() without the bodies (bodyText / bodyHtml stay empty; hasBody
    // still says whether one is cached): for callers after labels or headers.
    CachedMessage summary(const QString &id) const;
    // Ids of the cached messages carrying a label.
    QStringList messageIds(const QString &labelId) const;
    // The message list's rows, newest first, in one query: metadata only
    // (bodyText / bodyHtml stay empty; hasBody still says whether one is
    // cached) with each message's snooze joined in.
    struct Listed
    {
        CachedMessage message;
        qint64 snoozeWakeMs = 0;  // > 0: snoozed until then
        bool snoozeBadge = false; // woke from snooze, not opened since
    };
    QList<Listed> listing(int limit = 5000) const;
    QList<CachedMessage> messages(const QString &labelId, int limit = 5000) const;
    int count(const QString &labelId = {}) const;
    void rebuildFts(); // repopulate messages_fts from messages (FTS5 only)
    // Full-text over subject/from/to/snippet/body: pass the raw toolbar text;
    // FTS5 MATCH is built via SearchQuery::toFts5, with LIKE fallback.
    // *fullText (if given) says whether the FTS5 index answered: then every
    // free-text word was matched, bodies included; the LIKE fallback looks for
    // one word only.
    QStringList search(const QString &ftsQuery, int limit = 200, bool *fullText = nullptr) const;

    // Local snooze state (not a Gmail label). wake_ms is UTC epoch ms.
    // had_inbox: restore INBOX on wake. badge: show "was snoozed" until opened.
    struct SnoozeRow {
        QString messageId;
        qint64 wakeMs = 0;
        bool hadInbox = false;
        bool badge = false;
    };
    void setSnooze(const QString &id, qint64 wakeMs, bool hadInbox);
    void clearSnooze(const QString &id);
    void markSnoozeWoke(const QString &id); // keep row as badge-only (wakeMs=0, badge=1)
    void clearSnoozeBadge(const QString &id);
    SnoozeRow snooze(const QString &id) const;
    QList<SnoozeRow> snoozes(bool activeOnly = true) const; // active = wakeMs > 0
    QStringList dueSnoozes(qint64 nowMs) const;
    QStringList snoozeBadgeIds() const;

private:
    bool exec(const QString &sql);
    bool migrate();
    void backfillFtsIfNeeded();
    QString m_conn;
    QString m_error;
    bool m_fts5 = false;
    int m_txDepth = 0;
};

} // namespace zmail
