#include "MailCache.h"

#include "Log.h"
#include "SearchQuery.h"

#include <algorithm>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QSet>
#include <QUuid>

namespace zmail {

namespace {
CachedMessage fromRow(const QSqlQuery &q)
{
    CachedMessage m;
    m.id = q.value(0).toString();
    m.threadId = q.value(1).toString();
    m.historyId = q.value(2).toLongLong();
    m.internalDateMs = q.value(3).toLongLong();
    m.fromName = q.value(4).toString();
    m.fromAddr = q.value(5).toString();
    m.to = q.value(6).toString();
    m.subject = q.value(7).toString();
    m.snippet = q.value(8).toString();
    m.size = q.value(9).toLongLong();
    m.labels = q.value(10).toString().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    m.hasAttachment = q.value(11).toBool();
    m.hasBody = q.value(12).toBool();
    m.bodyText = q.value(13).toString();
    m.bodyHtml = q.value(14).toString();
    m.attachments = q.value(15).toString().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    m.cc = q.value(16).toString();
    m.replyTo = q.value(17).toString();
    m.messageIdHeader = q.value(18).toString();
    m.references = q.value(19).toString();
    return m;
}
const char *kCols = "id, thread_id, history_id, internal_date, from_name, from_addr, to_addr, subject, snippet, "
                    "size, labels, has_attachment, has_body, body_text, body_html, attachments, cc_addr, reply_to, "
                    "message_id_hdr, references_hdr";
// The same row without the bodies (fromRow() reads them as empty).
const char *kListCols = "id, thread_id, history_id, internal_date, from_name, from_addr, to_addr, subject, snippet, "
                        "size, labels, has_attachment, has_body, '', '', attachments, cc_addr, reply_to, "
                        "message_id_hdr, references_hdr";
} // namespace

MailCache::MailCache()
    : m_conn(QStringLiteral("zmail-cache-") + QUuid::createUuid().toString(QUuid::WithoutBraces))
{
}

MailCache::~MailCache()
{
    if (QSqlDatabase::contains(m_conn)) {
        {
            QSqlDatabase db = QSqlDatabase::database(m_conn, false);
            db.close();
        }
        QSqlDatabase::removeDatabase(m_conn);
    }
}

QString MailCache::defaultPath(const QString &account)
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    QString safe = account;
    safe.replace(QLatin1Char('/'), QLatin1Char('_'));
    return base + QStringLiteral("/zmail/") + safe + QStringLiteral("/zmail.db");
}

bool MailCache::exec(const QString &sql)
{
    QSqlQuery q(QSqlDatabase::database(m_conn));
    if (!q.exec(sql)) {
        m_error = q.lastError().text();
        qCWarning(lcSync) << "SQL failed:" << m_error;
        return false;
    }
    return true;
}

bool MailCache::isOpen() const
{
    return QSqlDatabase::contains(m_conn) && QSqlDatabase::database(m_conn, false).isOpen();
}

bool MailCache::open(const QString &path)
{
    if (path != QLatin1String(":memory:")) {
        const QString dir = QFileInfo(path).absolutePath();
        QDir().mkpath(dir);
        QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    }
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_conn);
    db.setDatabaseName(path);
    if (!db.open()) {
        m_error = db.lastError().text();
        return false;
    }
    if (path != QLatin1String(":memory:")) {
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
    exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    // WAL + NORMAL: commits don't fsync (checkpoints do). The file stays
    // consistent; a power cut can lose the last commits, which a sync refetches.
    exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
    if (path != QLatin1String(":memory:")) {
        for (const char *suffix : {"-wal", "-shm"}) {
            QFile::setPermissions(path + QLatin1String(suffix), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        }
    }
    exec(QStringLiteral("PRAGMA foreign_keys=ON"));
    const bool ok =
        exec(QStringLiteral("CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT)")) &&
        exec(QStringLiteral("CREATE TABLE IF NOT EXISTS labels (id TEXT PRIMARY KEY, name TEXT NOT NULL, "
                            "type TEXT, unread INTEGER DEFAULT 0, total INTEGER DEFAULT 0, color TEXT)")) &&
        exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS messages (rowid INTEGER PRIMARY KEY, id TEXT UNIQUE NOT NULL, "
            "thread_id TEXT, history_id INTEGER, internal_date INTEGER, from_name TEXT, from_addr TEXT, "
            "to_addr TEXT, subject TEXT, snippet TEXT, size INTEGER, labels TEXT, has_attachment INTEGER DEFAULT 0, "
            "has_body INTEGER DEFAULT 0, body_text TEXT, body_html TEXT, attachments TEXT)")) &&
        migrate() &&
        exec(QStringLiteral("CREATE INDEX IF NOT EXISTS messages_date ON messages(internal_date DESC)")) &&
        exec(QStringLiteral("CREATE TABLE IF NOT EXISTS message_labels (message_id TEXT NOT NULL "
                            "REFERENCES messages(id) ON DELETE CASCADE, label_id TEXT NOT NULL, "
                            "PRIMARY KEY (message_id, label_id))")) &&
        exec(QStringLiteral("CREATE INDEX IF NOT EXISTS message_labels_label ON message_labels(label_id)")) &&
        exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS snoozes (message_id TEXT PRIMARY KEY REFERENCES messages(id) ON DELETE CASCADE, "
            "wake_ms INTEGER NOT NULL DEFAULT 0, had_inbox INTEGER NOT NULL DEFAULT 0, "
            "badge INTEGER NOT NULL DEFAULT 0, created_ms INTEGER NOT NULL)")) &&
        exec(QStringLiteral("CREATE TABLE IF NOT EXISTS flags (message_id TEXT PRIMARY KEY, color TEXT NOT NULL)")) &&
        exec(QStringLiteral("CREATE TABLE IF NOT EXISTS purged (message_id TEXT PRIMARY KEY)")) &&
        exec(QStringLiteral("CREATE TABLE IF NOT EXISTS outbox (id INTEGER PRIMARY KEY AUTOINCREMENT, mime BLOB NOT NULL, "
                            "thread_id TEXT, draft_id TEXT, to_addr TEXT, cc_addr TEXT, subject TEXT, body_text TEXT, "
                            "created_ms INTEGER NOT NULL, error TEXT)"));
    if (!ok) {
        return false;
    }
    // FTS5 external-content index kept in sync by triggers.
    m_fts5 = exec(QStringLiteral(
        "CREATE VIRTUAL TABLE IF NOT EXISTS messages_fts USING fts5(subject, from_name, from_addr, to_addr, "
        "snippet, body_text, content='messages', content_rowid='rowid', tokenize='unicode61 remove_diacritics 2')"));
    if (m_fts5) {
        exec(QStringLiteral(
            "CREATE TRIGGER IF NOT EXISTS messages_ai AFTER INSERT ON messages BEGIN "
            "INSERT INTO messages_fts(rowid, subject, from_name, from_addr, to_addr, snippet, body_text) VALUES "
            "(new.rowid, new.subject, new.from_name, new.from_addr, new.to_addr, new.snippet, new.body_text); END"));
        exec(QStringLiteral(
            "CREATE TRIGGER IF NOT EXISTS messages_ad AFTER DELETE ON messages BEGIN "
            "INSERT INTO messages_fts(messages_fts, rowid, subject, from_name, from_addr, to_addr, snippet, body_text) "
            "VALUES ('delete', old.rowid, old.subject, old.from_name, old.from_addr, old.to_addr, old.snippet, "
            "old.body_text); END"));
        // Only when an indexed column is written: a label change (mark read,
        // move, trash) must not re-index the whole body. Caches from before
        // that have the trigger on every UPDATE, so it is always recreated.
        exec(QStringLiteral("DROP TRIGGER IF EXISTS messages_au"));
        exec(QStringLiteral(
            "CREATE TRIGGER messages_au AFTER UPDATE OF subject, from_name, from_addr, to_addr, snippet, body_text "
            "ON messages BEGIN "
            "INSERT INTO messages_fts(messages_fts, rowid, subject, from_name, from_addr, to_addr, snippet, body_text) "
            "VALUES ('delete', old.rowid, old.subject, old.from_name, old.from_addr, old.to_addr, old.snippet, "
            "old.body_text); "
            "INSERT INTO messages_fts(rowid, subject, from_name, from_addr, to_addr, snippet, body_text) VALUES "
            "(new.rowid, new.subject, new.from_name, new.from_addr, new.to_addr, new.snippet, new.body_text); END"));
        backfillFtsIfNeeded(); // caches from before full-text search: index existing mail once
    } else {
        qCWarning(lcSync) << "SQLite has no FTS5; search falls back to LIKE";
    }
    exec(QStringLiteral("INSERT OR IGNORE INTO meta(key, value) VALUES ('schema', '2')"));
    return true;
}

bool MailCache::migrate()
{
    // v0.3.0 columns for replies; 0.2.0 caches get them added in place.
    QSet<QString> have;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.exec(QStringLiteral("PRAGMA table_info(messages)"));
    while (q.next()) {
        have.insert(q.value(1).toString());
    }
    for (const char *col : {"cc_addr", "reply_to", "message_id_hdr", "references_hdr"}) {
        if (!have.contains(QLatin1String(col)) &&
            !exec(QStringLiteral("ALTER TABLE messages ADD COLUMN %1 TEXT").arg(QLatin1String(col)))) {
            return false;
        }
    }
    // Snooze table (local; schema meta "2"). CREATE IF NOT EXISTS is in open().
    if (meta(QStringLiteral("schema")).toInt() < 2) {
        if (!exec(QStringLiteral(
                "CREATE TABLE IF NOT EXISTS snoozes (message_id TEXT PRIMARY KEY REFERENCES messages(id) ON DELETE CASCADE, "
                "wake_ms INTEGER NOT NULL DEFAULT 0, had_inbox INTEGER NOT NULL DEFAULT 0, "
                "badge INTEGER NOT NULL DEFAULT 0, created_ms INTEGER NOT NULL)"))) {
            return false;
        }
        setMeta(QStringLiteral("schema"), QStringLiteral("2"));
    }
    return true;
}

bool MailCache::begin()
{
    if (m_txDepth++ > 0) {
        return true;
    }
    return QSqlDatabase::database(m_conn).transaction();
}

bool MailCache::commit()
{
    if (m_txDepth == 0) {
        return false;
    }
    if (--m_txDepth > 0) {
        return true;
    }
    return QSqlDatabase::database(m_conn).commit();
}

QVariant MailCache::pragma(const QString &name) const
{
    QSqlQuery q(QSqlDatabase::database(m_conn));
    if (q.exec(QStringLiteral("PRAGMA ") + name) && q.next()) {
        return q.value(0);
    }
    return {};
}

QString MailCache::meta(const QString &key) const
{
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("SELECT value FROM meta WHERE key = ?"));
    q.addBindValue(key);
    if (q.exec() && q.next()) {
        return q.value(0).toString();
    }
    return {};
}

void MailCache::setMeta(const QString &key, const QString &value)
{
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("INSERT INTO meta(key, value) VALUES (?, ?) "
                             "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    q.addBindValue(key);
    q.addBindValue(value);
    q.exec();
}

void MailCache::replaceLabels(const QList<CachedLabel> &labels)
{
    const Batch batch(*this);
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.exec(QStringLiteral("DELETE FROM labels"));
    q.prepare(QStringLiteral("INSERT INTO labels(id, name, type, unread, total, color) VALUES (?, ?, ?, ?, ?, ?)"));
    for (const CachedLabel &l : labels) {
        q.addBindValue(l.id);
        q.addBindValue(l.name);
        q.addBindValue(l.type);
        q.addBindValue(l.unread);
        q.addBindValue(l.total);
        q.addBindValue(l.color);
        q.exec();
    }
}

QList<CachedLabel> MailCache::labels() const
{
    QList<CachedLabel> out;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.exec(QStringLiteral("SELECT id, name, type, unread, total, color FROM labels ORDER BY type DESC, name COLLATE NOCASE"));
    while (q.next()) {
        out.append({q.value(0).toString(), q.value(1).toString(), q.value(2).toString(), q.value(3).toInt(),
                    q.value(4).toInt(), q.value(5).toString()});
    }
    return out;
}

void MailCache::upsert(const CachedMessage &m)
{
    const Batch batch(*this); // the row and its labels together
    QSqlDatabase db = QSqlDatabase::database(m_conn);
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO messages(id, thread_id, history_id, internal_date, from_name, from_addr, to_addr, subject, "
        "snippet, size, labels, has_attachment, cc_addr, reply_to, message_id_hdr, references_hdr) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(id) DO UPDATE SET thread_id=excluded.thread_id, history_id=excluded.history_id, "
        "internal_date=excluded.internal_date, from_name=excluded.from_name, from_addr=excluded.from_addr, "
        "to_addr=excluded.to_addr, subject=excluded.subject, snippet=excluded.snippet, size=excluded.size, "
        "labels=excluded.labels, has_attachment=MAX(has_attachment, excluded.has_attachment), "
        "cc_addr=excluded.cc_addr, reply_to=excluded.reply_to, "
        "message_id_hdr=COALESCE(NULLIF(excluded.message_id_hdr, ''), message_id_hdr), "
        "references_hdr=COALESCE(NULLIF(excluded.references_hdr, ''), references_hdr)"));
    q.addBindValue(m.id);
    q.addBindValue(m.threadId);
    q.addBindValue(m.historyId);
    q.addBindValue(m.internalDateMs);
    q.addBindValue(m.fromName);
    q.addBindValue(m.fromAddr);
    q.addBindValue(m.to);
    q.addBindValue(m.subject);
    q.addBindValue(m.snippet);
    q.addBindValue(m.size);
    q.addBindValue(m.labels.join(QLatin1Char(' ')));
    q.addBindValue(m.hasAttachment ? 1 : 0);
    q.addBindValue(m.cc);
    q.addBindValue(m.replyTo);
    q.addBindValue(m.messageIdHeader);
    q.addBindValue(m.references);
    if (!q.exec()) {
        m_error = q.lastError().text();
        qCWarning(lcSync) << "upsert failed:" << m_error;
        return;
    }
    setLabels(m.id, m.labels);
}

void MailCache::setBody(const QString &id, const QString &text, const QString &html, const QStringList &attachments)
{
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("UPDATE messages SET has_body = 1, body_text = ?, body_html = ?, attachments = ?, "
                             "has_attachment = ? WHERE id = ?"));
    q.addBindValue(text);
    q.addBindValue(html);
    q.addBindValue(attachments.join(QLatin1Char('\n')));
    q.addBindValue(attachments.isEmpty() ? 0 : 1);
    q.addBindValue(id);
    q.exec();
}

void MailCache::setLabels(const QString &id, const QStringList &labels)
{
    const Batch batch(*this);
    QSqlDatabase db = QSqlDatabase::database(m_conn);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE messages SET labels = ? WHERE id = ?"));
    q.addBindValue(labels.join(QLatin1Char(' ')));
    q.addBindValue(id);
    q.exec();
    q.prepare(QStringLiteral("DELETE FROM message_labels WHERE message_id = ?"));
    q.addBindValue(id);
    q.exec();
    q.prepare(QStringLiteral("INSERT OR IGNORE INTO message_labels(message_id, label_id) VALUES (?, ?)"));
    for (const QString &l : labels) {
        q.addBindValue(id);
        q.addBindValue(l);
        q.exec();
    }
}

void MailCache::modifyLabels(const QString &id, const QStringList &add, const QStringList &remove)
{
    // Just the labels column: this runs for every label change in a history page.
    QStringList labels;
    {
        QSqlQuery q(QSqlDatabase::database(m_conn));
        q.prepare(QStringLiteral("SELECT labels FROM messages WHERE id = ?"));
        q.addBindValue(id);
        if (!q.exec() || !q.next()) {
            return; // not cached
        }
        labels = q.value(0).toString().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    }
    for (const QString &r : remove) {
        labels.removeAll(r);
    }
    for (const QString &a : add) {
        if (!labels.contains(a)) {
            labels.append(a);
        }
    }
    setLabels(id, labels);
}

void MailCache::remove(const QString &id)
{
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("DELETE FROM messages WHERE id = ?"));
    q.addBindValue(id);
    q.exec();
}

void MailCache::clearMessages(bool keepSnoozed)
{
    const Batch batch(*this);
    if (keepSnoozed) {
        exec(QStringLiteral("DELETE FROM message_labels WHERE message_id NOT IN (SELECT message_id FROM snoozes)"));
        exec(QStringLiteral("DELETE FROM messages WHERE id NOT IN (SELECT message_id FROM snoozes)"));
    } else {
        exec(QStringLiteral("DELETE FROM message_labels"));
        exec(QStringLiteral("DELETE FROM messages"));
    }
    if (m_fts5) {
        exec(QStringLiteral("INSERT INTO messages_fts(messages_fts) VALUES ('rebuild')"));
    }
}

bool MailCache::contains(const QString &id) const
{
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("SELECT 1 FROM messages WHERE id = ?"));
    q.addBindValue(id);
    return q.exec() && q.next();
}

CachedMessage MailCache::message(const QString &id) const
{
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("SELECT %1 FROM messages WHERE id = ?").arg(QLatin1String(kCols)));
    q.addBindValue(id);
    if (q.exec() && q.next()) {
        return fromRow(q);
    }
    return {};
}

CachedMessage MailCache::summary(const QString &id) const
{
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("SELECT %1 FROM messages WHERE id = ?").arg(QLatin1String(kListCols)));
    q.addBindValue(id);
    if (q.exec() && q.next()) {
        return fromRow(q);
    }
    return {};
}

QStringList MailCache::messageIds(const QString &labelId) const
{
    QStringList ids;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("SELECT message_id FROM message_labels WHERE label_id = ?"));
    q.addBindValue(labelId);
    if (q.exec()) {
        while (q.next()) {
            ids.append(q.value(0).toString());
        }
    }
    return ids;
}

QList<CachedMessage> MailCache::messages(const QString &labelId, int limit) const
{
    QList<CachedMessage> out;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    if (labelId.isEmpty()) {
        q.prepare(QStringLiteral("SELECT %1 FROM messages ORDER BY internal_date DESC LIMIT ?").arg(QLatin1String(kCols)));
    } else {
        q.prepare(QStringLiteral("SELECT %1 FROM messages WHERE id IN (SELECT message_id FROM message_labels "
                                 "WHERE label_id = ?) ORDER BY internal_date DESC LIMIT ?")
                      .arg(QLatin1String(kCols)));
        q.addBindValue(labelId);
    }
    q.addBindValue(limit);
    if (q.exec()) {
        while (q.next()) {
            out.append(fromRow(q));
        }
    }
    return out;
}

QList<MailCache::Listed> MailCache::listing(int limit) const
{
    QList<Listed> out;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.setForwardOnly(true);
    q.prepare(QStringLiteral("SELECT %1, s.wake_ms, s.badge FROM messages LEFT JOIN snoozes s ON s.message_id = id "
                             "ORDER BY internal_date DESC LIMIT ?")
                  .arg(QLatin1String(kListCols)));
    q.addBindValue(limit);
    if (q.exec()) {
        while (q.next()) {
            Listed l;
            l.message = fromRow(q);
            l.snoozeWakeMs = q.value(20).toLongLong();
            l.snoozeBadge = q.value(21).toBool() && l.snoozeWakeMs == 0;
            out.append(std::move(l));
        }
    }
    return out;
}

int MailCache::count(const QString &labelId) const
{
    QSqlQuery q(QSqlDatabase::database(m_conn));
    if (labelId.isEmpty()) {
        q.prepare(QStringLiteral("SELECT COUNT(*) FROM messages"));
    } else {
        q.prepare(QStringLiteral("SELECT COUNT(*) FROM message_labels WHERE label_id = ?"));
        q.addBindValue(labelId);
    }
    if (q.exec() && q.next()) {
        return q.value(0).toInt();
    }
    return 0;
}

void MailCache::rebuildFts()
{
    if (!m_fts5) {
        return;
    }
    exec(QStringLiteral("INSERT INTO messages_fts(messages_fts) VALUES ('rebuild')"));
}

void MailCache::backfillFtsIfNeeded()
{
    if (!m_fts5) {
        return;
    }
    if (meta(QStringLiteral("fts_backfill")) == QLatin1String("1")) {
        return;
    }
    QSqlQuery q(QSqlDatabase::database(m_conn));
    qint64 msgs = 0, fts = 0;
    if (q.exec(QStringLiteral("SELECT COUNT(*) FROM messages")) && q.next()) {
        msgs = q.value(0).toLongLong();
    }
    if (q.exec(QStringLiteral("SELECT COUNT(*) FROM messages_fts")) && q.next()) {
        fts = q.value(0).toLongLong();
    }
    if (msgs > 0 && fts == 0) {
        rebuildFts();
    }
    setMeta(QStringLiteral("fts_backfill"), QStringLiteral("1"));
}

QStringList MailCache::search(const QString &userText, int limit, bool *fullText) const
{
    QStringList ids;
    bool viaFts = false;
    if (fullText) {
        *fullText = false;
    }
    if (userText.trimmed().isEmpty()) {
        return ids;
    }
    QSqlQuery q(QSqlDatabase::database(m_conn));
    if (m_fts5) {
        const QString match = SearchQuery::toFts5(userText);
        if (match.isEmpty()) {
            return ids;
        }
        q.prepare(QStringLiteral("SELECT m.id FROM messages_fts f JOIN messages m ON m.rowid = f.rowid "
                                 "WHERE messages_fts MATCH ? ORDER BY rank LIMIT ?"));
        q.addBindValue(match);
        q.addBindValue(limit);
    } else {
        const QString needle = SearchQuery::toLikeNeedle(userText);
        if (needle.isEmpty()) {
            return ids;
        }
        q.prepare(QStringLiteral(
            "SELECT id FROM messages WHERE subject LIKE ? OR snippet LIKE ? OR from_name LIKE ? "
            "OR from_addr LIKE ? OR to_addr LIKE ? OR body_text LIKE ? "
            "ORDER BY internal_date DESC LIMIT ?"));
        const QString like = QLatin1Char('%') + needle + QLatin1Char('%');
        for (int i = 0; i < 6; ++i) {
            q.addBindValue(like);
        }
        q.addBindValue(limit);
    }
    if (q.exec()) {
        viaFts = m_fts5;
        while (q.next()) {
            ids.append(q.value(0).toString());
        }
    } else if (m_fts5) {
        // Bad MATCH syntax → empty rather than warn-spam; LIKE fallback for this query.
        const QString needle = SearchQuery::toLikeNeedle(userText);
        if (!needle.isEmpty()) {
            QSqlQuery q2(QSqlDatabase::database(m_conn));
            q2.prepare(QStringLiteral(
                "SELECT id FROM messages WHERE subject LIKE ? OR snippet LIKE ? OR from_name LIKE ? "
                "OR from_addr LIKE ? OR to_addr LIKE ? OR body_text LIKE ? "
                "ORDER BY internal_date DESC LIMIT ?"));
            const QString like = QLatin1Char('%') + needle + QLatin1Char('%');
            for (int i = 0; i < 6; ++i) {
                q2.addBindValue(like);
            }
            q2.addBindValue(limit);
            if (q2.exec()) {
                while (q2.next()) {
                    ids.append(q2.value(0).toString());
                }
            }
        }
    }
    if (fullText) {
        *fullText = viaFts;
    }
    return ids;
}

void MailCache::setSnooze(const QString &id, qint64 wakeMs, bool hadInbox)
{
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral(
        "INSERT INTO snoozes(message_id, wake_ms, had_inbox, badge, created_ms) VALUES (?, ?, ?, 0, ?) "
        "ON CONFLICT(message_id) DO UPDATE SET wake_ms=excluded.wake_ms, had_inbox=excluded.had_inbox, "
        "badge=0, created_ms=excluded.created_ms"));
    q.addBindValue(id);
    q.addBindValue(wakeMs);
    q.addBindValue(hadInbox ? 1 : 0);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    if (!q.exec()) {
        m_error = q.lastError().text();
        qCWarning(lcSync) << "setSnooze failed:" << m_error;
    }
}

void MailCache::clearSnooze(const QString &id)
{
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("DELETE FROM snoozes WHERE message_id = ?"));
    q.addBindValue(id);
    q.exec();
}

void MailCache::markSnoozeWoke(const QString &id)
{
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("UPDATE snoozes SET wake_ms = 0, badge = 1 WHERE message_id = ?"));
    q.addBindValue(id);
    if (!q.exec() || q.numRowsAffected() == 0) {
        // Row may be missing if unsnoozed; insert badge-only.
        q.prepare(QStringLiteral(
            "INSERT INTO snoozes(message_id, wake_ms, had_inbox, badge, created_ms) VALUES (?, 0, 0, 1, ?) "
            "ON CONFLICT(message_id) DO UPDATE SET wake_ms=0, badge=1"));
        q.addBindValue(id);
        q.addBindValue(QDateTime::currentMSecsSinceEpoch());
        q.exec();
    }
}

void MailCache::clearSnoozeBadge(const QString &id)
{
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("DELETE FROM snoozes WHERE message_id = ? AND wake_ms = 0"));
    q.addBindValue(id);
    q.exec();
}

MailCache::SnoozeRow MailCache::snooze(const QString &id) const
{
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("SELECT message_id, wake_ms, had_inbox, badge FROM snoozes WHERE message_id = ?"));
    q.addBindValue(id);
    if (q.exec() && q.next()) {
        return {q.value(0).toString(), q.value(1).toLongLong(), q.value(2).toBool(), q.value(3).toBool()};
    }
    return {};
}

void MailCache::setFlag(const QString &id, const QString &color)
{
    if (id.isEmpty()) {
        return;
    }
    QSqlQuery q(QSqlDatabase::database(m_conn));
    if (color.isEmpty()) {
        q.prepare(QStringLiteral("DELETE FROM flags WHERE message_id = ?"));
        q.addBindValue(id);
    } else {
        q.prepare(QStringLiteral("INSERT INTO flags(message_id, color) VALUES (?, ?) "
                                 "ON CONFLICT(message_id) DO UPDATE SET color = excluded.color"));
        q.addBindValue(id);
        q.addBindValue(color);
    }
    q.exec();
}

QHash<QString, QString> MailCache::flags() const
{
    QHash<QString, QString> out;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    if (q.exec(QStringLiteral("SELECT message_id, color FROM flags"))) {
        while (q.next()) {
            out.insert(q.value(0).toString(), q.value(1).toString());
        }
    }
    return out;
}

void MailCache::setPurged(const QStringList &ids)
{
    const Batch batch(*this);
    exec(QStringLiteral("DELETE FROM purged"));
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("INSERT OR IGNORE INTO purged(message_id) VALUES (?)"));
    for (const QString &id : ids) {
        if (!id.isEmpty()) {
            q.addBindValue(id);
            q.exec();
        }
    }
}

QSet<QString> MailCache::purged() const
{
    QSet<QString> out;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    if (q.exec(QStringLiteral("SELECT message_id FROM purged"))) {
        while (q.next()) {
            out.insert(q.value(0).toString());
        }
    }
    return out;
}

qint64 MailCache::addQueued(const QueuedMessage &m)
{
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("INSERT INTO outbox(mime, thread_id, draft_id, to_addr, cc_addr, subject, body_text, created_ms) "
                             "VALUES (?,?,?,?,?,?,?,?)"));
    q.addBindValue(m.mime);
    q.addBindValue(m.threadId);
    q.addBindValue(m.draftId);
    q.addBindValue(m.to);
    q.addBindValue(m.cc);
    q.addBindValue(m.subject);
    q.addBindValue(m.text);
    q.addBindValue(m.createdMs > 0 ? m.createdMs : QDateTime::currentMSecsSinceEpoch());
    return q.exec() ? q.lastInsertId().toLongLong() : 0;
}

namespace {
MailCache::QueuedMessage queuedFromRow(const QSqlQuery &q, bool withMime)
{
    MailCache::QueuedMessage m;
    m.id = q.value(0).toLongLong();
    m.threadId = q.value(1).toString();
    m.draftId = q.value(2).toString();
    m.to = q.value(3).toString();
    m.cc = q.value(4).toString();
    m.subject = q.value(5).toString();
    m.text = q.value(6).toString();
    m.createdMs = q.value(7).toLongLong();
    m.error = q.value(8).toString();
    m.size = q.value(9).toLongLong();
    if (withMime) {
        m.mime = q.value(10).toByteArray();
    }
    return m;
}
const char *kQueuedColumns = "id, thread_id, draft_id, to_addr, cc_addr, subject, body_text, created_ms, error, length(mime)";
} // namespace

QList<MailCache::QueuedMessage> MailCache::queued(bool withMime) const
{
    QList<QueuedMessage> out;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    if (q.exec(QStringLiteral("SELECT %1%2 FROM outbox ORDER BY id")
                   .arg(QLatin1String(kQueuedColumns), withMime ? QStringLiteral(", mime") : QString()))) {
        while (q.next()) {
            out.append(queuedFromRow(q, withMime));
        }
    }
    return out;
}

MailCache::QueuedMessage MailCache::queuedMessage(qint64 id) const
{
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("SELECT %1, mime FROM outbox WHERE id = ?").arg(QLatin1String(kQueuedColumns)));
    q.addBindValue(id);
    return q.exec() && q.next() ? queuedFromRow(q, true) : QueuedMessage();
}

void MailCache::removeQueued(qint64 id)
{
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("DELETE FROM outbox WHERE id = ?"));
    q.addBindValue(id);
    q.exec();
}

void MailCache::setQueuedError(qint64 id, const QString &error)
{
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("UPDATE outbox SET error = ? WHERE id = ?"));
    q.addBindValue(error);
    q.addBindValue(id);
    q.exec();
}

QList<MailCache::SnoozeRow> MailCache::snoozes(bool activeOnly) const
{
    QList<SnoozeRow> out;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    if (activeOnly) {
        q.exec(QStringLiteral("SELECT message_id, wake_ms, had_inbox, badge FROM snoozes WHERE wake_ms > 0 "
                              "ORDER BY wake_ms ASC"));
    } else {
        q.exec(QStringLiteral("SELECT message_id, wake_ms, had_inbox, badge FROM snoozes ORDER BY wake_ms ASC"));
    }
    while (q.next()) {
        out.append({q.value(0).toString(), q.value(1).toLongLong(), q.value(2).toBool(), q.value(3).toBool()});
    }
    return out;
}

QStringList MailCache::dueSnoozes(qint64 nowMs) const
{
    QStringList ids;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("SELECT message_id FROM snoozes WHERE wake_ms > 0 AND wake_ms <= ?"));
    q.addBindValue(nowMs);
    if (q.exec()) {
        while (q.next()) {
            ids.append(q.value(0).toString());
        }
    }
    return ids;
}

QStringList MailCache::snoozeBadgeIds() const
{
    QStringList ids;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.exec(QStringLiteral("SELECT message_id FROM snoozes WHERE badge = 1 AND wake_ms = 0"));
    while (q.next()) {
        ids.append(q.value(0).toString());
    }
    return ids;
}

} // namespace zmail
