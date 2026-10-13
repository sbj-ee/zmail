// "Gmail (app password)" account, part 1: settings, keyring storage, the
// Test button (IMAP LOGIN + SMTP AUTH against local fakes) and the dialog.
// Every password here is a made-up test value; nothing talks to Google.
#include "LogCapture.h"
#include "core/AppPasswordAccount.h"
#include "core/ConnectionTester.h"
#include "core/MailSession.h"
#include "core/TokenStore.h"
#include "ui/AppPasswordDialog.h"
#include "ui/ConnectDialog.h"
#include "mock/FakeMailServer.h"

#include <QDir>
#include <QFile>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

using namespace zmail;

using zmail::test::FakeServer;

namespace {

// A made-up app password (Google's format: 16 letters, shown in groups of 4).
const QString kFakePw = QStringLiteral("abcdefghijklmnop");
const QString kFakePwSpaced = QStringLiteral("abcd efgh ijkl mnop");

AppPasswordAccount localAccount(const FakeServer &imap, const FakeServer &smtp)
{
    AppPasswordAccount a;
    a.email = QStringLiteral("alex.morgan@example.com");
    a.imapHost = QStringLiteral("127.0.0.1");
    a.imapPort = imap.port();
    a.smtpHost = QStringLiteral("127.0.0.1");
    a.smtpPort = smtp.port();
    return a;
}

ConnectionTester::Result runTest(ConnectionTester &t, const AppPasswordAccount &a, const QString &pw)
{
    ConnectionTester::Result out;
    bool done = false;
    t.test(a, pw, [&](const ConnectionTester::Result &r) {
        out = r;
        done = true;
    });
    if (!QTest::qWaitFor([&] { return done; }, 10000)) {
        qWarning("tester never finished");
    }
    return out;
}

// A TokenStore whose writes fail, like a missing Secret Service.
class BrokenStore : public TokenStore
{
public:
    using TokenStore::TokenStore;
    void read(const QString &, ReadCb cb) override { cb(false, {}, QStringLiteral("no keyring")); }
    void write(const QString &, const QString &, DoneCb cb) override
    {
        cb(false, QStringLiteral("The name org.freedesktop.secrets was not provided"));
    }
    void remove(const QString &, DoneCb cb) override { cb(false, QStringLiteral("no keyring")); }
};

QString settingsText()
{
    QSettings s;
    s.sync();
    QFile f(s.fileName());
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

} // namespace

class TstAppPassword : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("zmail-tst-apppassword"));
        QSettings().clear();
    }
    void cleanup() { QSettings().clear(); }

    void normalizesGoogleFormat()
    {
        QString err;
        QCOMPARE(AppPasswordAccount::normalizeAppPassword(kFakePwSpaced, &err), kFakePw);
        QVERIFY(err.isEmpty());
        QCOMPARE(AppPasswordAccount::normalizeAppPassword(QStringLiteral(" ABCD efgh\tIJKL mnop\n")), kFakePw);
        // A normal-looking Google password (or anything not 16 letters) is refused,
        // and the error never echoes what was typed.
        for (const QString &bad : {QStringLiteral("Hunter2-not-an-app-pw"), QStringLiteral("abcd efgh ijkl mno"),
                                   QStringLiteral("abcd efgh ijkl mnop q"), QStringLiteral("abcd1fgh ijkl mnop")}) {
            QVERIFY(AppPasswordAccount::normalizeAppPassword(bad, &err).isEmpty());
            QVERIFY(!err.isEmpty());
            QVERIFY(!err.contains(bad));
        }
        AppPasswordAccount::normalizeAppPassword(QString(), &err);
        QVERIFY(err.contains(QStringLiteral("Enter")));
    }

    void settingsRoundTripWithoutSecret()
    {
        QSettings s;
        QCOMPARE(AppPasswordAccount::savedKind(s), AccountKind::GmailOAuth);
        AppPasswordAccount a;
        a.email = QStringLiteral("alex.morgan@example.com");
        QVERIFY(a.isValid());
        QCOMPARE(a.imapHost, QStringLiteral("imap.gmail.com"));
        QCOMPARE(a.imapPort, 993);
        QCOMPARE(a.smtpHost, QStringLiteral("smtp.gmail.com"));
        QCOMPARE(a.smtpPort, 465);
        a.save(s);
        QCOMPARE(AppPasswordAccount::savedKind(s), AccountKind::GmailAppPassword);
        const AppPasswordAccount b = AppPasswordAccount::load(s);
        QCOMPARE(b.email, a.email);
        QCOMPARE(b.imapPort, 993);
        QCOMPARE(b.smtpPort, 465);
        AppPasswordAccount::clear(s);
        QCOMPARE(AppPasswordAccount::savedKind(s), AccountKind::GmailOAuth);
        QVERIFY(AppPasswordAccount::load(s).email.isEmpty());
        QCOMPARE(AppPasswordAccount::appPasswordKey(QStringLiteral(" Alex.Morgan@Example.com ")),
                 QStringLiteral("app-password:alex.morgan@example.com"));
        QVERIFY(!AppPasswordAccount::looksLikeEmail(QStringLiteral("alex")));
    }

    void imapQuotingAndAuthPlain()
    {
        QByteArray q;
        QVERIFY(ConnectionTester::imapQuote(QStringLiteral("a\"b\\c"), &q));
        QCOMPARE(q, QByteArray("\"a\\\"b\\\\c\""));
        QVERIFY(!ConnectionTester::imapQuote(QStringLiteral("a\r\nb"), &q));
        QCOMPARE(QByteArray::fromBase64(ConnectionTester::smtpAuthPlain(QStringLiteral("u@x.test"), kFakePw)),
                 QByteArray("\0u@x.test\0", 10) + kFakePw.toUtf8());
    }

    void testPassesAgainstFakeServers()
    {
        FakeServer imap(FakeServer::Imap, true);
        FakeServer smtp(FakeServer::Smtp, true);
        LogCapture log;
        ConnectionTester t;
        t.setPlaintextForTests(true);
        const auto r = runTest(t, localAccount(imap, smtp), kFakePw);
        QVERIFY2(r.imapOk, qPrintable(r.imapMessage));
        QVERIFY2(r.smtpOk, qPrintable(r.smtpMessage));
        QVERIFY(r.ok());
        QVERIFY(!imap.received.isEmpty());
        QVERIFY(imap.received.first().startsWith("a1 LOGIN \"alex.morgan@example.com\" \"abcdefghijklmnop\""));
        QTRY_VERIFY(imap.received.contains("a2 LOGOUT"));
        QVERIFY(smtp.received.contains("EHLO zmail"));
        QTRY_VERIFY(smtp.received.contains("QUIT"));
        QVERIFY(!log.all().contains(kFakePw));
        QVERIFY(!r.imapMessage.contains(kFakePw) && !r.smtpMessage.contains(kFakePw));
    }

    void wrongPasswordExplainedWithoutEcho()
    {
        FakeServer imap(FakeServer::Imap, false);
        FakeServer smtp(FakeServer::Smtp, true);
        ConnectionTester t;
        t.setPlaintextForTests(true);
        const auto r = runTest(t, localAccount(imap, smtp), kFakePw);
        QVERIFY(!r.imapOk);
        QVERIFY(!r.smtpOk);
        QVERIFY(r.imapMessage.contains(QStringLiteral("2-Step Verification")));
        QVERIFY(smtp.received.isEmpty()); // SMTP not tried once IMAP said no
        QVERIFY(!r.imapMessage.contains(kFakePw));

        FakeServer imap2(FakeServer::Imap, true);
        FakeServer smtp2(FakeServer::Smtp, false);
        const auto r2 = runTest(t, localAccount(imap2, smtp2), kFakePw);
        QVERIFY(r2.imapOk);
        QVERIFY(!r2.smtpOk);
        QVERIFY(r2.smtpMessage.contains(QStringLiteral("refused")));
    }

    void unreachableAndTimeout()
    {
        ConnectionTester t;
        t.setPlaintextForTests(true);
        t.setTimeoutMs(1500);
        QTcpServer closed;
        QVERIFY(closed.listen(QHostAddress::LocalHost));
        const quint16 deadPort = closed.serverPort();
        closed.close();
        AppPasswordAccount a;
        a.email = QStringLiteral("alex.morgan@example.com");
        a.imapHost = QStringLiteral("127.0.0.1");
        a.imapPort = deadPort;
        a.smtpHost = a.imapHost;
        a.smtpPort = deadPort;
        auto r = runTest(t, a, kFakePw);
        QVERIFY(!r.ok());
        QVERIFY2(r.imapMessage.contains(QStringLiteral("Couldn't connect")), qPrintable(r.imapMessage));

        FakeServer quiet(FakeServer::Imap, true);
        quiet.silent = true;
        a.imapPort = quiet.port();
        r = runTest(t, a, kFakePw);
        QVERIFY(!r.ok());
        QVERIFY2(r.imapMessage.contains(QStringLiteral("no answer")), qPrintable(r.imapMessage));
    }

    void dialogSaveNeedsPassingTestAndUsesKeyring()
    {
        FakeServer imap(FakeServer::Imap, true);
        FakeServer smtp(FakeServer::Smtp, true);
        MemoryTokenStore store;
        ui::AppPasswordDialog dlg(&store);
        dlg.tester()->setPlaintextForTests(true);
        auto *email = dlg.findChild<QLineEdit *>(QStringLiteral("emailEdit"));
        auto *pw = dlg.findChild<QLineEdit *>(QStringLiteral("appPasswordEdit"));
        auto *save = dlg.findChild<QPushButton *>(QStringLiteral("saveButton"));
        auto *servers = dlg.findChild<QGroupBox *>(QStringLiteral("serversBox"));
        QVERIFY(email && pw && save && servers);
        QCOMPARE(pw->echoMode(), QLineEdit::Password);
        QVERIFY(!save->isEnabled());

        // Bad format: refused before any connection.
        email->setText(QStringLiteral("alex.morgan@example.com"));
        pw->setText(QStringLiteral("my-real-google-password"));
        dlg.runTest();
        QVERIFY(dlg.statusText().contains(QStringLiteral("isn't an app password")));
        QVERIFY(imap.received.isEmpty());

        pw->setText(kFakePwSpaced);
        servers->setChecked(true);
        dlg.findChild<QLineEdit *>(QStringLiteral("imapHost"))->setText(QStringLiteral("127.0.0.1"));
        dlg.findChild<QSpinBox *>(QStringLiteral("imapPort"))->setValue(imap.port());
        dlg.findChild<QLineEdit *>(QStringLiteral("smtpHost"))->setText(QStringLiteral("127.0.0.1"));
        dlg.findChild<QSpinBox *>(QStringLiteral("smtpPort"))->setValue(smtp.port());
        dlg.runTest();
        QTRY_VERIFY(save->isEnabled());
        QVERIFY(dlg.statusText().contains(QStringLiteral("IMAP: signed in")));

        // Editing anything after a pass needs a new Test.
        dlg.findChild<QLineEdit *>(QStringLiteral("smtpHost"))->textEdited(QStringLiteral("x"));
        QVERIFY(!save->isEnabled());
        dlg.runTest();
        QTRY_VERIFY(save->isEnabled());

        QSignalSpy saved(&dlg, &ui::AppPasswordDialog::saved);
        save->click();
        QTRY_COMPARE(saved.count(), 1);
        QCOMPARE(store.values.value(QStringLiteral("app-password:alex.morgan@example.com")), kFakePw);
        QVERIFY(pw->text().isEmpty());
        QSettings s;
        QCOMPARE(AppPasswordAccount::savedKind(s), AccountKind::GmailAppPassword);
        QCOMPARE(AppPasswordAccount::load(s).email, QStringLiteral("alex.morgan@example.com"));
        const QString onDisk = settingsText();
        QVERIFY(onDisk.contains(QStringLiteral("alex.morgan@example.com")));
        QVERIFY(!onDisk.contains(kFakePw));
    }

    void noPlaintextFallbackWhenKeyringFails()
    {
        FakeServer imap(FakeServer::Imap, true);
        FakeServer smtp(FakeServer::Smtp, true);
        BrokenStore store;
        ui::AppPasswordDialog dlg(&store);
        dlg.tester()->setPlaintextForTests(true);
        dlg.findChild<QLineEdit *>(QStringLiteral("emailEdit"))->setText(QStringLiteral("alex.morgan@example.com"));
        dlg.findChild<QLineEdit *>(QStringLiteral("appPasswordEdit"))->setText(kFakePw);
        dlg.findChild<QGroupBox *>(QStringLiteral("serversBox"))->setChecked(true);
        dlg.findChild<QLineEdit *>(QStringLiteral("imapHost"))->setText(QStringLiteral("127.0.0.1"));
        dlg.findChild<QSpinBox *>(QStringLiteral("imapPort"))->setValue(imap.port());
        dlg.findChild<QLineEdit *>(QStringLiteral("smtpHost"))->setText(QStringLiteral("127.0.0.1"));
        dlg.findChild<QSpinBox *>(QStringLiteral("smtpPort"))->setValue(smtp.port());
        auto *save = dlg.findChild<QPushButton *>(QStringLiteral("saveButton"));
        dlg.runTest();
        QTRY_VERIFY(save->isEnabled());
        QSignalSpy saved(&dlg, &ui::AppPasswordDialog::saved);
        save->click();
        QTRY_VERIFY(dlg.statusText().contains(QStringLiteral("nothing was saved")));
        QCOMPARE(saved.count(), 0);
        QSettings s;
        QCOMPARE(AppPasswordAccount::savedKind(s), AccountKind::GmailOAuth);
        QVERIFY(!settingsText().contains(kFakePw));
    }

    void connectDialogOffersAppPassword()
    {
        MemoryTokenStore store;
        SessionOptions o;
        o.client.status = ClientConfig::LoadStatus::Missing;
        o.store = &store;
        o.rememberAccount = false;
        MailSession session(o);
        QCOMPARE(session.tokenStore(), static_cast<TokenStore *>(&store));
        ui::ConnectDialog dlg(&session);
        auto *b = dlg.findChild<QPushButton *>(QStringLiteral("appPasswordButtonSetup"));
        QVERIFY(b);
        QVERIFY(dlg.findChild<QPushButton *>(QStringLiteral("appPasswordButtonSignIn")));
        b->click();
        auto *apd = dlg.findChild<ui::AppPasswordDialog *>();
        QVERIFY(apd);
        QVERIFY(ui::AppPasswordDialog::instructionsHtml().contains(QStringLiteral("myaccount.google.com/apppasswords")));
        apd->reject();
    }
};

QTEST_MAIN(TstAppPassword)
#include "tst_apppassword.moc"
