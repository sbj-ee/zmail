#include "Theme.h"

#include <QApplication>
#include <QString>
#include <QVariant>
#include <algorithm>
#include <QStyleFactory>
#include <QStyleHints>
#include <cmath>

namespace zmail::ui {

namespace {
ThemeMode g_mode = ThemeMode::Light;

double channel(double c)
{
    return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double luminance(const QColor &c)
{
    return 0.2126 * channel(c.redF()) + 0.7152 * channel(c.greenF()) + 0.0722 * channel(c.blueF());
}

bool systemPrefersDark()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
#else
    return false;
#endif
}
} // namespace

QPalette lightPalette()
{
    QPalette p;
    const QColor window(0xef, 0xf0, 0xf1), base(0xff, 0xff, 0xff), text(0x23, 0x26, 0x29);
    const QColor accent(0x2a, 0x6f, 0xdb);
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, QColor(0xf6, 0xf7, 0xf9));
    p.setColor(QPalette::ToolTipBase, QColor(0xff, 0xff, 0xf0));
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, QColor(0x6b, 0x70, 0x78)); // >= 4.5:1 on Base, 3:1 on strong stripes
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, QColor(0xf4, 0xf5, 0xf6));
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Link, accent.darker(110));
    p.setColor(QPalette::Mid, QColor(0xc4, 0xc8, 0xcc));
    p.setColor(QPalette::Dark, QColor(0xa0, 0xa4, 0xa8));
    p.setColor(QPalette::Light, Qt::white);
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
        p.setColor(QPalette::Disabled, role, QColor(0xa0, 0xa4, 0xa8));
    }
    return p;
}

QPalette darkPalette()
{
    QPalette p;
    const QColor window(0x2a, 0x2d, 0x31), base(0x1f, 0x22, 0x25), text(0xe4, 0xe6, 0xe8);
    const QColor accent(0x3d, 0x8b, 0xfd);
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, QColor(0x26, 0x29, 0x2d));
    p.setColor(QPalette::ToolTipBase, QColor(0x31, 0x36, 0x3b));
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, QColor(0x8a, 0x90, 0x97));
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, QColor(0x33, 0x37, 0x3c));
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Link, QColor(0x7a, 0xb4, 0xff));
    p.setColor(QPalette::Mid, QColor(0x45, 0x4a, 0x50));
    p.setColor(QPalette::Dark, QColor(0x18, 0x1a, 0x1c));
    p.setColor(QPalette::Light, QColor(0x4a, 0x4f, 0x55));
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
        p.setColor(QPalette::Disabled, role, QColor(0x6c, 0x72, 0x78));
    }
    return p;
}

void applyTheme(ThemeMode mode)
{
    g_mode = mode;
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    const bool dark = mode == ThemeMode::Dark || (mode == ThemeMode::System && systemPrefersDark());
    QApplication::setPalette(dark ? darkPalette() : lightPalette());
}

ThemeMode currentTheme()
{
    return g_mode;
}

bool isDark(const QPalette &p)
{
    return luminance(p.color(QPalette::Base)) < 0.2;
}

double contrastRatio(const QColor &a, const QColor &b)
{
    double la = luminance(a), lb = luminance(b);
    if (la < lb) {
        std::swap(la, lb);
    }
    return (la + 0.05) / (lb + 0.05);
}

QColor readableOn(const QColor &fg, const QColor &bg, double minRatio)
{
    QColor c = fg.toHsl();
    const bool darkBg = luminance(bg) < 0.4;
    for (int i = 0; i < 40 && contrastRatio(c, bg) < minRatio; ++i) {
        const int l = c.lightness();
        c.setHsl(c.hslHue(), c.hslSaturation(), darkBg ? std::min(255, l + 8) : std::max(0, l - 8));
    }
    return c.toRgb();
}

QColor rowTint(const QColor &rule, const QColor &base)
{
    const double a = luminance(base) < 0.2 ? 0.22 : 0.12;
    return QColor::fromRgbF(base.redF() * (1 - a) + rule.redF() * a,
                            base.greenF() * (1 - a) + rule.greenF() * a,
                            base.blueF() * (1 - a) + rule.blueF() * a);
}

int stripePreset(StripeStrength s)
{
    switch (s) {
    case StripeStrength::Off: return 0;
    case StripeStrength::Subtle: return 20;
    case StripeStrength::Normal: return 40;
    case StripeStrength::Strong: return 72;
    }
    return kStripeDefault;
}

double stripeTargetContrast(int strength)
{
    return 1.0 + std::clamp(strength, 0, kStripeMax) / 200.0;
}

QColor stripeColor(const QPalette &p, int strength)
{
    const QColor base = p.color(QPalette::Base);
    if (strength <= 0) {
        return base;
    }
    const QColor text = p.color(QPalette::Text);
    const QColor hl = p.color(QPalette::Highlight);
    // A hint of the accent keeps the stripe from looking like a disabled row.
    const QColor toward = QColor::fromRgbF(text.redF() * 0.85 + hl.redF() * 0.15, text.greenF() * 0.85 + hl.greenF() * 0.15,
                                           text.blueF() * 0.85 + hl.blueF() * 0.15);
    const double target = stripeTargetContrast(strength);
    // Binary search on the blend amount (at most halfway to the text colour).
    double lo = 0.0, hi = 0.5;
    auto blend = [&](double a) {
        return QColor::fromRgbF(base.redF() * (1 - a) + toward.redF() * a, base.greenF() * (1 - a) + toward.greenF() * a,
                                base.blueF() * (1 - a) + toward.blueF() * a);
    };
    for (int i = 0; i < 24; ++i) {
        const double mid = (lo + hi) / 2;
        (contrastRatio(blend(mid), base) < target ? lo : hi) = mid;
    }
    return blend(hi);
}

int stripeStrengthFromSetting(const QVariant &v)
{
    if (!v.isValid()) {
        return kStripeDefault;
    }
    bool ok = false;
    const int n = v.toString().toInt(&ok);
    if (ok) {
        return std::clamp(n, 0, kStripeMax);
    }
    const QString l = v.toString().toLower();
    if (l == QLatin1String("off")) return stripePreset(StripeStrength::Off);
    if (l == QLatin1String("subtle")) return stripePreset(StripeStrength::Subtle);
    if (l == QLatin1String("strong")) return stripePreset(StripeStrength::Strong);
    return kStripeDefault;
}

QColor suspiciousForeground(const QPalette &p)
{
    return isDark(p) ? QColor(0xff, 0x8a, 0x80) : QColor(0xb7, 0x1c, 0x1c);
}

QColor suspiciousBackground(const QPalette &p)
{
    return isDark(p) ? QColor(0x4a, 0x1f, 0x22) : QColor(0xfd, 0xe7, 0xe9);
}

} // namespace zmail::ui
