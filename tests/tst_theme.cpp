#include "MainWindow.hpp"
#include "ui/BrandThemes.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

#include <QAction>
#include <QApplication>
#include <QHeaderView>
#include <QSettings>
#include <QStyle>
#include <QToolBar>
#include <QTreeView>
#include <QtTest>

#include <array>
#include <cstdint>
#include <string_view>

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

    // The brand themes shared with zterminal. This hex table is the one in
    // docs/THEMES.md; zterminal's tst_misc checks the same values.
    void brandThemesMatchSharedTable()
    {
        struct Row {
            const char *id, *name;
            // background surface foreground muted accent link selection selectionText chrome chromeText header headerText
            std::array<std::uint32_t, 12> roles;
            std::array<std::uint32_t, 16> ansi;
        };
        const Row rows[] = {
            {"boilermakers", "Boilermakers",
             {0x000000, 0x141414, 0xCFB991, 0x9D9795, 0xDAAA00, 0xDAAA00, 0xCFB991, 0x000000, 0x000000, 0xCFB991, 0xDAAA00, 0x000000},
             {0x262626, 0xE5534B, 0x8CC265, 0xDAAA00, 0x6CA0DC, 0xC792EA, 0x56B6C2, 0xC4BFC0,
              0x9D9795, 0xFF7B72, 0xB5E08A, 0xEBD99F, 0x9CC3F0, 0xE3B0F5, 0x8EDCE6, 0xFFFFFF}},
            {"badgers", "Badgers",
             {0x121212, 0x1E1E1E, 0xFFFFFF, 0xADB1B4, 0xC5050C, 0xFF7B80, 0x9B0000, 0xFFFFFF, 0xC5050C, 0xFFFFFF, 0xC5050C, 0xFFFFFF},
             {0x2A2A2A, 0xF0474E, 0x7FC97F, 0xF2C14E, 0x6FA8EC, 0xD88AD8, 0x5CC8C8, 0xE1E5E7,
              0x8A8D91, 0xFF7B80, 0xA8E6A3, 0xFFDA7A, 0x9DC4F5, 0xF0B0F0, 0x90E3E3, 0xFFFFFF}},
            {"packers", "Packers",
             {0x203731, 0x1A2D28, 0xFFFFFF, 0xB4C4BE, 0xFFB612, 0xFFB612, 0xFFB612, 0x203731, 0x14241F, 0xFFFFFF, 0xFFB612, 0x203731},
             {0x14241F, 0xFF8A80, 0x9EE07A, 0xFFB612, 0x8AB4F8, 0xE3A6F0, 0x7FE0D6, 0xE1E5E7,
              0x8FA89F, 0xFFB3AD, 0xC3F0A6, 0xFFD878, 0xB8D1FB, 0xF1C9F8, 0xB0F0E8, 0xFFFFFF}},
        };
        QCOMPARE(int(sbj::brand::kThemes.size()), 3);
        for (const Row &r : rows) {
            const sbj::brand::Theme *t = sbj::brand::findTheme(r.id);
            QVERIFY2(t, r.id);
            QCOMPARE(QByteArray(t->name.data(), qsizetype(t->name.size())), QByteArray(r.name));
            const std::array<std::uint32_t, 12> got{t->background, t->surface, t->foreground, t->muted,
                                                    t->accent, t->link, t->selection, t->selectionText,
                                                    t->chrome, t->chromeText, t->header, t->headerText};
            for (size_t i = 0; i < got.size(); ++i) {
                QVERIFY2(got[i] == r.roles[i], qPrintable(QStringLiteral("%1 role %2").arg(QLatin1String(r.id)).arg(i)));
            }
            QVERIFY(t->ansi == r.ansi);
        }
        // The official brand colours are carried as is.
        auto has = [](const char *id, std::string_view name, std::uint32_t rgb) {
            for (const sbj::brand::Swatch &s : sbj::brand::findTheme(id)->swatches) {
                if (s.name == name) {
                    return s.rgb == rgb;
                }
            }
            return false;
        };
        QVERIFY(has("boilermakers", "Boilermaker Gold", 0xCFB991));
        QVERIFY(has("boilermakers", "Black", 0x000000));
        QVERIFY(has("boilermakers", "Aged", 0x8E6F3E));
        QVERIFY(has("boilermakers", "Rush", 0xDAAA00));
        QVERIFY(has("badgers", "Badger Red", 0xC5050C));
        QVERIFY(has("badgers", "Dark Red", 0x9B0000));
        QVERIFY(has("badgers", "White", 0xFFFFFF));
        QVERIFY(has("packers", "Dark Green", 0x203731));
        QVERIFY(has("packers", "Gold", 0xFFB612));
        QVERIFY(has("packers", "White", 0xFFFFFF));
    }

    void brandPalettesFollowTheRoles()
    {
        for (const sbj::brand::Theme &t : sbj::brand::kThemes) {
            const QPalette p = brandPalette(t);
            QCOMPARE(p.color(QPalette::Base).rgb() & 0xffffff, t.background);
            QCOMPARE(p.color(QPalette::Window).rgb() & 0xffffff, t.surface);
            QCOMPARE(p.color(QPalette::Text).rgb() & 0xffffff, t.foreground);
            QCOMPARE(p.color(QPalette::PlaceholderText).rgb() & 0xffffff, t.muted);
            QCOMPARE(p.color(QPalette::Highlight).rgb() & 0xffffff, t.selection);
            QCOMPARE(p.color(QPalette::HighlightedText).rgb() & 0xffffff, t.selectionText);
            QCOMPARE(p.color(QPalette::Link).rgb() & 0xffffff, t.link);
            QCOMPARE(brandToolBarPalette(t).color(QPalette::Window).rgb() & 0xffffff, t.chrome);
            QCOMPARE(brandToolBarPalette(t).color(QPalette::ButtonText).rgb() & 0xffffff, t.chromeText);
            QCOMPARE(brandHeaderPalette(t).color(QPalette::Button).rgb() & 0xffffff, t.header);
            QCOMPARE(brandHeaderPalette(t).color(QPalette::ButtonText).rgb() & 0xffffff, t.headerText);
        }
    }

    void brandPalettesAreReadable()
    {
        for (const sbj::brand::Theme &t : sbj::brand::kThemes) {
            const QByteArray id(t.id.data(), qsizetype(t.id.size()));
            const QPalette p = brandPalette(t);
            QVERIFY(isDark(p));
            auto ratio = [&](QPalette::ColorRole a, QPalette::ColorRole b) { return contrastRatio(p.color(a), p.color(b)); };
            QVERIFY2(ratio(QPalette::Text, QPalette::Base) >= 7.0, id.constData());
            QVERIFY2(ratio(QPalette::WindowText, QPalette::Window) >= 7.0, id.constData());
            QVERIFY2(ratio(QPalette::HighlightedText, QPalette::Highlight) >= 7.0, id.constData());
            QVERIFY2(ratio(QPalette::PlaceholderText, QPalette::Base) >= 4.5, id.constData());
            QVERIFY2(ratio(QPalette::Link, QPalette::Base) >= 4.5, id.constData());
            const QPalette tb = brandToolBarPalette(t), hd = brandHeaderPalette(t);
            QVERIFY2(contrastRatio(tb.color(QPalette::ButtonText), tb.color(QPalette::Window)) >= 4.5, id.constData());
            QVERIFY2(contrastRatio(hd.color(QPalette::ButtonText), hd.color(QPalette::Button)) >= 4.5, id.constData());
            QVERIFY2(contrastRatio(suspiciousForeground(p), suspiciousBackground(p)) >= 4.5, id.constData());
            // Row stripes: text stays AAA, dimmed columns 3:1 up to Strong.
            for (int v = 5; v <= kStripeMax; v += 5) {
                const QColor c = stripeColor(p, v);
                QVERIFY2(contrastRatio(p.color(QPalette::Text), c) >= 7.0, qPrintable(QStringLiteral("%1 %2").arg(QLatin1String(id)).arg(v)));
                if (v <= stripePreset(StripeStrength::Strong)) {
                    QVERIFY2(contrastRatio(p.color(QPalette::PlaceholderText), c) >= 3.0, qPrintable(QStringLiteral("%1 %2").arg(QLatin1String(id)).arg(v)));
                }
            }
        }
    }

    void themeIdsRoundTrip()
    {
        for (ThemeMode m : {ThemeMode::Light, ThemeMode::Dark, ThemeMode::System, ThemeMode::Boilermakers,
                            ThemeMode::Badgers, ThemeMode::Packers}) {
            QCOMPARE(themeFromId(themeId(m)), m);
        }
        QCOMPARE(themeId(ThemeMode::Packers), QStringLiteral("packers"));
        QCOMPARE(themeFromId(QStringLiteral(" Badgers ")), ThemeMode::Badgers);
        QCOMPARE(themeFromId(QString()), ThemeMode::Light);
        QCOMPARE(themeFromId(QStringLiteral("bogus"), ThemeMode::Dark), ThemeMode::Dark);
        QVERIFY(!brandTheme(ThemeMode::Dark));
        QCOMPARE(brandTheme(ThemeMode::Boilermakers)->id, std::string_view("boilermakers"));
    }

    void applyBrandThemeColoursToolbarAndHeaders()
    {
        QWidget top; // class palettes reach child widgets (a toolbar in the main window)
        auto &tb = *new QToolBar(&top);
        auto &hv = *new QHeaderView(Qt::Horizontal, &top);
        applyTheme(ThemeMode::Packers);
        const sbj::brand::Theme &t = *brandTheme(ThemeMode::Packers);
        QCOMPARE(QApplication::palette().color(QPalette::Base).rgb() & 0xffffff, t.background);
        QCOMPARE(tb.palette().color(QPalette::Window).rgb() & 0xffffff, t.chrome);
        QCOMPARE(hv.palette().color(QPalette::Button).rgb() & 0xffffff, t.header);
        QCOMPARE(QApplication::palette(&tb).color(QPalette::Window).rgb() & 0xffffff, t.chrome);
        // Back to Light: the class palettes are reset too.
        applyTheme(ThemeMode::Light);
        QCOMPARE(QApplication::palette(&tb).color(QPalette::Window), lightPalette().color(QPalette::Window));
        QCOMPARE(QApplication::palette(&hv).color(QPalette::Button), lightPalette().color(QPalette::Button));
        QCOMPARE(tb.palette().color(QPalette::Window), lightPalette().color(QPalette::Window));
    }

    void viewThemeMenuOffersAndRemembersBrandThemes()
    {
        applyTheme(ThemeMode::Light);
        MainWindow w;
        for (const char *obj : {"actionThemeBoilermakers", "actionThemeBadgers", "actionThemePackers"}) {
            QAction *a = w.findChild<QAction *>(QString::fromLatin1(obj));
            QVERIFY2(a, obj);
            a->trigger();
            QVERIFY(a->isChecked());
            QVERIFY(brandTheme(currentTheme()));
            QCOMPARE(QSettings().value(QLatin1String(kThemeSettingKey)).toString(), themeId(currentTheme()));
            QCOMPARE(QApplication::palette().color(QPalette::Base).rgb() & 0xffffff, brandTheme(currentTheme())->background);
            // Column headers of the message list get the theme's header colours.
            auto *list = w.findChild<QTreeView *>(QStringLiteral("messageList"));
            QVERIFY(list);
            QCOMPARE(list->header()->palette().color(QPalette::Button).rgb() & 0xffffff, brandTheme(currentTheme())->header);
            QCOMPARE(list->palette().color(QPalette::Highlight).rgb() & 0xffffff, brandTheme(currentTheme())->selection);
        }
        QCOMPARE(currentTheme(), ThemeMode::Packers);
        w.findChild<QAction *>(QStringLiteral("actionThemeLight"))->trigger();
        QCOMPARE(currentTheme(), ThemeMode::Light);
        QCOMPARE(QSettings().value(QLatin1String(kThemeSettingKey)).toString(), QStringLiteral("light"));
        QVERIFY(!w.findChild<QAction *>(QStringLiteral("actionThemePackers"))->isChecked());
    }

    void selectedRowIconsUseTheSelectionTextColour()
    {
        // The mailbox tree's selected row: its icon must show on the selection
        // in every brand theme (Boilermakers: gold icon on gold before).
        for (ThemeMode m : {ThemeMode::Boilermakers, ThemeMode::Badgers, ThemeMode::Packers, ThemeMode::Light}) {
            applyTheme(m);
            const QPalette p = QApplication::palette();
            const QImage img = icon(QStringLiteral("inbox")).pixmap(QSize(32, 32), QIcon::Selected).toImage();
            QColor ink;
            for (int y = 0; y < img.height() && !ink.isValid(); ++y) {
                for (int x = 0; x < img.width(); ++x) {
                    if (qAlpha(img.pixel(x, y)) == 255) {
                        ink = QColor(img.pixel(x, y));
                        break;
                    }
                }
            }
            QVERIFY(ink.isValid());
            QCOMPARE(ink.rgb(), p.color(QPalette::HighlightedText).rgb());
            QVERIFY2(contrastRatio(ink, p.color(QPalette::Highlight)) >= 4.5, qPrintable(themeId(m)));
        }
        applyTheme(ThemeMode::Light);
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
