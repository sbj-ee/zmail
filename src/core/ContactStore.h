#pragma once

#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <QList>
#include <QUuid>

namespace zmail {

struct ContactEmail {
    QString email;
    bool primary = false;
};

struct Contact {
    QString id;
    QString displayName;
    QString source; // "google" | "other" | "local"
    QString googleResource;
    QString etag;
    QList<ContactEmail> emails;
    bool trusted = false; // from overlay keyed by primary email
};

struct AutocompleteHit {
    QString email;
    QString displayName;
    double score = 0;
};

// Local SQLite cache for Google Contacts (read-only sync) + local-only contacts.
// Overlays (trusted) are keyed by email and never written back to Google.
// Sound/color overlays deferred until Rules land.
class ContactStore
{
public:
    ContactStore();
    ~ContactStore();
    ContactStore(const ContactStore &) = delete;
    ContactStore &operator=(const ContactStore &) = delete;

    static QString defaultPath(const QString &account);

    bool open(const QString &path);
    bool isOpen() const;
    QString lastError() const { return m_error; }

    QString meta(const QString &key) const;
    void setMeta(const QString &key, const QString &value);

    void upsertContact(const Contact &c); // replaces emails for this id
    void removeContact(const QString &id);
    void clearGoogleContacts(); // source google|other only
    Contact contact(const QString &id) const;
    QList<Contact> contacts(const QString &query = {}, int limit = 500) const;
    // Local-only contact from a sender (never writes to Google).
    QString addLocalContact(const QString &displayName, const QString &email);

    void setTrusted(const QString &email, bool trusted);
    bool isTrusted(const QString &email) const;

    void noteSent(const QString &email);
    void noteReceived(const QString &email);
    // Contacts first, then frecency from address_stats.
    QList<AutocompleteHit> autocomplete(const QString &prefix, int limit = 12) const;

private:
    bool exec(const QString &sql);
    bool migrate();
    QString m_conn;
    QString m_error;
};

} // namespace zmail
