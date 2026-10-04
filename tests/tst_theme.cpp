#include "ui/Theme.h"

#include <QApplication>
#include <QStyle>
#include <QtTest>

using namespace zmail::ui;

class TstTheme : public QObject
{
    Q_OBJECT

private slots:
    void contrastBasics()
    {
        QCOMPARE(qRound(contrastRatio(Qt::black, Qt::white)), 21);
        QCOMPARE(contrastRatio(Qt::red, Qt::red), 1.0);
    }

    void readableOnLightAndDark_data()
    {
        QTest::addColumn<QColor>("fg");
        QTest::newRow("teal") << QColor(0x00, 0x89, 0x7b);
        QTest::newRow("purple") << QColor(0x7b, 0x3f, 0xb5);
        QTest::newRow("amber") << QColor(0xe0, 0x8e, 0x0b);
        QTest::newRow("blue") << QColor(0x1e, 0x6f, 0xd9);
        QTest::newRow("yellow") << QColor(0xff, 0xeb, 0x3b);
    }

    void readableOnLightAndDark()
    {
        QFETCH(QColor, fg);
        for (const QPalette &p : {lightPalette(), darkPalette()}) {
            const QColor bg = rowTint(fg, p.color(QPalette::Base));
            QVERIFY(contrastRatio(readableOn(fg, bg), bg) >= 4.5);
        }
    }

    void suspiciousColoursReadable()
    {
        for (const QPalette &p : {lightPalette(), darkPalette()}) {
            QVERIFY(contrastRatio(suspiciousForeground(p), suspiciousBackground(p)) >= 4.5);
        }
    }

    void applyThemeSwitchesPalette()
    {
        applyTheme(ThemeMode::Dark);
        QVERIFY(isDark(QApplication::palette()));
        QCOMPARE(QApplication::style()->name().toLower(), QStringLiteral("fusion"));
        applyTheme(ThemeMode::Light);
        QVERIFY(!isDark(QApplication::palette()));
    }
};

QTEST_MAIN(TstTheme)
#include "tst_theme.moc"
