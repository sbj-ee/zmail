#include "UpdateChecker.hpp"
#include "version.hpp"

#include <QtTest>

class TstVersion : public QObject
{
    Q_OBJECT

private slots:
    void versionMatchesParts()
    {
        const QString expected = QStringLiteral("%1.%2.%3")
                                     .arg(zmail::kVersionMajor)
                                     .arg(zmail::kVersionMinor)
                                     .arg(zmail::kVersionPatch);
        QCOMPARE(QString::fromLatin1(zmail::kVersionString), expected);
    }

    void semverCompare_data()
    {
        QTest::addColumn<QString>("tag");
        QTest::addColumn<QString>("current");
        QTest::addColumn<bool>("newer");
        QTest::newRow("same") << "v0.1.0" << "0.1.0" << false;
        QTest::newRow("patch") << "v0.1.1" << "0.1.0" << true;
        QTest::newRow("minor") << "0.2" << "0.1.9" << true;
        QTest::newRow("older") << "v0.0.9" << "0.1.0" << false;
        QTest::newRow("major") << "V1.0.0" << "0.9.9" << true;
        QTest::newRow("suffix") << "v0.1.0-rc1" << "0.1.0" << false;
    }

    void semverCompare()
    {
        QFETCH(QString, tag);
        QFETCH(QString, current);
        QFETCH(bool, newer);
        QCOMPARE(UpdateChecker::isNewer(tag, current), newer);
    }
};

QTEST_GUILESS_MAIN(TstVersion)
#include "tst_version.moc"
