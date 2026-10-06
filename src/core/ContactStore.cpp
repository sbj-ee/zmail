#include "ContactStore.h"
#include "Log.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QtMath>
#include <algorithm>

namespace zmail {

ContactStore::ContactStore()
    : m_conn(QStringLiteral("zmail-contacts-") + QUuid::createUuid().toString(QUuid::WithoutBraces))
{
}

ContactStore::~ContactStore()
{
    if (QSqlDatabase::contains(m_conn)) {
        {
            QSqlDatabase db = QSqlDatabase::database(m_conn, false);
            db.close();
        }
        QSqlDatabase::removeDatabase(m_conn);
    }
}

QString ContactStore::defaultPath(const QString &account)
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    QString safe = account;
    safe.replace(QLatin1Char('/'), QLatin1Char('_'));
    return base + QStringLiteral("/zmail/") + safe + QStringLiteral("/contacts.db");
}

bool ContactStore::exec(const QString &sql)
{
    QSqlQuery q(QSqlDatabase::database(m_conn));
    if (!q.exec(sql)) {
        m_error = q.lastError().text();
        qCWarning(lcSync) << "contacts SQL failed:" << m_error;
        return false;
    }
    return true;
}

bool ContactStore::isOpen() const
{
    return QSqlDatabase::contains(m_conn) && QSqlDatabase::database(m_conn, false).isOpen();
}

bool ContactStore::open(const QString &path)
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
    exec(QStringLiteral("PRAGMA foreign_keys=ON"));
    const bool ok =
        exec(QStringLiteral("CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT)")) &&
        exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS contacts (id TEXT PRIMARY KEY, display_name TEXT, source TEXT NOT NULL, "
            "google_resource TEXT, etag TEXT, updated_ms INTEGER DEFAULT 0)")) &&
        exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS contact_emails (contact_id TEXT NOT NULL REFERENCES contacts(id) ON DELETE CASCADE, "
            "email TEXT NOT NULL, is_primary INTEGER DEFAULT 0, PRIMARY KEY (contact_id, email))")) &&
        exec(QStringLiteral("CREATE INDEX IF NOT EXISTS contact_emails_email ON contact_emails(email)")) &&
        exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS contact_overlays (email TEXT PRIMARY KEY, trusted INTEGER DEFAULT 0)")) &&
        exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS address_stats (email TEXT PRIMARY KEY, sent_count INTEGER DEFAULT 0, "
            "last_sent_ms INTEGER DEFAULT 0, recv_count INTEGER DEFAULT 0)")) &&
        migrate();
    if (ok) {
        exec(QStringLiteral("INSERT OR IGNORE INTO meta(key, value) VALUES ('schema', '1')"));
    }
    return ok;
}

bool ContactStore::migrate()
{
    // schema 1 is the initial contacts layout; future ALTERs go here.
    return true;
}

QString ContactStore::meta(const QString &key) const
{
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("SELECT value FROM meta WHERE key = ?"));
    q.addBindValue(key);
    if (q.exec() && q.next()) {
        return q.value(0).toString();
    }
    return {};
}

void ContactStore::setMeta(const QString &key, const QString &value)
{
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("INSERT INTO meta(key, value) VALUES (?, ?) "
                             "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    q.addBindValue(key);
    q.addBindValue(value);
    q.exec();
}

void ContactStore::upsertContact(const Contact &c)
{
    QSqlDatabase db = QSqlDatabase::database(m_conn);
    db.transaction();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO contacts(id, display_name, source, google_resource, etag, updated_ms) VALUES (?,?,?,?,?,?) "
        "ON CONFLICT(id) DO UPDATE SET display_name=excluded.display_name, source=excluded.source, "
        "google_resource=excluded.google_resource, etag=excluded.etag, updated_ms=excluded.updated_ms"));
    q.addBindValue(c.id);
    q.addBindValue(c.displayName);
    q.addBindValue(c.source);
    q.addBindValue(c.googleResource);
    q.addBindValue(c.etag);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.exec();
    q.prepare(QStringLiteral("DELETE FROM contact_emails WHERE contact_id = ?"));
    q.addBindValue(c.id);
    q.exec();
    q.prepare(QStringLiteral("INSERT OR IGNORE INTO contact_emails(contact_id, email, is_primary) VALUES (?,?,?)"));
    for (const ContactEmail &e : c.emails) {
        const QString email = e.email.trimmed().toLower();
        if (email.isEmpty()) {
            continue;
        }
        q.addBindValue(c.id);
        q.addBindValue(email);
        q.addBindValue(e.primary ? 1 : 0);
        q.exec();
    }
    db.commit();
}

void ContactStore::removeContact(const QString &id)
{
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("DELETE FROM contacts WHERE id = ?"));
    q.addBindValue(id);
    q.exec();
}

void ContactStore::clearGoogleContacts()
{
    exec(QStringLiteral("DELETE FROM contacts WHERE source IN ('google', 'other')"));
}

Contact ContactStore::contact(const QString &id) const
{
    Contact c;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("SELECT id, display_name, source, google_resource, etag FROM contacts WHERE id = ?"));
    q.addBindValue(id);
    if (!(q.exec() && q.next())) {
        return {};
    }
    c.id = q.value(0).toString();
    c.displayName = q.value(1).toString();
    c.source = q.value(2).toString();
    c.googleResource = q.value(3).toString();
    c.etag = q.value(4).toString();
    q.prepare(QStringLiteral("SELECT email, is_primary FROM contact_emails WHERE contact_id = ?"));
    q.addBindValue(id);
    if (q.exec()) {
        while (q.next()) {
            c.emails.append({q.value(0).toString(), q.value(1).toBool()});
        }
    }
    if (!c.emails.isEmpty()) {
        c.trusted = isTrusted(c.emails.first().email);
    }
    return c;
}

QList<Contact> ContactStore::contacts(const QString &query, int limit) const
{
    QList<Contact> out;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    if (query.trimmed().isEmpty()) {
        q.prepare(QStringLiteral("SELECT id FROM contacts ORDER BY display_name COLLATE NOCASE LIMIT ?"));
        q.addBindValue(limit);
    } else {
        q.prepare(QStringLiteral(
            "SELECT DISTINCT c.id FROM contacts c LEFT JOIN contact_emails e ON e.contact_id = c.id "
            "WHERE c.display_name LIKE ? OR e.email LIKE ? "
            "ORDER BY c.display_name COLLATE NOCASE LIMIT ?"));
        const QString like = QLatin1Char('%') + query.trimmed() + QLatin1Char('%');
        q.addBindValue(like);
        q.addBindValue(like);
        q.addBindValue(limit);
    }
    if (q.exec()) {
        while (q.next()) {
            out.append(contact(q.value(0).toString()));
        }
    }
    return out;
}

QString ContactStore::addLocalContact(const QString &displayName, const QString &email)
{
    const QString addr = email.trimmed().toLower();
    if (addr.isEmpty()) {
        return {};
    }
    // Reuse existing local contact with this email.
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("SELECT contact_id FROM contact_emails WHERE email = ? LIMIT 1"));
    q.addBindValue(addr);
    if (q.exec() && q.next()) {
        return q.value(0).toString();
    }
    Contact c;
    c.id = QStringLiteral("local-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    c.displayName = displayName.trimmed().isEmpty() ? addr : displayName.trimmed();
    c.source = QStringLiteral("local");
    c.emails.append({addr, true});
    upsertContact(c);
    return c.id;
}

void ContactStore::setTrusted(const QString &email, bool trusted)
{
    const QString addr = email.trimmed().toLower();
    if (addr.isEmpty()) {
        return;
    }
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral(
        "INSERT INTO contact_overlays(email, trusted) VALUES (?, ?) "
        "ON CONFLICT(email) DO UPDATE SET trusted = excluded.trusted"));
    q.addBindValue(addr);
    q.addBindValue(trusted ? 1 : 0);
    q.exec();
}

bool ContactStore::isTrusted(const QString &email) const
{
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("SELECT trusted FROM contact_overlays WHERE email = ?"));
    q.addBindValue(email.trimmed().toLower());
    return q.exec() && q.next() && q.value(0).toBool();
}

void ContactStore::noteSent(const QString &email)
{
    const QString addr = email.trimmed().toLower();
    if (addr.isEmpty()) {
        return;
    }
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral(
        "INSERT INTO address_stats(email, sent_count, last_sent_ms, recv_count) VALUES (?, 1, ?, 0) "
        "ON CONFLICT(email) DO UPDATE SET sent_count = sent_count + 1, last_sent_ms = excluded.last_sent_ms"));
    q.addBindValue(addr);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.exec();
}

void ContactStore::noteReceived(const QString &email)
{
    const QString addr = email.trimmed().toLower();
    if (addr.isEmpty()) {
        return;
    }
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral(
        "INSERT INTO address_stats(email, sent_count, last_sent_ms, recv_count) VALUES (?, 0, 0, 1) "
        "ON CONFLICT(email) DO UPDATE SET recv_count = recv_count + 1"));
    q.addBindValue(addr);
    q.exec();
}

QList<AutocompleteHit> ContactStore::autocomplete(const QString &prefix, int limit) const
{
    const QString p = prefix.trimmed().toLower();
    if (p.isEmpty()) {
        return {};
    }
    const QString like = p + QLatin1Char('%');
    struct Row {
        QString email;
        QString name;
        bool isContact = false;
        int sent = 0;
        qint64 lastSent = 0;
    };
    QHash<QString, Row> byEmail;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral(
        "SELECT e.email, c.display_name FROM contact_emails e JOIN contacts c ON c.id = e.contact_id "
        "WHERE e.email LIKE ? OR lower(c.display_name) LIKE ? OR lower(c.display_name) LIKE ?"));
    q.addBindValue(like);
    q.addBindValue(like); // display name prefix
    q.addBindValue(QLatin1String("% ") + like); // word-prefix in multi-word names
    if (q.exec()) {
        while (q.next()) {
            Row &r = byEmail[q.value(0).toString()];
            r.email = q.value(0).toString();
            r.name = q.value(1).toString();
            r.isContact = true;
        }
    }
    q.prepare(QStringLiteral(
        "SELECT email, sent_count, last_sent_ms FROM address_stats WHERE email LIKE ?"));
    q.addBindValue(like);
    if (q.exec()) {
        while (q.next()) {
            Row &r = byEmail[q.value(0).toString()];
            r.email = q.value(0).toString();
            r.sent = q.value(1).toInt();
            r.lastSent = q.value(2).toLongLong();
        }
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<AutocompleteHit> hits;
    for (auto it = byEmail.begin(); it != byEmail.end(); ++it) {
        const Row &r = it.value();
        // Contacts rank above frecency; within each group score by recent sends.
        const double days = r.lastSent > 0 ? (now - r.lastSent) / 86400000.0 : 365.0;
        const double frecency = r.sent / (1.0 + days / 30.0);
        AutocompleteHit h;
        h.email = r.email;
        h.displayName = r.name;
        h.score = (r.isContact ? 1000.0 : 0.0) + frecency;
        hits.append(h);
    }
    std::sort(hits.begin(), hits.end(), [](const AutocompleteHit &a, const AutocompleteHit &b) {
        if (a.score != b.score) {
            return a.score > b.score;
        }
        return a.email < b.email;
    });
    if (hits.size() > limit) {
        hits.resize(limit);
    }
    return hits;
}

} // namespace zmail
