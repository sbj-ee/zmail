// Remote images: HTTPS only, public addresses only (literal, resolved, and
// after every redirect), and Ask by default.
#include "core/AddressGuard.h"
#include "ui/RemoteImages.h"
#include "ui/SafeHtmlView.h"

#include <QBuffer>
#include <QHostAddress>
#include <QHostInfo>
#include <QImage>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextDocument>
#include <QtTest>

using namespace zmail;
using namespace zmail::ui;

namespace {
// Plain HTTP on 127.0.0.1: counts connections, records request heads, and
// answers /go?<url> with a 302 to <url>, anything else with a 40x20 PNG.
class Server : public QTcpServer
{
public:
    int connections = 0;
    QList<QByteArray> requests;
    Server()
    {
        QImage img(40, 20, QImage::Format_RGB32);
        img.fill(Qt::red);
        QBuffer buf(&m_png);
        buf.open(QIODevice::WriteOnly);
        img.save(&buf, "PNG");
        listen(QHostAddress::LocalHost);
        connect(this, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket *s = nextPendingConnection()) {
                ++connections;
                connect(s, &QTcpSocket::readyRead, s, [this, s]() {
                    m_buf[s] += s->readAll();
                    const int end = m_buf[s].indexOf("\r\n\r\n");
                    if (end < 0) {
                        return;
                    }
                    const QByteArray head = m_buf[s].left(end);
                    requests << head;
                    const QByteArray path = head.split(' ').value(1);
                    if (path.startsWith("/go?")) {
                        s->write("HTTP/1.1 302 Found\r\nLocation: " + QByteArray::fromPercentEncoding(path.mid(4)) +
                                 "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    } else {
                        s->write("HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nConnection: close\r\nContent-Length: " +
                                 QByteArray::number(m_png.size()) + "\r\n\r\n" + m_png);
                    }
                    s->disconnectFromHost();
                });
                connect(s, &QTcpSocket::disconnected, s, [this, s]() {
                    m_buf.remove(s);
                    s->deleteLater();
                });
            }
        });
    }
    QString url(const QString &host, const QString &path) const
    {
        return QStringLiteral("http://%1:%2%3").arg(host).arg(serverPort()).arg(path);
    }

private:
    QByteArray m_png;
    QHash<QTcpSocket *, QByteArray> m_buf;
};

QSize imageSize(SafeHtmlView &v, const QString &url)
{
    return v.loadResource(QTextDocument::ImageResource, QUrl(url)).value<QImage>().size();
}
} // namespace

class TstRemoteImages : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QCoreApplication::setOrganizationName(QStringLiteral("zmail-tst-remoteimages"));
        QStandardPaths::setTestModeEnabled(true);
    }
    void init()
    {
        QSettings().clear();
        SafeHtmlView::setLoopbackAllowedForTests(false);
    }
    void cleanup() { SafeHtmlView::setLoopbackAllowedForTests(false); }

    void blockedRanges_data()
    {
        QTest::addColumn<QString>("address");
        QTest::addColumn<bool>("blocked");
        const char *blocked[] = {
            "127.0.0.1", "127.255.255.254",                         // loopback 127/8
            "10.0.0.1", "10.255.255.255",                           // RFC 1918
            "172.16.0.1", "172.31.255.255", "192.168.0.1", "192.168.255.255",
            "169.254.0.1", "169.254.169.254",                       // link-local, cloud metadata
            "100.64.0.1", "100.100.100.100", "100.127.255.255",     // CGNAT / Tailscale 100.64/10
            "0.0.0.0", "0.1.2.3",                                   // 0/8
            "224.0.0.1", "239.255.255.250", "255.255.255.255",      // multicast, broadcast
            "::1", "::",                                            // IPv6 loopback, unspecified
            "fe80::1", "febf:ffff::1",                              // fe80::/10
            "fc00::1", "fd7a:115c:a1e0::1",                         // fc00::/7 (incl. Tailscale ULA)
            "ff02::1", "ff05::fb",                                  // multicast
            "::ffff:127.0.0.1", "::ffff:10.1.2.3", "::ffff:172.20.0.1", "::ffff:192.168.1.1",
            "::ffff:169.254.169.254", "::ffff:100.64.0.1", "::ffff:0.0.0.0", "::ffff:224.0.0.1",
            "::ffff:7f00:1",                                        // mapped, hex form
            "::127.0.0.1", "::10.0.0.1",                            // IPv4-compatible
        };
        for (const char *a : blocked) {
            QTest::newRow(a) << QString::fromLatin1(a) << true;
        }
        const char *allowed[] = {
            "8.8.8.8", "1.1.1.1", "93.184.216.34",
            "100.63.255.255", "100.128.0.0",   // just outside 100.64/10
            "172.15.255.255", "172.32.0.0",    // just outside 172.16/12
            "169.253.255.255", "11.0.0.1", "126.255.255.255", "128.0.0.1", "223.255.255.255",
            "2606:4700:4700::1111", "2a00:1450::1", "fec0::1", "fbff::1", "::ffff:8.8.8.8",
        };
        for (const char *a : allowed) {
            QTest::newRow(a) << QString::fromLatin1(a) << false;
        }
    }
    void blockedRanges()
    {
        QFETCH(QString, address);
        QFETCH(bool, blocked);
        QHostAddress a;
        QVERIFY(a.setAddress(address));
        QCOMPARE(net::isBlockedAddress(a), blocked);
        QCOMPARE(net::isBlockedLiteral(address.contains(QLatin1Char(':')) ? QLatin1Char('[') + address + QLatin1Char(']') : address),
                 blocked);
    }

    void literalAddressesAreRefused_data()
    {
        QTest::addColumn<QString>("host");
        for (const char *h : {"127.0.0.1", "10.0.0.1", "172.16.5.4", "192.168.1.1", "169.254.169.254", "100.64.1.1",
                              "0.0.0.0", "224.0.0.251", "[::1]", "[fe80::1]", "[fd00::1]", "[ff02::1]",
                              "[::ffff:127.0.0.1]", "[::ffff:192.168.1.1]", "[::ffff:169.254.169.254]",
                              "[::ffff:100.64.0.1]"}) {
            QTest::newRow(h) << QString::fromLatin1(h);
        }
    }
    void literalAddressesAreRefused()
    {
        QFETCH(QString, host);
        Server server; // anything that reached 127.0.0.1 would show up here
        SafeHtmlView v;
        v.setRemoteImagesAllowed(true);
        const QString url = QStringLiteral("https://%1:%2/pic.png").arg(host).arg(server.serverPort());
        v.loadResource(QTextDocument::ImageResource, QUrl(url));
        QCOMPARE(v.refusedFetches(), 1); // refused before any connection
        QCOMPARE(imageSize(v, url), QSize(1, 1));
        QCOMPARE(v.remoteFetches(), 0);
        QTest::qWait(100);
        QCOMPARE(server.connections, 0);
    }

    void pickAllowedSkipsBlockedAddresses()
    {
        auto A = [](const char *a) { return QHostAddress(QString::fromLatin1(a)); };
        QCOMPARE(net::pickAllowed({A("127.0.0.1"), A("::1")}), QHostAddress());
        QCOMPARE(net::pickAllowed({A("10.0.0.5"), A("192.168.1.1"), A("fd00::1"), A("::ffff:10.0.0.1")}), QHostAddress());
        QCOMPARE(net::pickAllowed({}), QHostAddress());
        // Mixed answers: only a checked public address is ever used.
        QCOMPARE(net::pickAllowed({A("127.0.0.1"), A("93.184.216.34")}), A("93.184.216.34"));
        QCOMPARE(net::pickAllowed({A("2606:2800:220:1::1"), A("169.254.169.254"), A("93.184.216.34")}), A("93.184.216.34"));
        QCOMPARE(net::pickAllowed({A("fe80::1"), A("2606:2800:220:1::1")}), A("2606:2800:220:1::1"));
        // The tests' loopback switch allows loopback only.
        QCOMPARE(net::pickAllowed({A("::1"), A("127.0.0.1")}, true), A("127.0.0.1"));
        QCOMPARE(net::pickAllowed({A("10.0.0.1")}, true), QHostAddress());
    }

    void resolvedAddressesAreChecked_data()
    {
        // Names the system resolves locally (/etc/hosts) to blocked addresses.
        QTest::addColumn<QString>("host");
        QTest::newRow("ip6-localhost") << QStringLiteral("ip6-localhost");
        QTest::newRow("ip6-loopback") << QStringLiteral("ip6-loopback");
        QTest::newRow("this machine's name") << QHostInfo::localHostName();
        QTest::newRow("decimal 127.0.0.1") << QStringLiteral("2130706433");
    }
    void resolvedAddressesAreChecked()
    {
        QFETCH(QString, host);
        const QHostInfo info = QHostInfo::fromName(host);
        if (info.addresses().isEmpty() || !net::pickAllowed(info.addresses()).isNull()) {
            QSKIP("doesn't resolve to only blocked addresses here");
        }
        Server server;
        SafeHtmlView v;
        v.setRemoteImagesAllowed(true);
        const QString url = QStringLiteral("https://%1:%2/pic.png").arg(host).arg(server.serverPort());
        v.loadResource(QTextDocument::ImageResource, QUrl(url));
        QTRY_COMPARE_WITH_TIMEOUT(v.refusedFetches(), 1, 5000);
        QCOMPARE(v.remoteFetches(), 0);
        QCOMPARE(server.connections, 0);
        QCOMPARE(imageSize(v, url), QSize(1, 1));
    }

    void plainHttpIsRefused()
    {
        QVERIFY(!RemoteImages::allowInsecureHttp()); // default
        Server server;
        SafeHtmlView::setLoopbackAllowedForTests(true); // only http stands in the way now
        SafeHtmlView v;
        v.setRemoteImagesAllowed(true);
        for (const QString &url : {server.url(QStringLiteral("127.0.0.1"), QStringLiteral("/a.png")),
                                   server.url(QStringLiteral("localhost"), QStringLiteral("/b.png")),
                                   QStringLiteral("http://93.184.216.34/c.png"),
                                   QStringLiteral("HTTP://93.184.216.34/d.png")}) {
            v.loadResource(QTextDocument::ImageResource, QUrl(url));
            QCOMPARE(imageSize(v, url), QSize(1, 1));
        }
        QCOMPARE(v.refusedFetches(), 4);
        QCOMPARE(v.remoteFetches(), 0);
        QTest::qWait(200);
        QCOMPARE(server.connections, 0);

        // The hidden setting allows it; a resolved name is fetched from the
        // checked address with the name in the Host header.
        QSettings().setValue(QStringLiteral("privacy/allowHttpImages"), true);
        SafeHtmlView v2;
        v2.setRemoteImagesAllowed(true);
        const QString url = server.url(QStringLiteral("localhost"), QStringLiteral("/e.png"));
        v2.loadResource(QTextDocument::ImageResource, QUrl(url));
        QTRY_COMPARE_WITH_TIMEOUT(imageSize(v2, url), QSize(40, 20), 5000);
        QCOMPARE(server.requests.size(), 1);
        QVERIFY2(server.requests[0].toLower().contains("\r\nhost: localhost:" + QByteArray::number(server.serverPort())),
                 server.requests[0].constData());
    }

    void redirectsAreChecked()
    {
        QSettings().setValue(QStringLiteral("privacy/allowHttpImages"), true);
        SafeHtmlView::setLoopbackAllowedForTests(true);
        Server server;
        auto go = [&](const QString &to) {
            return server.url(QStringLiteral("127.0.0.1"), QStringLiteral("/go?") + QString::fromLatin1(QUrl::toPercentEncoding(to)));
        };
        SafeHtmlView v;
        v.setRemoteImagesAllowed(true);

        // A redirect to a public-looking name is followed (and checked again).
        const QString ok = go(server.url(QStringLiteral("localhost"), QStringLiteral("/real.png")));
        v.loadResource(QTextDocument::ImageResource, QUrl(ok));
        QTRY_COMPARE_WITH_TIMEOUT(imageSize(v, ok), QSize(40, 20), 5000);
        QCOMPARE(server.requests.size(), 2);

        // Into private ranges: refused at the redirect, nothing more is sent.
        const QStringList bad{QStringLiteral("http://10.0.0.1/x.png"), QStringLiteral("https://192.168.1.1/x.png"),
                              QStringLiteral("https://169.254.169.254/latest/meta-data/"),
                              QStringLiteral("https://[::ffff:10.0.0.1]/x.png"), QStringLiteral("https://100.64.0.1/x.png")};
        for (const QString &to : bad) {
            server.requests.clear();
            const int before = v.refusedFetches();
            const QString url = go(to);
            v.loadResource(QTextDocument::ImageResource, QUrl(url));
            QTRY_COMPARE_WITH_TIMEOUT(v.refusedFetches(), before + 1, 5000);
            QCOMPARE(server.requests.size(), 1);
            QCOMPARE(imageSize(v, url), QSize(1, 1));
        }

        // A loop stops after 5 redirects.
        server.requests.clear();
        QString loop = server.url(QStringLiteral("127.0.0.1"), QStringLiteral("/final.png"));
        for (int i = 0; i < 7; ++i) {
            loop = go(loop);
        }
        const int before = v.refusedFetches();
        v.loadResource(QTextDocument::ImageResource, QUrl(loop));
        QTRY_COMPARE_WITH_TIMEOUT(v.refusedFetches(), before + 1, 5000);
        QCOMPARE(server.requests.size(), 6); // the first request plus 5 followed redirects
    }

    void askIsTheDefaultAndSavedChoicesStay()
    {
        QCOMPARE(RemoteImages::mode(), RemoteImageMode::Ask);
        QCOMPARE(RemoteImages::modeFromKey(QStringLiteral("bogus")), RemoteImageMode::Ask);
        // A saved Always (the old default, or picked in Settings) is kept.
        QSettings().setValue(QStringLiteral("privacy/remoteImages"), QStringLiteral("always"));
        QCOMPARE(RemoteImages::mode(), RemoteImageMode::Always);
        QSettings().setValue(QStringLiteral("privacy/remoteImages"), QStringLiteral("never"));
        QCOMPARE(RemoteImages::mode(), RemoteImageMode::Never);
    }
};

QTEST_MAIN(TstRemoteImages)
#include "tst_remoteimages.moc"
