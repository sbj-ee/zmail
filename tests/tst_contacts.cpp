#include "core/ContactStore.h"
#include "core/PeopleClient.h"
#include "core/AuthManager.h"
#include "ui/ContactsWindow.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QCheckBox>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QLineEdit>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QtTest>

using namespace zmail;

class TstContacts : public QObject
{
    Q_OBJECT
private slots:
    void autocompleteRanksContactsAboveFrecency()
    {
        ContactStore s;
        QVERIFY(s.open(QStringLiteral(":memory:")));
        s.addLocalContact(QStringLiteral("Priya Raman"), QStringLiteral("priya@example.com"));
        s.noteSent(QStringLiteral("marcus@example.com"));
        s.noteSent(QStringLiteral("marcus@example.com"));
        s.noteSent(QStringLiteral("marcus@example.com"));
        // Prefix "p" hits Priya (contact) and not marcus.
        auto hits = s.autocomplete(QStringLiteral("p"), 10);
        QVERIFY(!hits.isEmpty());
        QCOMPARE(hits.first().email, QStringLiteral("priya@example.com"));
        // Prefix "m" is frecency-only.
        hits = s.autocomplete(QStringLiteral("m"), 10);
        QCOMPARE(hits.first().email, QStringLiteral("marcus@example.com"));
        QVERIFY(hits.first().score < 1000.0);
    }

    void localAddIsLocalSource()
    {
        ContactStore s;
        QVERIFY(s.open(QStringLiteral(":memory:")));
        const QString id = s.addLocalContact(QStringLiteral("Ada"), QStringLiteral("ada@example.org"));
        QVERIFY(!id.isEmpty());
        QCOMPARE(s.contact(id).source, QStringLiteral("local"));
        s.setTrusted(QStringLiteral("ada@example.org"), true);
        QVERIFY(s.isTrusted(QStringLiteral("ada@example.org")));
    }

    void personJsonMapsEmails()
    {
        QJsonObject person{
            {QStringLiteral("resourceName"), QStringLiteral("people/c123")},
            {QStringLiteral("etag"), QStringLiteral("e1")},
            {QStringLiteral("names"), QJsonArray{QJsonObject{{QStringLiteral("displayName"), QStringLiteral("Ada")}}}},
            {QStringLiteral("emailAddresses"),
             QJsonArray{QJsonObject{{QStringLiteral("value"), QStringLiteral("Ada@Example.org")},
                                    {QStringLiteral("metadata"), QJsonObject{{QStringLiteral("primary"), true}}}}}},
        };
        Contact c = PeopleClient::contactFromPerson(person, QStringLiteral("google"));
        QCOMPARE(c.googleResource, QStringLiteral("people/c123"));
        QCOMPARE(c.emails.first().email, QStringLiteral("ada@example.org"));
        QVERIFY(c.emails.first().primary);
    }

    void syncTokenResyncClearsGoogle()
    {
        // Pure store behaviour the sync path relies on: expired token → clear google/other.
        ContactStore s;
        QVERIFY(s.open(QStringLiteral(":memory:")));
        Contact g;
        g.id = QStringLiteral("people/1");
        g.displayName = QStringLiteral("G");
        g.source = QStringLiteral("google");
        g.emails.append({QStringLiteral("g@example.com"), true});
        s.upsertContact(g);
        Contact o;
        o.id = QStringLiteral("otherContacts/1");
        o.displayName = QStringLiteral("O");
        o.source = QStringLiteral("other");
        o.emails.append({QStringLiteral("o@example.com"), true});
        s.upsertContact(o);
        s.addLocalContact(QStringLiteral("Local"), QStringLiteral("local@example.com"));
        s.setMeta(QStringLiteral("connectionsSyncToken"), QStringLiteral("tok"));
        s.clearGoogleContacts();
        s.setMeta(QStringLiteral("connectionsSyncToken"), {});
        QCOMPARE(s.contacts().size(), 1);
        QCOMPARE(s.contacts().first().source, QStringLiteral("local"));
        QVERIFY(s.meta(QStringLiteral("connectionsSyncToken")).isEmpty());
    }

    void contactScopesListed()
    {
        const QStringList s = AuthManager::contactScopes();
        QVERIFY(s.contains(QStringLiteral("https://www.googleapis.com/auth/contacts.readonly")));
        QVERIFY(s.contains(QStringLiteral("https://www.googleapis.com/auth/contacts.other.readonly")));
        QVERIFY(!AuthManager::scopes().contains(s.first())); // not on initial sign-in
    }

    static Contact google(ContactStore &s, const QString &id, const QString &name, const QString &email,
                          const QString &source = QStringLiteral("google"))
    {
        Contact c;
        c.id = id;
        c.displayName = name;
        c.source = source;
        c.emails.append({email, true});
        s.upsertContact(c);
        return c;
    }
    static QStringList names(const QList<Contact> &list)
    {
        QStringList out;
        for (const Contact &c : list) {
            out << c.displayName;
        }
        return out;
    }

    // Categories, extra fields, a comment and "hidden" per contact; all of it
    // survives a Google resync and a restart, and none of it is on the
    // contact rows Google's data replaces.
    void organisationSurvivesResyncAndRestart()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("contacts.db"));
        {
            ContactStore s;
            QVERIFY(s.open(path));
            QCOMPARE(s.meta(QStringLiteral("schema")), QStringLiteral("2"));
            google(s, QStringLiteral("people/1"), QStringLiteral("Ada Lovelace"), QStringLiteral("ada@example.org"));
            google(s, QStringLiteral("otherContacts/2"), QStringLiteral("noreply"), QStringLiteral("noreply@shop.example.com"),
                   QStringLiteral("other"));
            s.setCategories(QStringLiteral("people/1"), {QStringLiteral("Work"), QStringLiteral(" Family ")});
            s.setFields(QStringLiteral("people/1"), {{QStringLiteral("Phone"), QStringLiteral("555 0100")},
                                                     {QStringLiteral(" "), QStringLiteral("dropped: no name")},
                                                     {QStringLiteral("Company"), QStringLiteral("Analytical Engines")}});
            s.setComment(QStringLiteral("people/1"), QStringLiteral("Met at the 2026 conference."));
            s.setHidden({QStringLiteral("otherContacts/2")}, true);

            // A resync after an expired token: Google's rows go and come back.
            s.clearGoogleContacts();
            QCOMPARE(s.contacts().size(), 0);
            google(s, QStringLiteral("people/1"), QStringLiteral("Ada King"), QStringLiteral("ada@example.org"));
            google(s, QStringLiteral("otherContacts/2"), QStringLiteral("noreply"), QStringLiteral("noreply@shop.example.com"),
                   QStringLiteral("other"));
        }
        ContactStore s;
        QVERIFY(s.open(path));
        const Contact ada = s.contact(QStringLiteral("people/1"));
        QCOMPARE(ada.displayName, QStringLiteral("Ada King")); // Google's data is Google's
        QCOMPARE(ada.categories, (QStringList{QStringLiteral("Family"), QStringLiteral("Work")}));
        QCOMPARE(ada.fields, (QList<ContactField>{{QStringLiteral("Phone"), QStringLiteral("555 0100")},
                                                  {QStringLiteral("Company"), QStringLiteral("Analytical Engines")}}));
        QCOMPARE(ada.comment, QStringLiteral("Met at the 2026 conference."));
        QVERIFY(!ada.hidden);
        QVERIFY(s.contact(QStringLiteral("otherContacts/2")).hidden);
    }

    void hiddenContactsAreOmitted()
    {
        ContactStore s;
        QVERIFY(s.open(QStringLiteral(":memory:")));
        google(s, QStringLiteral("people/1"), QStringLiteral("Priya Raman"), QStringLiteral("priya@example.com"));
        google(s, QStringLiteral("people/2"), QStringLiteral("Promo Bot"), QStringLiteral("promo@shop.example.com"));
        s.noteSent(QStringLiteral("promo@shop.example.com")); // written to once, too
        s.setHidden({QStringLiteral("people/2")}, true);

        ContactQuery q;
        QCOMPARE(names(s.contacts(q)), QStringList{QStringLiteral("Priya Raman")});
        QCOMPARE(s.count(q), 1);
        q.show = ContactQuery::Show::Hidden;
        QCOMPARE(names(s.contacts(q)), QStringList{QStringLiteral("Promo Bot")});
        q.show = ContactQuery::Show::All;
        QCOMPARE(s.count(q), 2);

        // Not suggested when writing a message: by name, by address, or from
        // the addresses written to before.
        QStringList suggested;
        for (const AutocompleteHit &h : s.autocomplete(QStringLiteral("pr"), 10)) {
            suggested << h.email;
        }
        QCOMPARE(suggested, QStringList{QStringLiteral("priya@example.com")});

        s.setHidden({QStringLiteral("people/2")}, false);
        QCOMPARE(s.autocomplete(QStringLiteral("pr"), 10).size(), 2);
    }

    void categoriesFilterRenameMergeAndDelete()
    {
        ContactStore s;
        QVERIFY(s.open(QStringLiteral(":memory:")));
        google(s, QStringLiteral("people/1"), QStringLiteral("Ada"), QStringLiteral("ada@example.org"));
        google(s, QStringLiteral("people/2"), QStringLiteral("Brook"), QStringLiteral("brook@example.org"));
        google(s, QStringLiteral("people/3"), QStringLiteral("Cy"), QStringLiteral("cy@example.org"));
        QVERIFY(s.addCategory(QStringLiteral("Vendors"))); // empty ones are kept
        QVERIFY(!s.addCategory(QStringLiteral("  ")));
        s.addToCategory({QStringLiteral("people/1"), QStringLiteral("people/2")}, QStringLiteral("Work"));
        s.addToCategory({QStringLiteral("people/2")}, QStringLiteral("work")); // the same category
        s.addToCategory({QStringLiteral("people/2")}, QStringLiteral("Family"));
        s.setHidden({QStringLiteral("people/1")}, true);

        QList<CategoryCount> cats = s.categories();
        QCOMPARE(cats.size(), 3);
        QCOMPARE(cats.at(0).name, QStringLiteral("Family"));
        QCOMPARE(cats.at(1).name, QStringLiteral("Vendors"));
        QCOMPARE(cats.at(1).contacts, 0);
        QCOMPARE(cats.at(2).name, QStringLiteral("Work"));
        QCOMPARE(cats.at(2).contacts, 1); // Ada is hidden

        ContactQuery q;
        q.category = QStringLiteral("WORK");
        QCOMPARE(names(s.contacts(q)), QStringList{QStringLiteral("Brook")});
        ContactQuery none;
        none.uncategorized = true;
        QCOMPARE(names(s.contacts(none)), QStringList{QStringLiteral("Cy")});

        // Search reaches categories, field values and comments.
        s.setFields(QStringLiteral("people/3"), {{QStringLiteral("Company"), QStringLiteral("Zebra Freight")}});
        s.setComment(QStringLiteral("people/2"), QStringLiteral("Prefers phone calls"));
        ContactQuery find;
        find.search = QStringLiteral("zebra");
        QCOMPARE(names(s.contacts(find)), QStringList{QStringLiteral("Cy")});
        find.search = QStringLiteral("phone calls");
        QCOMPARE(names(s.contacts(find)), QStringList{QStringLiteral("Brook")});
        find.search = QStringLiteral("famil");
        QCOMPARE(names(s.contacts(find)), QStringList{QStringLiteral("Brook")});

        // Respell, then merge into an existing category.
        QVERIFY(s.renameCategory(QStringLiteral("work"), QStringLiteral("WORK")));
        QCOMPARE(s.contact(QStringLiteral("people/2")).categories,
                 (QStringList{QStringLiteral("Family"), QStringLiteral("WORK")}));
        QVERIFY(s.renameCategory(QStringLiteral("WORK"), QStringLiteral("Family")));
        QCOMPARE(s.contact(QStringLiteral("people/2")).categories, QStringList{QStringLiteral("Family")});
        QCOMPARE(s.contact(QStringLiteral("people/1")).categories, QStringList{QStringLiteral("Family")});
        QVERIFY(!s.renameCategory(QStringLiteral("Nope"), QStringLiteral("X")));

        // Deleting a category keeps its contacts.
        s.deleteCategory(QStringLiteral("Family"));
        QCOMPARE(s.categories().size(), 1);
        QVERIFY(s.contact(QStringLiteral("people/2")).categories.isEmpty());
        QCOMPARE(s.contacts(QStringLiteral(""), 500).size(), 3); // the old listing still lists everything
    }

    void contactsWindowOrganises()
    {
        ContactStore s;
        QVERIFY(s.open(QStringLiteral(":memory:")));
        google(s, QStringLiteral("people/1"), QStringLiteral("Ada"), QStringLiteral("ada@example.org"));
        google(s, QStringLiteral("people/2"), QStringLiteral("Brook"), QStringLiteral("brook@example.org"));
        google(s, QStringLiteral("otherContacts/3"), QStringLiteral("noreply"), QStringLiteral("noreply@shop.example.com"),
               QStringLiteral("other"));
        zmail::ui::ContactsWindow w(&s);
        w.show();
        auto *groups = w.findChild<QListWidget *>(QStringLiteral("contactGroups"));
        auto *list = w.findChild<QListWidget *>(QStringLiteral("contactsList"));
        auto *detail = w.findChild<QWidget *>(QStringLiteral("contactDetail"));
        auto *hidden = w.findChild<QCheckBox *>(QStringLiteral("contactHidden"));
        auto *checks = w.findChild<QListWidget *>(QStringLiteral("contactCategories"));
        auto *fields = w.findChild<QTableWidget *>(QStringLiteral("contactFields"));
        auto *comment = w.findChild<QPlainTextEdit *>(QStringLiteral("contactComment"));
        const auto groupTexts = [groups]() {
            QStringList out;
            for (int r = 0; r < groups->count(); ++r) {
                out << groups->item(r)->text();
            }
            return out;
        };
        QCOMPARE(groupTexts(), (QStringList{QStringLiteral("All Contacts  (3)"), QStringLiteral("Uncategorized  (3)"),
                                            QStringLiteral("Hidden  (0)")}));
        QCOMPARE(list->count(), 3);
        QVERIFY(!detail->isEnabled()); // nothing selected

        // Categories: made on the left, ticked on the right, or in bulk.
        w.newCategory(QStringLiteral("Work"));
        QCOMPARE(w.group(), QStringLiteral("Work"));
        QCOMPARE(list->count(), 0);
        w.showGroup(QString::fromLatin1(zmail::ui::ContactsWindow::kAll));
        w.selectContacts({QStringLiteral("people/1")});
        QVERIFY(detail->isEnabled());
        QCOMPARE(checks->count(), 1);
        checks->item(0)->setCheckState(Qt::Checked);
        QCOMPARE(s.contact(QStringLiteral("people/1")).categories, QStringList{QStringLiteral("Work")});
        QCOMPARE(w.selectedIds(), QStringList{QStringLiteral("people/1")}); // still on show
        w.selectContacts({QStringLiteral("people/1"), QStringLiteral("people/2")});
        QVERIFY(!detail->isEnabled()); // several: bulk actions only
        w.addSelectedToCategory(QStringLiteral("Friends"));
        QCOMPARE(s.contact(QStringLiteral("people/2")).categories, QStringList{QStringLiteral("Friends")});
        QVERIFY(groupTexts().contains(QStringLiteral("Friends  (2)")));
        QVERIFY(groupTexts().contains(QStringLiteral("Uncategorized  (1)")));

        // Fields and a comment, saved as they are typed.
        w.selectContacts({QStringLiteral("people/2")});
        w.addField(QStringLiteral("Phone"));
        fields->item(0, 1)->setText(QStringLiteral("555 0199"));
        w.addField(QStringLiteral("Company"));
        fields->item(1, 1)->setText(QStringLiteral("Brook & Co"));
        comment->setPlainText(QStringLiteral("Plumber. Call before 9."));
        Contact brook = s.contact(QStringLiteral("people/2"));
        QCOMPARE(brook.fields, (QList<ContactField>{{QStringLiteral("Phone"), QStringLiteral("555 0199")},
                                                    {QStringLiteral("Company"), QStringLiteral("Brook & Co")}}));
        QCOMPARE(brook.comment, QStringLiteral("Plumber. Call before 9."));
        fields->setCurrentCell(0, 0);
        w.removeCurrentField();
        QCOMPARE(s.contact(QStringLiteral("people/2")).fields.size(), 1);
        // Another contact's comment is its own.
        w.selectContacts({QStringLiteral("people/1")});
        QCOMPARE(comment->toPlainText(), QString());
        QCOMPARE(fields->rowCount(), 0);
        QCOMPARE(s.contact(QStringLiteral("people/2")).comment, QStringLiteral("Plumber. Call before 9."));
        w.findChild<QLineEdit *>(QStringLiteral("contactsSearch"))->setText(QStringLiteral("plumber"));
        QCOMPARE(list->count(), 1);
        w.findChild<QLineEdit *>(QStringLiteral("contactsSearch"))->clear();

        // Hide: gone from the list, found under Hidden, and back again.
        w.selectContacts({QStringLiteral("otherContacts/3")});
        hidden->setChecked(true);
        QVERIFY(s.contact(QStringLiteral("otherContacts/3")).hidden);
        QCOMPARE(list->count(), 2);
        QVERIFY(groupTexts().contains(QStringLiteral("Hidden  (1)")));
        w.showGroup(QString::fromLatin1(zmail::ui::ContactsWindow::kHidden));
        QCOMPARE(list->count(), 1);
        QCOMPARE(w.findChild<QPushButton *>(QStringLiteral("hideContactsButton"))->text(), QStringLiteral("Unhide"));
        w.selectContacts({QStringLiteral("otherContacts/3")});
        w.findChild<QPushButton *>(QStringLiteral("hideContactsButton"))->click();
        QVERIFY(!s.contact(QStringLiteral("otherContacts/3")).hidden);
        QCOMPARE(list->count(), 0);

        // Rename and delete from the left.
        w.showGroup(QStringLiteral("Friends"));
        QCOMPARE(list->count(), 2);
        w.renameCategory(QStringLiteral("Friends"), QStringLiteral("Mates"));
        QCOMPARE(w.group(), QStringLiteral("Mates"));
        QCOMPARE(list->count(), 2);
        w.selectContacts({QStringLiteral("people/1")});
        w.removeSelectedFromCategory(QStringLiteral("Mates"));
        QCOMPARE(list->count(), 1);
        w.deleteCategory(QStringLiteral("Mates"));
        QCOMPARE(w.group(), QString::fromLatin1(zmail::ui::ContactsWindow::kAll));
        QCOMPARE(list->count(), 3);
        QVERIFY(s.contact(QStringLiteral("people/2")).categories.isEmpty());
    }
};

QTEST_MAIN(TstContacts)
#include "tst_contacts.moc"
