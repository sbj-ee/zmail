#include "core/ContactStore.h"
#include "core/PeopleClient.h"
#include "core/AuthManager.h"
#include "ui/ContactsWindow.h"
#include "ui/ContactEditDialog.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QCheckBox>
#include <QTextBrowser>
#include <QLabel>
#include <memory>
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
            QCOMPARE(s.meta(QStringLiteral("schema")), QStringLiteral("4"));
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
        auto *summary = w.findChild<QTextBrowser *>(QStringLiteral("contactSummary"));
        auto *editButton = w.findChild<QPushButton *>(QStringLiteral("editContactButton"));
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
        QVERIFY(!editButton->isEnabled()); // nothing selected
        QVERIFY(w.findChild<QPushButton *>(QStringLiteral("newContactButton")));

        // Categories: made on the left, or for several contacts at once.
        w.newCategory(QStringLiteral("Work"));
        QCOMPARE(w.group(), QStringLiteral("Work"));
        QCOMPARE(list->count(), 0);
        w.showGroup(QString::fromLatin1(zmail::ui::ContactsWindow::kAll));
        w.selectContacts({QStringLiteral("people/1"), QStringLiteral("people/2")});
        QVERIFY(!editButton->isEnabled()); // several: bulk actions only
        w.addSelectedToCategory(QStringLiteral("Friends"));
        QCOMPARE(s.contact(QStringLiteral("people/2")).categories, QStringList{QStringLiteral("Friends")});
        QVERIFY(groupTexts().contains(QStringLiteral("Friends  (2)")));
        QVERIFY(groupTexts().contains(QStringLiteral("Uncategorized  (1)")));

        // One selected: it can be read on the right, and Edit is offered.
        w.selectContacts({QStringLiteral("people/2")});
        QVERIFY(editButton->isEnabled());
        QVERIFY(summary->toPlainText().contains(QStringLiteral("Brook")));
        QVERIFY(summary->toPlainText().contains(QStringLiteral("brook@example.org")));
        QVERIFY(summary->toPlainText().contains(QStringLiteral("Friends")));

        // Hide in bulk: gone from the list, found under Hidden, and back again.
        w.selectContacts({QStringLiteral("otherContacts/3")});
        w.setSelectedHidden(true);
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

    // A contact is edited in a window of its own: its name, addresses,
    // nickname, categories, fields, comment and hidden, all at once, and
    // nothing is saved unless OK is pressed.
    void contactIsEditedInItsOwnDialog()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("contacts.db"));
        ContactStore s;
        QVERIFY(s.open(path));
        QCOMPARE(s.meta(QStringLiteral("schema")), QStringLiteral("4"));
        google(s, QStringLiteral("people/1"), QStringLiteral("ada.l"), QStringLiteral("ada@example.org"));
        google(s, QStringLiteral("people/2"), QStringLiteral("Brook"), QStringLiteral("brook@example.org"));
        s.addCategory(QStringLiteral("Work"));
        zmail::ui::ContactsWindow w(&s);
        using zmail::ui::ContactEditDialog;

        std::unique_ptr<ContactEditDialog> d(w.makeEditDialog(QStringLiteral("people/1")));
        QVERIFY(d->isModal());
        QCOMPARE(d->windowTitle(), QStringLiteral("Edit Contact"));
        auto *name = d->findChild<QLineEdit *>(QStringLiteral("editName"));
        auto *emails = d->findChild<QListWidget *>(QStringLiteral("editEmails"));
        auto *cats = d->findChild<QListWidget *>(QStringLiteral("editCategories"));
        auto *fields = d->findChild<QTableWidget *>(QStringLiteral("editFields"));
        QCOMPARE(name->text(), QStringLiteral("ada.l"));
        QCOMPARE(emails->count(), 1);
        QVERIFY(d->findChild<QLabel *>(QStringLiteral("editSourceNote"))); // from Google: says where changes are kept
        QVERIFY(d->findChild<QPushButton *>(QStringLiteral("editRevert"))->isHidden()); // nothing to revert yet

        // Change everything.
        name->setText(QStringLiteral("Ada Lovelace"));
        d->findChild<QLineEdit *>(QStringLiteral("editNickname"))->setText(QStringLiteral("ada"));
        d->addEmail(QStringLiteral("Countess@Example.com"));
        d->makeCurrentEmailPrimary(); // the one just added
        cats->item(0)->setCheckState(Qt::Checked);
        d->addCategory(QStringLiteral("Family")); // a new one, ticked
        d->addCategory(QStringLiteral("work"));   // exists: just ticked, not added twice
        QCOMPARE(cats->count(), 2);
        d->addField(QStringLiteral("Phone"));
        fields->item(0, 1)->setText(QStringLiteral("555 0100"));
        d->addField(QStringLiteral("Company"));
        fields->item(1, 1)->setText(QStringLiteral("Analytical Engines"));
        d->findChild<QPlainTextEdit *>(QStringLiteral("editComment"))->setPlainText(QStringLiteral("Met in 1833."));
        // Nothing is saved yet.
        QCOMPARE(s.contact(QStringLiteral("people/1")).displayName, QStringLiteral("ada.l"));

        // An address that isn't one keeps the dialog open and says why.
        d->addEmail(QStringLiteral("not an address"));
        d->show();
        d->accept();
        QVERIFY(d->isVisible());
        QVERIFY(d->findChild<QLabel *>(QStringLiteral("editProblem"))->text().contains(QStringLiteral("not an address")));
        emails->setCurrentRow(emails->count() - 1); // select the bad one, Remove
        d->removeCurrentEmail();
        d->accept();
        QVERIFY(!d->isVisible());
        QCOMPARE(w.applyEdit(d.get()), QStringLiteral("people/1"));

        Contact ada = s.contact(QStringLiteral("people/1"));
        QCOMPARE(ada.displayName, QStringLiteral("Ada Lovelace"));
        QCOMPARE(ada.emails.size(), 2);
        QCOMPARE(ada.emails.first().email, QStringLiteral("countess@example.com")); // primary first, lower-cased
        QVERIFY(ada.emails.first().primary && !ada.emails.last().primary);
        QCOMPARE(ada.nickname, QStringLiteral("ada"));
        QCOMPARE(ada.categories, (QStringList{QStringLiteral("Family"), QStringLiteral("Work")}));
        QCOMPARE(ada.fields, (QList<ContactField>{{QStringLiteral("Phone"), QStringLiteral("555 0100")},
                                                  {QStringLiteral("Company"), QStringLiteral("Analytical Engines")}}));
        QCOMPARE(ada.comment, QStringLiteral("Met in 1833."));
        QVERIFY(ada.edited);
        // The edit is what the rest of zmail uses: nicknames, autocomplete, "is this a contact".
        QCOMPARE(s.expandNickname(QStringLiteral("ada")), QStringList{QStringLiteral("Ada Lovelace <countess@example.com>")});
        QVERIFY(s.hasEmail(QStringLiteral("countess@example.com")));
        QCOMPARE(w.selectedIds(), QStringList{QStringLiteral("people/1")});
        QVERIFY(w.findChild<QTextBrowser *>(QStringLiteral("contactSummary"))->toPlainText().contains(QStringLiteral("Analytical Engines")));

        // A resync brings Google's version again; the edit stays, here and after a restart.
        s.clearGoogleContacts();
        google(s, QStringLiteral("people/1"), QStringLiteral("ada.l (work)"), QStringLiteral("ada@example.org"));
        google(s, QStringLiteral("people/2"), QStringLiteral("Brook"), QStringLiteral("brook@example.org"));
        QCOMPARE(s.contact(QStringLiteral("people/1")).displayName, QStringLiteral("Ada Lovelace"));
        {
            ContactStore again;
            QVERIFY(again.open(path));
            QCOMPARE(again.contact(QStringLiteral("people/1")).emails.first().email, QStringLiteral("countess@example.com"));
        }

        // Cancel changes nothing.
        d.reset(w.makeEditDialog(QStringLiteral("people/1")));
        d->findChild<QLineEdit *>(QStringLiteral("editName"))->setText(QStringLiteral("Someone Else"));
        d->reject();
        QCOMPARE(s.contact(QStringLiteral("people/1")).displayName, QStringLiteral("Ada Lovelace"));

        // "Use Google's": the name and addresses go back to what Google has
        // now; the nickname, categories, fields and comment are kept.
        d.reset(w.makeEditDialog(QStringLiteral("people/1")));
        auto *revert = d->findChild<QPushButton *>(QStringLiteral("editRevert"));
        QVERIFY(!revert->isHidden());
        revert->click();
        QVERIFY(d->revertRequested());
        w.applyEdit(d.get());
        ada = s.contact(QStringLiteral("people/1"));
        QCOMPARE(ada.displayName, QStringLiteral("ada.l (work)"));
        QCOMPARE(ada.emails.size(), 1);
        QCOMPARE(ada.emails.first().email, QStringLiteral("ada@example.org"));
        QVERIFY(!ada.edited);
        QCOMPARE(ada.nickname, QStringLiteral("ada"));
        QCOMPARE(ada.fields.size(), 2);

        // Changing only the comment doesn't count as editing Google's data.
        d.reset(w.makeEditDialog(QStringLiteral("people/2")));
        d->findChild<QPlainTextEdit *>(QStringLiteral("editComment"))->setPlainText(QStringLiteral("Plumber."));
        d->findChild<QCheckBox *>(QStringLiteral("editHidden"))->setChecked(true);
        d->accept();
        w.applyEdit(d.get());
        QVERIFY(!s.contact(QStringLiteral("people/2")).edited);
        QVERIFY(s.contact(QStringLiteral("people/2")).hidden);

        // New Contact: made here, in the category on show, and deletable.
        w.showGroup(QStringLiteral("Work"));
        d.reset(w.makeEditDialog(QString()));
        QCOMPARE(d->windowTitle(), QStringLiteral("New Contact"));
        QVERIFY(!d->findChild<QLabel *>(QStringLiteral("editSourceNote")));
        d->accept(); // nothing filled in: refused
        QVERIFY(!d->findChild<QLabel *>(QStringLiteral("editProblem"))->isHidden());
        d->findChild<QLineEdit *>(QStringLiteral("editName"))->setText(QStringLiteral("Cy Vance"));
        d->addEmail(QStringLiteral("cy@example.com"));
        d->accept();
        const QString cy = w.applyEdit(d.get());
        QVERIFY(cy.startsWith(QStringLiteral("local-")));
        QCOMPARE(s.contact(cy).source, QStringLiteral("local"));
        QCOMPARE(s.contact(cy).categories, QStringList{QStringLiteral("Work")});
        QCOMPARE(w.selectedIds(), QStringList{cy});
        // A contact of your own: edits are just edits, with nothing to revert to.
        d.reset(w.makeEditDialog(cy));
        d->findChild<QLineEdit *>(QStringLiteral("editName"))->setText(QStringLiteral("Cyrus Vance"));
        d->accept();
        w.applyEdit(d.get());
        QCOMPARE(s.contact(cy).displayName, QStringLiteral("Cyrus Vance"));
        QVERIFY(!s.contact(cy).edited);
        w.selectContacts({cy, QStringLiteral("people/1")});
        w.deleteSelected(); // only the one made here goes
        QVERIFY(s.contact(cy).id.isEmpty());
        QVERIFY(!s.contact(QStringLiteral("people/1")).id.isEmpty());
    }

    // Export: the contacts on show, or all of them, as JSON with the user's
    // own organisation, in a file nobody else can read.
    void exportsToJson()
    {
        ContactStore s;
        QVERIFY(s.open(QStringLiteral(":memory:")));
        google(s, QStringLiteral("people/1"), QStringLiteral("Ada \"Countess\" Lovelace"), QStringLiteral("ada@example.org"));
        google(s, QStringLiteral("people/2"), QStringLiteral("Brook"), QStringLiteral("brook@example.org"));
        google(s, QStringLiteral("otherContacts/3"), QStringLiteral("noreply"), QStringLiteral("noreply@shop.example.com"),
               QStringLiteral("other"));
        s.addToCategory({QStringLiteral("people/1")}, QStringLiteral("Work"));
        s.setFields(QStringLiteral("people/1"), {{QStringLiteral("Phone"), QStringLiteral("555 0100")}});
        s.setComment(QStringLiteral("people/1"), QStringLiteral("Line one\nline two \u2014 caf\u00e9"));
        s.setHidden({QStringLiteral("otherContacts/3")}, true);

        QTemporaryDir dir;
        zmail::ui::ContactsWindow w(&s);
        QVERIFY(w.findChild<QPushButton *>(QStringLiteral("contactsExportButton")));
        const auto read = [](const QString &path) {
            QFile f(path);
            return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject();
        };

        // What is on show: the Work category.
        w.showGroup(QStringLiteral("Work"));
        const QString shown = dir.filePath(QStringLiteral("shown.json"));
        QVERIFY(w.exportJson(shown, false));
        QJsonObject doc = read(shown);
        QCOMPARE(doc.value(QStringLiteral("format")).toString(), QStringLiteral("zmail-contacts"));
        QCOMPARE(doc.value(QStringLiteral("count")).toInt(), 1);
        const QJsonObject ada = doc.value(QStringLiteral("contacts")).toArray().first().toObject();
        QCOMPARE(ada.value(QStringLiteral("name")).toString(), QStringLiteral("Ada \"Countess\" Lovelace"));
        QCOMPARE(ada.value(QStringLiteral("emails")).toArray().first().toObject().value(QStringLiteral("email")).toString(),
                 QStringLiteral("ada@example.org"));
        QCOMPARE(ada.value(QStringLiteral("categories")).toArray().first().toString(), QStringLiteral("Work"));
        QCOMPARE(ada.value(QStringLiteral("fields")).toArray().first().toObject().value(QStringLiteral("value")).toString(),
                 QStringLiteral("555 0100"));
        QCOMPARE(ada.value(QStringLiteral("comment")).toString(), QStringLiteral("Line one\nline two \u2014 caf\u00e9"));
        QCOMPARE(ada.value(QStringLiteral("hidden")).toBool(), false);
        QCOMPARE(QFile::permissions(shown) & (QFileDevice::ReadGroup | QFileDevice::ReadOther | QFileDevice::WriteGroup |
                                               QFileDevice::WriteOther),
                 QFileDevice::Permissions());

        // Everything, hidden ones too, whatever is on show.
        const QString all = dir.filePath(QStringLiteral("all.json"));
        QVERIFY(w.exportJson(all, true));
        doc = read(all);
        QCOMPARE(doc.value(QStringLiteral("count")).toInt(), 3);
        int hidden = 0;
        for (const auto &v : doc.value(QStringLiteral("contacts")).toArray()) {
            hidden += v.toObject().value(QStringLiteral("hidden")).toBool();
        }
        QCOMPARE(hidden, 1);

        // Nowhere to write: reported, not ignored.
        QVERIFY(!w.exportJson(dir.filePath(QStringLiteral("no/such/dir/x.json")), true));

        // More than the list's display limit still exports in full.
        for (int i = 0; i < 2100; ++i) {
            google(s, QStringLiteral("people/x%1").arg(i), QStringLiteral("Bulk %1").arg(i),
                   QStringLiteral("bulk%1@example.com").arg(i));
        }
        ContactQuery q;
        q.show = ContactQuery::Show::All;
        QCOMPARE(QJsonDocument::fromJson(s.exportJson(q)).object().value(QStringLiteral("contacts")).toArray().size(), 2103);
    }

    // Nicknames, as in Eudora: a contact's short name, or a category's name
    // for everyone in it, typed in an address field.
    void nicknamesExpandToAddresses()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("contacts.db"));
        {
            ContactStore s;
            QVERIFY(s.open(path));
            google(s, QStringLiteral("people/1"), QStringLiteral("Ada Lovelace"), QStringLiteral("ada@example.org"));
            google(s, QStringLiteral("people/2"), QStringLiteral("Brook Plumbing"), QStringLiteral("brook@example.org"));
            google(s, QStringLiteral("people/3"), QStringLiteral("Cy Vance"), QStringLiteral("cy@example.com"));
            google(s, QStringLiteral("people/4"), QStringLiteral("dana@example.net"), QStringLiteral("dana@example.net"));
            s.setNickname(QStringLiteral("people/1"), QStringLiteral("  ada, <the> \"countess\"@ "));
            QCOMPARE(s.contact(QStringLiteral("people/1")).nickname, QStringLiteral("ada the countess")); // nothing that breaks an address list
            s.setNickname(QStringLiteral("people/1"), QStringLiteral("ada"));
            s.setNickname(QStringLiteral("people/2"), QStringLiteral("plumber"));
            s.addToCategory({QStringLiteral("people/2"), QStringLiteral("people/3"), QStringLiteral("people/4")}, QStringLiteral("Crew"));
        }
        ContactStore s; // nicknames are kept across a restart
        QVERIFY(s.open(path));
        QCOMPARE(s.expandNickname(QStringLiteral("ADA")), QStringList{QStringLiteral("Ada Lovelace <ada@example.org>")});
        QCOMPARE(s.expandNickname(QStringLiteral("nobody")), QStringList{});
        QCOMPARE(s.expandNickname(QStringLiteral("ada@example.org")), QStringList{}); // an address is never a nickname
        // A category: everyone in it, by name; a bare address has no name to repeat.
        QCOMPARE(s.expandNickname(QStringLiteral("crew")),
                 (QStringList{QStringLiteral("Brook Plumbing <brook@example.org>"), QStringLiteral("Cy Vance <cy@example.com>"),
                              QStringLiteral("dana@example.net")}));
        // Hidden contacts are left out, of a category and of their own nickname.
        s.setHidden({QStringLiteral("people/3")}, true);
        QCOMPARE(s.expandNickname(QStringLiteral("Crew")).size(), 2);
        s.setHidden({QStringLiteral("people/1")}, true);
        QCOMPARE(s.expandNickname(QStringLiteral("ada")), QStringList{});
        s.setHidden({QStringLiteral("people/1"), QStringLiteral("people/3")}, false);
        // A contact's nickname wins over a category of the same name; two
        // contacts sharing a nickname are both written to.
        s.setNickname(QStringLiteral("people/3"), QStringLiteral("crew"));
        QCOMPARE(s.expandNickname(QStringLiteral("crew")), QStringList{QStringLiteral("Cy Vance <cy@example.com>")});
        s.setNickname(QStringLiteral("people/3"), QStringLiteral("plumber"));
        QCOMPARE(s.expandNickname(QStringLiteral("plumber")).size(), 2);
        s.setNickname(QStringLiteral("people/3"), QString());

        // A whole field: nicknames written out, the rest as typed, nobody twice.
        QCOMPARE(s.expandRecipients(QStringLiteral("ada, Someone Else <else@example.com>; crew , brook@example.org, typo")),
                 QStringLiteral("Ada Lovelace <ada@example.org>, Someone Else <else@example.com>, "
                                "Brook Plumbing <brook@example.org>, Cy Vance <cy@example.com>, dana@example.net, typo"));
        QCOMPARE(s.expandRecipients(QStringLiteral("\"Lovelace, Ada\" <ada@example.org>")),
                 QStringLiteral("\"Lovelace, Ada\" <ada@example.org>"));
        QCOMPARE(s.expandRecipients(QString()), QString());

        // Suggested while typing, ahead of addresses; found by search; exported.
        const QList<AutocompleteHit> hits = s.autocomplete(QStringLiteral("cr"), 10);
        QVERIFY(!hits.isEmpty());
        QCOMPARE(hits.first().displayName, QStringLiteral("Crew"));
        QVERIFY(hits.first().email.isEmpty());
        QCOMPARE(s.autocomplete(QStringLiteral("plu"), 10).first().displayName, QStringLiteral("plumber"));
        s.addCategory(QStringLiteral("Crickets")); // nobody in it: nothing to suggest
        for (const AutocompleteHit &h : s.autocomplete(QStringLiteral("cr"), 10)) {
            QVERIFY(h.displayName != QLatin1String("Crickets"));
        }
        ContactQuery q;
        q.search = QStringLiteral("plumber");
        QCOMPARE(names(s.contacts(q)), QStringList{QStringLiteral("Brook Plumbing")});
        QVERIFY(s.exportJson(q).contains("\"nickname\": \"plumber\""));

        // In the Contacts window: set in the contact's edit dialog, shown in its summary.
        zmail::ui::ContactsWindow w(&s);
        std::unique_ptr<zmail::ui::ContactEditDialog> edit(w.makeEditDialog(QStringLiteral("people/4")));
        auto *nick = edit->findChild<QLineEdit *>(QStringLiteral("editNickname"));
        QVERIFY(nick && nick->text().isEmpty());
        nick->setText(QStringLiteral("dana"));
        edit->accept();
        w.applyEdit(edit.get());
        QCOMPARE(s.contact(QStringLiteral("people/4")).nickname, QStringLiteral("dana"));
        w.selectContacts({QStringLiteral("people/2")});
        QVERIFY(w.findChild<QTextBrowser *>(QStringLiteral("contactSummary"))->toPlainText().contains(QStringLiteral("plumber")));
    }
};

QTEST_MAIN(TstContacts)
#include "tst_contacts.moc"
