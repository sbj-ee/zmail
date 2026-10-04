#pragma once

#include <QColor>
#include <QPalette>

class QApplication;

namespace zmail::ui {

enum class ThemeMode { Light, Dark, System };

// Fusion style plus a tuned light or dark palette. System follows
// QStyleHints::colorScheme() on Qt >= 6.5 and falls back to Light on 6.4
// (the portal/GNOME fallback lands with Settings; see PLAN.md 4.14).
void applyTheme(ThemeMode mode);
ThemeMode currentTheme();
QPalette lightPalette();
QPalette darkPalette();
bool isDark(const QPalette &p);

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

} // namespace zmail::ui
