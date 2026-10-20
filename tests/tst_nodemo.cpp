// A real zmail never shows the built-in sample mail (Priya Raman's "Q4
// budget review", example.com people, the sample folders): not before the
// account's mail is in, not in the preview once it is, not after signing
// out. Sample mail is for `zmail --demo` and the other test programs only;
// this program is built WITHOUT tests/DemoData.cpp, like the real app.
#include "MainWindow.hpp"
#include "core/AppPasswordAccount.h"
#include "core/MailSession.h"
#include "core/TokenStore.h"
#include "mock/FakeImapServer.h"
#include "ui/MessageListModel.h"
#include "ui/MessageView.h"
#include "ui/SafeHtmlView.h"

#include <QSettings>
#include <QStandardPaths>
#include <QTest>
#include <QTreeView>
#include <QTreeWidget>

using namespace zmail;
using zmail::test::FakeImapServer;
using zmail::ui::MessageView;

namespace {

QByteArray mail(const char *from, const char *subject, const char *date, const char *body)
{
    return QByteArray("From: ") + from + "\r\nTo: Stephen Johnson <stephen@example.com>\r\nSubject: " + subject
           + "\r\nDate: " + date + "\r\nMessage-ID: <" + QByteArray(subject).toHex().left(24)
           + "@mail.test>\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n" + body + "\r\n";
}

// Every sign of the sample data a window could show.
QString sampleTrace(MainWindow &w)
{
    const QString needles[] = {QStringLiteral("Priya"), QStringLiteral("Q4 budget"), QStringLiteral("example.com"),
                               QStringLiteral("Alex Morgan"), QStringLiteral("Q4-budget-draft")};
    QStringList seen;
    auto *view = w.findChild<MessageView *>(QStringLiteral("messageView"));
    seen << view->message().subject << view->message().from << view->body()->toPlainText();
    if (auto *list = w.findChild<QTreeView *>(QStringLiteral("messageList"))) {
        for (int r = 0; r < list->model()->rowCount(); ++r) {
            for (int c = 0; c < list->model()->columnCount(); ++c) {
                seen << list->model()->index(r, c).data().toString();
            }
        }
    }
    auto *tree = w.findChild<QTreeWidget *>(QStringLiteral("mailboxTree"));
    for (QTreeWidgetItemIterator it(tree); *it; ++it) {
        seen << (*it)->text(0);
        if ((*it)->data(0, Qt::UserRole).toString().startsWith(QLatin1String("label:"))) {
            return QStringLiteral("sample folder ") + (*it)->text(0);
        }
    }
    const QString all = seen.join(QLatin1Char('\n'));
    for (const QString &n : needles) {
        if (all.contains(n)) {
            return n;
        }
    }
    return {};
}

bool previewEmpty(MainWindow &w)
{
    auto *view = w.findChild<MessageView *>(QStringLiteral("messageView"));
    return view->message().id.isEmpty() && view->message().subject.isEmpty()
           && view->body()->toPlainText().trimmed().isEmpty();
}

struct Live
{
    FakeImapServer imap;
    MemoryTokenStore store;
    std::unique_ptr<MailSession> session;
    std::unique_ptr<MainWindow> w;

    Live()
    {
        imap.deliver(mail("Dana Whitfield <dana.whitfield@lakeshore-electric.example>", "Panel schedule for the Oak St job",
                          "Mon, 12 Oct 2026 09:14:00 -0500", "Revised panel schedule attached for review."));
        imap.deliver(mail("Chicago Public Library <noreply@chipublib.org>", "Your hold is ready for pickup",
                          "Mon, 12 Oct 2026 13:02:00 -0500", "Your hold is ready at the Lincoln Park branch."));
        imap.deliver(mail("Mark Ostrowski <mark.ostrowski@example.net>", "Saturday tee time",
                          "Tue, 13 Oct 2026 07:45:00 -0500", "8:10 at Harborside, still on?"));
        imap.deliver(mail("Northside Power <billing@northside-power.example>", "Your October bill is ready",
                          "Tue, 13 Oct 2026 08:30:00 -0500", "Amount due: $112.40."));
        store.values.insert(AppPasswordAccount::appPasswordKey(imap.user), imap.password);
        AppPasswordAccount a;
        a.email = imap.user;
        a.imapHost = QStringLiteral("127.0.0.1");
        a.imapPort = imap.port();
        a.smtpHost = QStringLiteral("127.0.0.1");
        a.smtpPort = 1;
        QSettings s;
        s.clear();
        a.save(s);
        SessionOptions o;
        o.client.status = ClientConfig::LoadStatus::Missing;
        o.store = &store;
        o.cachePathOverride = QStringLiteral(":memory:");
        o.contactsPathOverride = QStringLiteral(":memory:");
        o.imapPlaintextForTests = true;
        o.pollIntervalMs = 3600 * 1000;
        session = std::make_unique<MailSession>(o);
        w = std::make_unique<MainWindow>();
        w->resize(1100, 700);
        w->show();
    }
    void start()
    {
        w->setSession(session.get());
        session->restoreSaved();
    }
    ~Live()
    {
        w.reset();
        session.reset();
        QSettings().clear();
    }
    int rows() const { return w->findChild<QTreeView *>(QStringLiteral("messageList"))->model()->rowCount(); }
};

} // namespace

class TstNoDemo : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(!MainWindow::demoData()); // the default, as in the real app
    }

    // Before any account is attached: an empty window, not sample mail.
    void freshWindowHasNoSampleMail()
    {
        MainWindow w;
        w.show();
        QCOMPARE(sampleTrace(w), QString());
        QVERIFY(previewEmpty(w));
        QCOMPARE(w.findChild<QTreeView *>(QStringLiteral("messageList"))->model()->rowCount(), 0);
    }

    // Stephen's case: the In list has real mail, none selected, empty preview.
    void liveAccountPreviewStartsEmpty()
    {
        Live l;
        QCOMPARE(sampleTrace(*l.w), QString()); // also while connecting / backing off
        l.start();
        QTRY_VERIFY_WITH_TIMEOUT(l.w->isLive(), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(l.rows(), 4, 15000);
        QTest::qWait(300);
        QCOMPARE(sampleTrace(*l.w), QString());
        QVERIFY(previewEmpty(*l.w));
        if (const QString dir = qEnvironmentVariable("ZMAIL_SHOT_DIR"); !dir.isEmpty()) {
            l.w->grab().save(dir + QStringLiteral("/empty-preview-real-account.png"));
        }
        // Signing out leaves an empty window, not the sample data.
        l.session->signOut();
        QTRY_VERIFY_WITH_TIMEOUT(!l.w->isLive(), 10000);
        QCOMPARE(sampleTrace(*l.w), QString());
        QVERIFY(previewEmpty(*l.w));
    }

    // --demo shows the sample mail; when an account's mail arrives, the
    // sample message that was in the preview goes (the 0.7.2 bug).
    void demoPreviewIsClearedWhenTheAccountIsReady()
    {
        MainWindow::setDemoData(true);
        {
            Live l;
            QVERIFY(!sampleTrace(*l.w).isEmpty()); // sample mail on show
            auto *list = l.w->findChild<QTreeView *>(QStringLiteral("messageList"));
            list->setCurrentIndex(list->model()->index(0, 0));
            QTRY_VERIFY(!previewEmpty(*l.w));
            l.start();
            QTRY_VERIFY_WITH_TIMEOUT(l.w->isLive(), 15000);
            QTRY_COMPARE_WITH_TIMEOUT(l.rows(), 4, 15000);
            QTest::qWait(300);
            auto *view = l.w->findChild<MessageView *>(QStringLiteral("messageView"));
            QVERIFY2(!view->message().from.contains(QLatin1String("example.com")), qPrintable(view->message().from));
            QVERIFY(!view->body()->toPlainText().contains(QLatin1String("Q4 budget")));
        }
        MainWindow::setDemoData(false);
    }
};

QTEST_MAIN(TstNoDemo)
#include "tst_nodemo.moc"
