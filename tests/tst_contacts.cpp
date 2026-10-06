#include "core/ContactStore.h"
#include "core/PeopleClient.h"
#include "core/AuthManager.h"

#include <QJsonArray>
#include <QJsonObject>
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
};

QTEST_MAIN(TstContacts)
#include "tst_contacts.moc"
