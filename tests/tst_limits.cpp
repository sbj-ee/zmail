#include "core/Limits.h"
#include "core/MessageParser.h"
#include "core/RawMime.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QtTest>

using namespace zmail::limits;

class TstLimits : public QObject
{
    Q_OBJECT

private slots:
    // 1.0.5: a hostile multipart with thousands of parts is read only so far.
    void multipartPartCountIsCapped()
    {
        QByteArray raw = "From: a@example.com\r\nContent-Type: multipart/mixed; boundary=b\r\n\r\n";
        for (int i = 0; i < 5000; ++i) {
            raw += "--b\r\nContent-Type: text/plain\r\n\r\nx\r\n";
        }
        raw += "--b--\r\n";
        const QJsonObject payload = zmail::RawMime::toGmailPayload(raw);
        QCOMPARE(payload.value(QStringLiteral("parts")).toArray().size(), 1000);
        QJsonObject json;
        json.insert(QStringLiteral("payload"), payload);
        const auto body = zmail::MessageParser::bodyFromFull(json);
        QVERIFY(body.text.count(QLatin1Char('x')) <= 1000);
    }

    // base64MimeSize must match real MIME base64 output (76-char lines, CRLF).
    void base64SizeMatchesRealEncoding_data()
    {
        QTest::addColumn<int>("raw");
        for (int n : {0, 1, 2, 3, 56, 57, 58, 114, 1000, 65536, 1'000'003}) {
            QTest::newRow(QByteArray::number(n).constData()) << n;
        }
    }

    void base64SizeMatchesRealEncoding()
    {
        QFETCH(int, raw);
        const QByteArray b64 = QByteArray(raw, 'x').toBase64();
        qint64 mime = b64.size();
        if (!b64.isEmpty()) {
            mime += 2 * ((b64.size() - 1) / kBase64LineLength);
        }
        QCOMPARE(base64MimeSize(raw), mime);
    }

    void overheadIsAboutThirtySevenPercent()
    {
        const double ratio = double(base64MimeSize(10'000'000)) / 10'000'000.0;
        QVERIFY(ratio > 1.36 && ratio < 1.38);
    }

    void thresholds()
    {
        QCOMPARE(classifySendSize(0), SizeLevel::Ok);
        QCOMPARE(classifySendSize(kSendLimitBytes * 80 / 100 - 1), SizeLevel::Ok);
        QCOMPARE(classifySendSize(kSendLimitBytes * 80 / 100), SizeLevel::Warn);
        QCOMPARE(classifySendSize(kSendLimitBytes), SizeLevel::Warn);
        QCOMPARE(classifySendSize(kSendLimitBytes + 1), SizeLevel::Blocked);
    }

    void eighteenMegabyteFileIsBlocked()
    {
        // An 18.5 MB attachment looks under 25 MB but encodes to about 25.3 MB.
        QCOMPARE(classifySendSize(base64MimeSize(18'500'000)), SizeLevel::Blocked);
    }

    void sendLimitFitsApiCap()
    {
        QVERIFY(kSendLimitBytes <= kApiSendUploadMaxBytes);
        QCOMPARE(kApiSendUploadMaxBytes, qint64(35) * 1024 * 1024);
    }
};

QTEST_GUILESS_MAIN(TstLimits)
#include "tst_limits.moc"
