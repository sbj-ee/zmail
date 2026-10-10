#include "Theme.h"

#include "BrandThemes.h"

#include <QApplication>
#include <QDialog>
#include <QHeaderView>
#include <QToolBar>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QString>
#include <QVariant>
#include <QStyleFactory>
#include <QStyleHints>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>
#include <iterator>

namespace zmail::ui {

namespace {
ThemeMode g_mode = ThemeMode::Light;
QString g_customId;
std::optional<sbj::theme::Theme> g_custom;
std::optional<QFont> g_defaultFont; // the font before any theme changed it

double channel(double c)
{
    return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double luminance(const QColor &c)
{
    return 0.2126 * channel(c.redF()) + 0.7152 * channel(c.greenF()) + 0.0722 * channel(c.blueF());
}

QColor rgb(std::uint32_t v)
{
    return QColor(int((v >> 16) & 0xff), int((v >> 8) & 0xff), int(v & 0xff));
}

QColor mix(const QColor &a, const QColor &b, double t)
{
    return QColor::fromRgbF(a.redF() * (1 - t) + b.redF() * t, a.greenF() * (1 - t) + b.greenF() * t,
                            a.blueF() * (1 - t) + b.blueF() * t);
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
    // Selected rows: white on this blue is 5.0:1 (WCAG AA needs 4.5:1; the
    // old #3d8bfd was 3.3:1), and the band still stands out from Base (3.2:1).
    const QColor accent(0x2b, 0x6c, 0xd4);
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

const sbj::brand::Theme *brandTheme(ThemeMode mode)
{
    switch (mode) {
    case ThemeMode::Boilermakers: return sbj::brand::findTheme("boilermakers");
    case ThemeMode::Badgers: return sbj::brand::findTheme("badgers");
    case ThemeMode::Packers: return sbj::brand::findTheme("packers");
    default: return nullptr;
    }
}

QPalette rolesPalette(const sbj::theme::Roles &r)
{
    using namespace sbj::theme;
    QPalette p;
    const QColor bg = rgb(r[Background]), surface = rgb(r[Surface]), fg = rgb(r[Foreground]);
    p.setColor(QPalette::Window, surface);
    p.setColor(QPalette::WindowText, fg);
    p.setColor(QPalette::Base, bg);
    p.setColor(QPalette::AlternateBase, mix(bg, surface, 0.5)); // the list's stripe comes from stripeColor()
    p.setColor(QPalette::ToolTipBase, surface);
    p.setColor(QPalette::ToolTipText, fg);
    p.setColor(QPalette::PlaceholderText, rgb(r[Muted]));
    p.setColor(QPalette::Text, fg);
    p.setColor(QPalette::Button, surface);
    p.setColor(QPalette::ButtonText, fg);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Highlight, rgb(r[Selection]));
    p.setColor(QPalette::HighlightedText, rgb(r[SelectionText]));
    p.setColor(QPalette::Link, rgb(r[Link]));
    p.setColor(QPalette::LinkVisited, rgb(r[Link]));
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    p.setColor(QPalette::Accent, rgb(r[Accent]));
#endif
    p.setColor(QPalette::Light, surface.lighter(160));
    p.setColor(QPalette::Midlight, surface.lighter(130));
    p.setColor(QPalette::Mid, mix(surface, fg, 0.25));
    p.setColor(QPalette::Dark, surface.darker(150));
    p.setColor(QPalette::Shadow, Qt::black);
    const QColor disabled = mix(fg, surface, 0.5);
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
        p.setColor(QPalette::Disabled, role, disabled);
    }
    return p;
}

QPalette rolesToolBarPalette(const sbj::theme::Roles &r)
{
    QPalette p = rolesPalette(r);
    for (auto role : {QPalette::Window, QPalette::Button}) {
        p.setColor(role, rgb(r[sbj::theme::Chrome]));
    }
    for (auto role : {QPalette::WindowText, QPalette::ButtonText}) {
        p.setColor(role, rgb(r[sbj::theme::ChromeText]));
    }
    return p;
}

QPalette rolesHeaderPalette(const sbj::theme::Roles &r)
{
    QPalette p = rolesPalette(r);
    p.setColor(QPalette::Button, rgb(r[sbj::theme::Header]));
    p.setColor(QPalette::Window, rgb(r[sbj::theme::Header]));
    p.setColor(QPalette::ButtonText, rgb(r[sbj::theme::HeaderText]));
    p.setColor(QPalette::WindowText, rgb(r[sbj::theme::HeaderText]));
    return p;
}

QPalette dialogPalette(const QPalette &app)
{
    QPalette p = app;
    const QColor tint = app.color(QPalette::Highlight);
    for (auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        for (auto role : {QPalette::Window, QPalette::Button}) {
            p.setColor(group, role, mix(app.color(group, role), tint, kDialogTint));
        }
    }
    return p;
}

sbj::theme::Roles rolesFromPalette(const QPalette &p)
{
    using namespace sbj::theme;
    auto c = [&p](QPalette::ColorRole role) { return std::uint32_t(p.color(role).rgb() & 0xFFFFFF); };
    Roles r{};
    r[Background] = c(QPalette::Base);
    r[Surface] = c(QPalette::Window);
    r[Foreground] = c(QPalette::Text);
    r[Muted] = c(QPalette::PlaceholderText);
    r[Accent] = c(QPalette::Highlight);
    r[Link] = c(QPalette::Link);
    r[Selection] = c(QPalette::Highlight);
    r[SelectionText] = c(QPalette::HighlightedText);
    r[Chrome] = c(QPalette::Window);
    r[ChromeText] = c(QPalette::WindowText);
    r[Header] = c(QPalette::Button);
    r[HeaderText] = c(QPalette::ButtonText);
    return r;
}

QPalette brandPalette(const sbj::brand::Theme &t)
{
    return rolesPalette(sbj::theme::fromBrand(t).roles);
}

QPalette brandToolBarPalette(const sbj::brand::Theme &t)
{
    return rolesToolBarPalette(sbj::theme::fromBrand(t).roles);
}

QPalette brandHeaderPalette(const sbj::brand::Theme &t)
{
    return rolesHeaderPalette(sbj::theme::fromBrand(t).roles);
}

QString userThemesDir()
{
    return sbj::theme::themesDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation),
                                 QStringLiteral("zmail"));
}

QString zterminalThemesDir()
{
    return sbj::theme::themesDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation),
                                 QStringLiteral("zterminal"));
}

QList<CustomTheme> customThemes()
{
    QList<CustomTheme> out;
    for (const sbj::theme::Entry &e : sbj::theme::scan(userThemesDir())) {
        out.append({QStringLiteral("custom:") + e.stem, e.theme.name, e.path, true});
    }
    for (const sbj::theme::Entry &e : sbj::theme::scan(zterminalThemesDir())) {
        out.append({QStringLiteral("zterminal:") + e.stem, e.theme.name + QStringLiteral(" (zterminal)"), e.path, false});
    }
    return out;
}

std::optional<sbj::theme::Theme> themeFileFor(const QString &id)
{
    for (const CustomTheme &c : customThemes()) {
        if (c.id == id) {
            return sbj::theme::load(c.path);
        }
    }
    const ThemeMode m = themeFromId(id, ThemeMode::Custom);
    if (m == ThemeMode::Custom) {
        return std::nullopt;
    }
    if (const sbj::brand::Theme *b = brandTheme(m)) {
        return sbj::theme::fromBrand(*b);
    }
    sbj::theme::Theme t;
    const bool dark = m == ThemeMode::Dark || (m == ThemeMode::System && systemPrefersDark());
    t.name = m == ThemeMode::System ? QStringLiteral("System") : dark ? QStringLiteral("Dark") : QStringLiteral("Light");
    t.roles = rolesFromPalette(dark ? darkPalette() : lightPalette());
    return t;
}

QString themeId(ThemeMode mode)
{
    switch (mode) {
    case ThemeMode::Light: return QStringLiteral("light");
    case ThemeMode::Dark: return QStringLiteral("dark");
    case ThemeMode::System: return QStringLiteral("system");
    case ThemeMode::Custom: return g_customId;
    default: break;
    }
    const sbj::brand::Theme *t = brandTheme(mode);
    return t ? QString::fromLatin1(t->id.data(), qsizetype(t->id.size())) : QStringLiteral("light");
}

ThemeMode themeFromId(const QString &id, ThemeMode fallback)
{
    const QString l = id.trimmed().toLower();
    for (ThemeMode m : {ThemeMode::Light, ThemeMode::Dark, ThemeMode::System, ThemeMode::Boilermakers,
                        ThemeMode::Badgers, ThemeMode::Packers}) {
        if (l == themeId(m)) {
            return m;
        }
    }
    return fallback;
}

namespace {
void applyPalettes(const QPalette &app, const QPalette &toolBar, const QPalette &header, const QFont &font)
{
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QApplication::setPalette(app);
    // Then the class palettes (setting the application palette clears them):
    // the toolbar band and column headers. Set for every theme, so Light and
    // Dark put the plain palette back.
    QApplication::setPalette(toolBar, QToolBar::staticMetaObject.className());
    QApplication::setPalette(header, QHeaderView::staticMetaObject.className());
    // Dialogs (Contacts, Settings, message boxes ...) are tinted, so one
    // lying over the main window doesn't blend into it.
    QApplication::setPalette(dialogPalette(app), QDialog::staticMetaObject.className());
    // Only themes with a UI font touch the font (and the next theme puts the
    // default back), so switching built-in themes doesn't relayout.
    static bool themeFont = false;
    const bool custom = font != *g_defaultFont;
    if (custom || themeFont) {
        QApplication::setFont(font);
    }
    themeFont = custom;
    installEmojiFallback();
}

QFont defaultFont()
{
    if (!g_defaultFont) {
        g_defaultFont = QApplication::font();
    }
    return *g_defaultFont;
}
} // namespace

void applyTheme(ThemeMode mode)
{
    if (mode == ThemeMode::Custom) {
        applyThemeId(g_customId);
        return;
    }
    const QFont font = defaultFont();
    g_mode = mode;
    g_customId.clear();
    g_custom.reset();
    if (const sbj::brand::Theme *t = brandTheme(mode)) {
        applyPalettes(brandPalette(*t), brandToolBarPalette(*t), brandHeaderPalette(*t), font);
    } else {
        const bool dark = mode == ThemeMode::Dark || (mode == ThemeMode::System && systemPrefersDark());
        const QPalette p = dark ? darkPalette() : lightPalette();
        applyPalettes(p, p, p, font);
    }
}

void applyTheme(const sbj::theme::Theme &t, const QString &id)
{
    QFont font = defaultFont();
    if (!t.fonts.ui.isEmpty()) {
        font.setFamilies({t.fonts.ui});
    }
    if (t.fonts.uiSize > 0) {
        font.setPointSize(t.fonts.uiSize);
    }
    g_mode = ThemeMode::Custom;
    g_customId = id;
    g_custom = t;
    applyPalettes(rolesPalette(t.roles), rolesToolBarPalette(t.roles), rolesHeaderPalette(t.roles), font);
}

bool applyThemeId(const QString &id)
{
    const ThemeMode m = themeFromId(id, ThemeMode::Custom);
    if (m != ThemeMode::Custom) {
        applyTheme(m);
        return true;
    }
    if (const std::optional<sbj::theme::Theme> t = themeFileFor(id)) {
        applyTheme(*t, id);
        return true;
    }
    applyTheme(ThemeMode::Light);
    return false;
}

QString currentThemeId()
{
    return themeId(g_mode);
}

const sbj::theme::Theme *currentCustomTheme()
{
    return g_custom ? &*g_custom : nullptr;
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

QColor connectedForeground(const QPalette &p)
{
    // Material-ish greens that stay >= 4.5:1 on typical status-bar greys.
    return isDark(p) ? QColor(0x81, 0xc7, 0x84) : QColor(0x2e, 0x7d, 0x32);
}

QColor disconnectedForeground(const QPalette &p)
{
    return p.color(QPalette::Disabled, QPalette::WindowText);
}

QColor suspiciousBackground(const QPalette &p)
{
    return isDark(p) ? QColor(0x4a, 0x1f, 0x22) : QColor(0xfd, 0xe7, 0xe9);
}

} // namespace zmail::ui
