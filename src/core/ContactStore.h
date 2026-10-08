#pragma once

#include <QByteArray>
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
    QString nickname;            // a short name to type instead of the address (Eudora's nicknames)
    // The name or addresses were changed here and differ from Google's. A
    // resync keeps the edited ones; revertToSource() goes back to Google's.
    bool edited = false;
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
    QString email;       // empty: displayName is a nickname or category to type as it is
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
    // The contacts a query finds (every one: limit is ignored) as a JSON
    // document: name, addresses, source, and the user's categories, fields,
    // comment and hidden flag.
    QByteArray exportJson(const ContactQuery &query) const;

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
    void setNickname(const QString &contactId, const QString &nickname);

    // Everything about a contact in one go, from the edit dialog: its name
    // and addresses (kept across resyncs even for a Google contact, which
    // Google's own copy never sees), nickname, categories, fields, comment
    // and hidden. False if the contact doesn't exist.
    bool updateContact(const Contact &c);
    // A new contact of the user's own ("local"); its id. Unlike
    // addLocalContact() it doesn't reuse a contact with the same address.
    QString createContact(const QString &displayName, const QList<ContactEmail> &emails);
    // Drop the edits to a Google contact's name and addresses.
    bool revertToSource(const QString &contactId);

    // Nicknames, as in Eudora: a short name that stands for an address. A
    // contact's nickname stands for that contact (all of them, if several
    // share it); a category's name stands for everyone in it. Hidden
    // contacts are left out. Case doesn't matter. Empty: not a nickname.
    QStringList expandNickname(const QString &name) const; // "Name <address>" each
    // An address field with its nicknames written out: anything without an
    // "@" that is a nickname is replaced, the rest is left as typed, and an
    // address is listed once.
    QString expandRecipients(const QString &field) const;
    void setHidden(const QStringList &contactIds, bool hidden);
    // Local-only contact from a sender (never writes to Google).
    QString addLocalContact(const QString &displayName, const QString &email);
    bool hasEmail(const QString &email) const; // some contact, hidden or not, has this address

    void setTrusted(const QString &email, bool trusted);
    bool isTrusted(const QString &email) const;

    void noteSent(const QString &email);
    void noteReceived(const QString &email);
    // Contacts first, then frecency from address_stats.
    QList<AutocompleteHit> autocomplete(const QString &prefix, int limit = 12) const;

private:
    bool exec(const QString &sql);
    bool migrate();
    void writeIdentity(const QString &id, const QString &name, const QList<ContactEmail> &emails); // rows only
    QString canonicalCategory(const QString &name) const; // the stored spelling, or empty
    QString m_conn;
    QString m_error;
};

} // namespace zmail
