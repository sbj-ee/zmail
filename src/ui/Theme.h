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

// Fixed warning colours for suspicious mail. Colour rules can't override them.
QColor suspiciousForeground(const QPalette &p);
QColor suspiciousBackground(const QPalette &p);

} // namespace zmail::ui
