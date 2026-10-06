#pragma once

#include <QColor>
#include <QPalette>

class QApplication;

namespace sbj::brand {
struct Theme;
}

namespace zmail::ui {

// Light/Dark/System plus the brand themes shared with zterminal
// (BrandThemes.h, docs/THEMES.md).
enum class ThemeMode { Light, Dark, System, Boilermakers, Badgers, Packers };

// Fusion style plus a tuned light or dark palette. System follows
// QStyleHints::colorScheme() on Qt >= 6.5 and falls back to Light on 6.4
// (the portal/GNOME fallback lands with Settings; see PLAN.md 4.14).
// A brand theme sets brandPalette() and gives the toolbar its "chrome" band
// and the column headers its "header" colours (class palettes, no style sheet).
void applyTheme(ThemeMode mode);
ThemeMode currentTheme();
QPalette lightPalette();
QPalette darkPalette();
bool isDark(const QPalette &p);

// The brand theme behind a mode (nullptr for Light/Dark/System).
const sbj::brand::Theme *brandTheme(ThemeMode mode);
// Application palette for a brand theme (Base = background, Window = surface,
// Highlight = selection ...; see docs/THEMES.md for the role mapping).
QPalette brandPalette(const sbj::brand::Theme &t);
// Toolbar (chrome) and column header palettes for a brand theme.
QPalette brandToolBarPalette(const sbj::brand::Theme &t);
QPalette brandHeaderPalette(const sbj::brand::Theme &t);

// Settings key and ids: "light", "dark", "system", "boilermakers", "badgers",
// "packers". Remembered by MainWindow::setTheme(), read at start-up.
inline constexpr char kThemeSettingKey[] = "ui/theme";
QString themeId(ThemeMode mode);
// Unknown or empty ids give `fallback`.
ThemeMode themeFromId(const QString &id, ThemeMode fallback = ThemeMode::Light);

// WCAG 2.x contrast ratio between two colours (1.0 .. 21.0).
double contrastRatio(const QColor &a, const QColor &b);
// Lighten/darken fg until it reaches minRatio against bg (keeps the hue).
QColor readableOn(const QColor &fg, const QColor &bg, double minRatio = 4.5);
// Subtle row tint for a rule colour over a base colour.
QColor rowTint(const QColor &rule, const QColor &base);

// Alternating-row stripe for the message list. Strength 0..100 (Settings >
// Row Stripes slider, QSettings "ui/rowStripes"); 0 = off. The stripe is Base
// blended toward Text until it reaches a target contrast against Base of
// 1 + strength/200 (so it looks the same in light and dark themes); Text
// keeps >= 7:1 on it at any strength.
enum class StripeStrength { Off, Subtle, Normal, Strong }; // slider presets
constexpr int kStripeMax = 100;
int stripePreset(StripeStrength s);   // Off 0, Subtle 20, Normal 40, Strong 72
constexpr int kStripeDefault = 40;    // Normal
double stripeTargetContrast(int strength);
QColor stripeColor(const QPalette &p, int strength);
// Stored value -> strength; accepts 0..100 or a preset name ("subtle").
int stripeStrengthFromSetting(const QVariant &v);

// Fixed warning colours for suspicious mail. Colour rules can't override them.
QColor suspiciousForeground(const QPalette &p);
QColor suspiciousBackground(const QPalette &p);

// Colour emoji. Qt 6.4's fontconfig fallback doesn't reach the colour emoji
// font for emoji (Unicode "Common" script), so subjects showed tofu boxes
// even with fonts-noto-color-emoji installed. emojiFamily() is the first
// installed colour emoji family ("Noto Color Emoji" on Ubuntu), or empty.
QString emojiFamily();
// Appends emojiFamily() to the application font's family list, behind the
// font's real (resolved) family so text, spaces and digits keep their font.
// Called by applyTheme(); harmless to call again.
void installEmojiFallback();
// True for a code point that should come from the emoji font: pictographs,
// default-emoji symbols, joiners/VS16/skin tones, or a text-default symbol
// followed by VS16 (`next`).
bool isEmojiCodePoint(char32_t c, char32_t next = 0);

} // namespace zmail::ui
