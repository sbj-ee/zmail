#include "Theme.h"

#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QStyleFactory>
#include <QStyleHints>
#include <algorithm>
#include <cmath>
#include <iterator>

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
    p.setColor(QPalette::PlaceholderText, QColor(0x8a, 0x8f, 0x96));
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
    installEmojiFallback();
}

QString emojiFamily()
{
    static const QString family = []() {
        const QStringList installed = QFontDatabase::families();
        for (const char *name : {"Noto Color Emoji", "Apple Color Emoji", "Segoe UI Emoji", "Twemoji", "JoyPixels",
                                 "EmojiOne Color"}) {
            const QString n = QString::fromLatin1(name);
            if (installed.contains(n, Qt::CaseInsensitive)) {
                return n;
            }
        }
        return QString();
    }();
    return family;
}

void installEmojiFallback()
{
    const QString emoji = emojiFamily();
    if (emoji.isEmpty()) {
        return;
    }
    QFont f = QApplication::font();
    QStringList fams = f.families();
    if (fams.contains(emoji)) {
        return;
    }
    // Lead with the font Qt actually resolved: a generic name such as
    // "Sans Serif" at the head of a family list is skipped, and everything
    // (spaces, digits) would then be drawn from the emoji font.
    const QString real = QFontInfo(f).family();
    if (fams.isEmpty()) {
        fams << real;
    } else {
        fams.first() = real;
    }
    fams << emoji;
    f.setFamilies(fams);
    QApplication::setFont(f);
}

bool isEmojiCodePoint(char32_t c, char32_t next)
{
    if (c == 0x200D || c == 0xFE0F || c == 0x20E3 || (c >= 0x1F3FB && c <= 0x1F3FF) || (c >= 0xE0020 && c <= 0xE007F)) {
        return true; // joiners, VS16, keycap, skin tones, tag sequences: part of the emoji before them
    }
    if (c >= 0x1F000 && c <= 0x1FAFF) {
        return true; // pictographs, emoticons, transport, flags, supplemental symbols
    }
    // Default-emoji-presentation characters in the BMP (Unicode Emoji_Presentation=Yes).
    static const char32_t bmp[] = {0x231A, 0x231B, 0x23E9, 0x23EA, 0x23EB, 0x23EC, 0x23F0, 0x23F3, 0x25FD, 0x25FE,
                                   0x2614, 0x2615, 0x267F, 0x2693, 0x26A1, 0x26AA, 0x26AB, 0x26BD, 0x26BE, 0x26C4,
                                   0x26C5, 0x26CE, 0x26D4, 0x26EA, 0x26F2, 0x26F3, 0x26F5, 0x26FA, 0x26FD, 0x2705,
                                   0x270A, 0x270B, 0x2728, 0x274C, 0x274E, 0x2753, 0x2754, 0x2755, 0x2757, 0x2795,
                                   0x2796, 0x2797, 0x27B0, 0x27BF, 0x2B1B, 0x2B1C, 0x2B50, 0x2B55};
    if ((c >= 0x2648 && c <= 0x2653) || std::find(std::begin(bmp), std::end(bmp), c) != std::end(bmp)) {
        return true;
    }
    // Text-default symbols (\u2764 heart, \u2600 sun, \u2714 check ...) only when
    // VS16 asks for the emoji form.
    return next == 0xFE0F && c >= 0x2000 && c <= 0x3299;
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

QColor suspiciousForeground(const QPalette &p)
{
    return isDark(p) ? QColor(0xff, 0x8a, 0x80) : QColor(0xb7, 0x1c, 0x1c);
}

QColor suspiciousBackground(const QPalette &p)
{
    return isDark(p) ? QColor(0x4a, 0x1f, 0x22) : QColor(0xfd, 0xe7, 0xe9);
}

} // namespace zmail::ui
