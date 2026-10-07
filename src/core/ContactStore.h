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

// A field the user added to a contact ("Phone", "Company", anything).
struct ContactField {
    QString name;
    QString value;
    bool operator==(const ContactField &) const = default;
};

struct Contact {
    QString id;
    QString displayName;
    QString source; // "google" | "other" | "local"
    QString googleResource;
    QString etag;
    QList<ContactEmail> emails;
    bool trusted = false; // from overlay keyed by primary email
    // The user's own organisation, kept beside the Google data (keyed by
    // contact id, never written to Google, untouched by a resync):
    QStringList categories;      // sorted
    QList<ContactField> fields;  // in the user's order
    QString comment;
    bool hidden = false;         // left out of the Contacts list and of autocomplete
};

// Which contacts to list. Search matches the name, an address, a category,
// a field value or the comment.
struct ContactQuery {
    enum class Show { Visible, Hidden, All };
    QString search;
    Show show = Show::Visible;
    QString category;           // only contacts in this category...
    bool uncategorized = false; // ...or only those in none
    int limit = 500;
};

struct CategoryCount {
    QString name;
    int contacts = 0; // visible ones
};

struct AutocompleteHit {
    QString email;
    QString displayName;
    double score = 0;
};

// Local SQLite cache for Google Contacts (read-only sync) + local-only contacts.
// Overlays (trusted) are keyed by email and never written back to Google.
// Categories, extra fields, comments and "hidden" are keyed by contact id in
// tables of their own, so a Google resync (which deletes and re-adds the
// contact rows) leaves them alone.
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
    QList<Contact> contacts(const ContactQuery &query) const;
    int count(const ContactQuery &query) const; // ignores limit

    // Categories exist on their own (an empty one is kept until deleted);
    // a contact can be in several. Names are trimmed; case is kept, but
    // "Family" and "family" are the same category.
    QList<CategoryCount> categories() const; // by name
    bool addCategory(const QString &name);
    bool renameCategory(const QString &from, const QString &to); // merges into an existing `to`
    void deleteCategory(const QString &name);                    // contacts stay, uncategorised
    void setCategories(const QString &contactId, const QStringList &names); // creates missing ones
    void addToCategory(const QStringList &contactIds, const QString &name);
    void setFields(const QString &contactId, const QList<ContactField> &fields); // nameless ones dropped
    void setComment(const QString &contactId, const QString &comment);
    void setHidden(const QStringList &contactIds, bool hidden);
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
    QString canonicalCategory(const QString &name) const; // the stored spelling, or empty
    QString m_conn;
    QString m_error;
};

} // namespace zmail
