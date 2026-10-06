#include "MailCache.h"

#include "Log.h"

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
            "badge INTEGER NOT NULL DEFAULT 0, created_ms INTEGER NOT NULL)"));
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
        exec(QStringLiteral(
            "CREATE TRIGGER IF NOT EXISTS messages_au AFTER UPDATE ON messages BEGIN "
            "INSERT INTO messages_fts(messages_fts, rowid, subject, from_name, from_addr, to_addr, snippet, body_text) "
            "VALUES ('delete', old.rowid, old.subject, old.from_name, old.from_addr, old.to_addr, old.snippet, "
            "old.body_text); "
            "INSERT INTO messages_fts(rowid, subject, from_name, from_addr, to_addr, snippet, body_text) VALUES "
            "(new.rowid, new.subject, new.from_name, new.from_addr, new.to_addr, new.snippet, new.body_text); END"));
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
    return QSqlDatabase::database(m_conn).transaction();
}

bool MailCache::commit()
{
    return QSqlDatabase::database(m_conn).commit();
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
    QSqlDatabase db = QSqlDatabase::database(m_conn);
    db.transaction();
    QSqlQuery q(db);
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
    db.commit();
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
    if (!contains(id)) {
        return;
    }
    QStringList labels = message(id).labels;
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

void MailCache::clearMessages()
{
    exec(QStringLiteral("DELETE FROM message_labels"));
    exec(QStringLiteral("DELETE FROM messages"));
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

QStringList MailCache::search(const QString &ftsQuery, int limit) const
{
    QStringList ids;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    if (m_fts5) {
        q.prepare(QStringLiteral("SELECT m.id FROM messages_fts f JOIN messages m ON m.rowid = f.rowid "
                                 "WHERE messages_fts MATCH ? ORDER BY rank LIMIT ?"));
        q.addBindValue(ftsQuery);
    } else {
        q.prepare(QStringLiteral("SELECT id FROM messages WHERE subject LIKE ? OR snippet LIKE ? OR from_name LIKE ? "
                                 "LIMIT ?"));
        const QString like = QLatin1Char('%') + ftsQuery + QLatin1Char('%');
        q.addBindValue(like);
        q.addBindValue(like);
        q.addBindValue(like);
    }
    q.addBindValue(limit);
    if (q.exec()) {
        while (q.next()) {
            ids.append(q.value(0).toString());
        }
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
