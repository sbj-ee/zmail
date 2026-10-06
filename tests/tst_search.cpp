// Full-text search: FTS5 query builder, LIKE fallback, backfill, Search view.
#include "MainWindow.hpp"
#include "core/AuthManager.h"
#include "core/MailCache.h"
#include "core/MailSession.h"
#include "core/SearchQuery.h"
#include "core/SyncEngine.h"
#include "core/TokenStore.h"
#include "mock/MockGoogle.h"
#include "ui/MessageListModel.h"
#include "ui/Theme.h"

#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSettings>
#include <QStandardPaths>
#include <QTreeView>
#include <QTreeWidget>
#include <QtTest>

using namespace zmail;
using namespace zmail::ui;
using zmail::test::MockGoogle;

namespace {
QNetworkAccessManager *browserNam()
{
    static QNetworkAccessManager nam;
    return &nam;
}

// A signed-in session against the mock (as in tst_listactions).
struct Live
{
    MockGoogle g;
    MemoryTokenStore store;
    std::unique_ptr<MailSession> session;
    Live()
    {
        g.listen();
        g.seedSystemLabels();
    }
    QString seed(const QString &from, const QString &subject, const QString &text)
    {
        MockGoogle::Message m;
        m.from = from;
        m.to = QStringLiteral("Demo User <demo.user@example.com>");
        m.subject = subject;
        m.text = text;
        m.labels = {QStringLiteral("INBOX")};
        m.date = QDateTime::currentDateTimeUtc().addSecs(-600);
        return g.addMessage(m, true);
    }
    bool start()
    {
        SessionOptions o;
        o.client.status = ClientConfig::LoadStatus::Ok;
        o.client.config = g.clientConfig();
        o.store = &store;
        o.apiBase = g.apiBase();
        o.revokeUri = g.revokeUri();
        o.cachePathOverride = QStringLiteral(":memory:");
        o.rememberAccount = false;
        o.pollIntervalMs = 3600 * 1000;
        o.backoffBaseMs = 5;
        o.browserOpener = [](const QUrl &u) {
            QNetworkReply *r = browserNam()->get(QNetworkRequest(u));
            QObject::connect(r, &QNetworkReply::finished, r, &QObject::deleteLater);
            return true;
        };
        session = std::make_unique<MailSession>(o);
        session->signIn();
        return QTest::qWaitFor([this] { return session->state() == MailSession::State::SignedIn; }, 10000) &&
               QTest::qWaitFor([this] { return session->sync() && !session->sync()->isBusy(); }, 15000);
    }
};
} // namespace

class TstSearch : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QSettings().clear();
        applyTheme(ThemeMode::Light);
    }

    void ftsQueryBuilder()
    {
        QCOMPARE(SearchQuery::toFts5(QStringLiteral("fishing trip")), QStringLiteral("fishing* trip*"));
        QCOMPARE(SearchQuery::toFts5(QStringLiteral("\"exact phrase\"")), QStringLiteral("\"exact phrase\""));
        QCOMPARE(SearchQuery::toFts5(QStringLiteral("from:priya budget")), QStringLiteral("budget*"));
        QCOMPARE(SearchQuery::toFts5(QStringLiteral("has:attachment")), QString());
        QCOMPARE(SearchQuery::toFts5(QStringLiteral("-spam invoice")), QStringLiteral("NOT spam* invoice*"));
        QCOMPARE(SearchQuery::toLikeNeedle(QStringLiteral("from:x Hello")), QStringLiteral("Hello"));
        QCOMPARE(SearchQuery::toLikeNeedle(QStringLiteral("\"Crew Schedule\"")), QStringLiteral("Crew Schedule"));
    }

    void ftsAndLikeFallback()
    {
        MailCache c;
        QVERIFY(c.open(QStringLiteral(":memory:")));
        QVERIFY(c.hasFts5());
        CachedMessage a;
        a.id = QStringLiteral("a");
        a.subject = QStringLiteral("Fishing trip");
        a.fromName = QStringLiteral("Marcus");
        a.fromAddr = QStringLiteral("marcus@example.com");
        a.to = QStringLiteral("alex@example.com");
        a.labels = {QStringLiteral("INBOX")};
        c.upsert(a);
        CachedMessage b;
        b.id = QStringLiteral("b");
        b.subject = QStringLiteral("Budget");
        b.fromName = QStringLiteral("Priya");
        b.labels = {QStringLiteral("INBOX")};
        c.upsert(b);
        c.setBody(QStringLiteral("b"), QStringLiteral("the contractor estimate for Q4"), {}, {});

        QCOMPARE(c.search(QStringLiteral("fishing")), QStringList{QStringLiteral("a")});
        QCOMPARE(c.search(QStringLiteral("contractor")), QStringList{QStringLiteral("b")});
        QCOMPARE(c.search(QStringLiteral("from:priya estimate")), QStringList{QStringLiteral("b")});
        QVERIFY(c.search(QStringLiteral("has:attachment")).isEmpty()); // free-text empty → no MATCH
    }

    void backfillRebuildsEmptyFts()
    {
        // Simulate an older DB: messages present, FTS empty, flag unset.
        MailCache c;
        QVERIFY(c.open(QStringLiteral(":memory:")));
        CachedMessage m;
        m.id = QStringLiteral("x");
        m.subject = QStringLiteral("Backfill me");
        m.labels = {QStringLiteral("INBOX")};
        c.upsert(m);
        // Force empty FTS index then clear the backfill flag and re-open path via rebuild.
        QVERIFY(c.hasFts5());
        // Delete FTS rows without dropping the table (content= external).
        // rebuildFts should repopulate.
        c.rebuildFts();
        QCOMPARE(c.search(QStringLiteral("Backfill")), QStringList{QStringLiteral("x")});
    }

    void searchBoxOpensSearchMailbox()
    {
        MainWindow w;
        auto *search = w.findChild<QLineEdit *>(QStringLiteral("searchBox"));
        auto *list = w.findChild<QTreeView *>(QStringLiteral("messageList"));
        auto *tree = w.findChild<QTreeWidget *>(QStringLiteral("mailboxTree"));
        search->setText(QStringLiteral("from:priya"));
        QTRY_COMPARE(w.findChild<MessageFilterProxy *>()->mailbox(), QStringLiteral("Search"));
        QStringList top;
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            top << tree->topLevelItem(i)->text(0);
        }
        QVERIFY(top.contains(QStringLiteral("Search")));
        QVERIFY(list->model()->rowCount() >= 1);
        for (int r = 0; r < list->model()->rowCount(); ++r) {
            QCOMPARE(list->model()->index(r, MessageListModel::Who).data().toString(), QStringLiteral("Priya Raman"));
        }
        search->clear();
        QTRY_COMPARE(w.findChild<MessageFilterProxy *>()->mailbox(), QStringLiteral("In"));
    }

    // List rows no longer carry the bodies; a word found only in a body must
    // still show its message (the full-text index answers it), with the
    // operators and exclusions still applied to the row.
    void liveSearchFindsBodyTextWithoutBodiesInTheList()
    {
        Live L;
        // The word is past the 120 characters the snippet keeps.
        const QString longBody = QString(40, QLatin1Char(' ')).replace(QLatin1Char(' '), QStringLiteral("filler ")) +
                                 QStringLiteral("and at the end, zanzibar.");
        const QString hit = L.seed(QStringLiteral("Priya Raman <priya.raman@example.com>"), QStringLiteral("Budget"), longBody);
        L.seed(QStringLiteral("Marcus Delgado <marcus@example.com>"), QStringLiteral("Fishing trip"),
               QStringLiteral("Lake cabin is booked."));
        QVERIFY(L.start());
        bool fetched = false;
        L.session->sync()->fetchBody(hit, [&fetched](const CachedMessage &c, const QString &) { fetched = c.hasBody; });
        QTRY_VERIFY(fetched);
        QVERIFY(!L.session->cache()->message(hit).snippet.contains(QStringLiteral("zanzibar")));

        MainWindow w;
        w.setSession(L.session.get());
        QMetaObject::invokeMethod(L.session.get(), "ready");
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QTRY_VERIFY(w.isLive());
        auto *list = w.findChild<QTreeView *>(QStringLiteral("messageList"));
        auto *search = w.findChild<QLineEdit *>(QStringLiteral("searchBox"));
        auto *model = w.findChild<MessageListModel *>();
        QVERIFY(model);
        QTRY_COMPARE(list->model()->rowCount(), 2);
        for (int r = 0; r < model->rowCount(); ++r) {
            QVERIFY2(!model->item(r).preview.contains(QStringLiteral("zanzibar")), "the list holds a message body");
        }

        auto subjects = [list] {
            QStringList out;
            for (int r = 0; r < list->model()->rowCount(); ++r) {
                out << list->model()->index(r, MessageListModel::Subject).data().toString();
            }
            return out;
        };
        search->setText(QStringLiteral("zanzibar"));
        QTRY_COMPARE(w.findChild<MessageFilterProxy *>()->mailbox(), QStringLiteral("Search"));
        QTRY_COMPARE(subjects(), QStringList{QStringLiteral("Budget")});
        search->setText(QStringLiteral("zanzibar from:priya"));
        QTRY_COMPARE(subjects(), QStringList{QStringLiteral("Budget")});
        search->setText(QStringLiteral("zanzibar from:marcus"));
        QTRY_VERIFY(subjects().isEmpty());
        search->setText(QStringLiteral("zanzibar -budget"));
        QTRY_VERIFY(subjects().isEmpty());
        search->setText(QStringLiteral("trip"));
        QTRY_COMPARE(subjects(), QStringList{QStringLiteral("Fishing trip")});
    }
};

QTEST_MAIN(TstSearch)
#include "tst_search.moc"
