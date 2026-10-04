#include "core/Limits.h"

#include <QByteArray>
#include <QtTest>

using namespace zmail::limits;

class TstLimits : public QObject
{
    Q_OBJECT

private slots:
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
