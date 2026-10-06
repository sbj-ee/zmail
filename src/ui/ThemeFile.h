// ThemeFile.h: the shared custom-theme file format (*.ztheme.json) of
// sbj-ee/zmail and sbj-ee/zterminal.
//
// THIS FILE AND ThemeFile.cpp ARE BYTE-IDENTICAL IN BOTH REPOS:
//   zmail:     src/ui/ThemeFile.{h,cpp}
//   zterminal: core/ThemeFile.{h,cpp}
// The format is documented in docs/THEMES.md ("Theme files") of each repo.
// Change both repos (and the fixtures in tests/data/themes/) together.
//
// A theme file holds the shared palette roles of BrandThemes.h, plus optional
// "terminal", "fonts" and "ui" blocks. Each app ignores the blocks it doesn't
// use and derives what's missing (deriveRoles(), deriveTerminal()).
#pragma once

#include "BrandThemes.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <array>
#include <cstdint>
#include <optional>

namespace sbj::theme {

inline constexpr int kVersion = 1;                 // "version" written; newer files are refused
inline constexpr char kFormat[] = "ztheme";        // "format"
inline constexpr char kSuffix[] = ".ztheme.json";  // file name suffix
inline constexpr qsizetype kMaxFileBytes = 64 * 1024;
inline constexpr int kMaxNameLength = 64;
inline constexpr int kMinFontSize = 4, kMaxFontSize = 72;

// The shared palette roles, in BrandThemes.h order. JSON keys: roleKey().
enum Role {
    Background, Surface, Foreground, Muted, Accent, Link,
    Selection, SelectionText, Chrome, ChromeText, Header, HeaderText,
    RoleCount
};
const char *roleKey(int role);
using Roles = std::array<std::uint32_t, RoleCount>; // 0xRRGGBB

// zterminal: the 16 ANSI colours, cursor, selection and cursor style.
struct Terminal {
    std::array<std::uint32_t, 16> ansi{};
    std::uint32_t cursor = 0;
    std::uint32_t selection = 0;
    std::uint32_t selectionText = 0;
    QString cursorShape;            // "block", "underline", "bar"; empty = the app's default
    std::optional<bool> cursorBlink; // unset = the app's default
    bool operator==(const Terminal &) const = default;
};

// Empty family / size 0 = the app's default font.
struct Fonts {
    QString ui;           // zmail
    int uiSize = 0;
    QString terminal;     // zterminal
    int terminalSize = 0;
    bool operator==(const Fonts &) const = default;
};

// Other app settings a theme may carry.
struct Ui {
    std::optional<int> rowStripes; // zmail message list stripes, 0..100
    bool operator==(const Ui &) const = default;
};

struct Theme {
    QString name;
    QString basedOn; // id of the theme it was duplicated from (informational)
    Roles roles{};
    std::optional<Terminal> terminal;
    Fonts fonts;
    Ui ui;
    bool operator==(const Theme &) const = default;
};

// Parses a theme file. Required: "format": "ztheme", "version" <= kVersion,
// a non-empty "name", palette.background and palette.foreground; the other
// roles are derived when missing. Unknown keys are ignored. On error returns
// nullopt and sets *error to a one-line reason.
std::optional<Theme> parse(const QByteArray &json, QString *error = nullptr);
// Indented JSON, keys sorted; every role is written.
QByteArray serialize(const Theme &t);
std::optional<Theme> load(const QString &path, QString *error = nullptr);
bool save(const Theme &t, const QString &path, QString *error = nullptr);

// A built-in brand theme as a theme file (with its terminal block).
Theme fromBrand(const sbj::brand::Theme &b);
// Missing roles from the ones present (`have` marks them); background and
// foreground must be present.
Roles deriveRoles(const Roles &r, const std::array<bool, RoleCount> &have);
// 16 readable ANSI colours (>= 4.5:1 for 1..15), cursor = accent, selection.
Terminal deriveTerminal(const Roles &r);
// t.terminal, or deriveTerminal(t.roles).
Terminal terminalOf(const Theme &t);

// "My Theme!" -> "my-theme" (file stem; "theme" if nothing is left).
QString fileStem(const QString &name);
// <configHome>/<app>/themes, e.g. ~/.config/zterminal/themes.
QString themesDir(const QString &configHome, const QString &app);

struct Entry {
    QString path;
    QString stem; // file name without kSuffix
    Theme theme;
};
// Valid *.ztheme.json files in dir, sorted by name (bad files are skipped).
QList<Entry> scan(const QString &dir);
// A file path in dir for a new theme called `name` that doesn't exist yet.
QString uniquePath(const QString &dir, const QString &name);

double contrast(std::uint32_t a, std::uint32_t b); // WCAG 2.x, 1..21
QString hex(std::uint32_t rgb);                    // "#RRGGBB"
std::optional<std::uint32_t> parseHex(const QString &s); // "#RRGGBB" or "#RGB"

} // namespace sbj::theme
