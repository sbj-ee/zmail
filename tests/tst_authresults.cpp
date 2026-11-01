// 1.0.4: the sender-check banner. Gmail's own (topmost, authserv-id
// mx.google.com) Authentication-Results header for SPF/DKIM/DMARC, and a
// display name that poses as another address. All headers here are synthetic;
// every address is example.com / example.org / example.net.
#include "MainWindowDetail.h"
#include "core/AuthResults.h"
#include "core/MailCache.h"
#include "core/MessageParser.h"
#include "core/RawMime.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QtTest>

using namespace zmail;

namespace {
QJsonObject hdr(const QString &name, const QString &value)
{
    return QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("value"), value}};
}

const char *kPass = "mx.google.com; dkim=pass header.i=@example.com header.s=s1 header.b=abc; "
                    "spf=pass (google.com: domain of a@example.com designates 192.0.2.1 as permitted sender) "
                    "smtp.mailfrom=a@example.com; dmarc=pass (p=REJECT sp=REJECT dis=NONE) header.from=example.com";
} // namespace

class TestAuthResults : public QObject
{
    Q_OBJECT

private slots:
    void parsesGmailHeader()
    {
        const AuthResults r = Auth::parse(QString::fromLatin1(kPass));
        QVERIFY(r.trusted);
        QCOMPARE(r.spf, QStringLiteral("pass"));
        QCOMPARE(r.dkim, QStringLiteral("pass"));
        QCOMPARE(r.dmarc, QStringLiteral("pass"));
        QVERIFY(!r.suspicious());
    }

    void versionAndCaseAndComments()
    {
        const AuthResults r = Auth::parse(QStringLiteral(
            "MX.Google.COM 1 (comment; with=semicolons); DKIM/1=Fail (bad sig) header.d=example.net; "
            "dkim=pass header.d=example.net; spf=softfail smtp.mailfrom=x@example.net; dmarc=none"));
        QVERIFY(r.trusted);
        QCOMPARE(r.dkim, QStringLiteral("pass")); // one passing signature is enough
        QCOMPARE(r.spf, QStringLiteral("softfail"));
        QCOMPARE(r.dmarc, QStringLiteral("none"));
        QVERIFY(!r.suspicious());
    }

    void dmarcFailWarns()
    {
        const AuthResults r = Auth::parse(QStringLiteral(
            "mx.google.com; dkim=pass header.d=example.net; spf=pass smtp.mailfrom=example.net; "
            "dmarc=fail (p=QUARANTINE) header.from=example.com"));
        QVERIFY(r.dmarcFailed());
        QVERIFY(r.suspicious());
        QVERIFY(Auth::warningHtml(r, QString(), QStringLiteral("a@example.com")).contains(QStringLiteral("DMARC")));
    }

    void nonePassWarns()
    {
        const AuthResults r = Auth::parse(QStringLiteral(
            "mx.google.com; spf=softfail smtp.mailfrom=example.net; dkim=none; dmarc=none"));
        QVERIFY(r.suspicious());
        QVERIFY(!Auth::warningHtml(r, QString(), QStringLiteral("a@example.net")).isEmpty());
        // Nothing reported at all from Gmail is also "none passed".
        QVERIFY(Auth::parse(QStringLiteral("mx.google.com; none")).suspicious());
    }

    void otherAuthServIdIgnored()
    {
        // A header the sender wrote (or another hop's) proves nothing either way.
        const AuthResults fake = Auth::parse(QStringLiteral("evil.example.com; spf=pass; dkim=pass; dmarc=pass"));
        QVERIFY(!fake.trusted);
        QVERIFY(fake.spf.isEmpty());
        QVERIFY(!fake.suspicious());
        QVERIFY(!Auth::parse(QStringLiteral("mx.google.com.evil.example; dmarc=fail")).trusted);
        QVERIFY(!Auth::parse(QString()).trusted);
    }

    void onlyTopmostHeaderCounts()
    {
        // Gmail prepends its own; a forged one further down is ignored.
        QJsonArray h{hdr(QStringLiteral("Authentication-Results"), QStringLiteral("mx.google.com; dmarc=fail")),
                     hdr(QStringLiteral("Authentication-Results"), QStringLiteral("mx.google.com; dmarc=pass"))};
        QCOMPARE(Auth::topmostHeader(h), QStringLiteral("mx.google.com; dmarc=fail"));
        // Topmost isn't Gmail's: nothing is trusted, even if a lower one claims to be.
        QJsonArray h2{hdr(QStringLiteral("authentication-results"), QStringLiteral("relay.example.org; spf=pass")),
                      hdr(QStringLiteral("Authentication-Results"), QStringLiteral("mx.google.com; dmarc=fail"))};
        QVERIFY(!Auth::parse(Auth::topmostHeader(h2)).trusted);
        QVERIFY(Auth::topmostHeader(QJsonArray{hdr(QStringLiteral("From"), QStringLiteral("a@example.com"))}).isEmpty());
    }

    void displayNameMismatch_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("addr");
        QTest::addColumn<bool>("mismatch");
        QTest::newRow("plain name") << "Example Bank" << "alerts@example.com" << false;
        QTest::newRow("empty") << "" << "alerts@example.com" << false;
        QTest::newRow("same address") << "alerts@example.com" << "alerts@example.com" << false;
        QTest::newRow("same address, case") << "Alerts@Example.COM" << "alerts@example.com" << false;
        QTest::newRow("other address") << "security@example.com" << "x@example.net" << true;
        QTest::newRow("other address in text") << "Example Bank (security@example.com)" << "x@example.net" << true;
        QTest::newRow("own domain") << "example.com" << "alerts@example.com" << false;
        QTest::newRow("parent domain") << "Example.com Alerts" << "alerts@mail.example.com" << false;
        QTest::newRow("other domain") << "example.com" << "x@example.net" << true;
        QTest::newRow("other domain in text") << "Support - example.com" << "x@example.net" << true;
        QTest::newRow("lookalike suffix") << "example.com" << "x@badexample.com" << true;
        QTest::newRow("initials not a domain") << "J. R. Smith" << "jr@example.com" << false;
        QTest::newRow("version number") << "Release 1.0.4" << "bot@example.com" << false;
    }
    void displayNameMismatch()
    {
        QFETCH(QString, name);
        QFETCH(QString, addr);
        QFETCH(bool, mismatch);
        QCOMPARE(Auth::displayNameMismatch(name, addr), mismatch);
    }

    void bannerEscapesName()
    {
        const QString html = Auth::warningHtml({}, QStringLiteral("<b>x@example.com</b>"), QStringLiteral("y@example.net"));
        QVERIFY(!html.isEmpty());
        QVERIFY(!html.contains(QStringLiteral("<b>x@")));
        QVERIFY(html.contains(QStringLiteral("&lt;b&gt;")));
    }

    void hostileInputIsBounded()
    {
        QString big = QStringLiteral("mx.google.com; ");
        big += QString(200000, QLatin1Char('('));
        big += QStringLiteral("dmarc=fail");
        (void)Auth::parse(big);
        (void)Auth::displayNameMismatch(QString(100000, QLatin1Char('a')) + QStringLiteral(".com"), QStringLiteral("a@example.com"));
        QVERIFY(true);
    }

    void rawMessageToBannerEndToEnd()
    {
        // IMAP path: raw RFC 822 -> payload -> Body -> cache -> view.
        const QByteArray raw =
            "Authentication-Results: mx.google.com;\r\n"
            "       spf=fail smtp.mailfrom=x@example.net;\r\n"
            "       dmarc=fail header.from=example.com\r\n"
            "Authentication-Results: mx.google.com; spf=pass; dkim=pass; dmarc=pass\r\n"
            "From: \"security@example.com\" <x@example.net>\r\n"
            "To: me@example.org\r\n"
            "Subject: Verify your account\r\n"
            "Message-ID: <1@example.net>\r\n"
            "Content-Type: text/plain\r\n\r\nHello\r\n";
        QJsonObject json;
        json.insert(QStringLiteral("id"), QStringLiteral("m1"));
        json.insert(QStringLiteral("payload"), RawMime::toGmailPayload(raw));
        const MessageParser::Body body = MessageParser::bodyFromFull(json);
        QVERIFY(body.authResults.startsWith(QStringLiteral("mx.google.com;")));
        QVERIFY(body.authResults.contains(QStringLiteral("dmarc=fail")));

        MailCache cache;
        QVERIFY(cache.open(QStringLiteral(":memory:")));
        CachedMessage meta = MessageParser::fromMetadata(json);
        meta.id = QStringLiteral("m1");
        cache.upsert(meta);
        cache.setBody(QStringLiteral("m1"), body.text, body.html, {}, {{}, {}, {}, body.authResults});
        const CachedMessage c = cache.message(QStringLiteral("m1"));
        QCOMPARE(c.authResults, body.authResults);
        QCOMPARE(c.extrasVersion, MailCache::kExtrasVersion);

        const ui::ViewMessage v = ui::detail::liveViewMessage(c, false, {});
        QVERIFY(v.warning.contains(QStringLiteral("DMARC")));
        QVERIFY(v.warning.contains(QStringLiteral("looks like a different address")));
    }

    void goodMessageHasNoBanner()
    {
        CachedMessage c;
        c.id = QStringLiteral("m2");
        c.fromName = QStringLiteral("Example Store");
        c.fromAddr = QStringLiteral("orders@example.com");
        c.hasBody = true;
        c.authResults = QString::fromLatin1(kPass);
        QVERIFY(ui::detail::liveViewMessage(c, false, {}).warning.isEmpty());
        // Mail with no Authentication-Results (your own Sent mail): no banner.
        c.authResults.clear();
        QVERIFY(ui::detail::liveViewMessage(c, false, {}).warning.isEmpty());
    }
};

QTEST_MAIN(TestAuthResults)
#include "tst_authresults.moc"
