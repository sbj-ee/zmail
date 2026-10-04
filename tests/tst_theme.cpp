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

    void rowStripesVisibleAndReadable_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }
    void rowStripesVisibleAndReadable()
    {
        QFETCH(bool, dark);
        const QPalette p = dark ? darkPalette() : lightPalette();
        const QColor base = p.color(QPalette::Base);
        QCOMPARE(stripeColor(p, 0), base);
        double last = 1.0;
        for (int v = 5; v <= kStripeMax; v += 5) {
            const QColor c = stripeColor(p, v);
            const double vs = contrastRatio(c, base);
            QVERIFY2(qAbs(vs - stripeTargetContrast(v)) < 0.01, qPrintable(QStringLiteral("%1: %2").arg(v).arg(vs)));
            QVERIFY(vs > last); // monotonic slider
            last = vs;
            // Text stays readable on the stripe at any strength (WCAG AAA 7:1),
            // dimmed text (K and priority columns) keeps 3:1 up to Strong,
            // and the selection still stands out.
            QVERIFY2(contrastRatio(p.color(QPalette::Text), c) >= 7.0, qPrintable(c.name()));
            if (v <= stripePreset(StripeStrength::Strong)) {
                QVERIFY2(contrastRatio(p.color(QPalette::PlaceholderText), c) >= 3.0, qPrintable(QString::number(v)));
            }
            QVERIFY(contrastRatio(p.color(QPalette::Highlight), c) >= 1.5);
        }
        // Default (Normal) is clearly visible; 0.2.x's dark stripe was ~1.08:1.
        QVERIFY(contrastRatio(stripeColor(p, kStripeDefault), base) >= 1.19);
        QVERIFY(contrastRatio(p.color(QPalette::AlternateBase), base) < 1.19);
    }

    void stripeSettingValues()
    {
        QCOMPARE(stripeStrengthFromSetting(QVariant()), kStripeDefault);
        QCOMPARE(stripeStrengthFromSetting(QVariant(65)), 65);
        QCOMPARE(stripeStrengthFromSetting(QVariant(QStringLiteral("65"))), 65);
        QCOMPARE(stripeStrengthFromSetting(QVariant(250)), kStripeMax);
        QCOMPARE(stripeStrengthFromSetting(QVariant(-3)), 0);
        QCOMPARE(stripeStrengthFromSetting(QVariant(QStringLiteral("subtle"))), stripePreset(StripeStrength::Subtle));
        QCOMPARE(stripeStrengthFromSetting(QVariant(QStringLiteral("bogus"))), kStripeDefault);
        QVERIFY(stripePreset(StripeStrength::Off) < stripePreset(StripeStrength::Subtle));
        QVERIFY(stripePreset(StripeStrength::Subtle) < stripePreset(StripeStrength::Normal));
        QVERIFY(stripePreset(StripeStrength::Normal) < stripePreset(StripeStrength::Strong));
        QCOMPARE(stripePreset(StripeStrength::Normal), kStripeDefault);
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
