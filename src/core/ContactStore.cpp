#include "ContactStore.h"
#include "Log.h"
#include "MimeBuilder.h"
#include "MessageParser.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QVariant>
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
    // schema 1 is the initial contacts layout. Schema 2 (0.5.9) adds the
    // user's own organisation. No foreign keys to contacts: a Google resync
    // deletes those rows and must not take these with them.
    const bool ok =
        exec(QStringLiteral("CREATE TABLE IF NOT EXISTS categories (name TEXT PRIMARY KEY COLLATE NOCASE)")) &&
        exec(QStringLiteral("CREATE TABLE IF NOT EXISTS contact_categories (contact_id TEXT NOT NULL, "
                            "category TEXT NOT NULL COLLATE NOCASE, PRIMARY KEY (contact_id, category))")) &&
        exec(QStringLiteral("CREATE INDEX IF NOT EXISTS contact_categories_category ON contact_categories(category)")) &&
        exec(QStringLiteral("CREATE TABLE IF NOT EXISTS contact_fields (contact_id TEXT NOT NULL, position INTEGER NOT NULL, "
                            "name TEXT NOT NULL, value TEXT, PRIMARY KEY (contact_id, position))")) &&
        exec(QStringLiteral("CREATE TABLE IF NOT EXISTS contact_notes (contact_id TEXT PRIMARY KEY, "
                            "comment TEXT, hidden INTEGER DEFAULT 0)"));
    if (!ok) {
        return false;
    }
    // Schema 3 (0.6.1): nicknames.
    bool hasNickname = false;
    {
        QSqlQuery q(QSqlDatabase::database(m_conn));
        if (q.exec(QStringLiteral("PRAGMA table_info(contact_notes)"))) {
            while (q.next()) {
                hasNickname = hasNickname || q.value(1).toString() == QLatin1String("nickname");
            }
        }
    }
    if (!hasNickname && !exec(QStringLiteral("ALTER TABLE contact_notes ADD COLUMN nickname TEXT"))) {
        return false;
    }
    // Schema 4 (0.6.8): a contact's name and addresses can be edited here.
    // The edit is kept (name_edit / emails_edit) and so is what the source
    // last said (source_name / source_emails), so a resync can re-apply the
    // one and "revert" can restore the other.
    QStringList have;
    {
        QSqlQuery q(QSqlDatabase::database(m_conn));
        if (q.exec(QStringLiteral("PRAGMA table_info(contact_notes)"))) {
            while (q.next()) {
                have << q.value(1).toString();
            }
        }
    }
    for (const char *col : {"name_edit", "emails_edit", "source_name", "source_emails"}) {
        if (!have.contains(QLatin1String(col)) &&
            !exec(QStringLiteral("ALTER TABLE contact_notes ADD COLUMN %1 TEXT").arg(QLatin1String(col)))) {
            return false;
        }
    }
    setMeta(QStringLiteral("schema"), QStringLiteral("4"));
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

namespace {
QString emailsToJson(const QList<ContactEmail> &emails)
{
    QJsonArray arr;
    for (const ContactEmail &e : emails) {
        if (!e.email.trimmed().isEmpty()) {
            arr.append(QJsonObject{{QStringLiteral("email"), e.email.trimmed().toLower()}, {QStringLiteral("primary"), e.primary}});
        }
    }
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

QList<ContactEmail> emailsFromJson(const QString &json)
{
    QList<ContactEmail> out;
    for (const auto &v : QJsonDocument::fromJson(json.toUtf8()).array()) {
        out.append({v.toObject().value(QStringLiteral("email")).toString(), v.toObject().value(QStringLiteral("primary")).toBool()});
    }
    return out;
}
} // namespace

// The contacts / contact_emails rows for one contact: its name and addresses.
void ContactStore::writeIdentity(const QString &id, const QString &name, const QList<ContactEmail> &emails)
{
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("UPDATE contacts SET display_name = ?, updated_ms = ? WHERE id = ?"));
    q.addBindValue(name);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(id);
    q.exec();
    q.prepare(QStringLiteral("DELETE FROM contact_emails WHERE contact_id = ?"));
    q.addBindValue(id);
    q.exec();
    q.prepare(QStringLiteral("INSERT OR IGNORE INTO contact_emails(contact_id, email, is_primary) VALUES (?,?,?)"));
    for (const ContactEmail &e : emails) {
        const QString email = e.email.trimmed().toLower();
        if (email.isEmpty()) {
            continue;
        }
        q.addBindValue(id);
        q.addBindValue(email);
        q.addBindValue(e.primary ? 1 : 0);
        q.exec();
    }
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
    // Edited here? Then what just arrived is remembered as the source's
    // version (for "revert"), and the edit stays what is shown and used.
    QString nameEdit, emailsEdit;
    bool hasEdit = false;
    q.prepare(QStringLiteral("SELECT name_edit, emails_edit FROM contact_notes WHERE contact_id = ?"));
    q.addBindValue(c.id);
    if (q.exec() && q.next()) {
        nameEdit = q.value(0).toString();
        emailsEdit = q.value(1).toString();
        hasEdit = !q.value(0).isNull() || !q.value(1).isNull();
    }
    if (hasEdit) {
        q.prepare(QStringLiteral("UPDATE contact_notes SET source_name = ?, source_emails = ? WHERE contact_id = ?"));
        q.addBindValue(c.displayName);
        q.addBindValue(emailsToJson(c.emails));
        q.addBindValue(c.id);
        q.exec();
        writeIdentity(c.id, nameEdit, emailsFromJson(emailsEdit));
    } else {
        writeIdentity(c.id, c.displayName, c.emails);
    }
    db.commit();
}

bool ContactStore::updateContact(const Contact &c)
{
    const Contact old = contact(c.id);
    if (old.id.isEmpty()) {
        return false;
    }
    QList<ContactEmail> emails;
    QStringList seen;
    for (const ContactEmail &e : c.emails) {
        const QString email = e.email.trimmed().toLower();
        if (!email.isEmpty() && !seen.contains(email)) {
            seen << email;
            emails.append({email, e.primary});
        }
    }
    // Exactly one primary: the first marked, else the first.
    bool primary = false;
    for (ContactEmail &e : emails) {
        e.primary = e.primary && !primary;
        primary = primary || e.primary;
    }
    if (!primary && !emails.isEmpty()) {
        emails.first().primary = true;
    }
    QString name = c.displayName.trimmed();
    if (name.isEmpty() && !emails.isEmpty()) {
        name = emails.first().email; // a contact is called something
    }
    QStringList oldAddrs, newAddrs;
    for (const ContactEmail &e : old.emails) oldAddrs << (e.primary ? QLatin1Char('*') : QLatin1Char(' ')) + e.email;
    for (const ContactEmail &e : std::as_const(emails)) newAddrs << (e.primary ? QLatin1Char('*') : QLatin1Char(' ')) + e.email;
    oldAddrs.sort();
    newAddrs.sort();
    const bool identityChanged = name != old.displayName || oldAddrs != newAddrs;

    QSqlDatabase db = QSqlDatabase::database(m_conn);
    if (identityChanged) {
        db.transaction();
        QSqlQuery q(db);
        if (old.source != QLatin1String("local")) {
            // Synced from Google: keep the edit, and (the first time) what
            // Google had, so a resync re-applies one and revert restores the other.
            q.prepare(QStringLiteral(
                "INSERT INTO contact_notes(contact_id, name_edit, emails_edit, source_name, source_emails) VALUES (?,?,?,?,?) "
                "ON CONFLICT(contact_id) DO UPDATE SET name_edit = excluded.name_edit, emails_edit = excluded.emails_edit, "
                "source_name = COALESCE(contact_notes.source_name, excluded.source_name), "
                "source_emails = COALESCE(contact_notes.source_emails, excluded.source_emails)"));
            q.addBindValue(c.id);
            q.addBindValue(name);
            q.addBindValue(emailsToJson(emails));
            q.addBindValue(old.displayName);
            q.addBindValue(emailsToJson(old.emails));
            q.exec();
        }
        writeIdentity(c.id, name, emails);
        db.commit();
    }
    setNickname(c.id, c.nickname);
    setCategories(c.id, c.categories);
    setFields(c.id, c.fields);
    setComment(c.id, c.comment);
    setHidden({c.id}, c.hidden);
    return true;
}

QString ContactStore::createContact(const QString &displayName, const QList<ContactEmail> &emails)
{
    Contact c;
    c.id = QStringLiteral("local-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    c.source = QStringLiteral("local");
    c.displayName = displayName.trimmed();
    c.emails = emails;
    if (c.displayName.isEmpty()) {
        for (const ContactEmail &e : emails) {
            if (!e.email.trimmed().isEmpty()) {
                c.displayName = e.email.trimmed().toLower();
                break;
            }
        }
    }
    if (c.displayName.isEmpty()) {
        return {};
    }
    upsertContact(c);
    return c.id;
}

bool ContactStore::revertToSource(const QString &contactId)
{
    QSqlDatabase db = QSqlDatabase::database(m_conn);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT source_name, source_emails, name_edit, emails_edit FROM contact_notes WHERE contact_id = ?"));
    q.addBindValue(contactId);
    if (!(q.exec() && q.next()) || (q.value(2).isNull() && q.value(3).isNull())) {
        return false;
    }
    const QString name = q.value(0).toString();
    const QList<ContactEmail> emails = emailsFromJson(q.value(1).toString());
    db.transaction();
    writeIdentity(contactId, name, emails);
    q.prepare(QStringLiteral("UPDATE contact_notes SET name_edit = NULL, emails_edit = NULL, source_name = NULL, "
                             "source_emails = NULL WHERE contact_id = ?"));
    q.addBindValue(contactId);
    q.exec();
    return db.commit();
}

void ContactStore::removeContact(const QString &id)
{
    QSqlQuery q(QSqlDatabase::database(m_conn));
    // Removed for good (unlike a resync): its organisation goes with it.
    for (const char *table : {"contact_categories", "contact_fields", "contact_notes"}) {
        q.prepare(QStringLiteral("DELETE FROM %1 WHERE contact_id = ?").arg(QLatin1String(table)));
        q.addBindValue(id);
        q.exec();
    }
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
    q.prepare(QStringLiteral("SELECT email, is_primary FROM contact_emails WHERE contact_id = ? ORDER BY is_primary DESC, rowid"));
    q.addBindValue(id);
    if (q.exec()) {
        while (q.next()) {
            c.emails.append({q.value(0).toString(), q.value(1).toBool()});
        }
    }
    if (!c.emails.isEmpty()) {
        c.trusted = isTrusted(c.emails.first().email);
    }
    q.prepare(QStringLiteral("SELECT g.name FROM contact_categories cc JOIN categories g ON g.name = cc.category "
                             "WHERE cc.contact_id = ? ORDER BY g.name COLLATE NOCASE"));
    q.addBindValue(id);
    if (q.exec()) {
        while (q.next()) {
            c.categories.append(q.value(0).toString());
        }
    }
    q.prepare(QStringLiteral("SELECT name, value FROM contact_fields WHERE contact_id = ? ORDER BY position"));
    q.addBindValue(id);
    if (q.exec()) {
        while (q.next()) {
            c.fields.append({q.value(0).toString(), q.value(1).toString()});
        }
    }
    q.prepare(QStringLiteral("SELECT comment, hidden, nickname, name_edit, emails_edit FROM contact_notes WHERE contact_id = ?"));
    q.addBindValue(id);
    if (q.exec() && q.next()) {
        c.comment = q.value(0).toString();
        c.hidden = q.value(1).toBool();
        c.nickname = q.value(2).toString();
        c.edited = !q.value(3).isNull() || !q.value(4).isNull();
    }
    return c;
}

namespace {
// The WHERE clause for a ContactQuery (alias c = contacts) and its bindings.
QString contactFilter(const ContactQuery &query, QVariantList *binds)
{
    QStringList where;
    const QString hidden = QStringLiteral("EXISTS (SELECT 1 FROM contact_notes n WHERE n.contact_id = c.id AND n.hidden = 1)");
    if (query.show == ContactQuery::Show::Visible) {
        where << QStringLiteral("NOT ") + hidden;
    } else if (query.show == ContactQuery::Show::Hidden) {
        where << hidden;
    }
    if (query.uncategorized) {
        where << QStringLiteral("NOT EXISTS (SELECT 1 FROM contact_categories cc WHERE cc.contact_id = c.id)");
    } else if (!query.category.trimmed().isEmpty()) {
        where << QStringLiteral("EXISTS (SELECT 1 FROM contact_categories cc WHERE cc.contact_id = c.id AND cc.category = ?)");
        binds->append(query.category.trimmed());
    }
    const QString search = query.search.trimmed();
    if (!search.isEmpty()) {
        where << QStringLiteral(
            "(c.display_name LIKE ? "
            "OR EXISTS (SELECT 1 FROM contact_emails e WHERE e.contact_id = c.id AND e.email LIKE ?) "
            "OR EXISTS (SELECT 1 FROM contact_categories cc WHERE cc.contact_id = c.id AND cc.category LIKE ?) "
            "OR EXISTS (SELECT 1 FROM contact_fields f WHERE f.contact_id = c.id AND f.value LIKE ?) "
            "OR EXISTS (SELECT 1 FROM contact_notes n WHERE n.contact_id = c.id AND (n.comment LIKE ? OR n.nickname LIKE ?)))");
        const QString like = QLatin1Char('%') + search + QLatin1Char('%');
        for (int i = 0; i < 6; ++i) {
            binds->append(like);
        }
    }
    return where.isEmpty() ? QString() : QStringLiteral(" WHERE ") + where.join(QStringLiteral(" AND "));
}
} // namespace

QList<Contact> ContactStore::contacts(const ContactQuery &query) const
{
    QList<Contact> out;
    QVariantList binds;
    const QString where = contactFilter(query, &binds);
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("SELECT c.id FROM contacts c") + where +
              QStringLiteral(" ORDER BY c.display_name COLLATE NOCASE, c.id LIMIT ?"));
    for (const QVariant &b : std::as_const(binds)) {
        q.addBindValue(b);
    }
    q.addBindValue(query.limit);
    QStringList ids;
    if (q.exec()) {
        while (q.next()) {
            ids.append(q.value(0).toString());
        }
    }
    for (const QString &id : std::as_const(ids)) {
        out.append(contact(id));
    }
    return out;
}

int ContactStore::count(const ContactQuery &query) const
{
    QVariantList binds;
    const QString where = contactFilter(query, &binds);
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("SELECT COUNT(*) FROM contacts c") + where);
    for (const QVariant &b : std::as_const(binds)) {
        q.addBindValue(b);
    }
    return q.exec() && q.next() ? q.value(0).toInt() : 0;
}

QByteArray ContactStore::exportJson(const ContactQuery &query) const
{
    ContactQuery all = query;
    all.limit = -1; // SQLite: no limit
    QJsonArray contacts;
    for (const Contact &c : this->contacts(all)) {
        QJsonArray emails;
        for (const ContactEmail &e : c.emails) {
            emails.append(QJsonObject{{QStringLiteral("email"), e.email}, {QStringLiteral("primary"), e.primary}});
        }
        QJsonArray fields;
        for (const ContactField &f : c.fields) {
            fields.append(QJsonObject{{QStringLiteral("name"), f.name}, {QStringLiteral("value"), f.value}});
        }
        QJsonObject o{{QStringLiteral("name"), c.displayName},
                      {QStringLiteral("emails"), emails},
                      {QStringLiteral("source"), c.source},
                      {QStringLiteral("categories"), QJsonArray::fromStringList(c.categories)},
                      {QStringLiteral("fields"), fields},
                      {QStringLiteral("comment"), c.comment},
                      {QStringLiteral("nickname"), c.nickname},
                      {QStringLiteral("edited"), c.edited},
                      {QStringLiteral("hidden"), c.hidden},
                      {QStringLiteral("trusted"), c.trusted}};
        if (!c.googleResource.isEmpty()) {
            o.insert(QStringLiteral("googleResource"), c.googleResource);
        }
        contacts.append(o);
    }
    const QJsonObject doc{{QStringLiteral("format"), QStringLiteral("zmail-contacts")},
                          {QStringLiteral("version"), 1},
                          {QStringLiteral("exported"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                          {QStringLiteral("count"), contacts.size()},
                          {QStringLiteral("contacts"), contacts}};
    return QJsonDocument(doc).toJson(QJsonDocument::Indented);
}

QString ContactStore::canonicalCategory(const QString &name) const
{
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("SELECT name FROM categories WHERE name = ?"));
    q.addBindValue(name.trimmed());
    return q.exec() && q.next() ? q.value(0).toString() : QString();
}

QList<CategoryCount> ContactStore::categories() const
{
    QList<CategoryCount> out;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    // Counts are of contacts that exist and aren't hidden: what the list shows.
    if (q.exec(QStringLiteral(
            "SELECT g.name, (SELECT COUNT(*) FROM contact_categories cc JOIN contacts c ON c.id = cc.contact_id "
            "WHERE cc.category = g.name AND NOT EXISTS (SELECT 1 FROM contact_notes n WHERE n.contact_id = c.id AND n.hidden = 1)) "
            "FROM categories g ORDER BY g.name COLLATE NOCASE"))) {
        while (q.next()) {
            out.append({q.value(0).toString(), q.value(1).toInt()});
        }
    }
    return out;
}

bool ContactStore::addCategory(const QString &name)
{
    const QString n = name.trimmed();
    if (n.isEmpty()) {
        return false;
    }
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("INSERT OR IGNORE INTO categories(name) VALUES (?)"));
    q.addBindValue(n);
    return q.exec();
}

bool ContactStore::renameCategory(const QString &from, const QString &to)
{
    const QString old = canonicalCategory(from);
    const QString n = to.trimmed();
    if (old.isEmpty() || n.isEmpty()) {
        return false;
    }
    if (old == n) {
        return true;
    }
    QSqlDatabase db = QSqlDatabase::database(m_conn);
    db.transaction();
    QSqlQuery q(db);
    const bool sameCategory = old.compare(n, Qt::CaseInsensitive) == 0; // only the spelling changes
    if (sameCategory) {
        q.prepare(QStringLiteral("UPDATE categories SET name = ? WHERE name = ?"));
        q.addBindValue(n);
        q.addBindValue(old);
        q.exec();
    } else {
        // Into `to`, which may exist already: its members and these, once each.
        q.prepare(QStringLiteral("INSERT OR IGNORE INTO categories(name) VALUES (?)"));
        q.addBindValue(n);
        q.exec();
        q.prepare(QStringLiteral("INSERT OR IGNORE INTO contact_categories(contact_id, category) "
                                 "SELECT contact_id, ? FROM contact_categories WHERE category = ?"));
        q.addBindValue(n);
        q.addBindValue(old);
        q.exec();
        q.prepare(QStringLiteral("DELETE FROM contact_categories WHERE category = ?"));
        q.addBindValue(old);
        q.exec();
        q.prepare(QStringLiteral("DELETE FROM categories WHERE name = ?"));
        q.addBindValue(old);
        q.exec();
    }
    return db.commit();
}

void ContactStore::deleteCategory(const QString &name)
{
    QSqlDatabase db = QSqlDatabase::database(m_conn);
    db.transaction();
    QSqlQuery q(db);
    q.prepare(QStringLiteral("DELETE FROM contact_categories WHERE category = ?"));
    q.addBindValue(name.trimmed());
    q.exec();
    q.prepare(QStringLiteral("DELETE FROM categories WHERE name = ?"));
    q.addBindValue(name.trimmed());
    q.exec();
    db.commit();
}

void ContactStore::setCategories(const QString &contactId, const QStringList &names)
{
    if (contactId.isEmpty()) {
        return;
    }
    QSqlDatabase db = QSqlDatabase::database(m_conn);
    db.transaction();
    QSqlQuery q(db);
    q.prepare(QStringLiteral("DELETE FROM contact_categories WHERE contact_id = ?"));
    q.addBindValue(contactId);
    q.exec();
    db.commit();
    for (const QString &name : names) {
        addToCategory({contactId}, name);
    }
}

void ContactStore::addToCategory(const QStringList &contactIds, const QString &name)
{
    if (!addCategory(name)) {
        return;
    }
    const QString stored = canonicalCategory(name);
    QSqlDatabase db = QSqlDatabase::database(m_conn);
    db.transaction();
    QSqlQuery q(db);
    q.prepare(QStringLiteral("INSERT OR IGNORE INTO contact_categories(contact_id, category) VALUES (?, ?)"));
    for (const QString &id : contactIds) {
        if (id.isEmpty()) {
            continue;
        }
        q.addBindValue(id);
        q.addBindValue(stored);
        q.exec();
    }
    db.commit();
}

void ContactStore::setFields(const QString &contactId, const QList<ContactField> &fields)
{
    if (contactId.isEmpty()) {
        return;
    }
    QSqlDatabase db = QSqlDatabase::database(m_conn);
    db.transaction();
    QSqlQuery q(db);
    q.prepare(QStringLiteral("DELETE FROM contact_fields WHERE contact_id = ?"));
    q.addBindValue(contactId);
    q.exec();
    q.prepare(QStringLiteral("INSERT INTO contact_fields(contact_id, position, name, value) VALUES (?,?,?,?)"));
    int position = 0;
    for (const ContactField &f : fields) {
        if (f.name.trimmed().isEmpty()) {
            continue;
        }
        q.addBindValue(contactId);
        q.addBindValue(position++);
        q.addBindValue(f.name.trimmed());
        q.addBindValue(f.value);
        q.exec();
    }
    db.commit();
}

void ContactStore::setComment(const QString &contactId, const QString &comment)
{
    if (contactId.isEmpty()) {
        return;
    }
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("INSERT INTO contact_notes(contact_id, comment) VALUES (?, ?) "
                             "ON CONFLICT(contact_id) DO UPDATE SET comment = excluded.comment"));
    q.addBindValue(contactId);
    q.addBindValue(comment);
    q.exec();
}

void ContactStore::setNickname(const QString &contactId, const QString &nickname)
{
    if (contactId.isEmpty()) {
        return;
    }
    // One word, no commas or "@": it has to survive being typed in an address field.
    QString nick = nickname.trimmed();
    nick.remove(QLatin1Char(','));
    nick.remove(QLatin1Char(';'));
    nick.remove(QLatin1Char('@'));
    nick.remove(QLatin1Char('<'));
    nick.remove(QLatin1Char('>'));
    nick.remove(QLatin1Char('"'));
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("INSERT INTO contact_notes(contact_id, nickname) VALUES (?, ?) "
                             "ON CONFLICT(contact_id) DO UPDATE SET nickname = excluded.nickname"));
    q.addBindValue(contactId);
    q.addBindValue(nick);
    q.exec();
}

QStringList ContactStore::expandNickname(const QString &name) const
{
    const QString key = name.trimmed();
    if (key.isEmpty() || key.contains(QLatin1Char('@'))) {
        return {};
    }
    const QString visible = QStringLiteral(
        " AND NOT EXISTS (SELECT 1 FROM contact_notes h WHERE h.contact_id = c.id AND h.hidden = 1)");
    // The address to write to: the primary one, else the first.
    const QString address = QStringLiteral(
        "(SELECT e.email FROM contact_emails e WHERE e.contact_id = c.id ORDER BY e.is_primary DESC, e.email LIMIT 1)");
    QStringList out;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    const auto collect = [&]() {
        if (!q.exec()) {
            return;
        }
        while (q.next()) {
            const QString display = q.value(0).toString().trimmed();
            const QString email = q.value(1).toString();
            if (email.isEmpty()) {
                continue;
            }
            out << (display.isEmpty() || display == email ? email : QStringLiteral("%1 <%2>").arg(display, email));
        }
    };
    // A contact's own nickname first; a category of the same name only if no contact has it.
    q.prepare(QStringLiteral("SELECT c.display_name, %1 FROM contacts c JOIN contact_notes n ON n.contact_id = c.id "
                             "WHERE n.nickname = ? COLLATE NOCASE%2 ORDER BY c.display_name COLLATE NOCASE")
                  .arg(address, visible));
    q.addBindValue(key);
    collect();
    if (out.isEmpty()) {
        q.prepare(QStringLiteral("SELECT c.display_name, %1 FROM contacts c JOIN contact_categories cc ON cc.contact_id = c.id "
                                 "WHERE cc.category = ?%2 ORDER BY c.display_name COLLATE NOCASE")
                      .arg(address, visible));
        q.addBindValue(key);
        collect();
    }
    return out;
}

QString ContactStore::expandRecipients(const QString &field) const
{
    QStringList out;
    QSet<QString> seen;
    const auto add = [&](const QString &entry) {
        const QString email = MessageParser::splitAddress(entry).second.trimmed().toLower();
        const QString key = email.isEmpty() ? entry.trimmed().toLower() : email;
        if (!seen.contains(key)) {
            seen.insert(key);
            out << entry.trimmed();
        }
    };
    for (const QString &entry : MimeBuilder::splitAddresses(field)) {
        const QStringList expanded = expandNickname(entry);
        if (expanded.isEmpty()) {
            add(entry);
        } else {
            for (const QString &e : expanded) {
                add(e);
            }
        }
    }
    return out.join(QStringLiteral(", "));
}

void ContactStore::setHidden(const QStringList &contactIds, bool hidden)
{
    QSqlDatabase db = QSqlDatabase::database(m_conn);
    db.transaction();
    QSqlQuery q(db);
    q.prepare(QStringLiteral("INSERT INTO contact_notes(contact_id, hidden) VALUES (?, ?) "
                             "ON CONFLICT(contact_id) DO UPDATE SET hidden = excluded.hidden"));
    for (const QString &id : contactIds) {
        if (id.isEmpty()) {
            continue;
        }
        q.addBindValue(id);
        q.addBindValue(hidden ? 1 : 0);
        q.exec();
    }
    db.commit();
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

bool ContactStore::hasEmail(const QString &email) const
{
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("SELECT 1 FROM contact_emails WHERE email = ? LIMIT 1"));
    q.addBindValue(email.trimmed().toLower());
    return q.exec() && q.next();
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
        "WHERE (e.email LIKE ? OR lower(c.display_name) LIKE ? OR lower(c.display_name) LIKE ?) "
        "AND NOT EXISTS (SELECT 1 FROM contact_notes n WHERE n.contact_id = c.id AND n.hidden = 1)"));
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
        "SELECT email, sent_count, last_sent_ms FROM address_stats WHERE email LIKE ? "
        // Hiding a contact hides its addresses, however often they were written to.
        "AND email NOT IN (SELECT e.email FROM contact_emails e JOIN contact_notes n ON n.contact_id = e.contact_id "
        "WHERE n.hidden = 1)"));
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
    // Nicknames and categories first: typed as they are, expanded when the
    // message is addressed.
    q.prepare(QStringLiteral(
        "SELECT DISTINCT n.nickname FROM contact_notes n JOIN contacts c ON c.id = n.contact_id "
        "WHERE n.nickname LIKE ? AND n.nickname <> '' AND n.hidden = 0 ORDER BY n.nickname COLLATE NOCASE"));
    q.addBindValue(like);
    if (q.exec()) {
        while (q.next()) {
            hits.append({QString(), q.value(0).toString(), 3000.0});
        }
    }
    q.prepare(QStringLiteral("SELECT name FROM categories WHERE name LIKE ? ORDER BY name COLLATE NOCASE"));
    q.addBindValue(like);
    if (q.exec()) {
        while (q.next()) {
            const QString name = q.value(0).toString();
            if (!expandNickname(name).isEmpty()) { // an empty category addresses nobody
                hits.append({QString(), name, 2000.0});
            }
        }
    }
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
        return a.email != b.email ? a.email < b.email : a.displayName.compare(b.displayName, Qt::CaseInsensitive) < 0;
    });
    if (hits.size() > limit) {
        hits.resize(limit);
    }
    return hits;
}

} // namespace zmail
