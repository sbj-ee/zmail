// ThemeFile.cpp: see ThemeFile.h. Byte-identical in sbj-ee/zmail and
// sbj-ee/zterminal.
#include "ThemeFile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <algorithm>
#include <cmath>

namespace sbj::theme {

namespace {

constexpr const char *kRoleKeys[RoleCount] = {"background", "surface", "foreground", "muted", "accent", "link",
                                              "selection", "selectionText", "chrome", "chromeText", "header",
                                              "headerText"};

double channel(int v)
{
    const double c = v / 255.0;
    return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double luminance(std::uint32_t c)
{
    return 0.2126 * channel(int((c >> 16) & 0xff)) + 0.7152 * channel(int((c >> 8) & 0xff)) +
           0.0722 * channel(int(c & 0xff));
}

std::uint32_t mix(std::uint32_t a, std::uint32_t b, double t)
{
    auto ch = [&](int shift) {
        const double x = ((a >> shift) & 0xff) * (1 - t) + ((b >> shift) & 0xff) * t;
        return std::uint32_t(std::clamp(int(std::lround(x)), 0, 255)) << shift;
    };
    return ch(16) | ch(8) | ch(0);
}

// Moves c toward white (dark bg) or black (light bg) until it reaches minRatio.
std::uint32_t readable(std::uint32_t c, std::uint32_t bg, double minRatio)
{
    const std::uint32_t target = luminance(bg) < 0.18 ? 0xFFFFFF : 0x000000;
    for (int i = 0; i <= 20; ++i) {
        const std::uint32_t m = mix(c, target, i / 20.0);
        if (contrast(m, bg) >= minRatio) {
            return m;
        }
    }
    return target;
}

std::uint32_t bestTextOn(std::uint32_t bg, std::uint32_t a, std::uint32_t b)
{
    const std::uint32_t best = contrast(a, bg) >= contrast(b, bg) ? a : b;
    return contrast(best, bg) >= 4.5 ? best : (contrast(0xFFFFFF, bg) >= contrast(0x000000, bg) ? 0xFFFFFF : 0x000000);
}

bool fail(QString *error, const QString &why)
{
    if (error) {
        *error = why;
    }
    return false;
}

bool readColour(const QJsonValue &v, const QString &what, std::uint32_t *out, QString *error)
{
    const std::optional<std::uint32_t> c = v.isString() ? parseHex(v.toString()) : std::nullopt;
    if (!c) {
        return fail(error, QStringLiteral("%1: not a colour (want \"#RRGGBB\")").arg(what));
    }
    *out = *c;
    return true;
}

bool readSize(const QJsonObject &o, const char *key, int *out, QString *error)
{
    if (!o.contains(QLatin1String(key))) {
        return true;
    }
    const QJsonValue v = o.value(QLatin1String(key));
    const double d = v.toDouble(-1);
    if (!v.isDouble() || d != std::floor(d) || d < kMinFontSize || d > kMaxFontSize) {
        return fail(error, QStringLiteral("fonts.%1: want a whole number %2..%3")
                               .arg(QLatin1String(key)).arg(kMinFontSize).arg(kMaxFontSize));
    }
    *out = int(d);
    return true;
}

} // namespace

const char *roleKey(int role)
{
    return role >= 0 && role < RoleCount ? kRoleKeys[role] : "";
}

double contrast(std::uint32_t a, std::uint32_t b)
{
    double la = luminance(a), lb = luminance(b);
    if (la < lb) {
        std::swap(la, lb);
    }
    return (la + 0.05) / (lb + 0.05);
}

QString hex(std::uint32_t rgb)
{
    return QStringLiteral("#%1").arg(rgb & 0xFFFFFF, 6, 16, QLatin1Char('0')).toUpper();
}

std::optional<std::uint32_t> parseHex(const QString &s)
{
    const QString t = s.trimmed();
    if (!t.startsWith(QLatin1Char('#')) || (t.size() != 7 && t.size() != 4)) {
        return std::nullopt;
    }
    bool ok = false;
    std::uint32_t v = t.mid(1).toUInt(&ok, 16);
    if (!ok) {
        return std::nullopt;
    }
    if (t.size() == 4) { // #RGB -> #RRGGBB
        v = ((v & 0xF00) * 0x1100) | ((v & 0x0F0) * 0x110) | ((v & 0x00F) * 0x11);
    }
    return v;
}

Roles deriveRoles(const Roles &in, const std::array<bool, RoleCount> &have)
{
    Roles r = in;
    const std::uint32_t bg = r[Background], fg = r[Foreground];
    auto set = [&](int role, std::uint32_t v) {
        if (!have[size_t(role)]) {
            r[size_t(role)] = v;
        }
    };
    set(Surface, mix(bg, fg, 0.06));
    set(Muted, readable(mix(fg, bg, 0.35), bg, 4.5));
    set(Accent, have[Link] ? r[Link] : have[Selection] ? r[Selection] : fg);
    set(Link, readable(r[Accent], bg, 4.5));
    set(Selection, have[Accent] ? r[Accent] : mix(bg, fg, 0.3));
    set(SelectionText, bestTextOn(r[Selection], bg, fg));
    set(Chrome, r[Surface]);
    set(ChromeText, bestTextOn(r[Chrome], fg, bg));
    set(Header, r[Surface]);
    set(HeaderText, bestTextOn(r[Header], fg, bg));
    return r;
}

Terminal deriveTerminal(const Roles &r)
{
    // VS Code's default ANSI hues, made readable on the background.
    static constexpr std::uint32_t base[16] = {0x000000, 0xCD3131, 0x0DBC79, 0xE5E510, 0x2472C8, 0xBC3FBC,
                                               0x11A8CD, 0xE5E5E5, 0x666666, 0xF14C4C, 0x23D18B, 0xF5F543,
                                               0x3B8EEA, 0xD670D6, 0x29B8DB, 0xFFFFFF};
    const std::uint32_t bg = r[Background], fg = r[Foreground];
    const bool dark = luminance(bg) < 0.18;
    Terminal t;
    t.ansi[0] = mix(bg, fg, 0.12);
    for (int i = 1; i < 16; ++i) {
        std::uint32_t c = base[i];
        if (i == 7) {
            c = mix(fg, bg, 0.2);
        } else if (i == 8) {
            c = r[Muted];
        } else if (i == 15) {
            c = fg;
        } else if (i > 8 && dark) {
            c = mix(c, 0xFFFFFF, 0.2);
        }
        t.ansi[size_t(i)] = readable(c, bg, 4.5);
    }
    // Keep the 16 distinct (e.g. muted == the derived white on some palettes).
    for (int i = 1; i < 16; ++i) {
        for (int j = 0; j < i; ++j) {
            if (t.ansi[size_t(i)] == t.ansi[size_t(j)]) {
                t.ansi[size_t(i)] ^= 0x000001;
            }
        }
    }
    t.cursor = r[Accent];
    t.selection = r[Selection];
    t.selectionText = r[SelectionText];
    return t;
}

Terminal terminalOf(const Theme &t)
{
    return t.terminal ? *t.terminal : deriveTerminal(t.roles);
}

Theme fromBrand(const sbj::brand::Theme &b)
{
    Theme t;
    t.name = QString::fromLatin1(b.name.data(), qsizetype(b.name.size()));
    t.basedOn = QString::fromLatin1(b.id.data(), qsizetype(b.id.size()));
    t.roles = {b.background, b.surface, b.foreground, b.muted, b.accent, b.link,
               b.selection, b.selectionText, b.chrome, b.chromeText, b.header, b.headerText};
    Terminal term;
    term.ansi = b.ansi;
    term.cursor = b.accent;
    term.selection = b.selection;
    term.selectionText = b.selectionText;
    t.terminal = term;
    return t;
}

std::optional<Theme> parse(const QByteArray &json, QString *error)
{
    if (json.size() > kMaxFileBytes) {
        fail(error, QStringLiteral("file too large (over %1 KB)").arg(kMaxFileBytes / 1024));
        return std::nullopt;
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        fail(error, pe.error != QJsonParseError::NoError ? QStringLiteral("not valid JSON: %1").arg(pe.errorString())
                                                         : QStringLiteral("not a JSON object"));
        return std::nullopt;
    }
    const QJsonObject o = doc.object();
    if (o.value(QLatin1String("format")).toString() != QLatin1String(kFormat)) {
        fail(error, QStringLiteral("not a theme file (\"format\" must be \"%1\")").arg(QLatin1String(kFormat)));
        return std::nullopt;
    }
    const QJsonValue ver = o.value(QLatin1String("version"));
    if (!ver.isDouble() || ver.toDouble() != std::floor(ver.toDouble()) || ver.toInt() < 1) {
        fail(error, QStringLiteral("\"version\" must be a whole number >= 1"));
        return std::nullopt;
    }
    if (ver.toInt() > kVersion) {
        fail(error, QStringLiteral("theme format version %1 is newer than this app reads (%2)").arg(ver.toInt()).arg(kVersion));
        return std::nullopt;
    }
    Theme t;
    t.name = o.value(QLatin1String("name")).toString().trimmed();
    if (t.name.isEmpty() || t.name.size() > kMaxNameLength) {
        fail(error, QStringLiteral("\"name\" must be 1..%1 characters").arg(kMaxNameLength));
        return std::nullopt;
    }
    t.basedOn = o.value(QLatin1String("basedOn")).toString().left(kMaxNameLength);

    const QJsonValue pal = o.value(QLatin1String("palette"));
    if (!pal.isObject()) {
        fail(error, QStringLiteral("\"palette\" is missing"));
        return std::nullopt;
    }
    const QJsonObject p = pal.toObject();
    std::array<bool, RoleCount> have{};
    for (int i = 0; i < RoleCount; ++i) {
        const QLatin1String key(kRoleKeys[i]);
        if (p.contains(key)) {
            if (!readColour(p.value(key), QStringLiteral("palette.") + key, &t.roles[size_t(i)], error)) {
                return std::nullopt;
            }
            have[size_t(i)] = true;
        }
    }
    if (!have[Background] || !have[Foreground]) {
        fail(error, QStringLiteral("palette.background and palette.foreground are required"));
        return std::nullopt;
    }
    t.roles = deriveRoles(t.roles, have);

    if (o.contains(QLatin1String("terminal"))) {
        const QJsonValue tv = o.value(QLatin1String("terminal"));
        if (!tv.isObject()) {
            fail(error, QStringLiteral("\"terminal\" must be an object"));
            return std::nullopt;
        }
        const QJsonObject to = tv.toObject();
        Terminal term = deriveTerminal(t.roles);
        if (to.contains(QLatin1String("ansi"))) {
            const QJsonArray a = to.value(QLatin1String("ansi")).toArray();
            if (!to.value(QLatin1String("ansi")).isArray() || a.size() != 16) {
                fail(error, QStringLiteral("terminal.ansi must be a list of 16 colours"));
                return std::nullopt;
            }
            for (int i = 0; i < 16; ++i) {
                if (!readColour(a.at(i), QStringLiteral("terminal.ansi[%1]").arg(i), &term.ansi[size_t(i)], error)) {
                    return std::nullopt;
                }
            }
        }
        for (auto [key, out] : {std::pair{"cursor", &term.cursor}, std::pair{"selection", &term.selection},
                                std::pair{"selectionText", &term.selectionText}}) {
            if (to.contains(QLatin1String(key)) &&
                !readColour(to.value(QLatin1String(key)), QStringLiteral("terminal.") + QLatin1String(key), out, error)) {
                return std::nullopt;
            }
        }
        if (to.contains(QLatin1String("cursorShape"))) {
            term.cursorShape = to.value(QLatin1String("cursorShape")).toString();
            if (term.cursorShape != QLatin1String("block") && term.cursorShape != QLatin1String("underline") &&
                term.cursorShape != QLatin1String("bar")) {
                fail(error, QStringLiteral("terminal.cursorShape must be \"block\", \"underline\" or \"bar\""));
                return std::nullopt;
            }
        }
        if (to.contains(QLatin1String("cursorBlink"))) {
            if (!to.value(QLatin1String("cursorBlink")).isBool()) {
                fail(error, QStringLiteral("terminal.cursorBlink must be true or false"));
                return std::nullopt;
            }
            term.cursorBlink = to.value(QLatin1String("cursorBlink")).toBool();
        }
        t.terminal = term;
    }

    if (o.contains(QLatin1String("fonts"))) {
        const QJsonObject f = o.value(QLatin1String("fonts")).toObject();
        for (const char *key : {"ui", "terminal"}) {
            const QJsonValue v = f.value(QLatin1String(key));
            if (!v.isUndefined() && !v.isString()) {
                fail(error, QStringLiteral("fonts.%1: want a font family name").arg(QLatin1String(key)));
                return std::nullopt;
            }
        }
        t.fonts.ui = f.value(QLatin1String("ui")).toString().left(128);
        t.fonts.terminal = f.value(QLatin1String("terminal")).toString().left(128);
        if (!readSize(f, "uiSize", &t.fonts.uiSize, error) || !readSize(f, "terminalSize", &t.fonts.terminalSize, error)) {
            return std::nullopt;
        }
    }
    if (o.contains(QLatin1String("ui"))) {
        const QJsonObject u = o.value(QLatin1String("ui")).toObject();
        if (u.contains(QLatin1String("rowStripes"))) {
            const QJsonValue v = u.value(QLatin1String("rowStripes"));
            const double d = v.toDouble(-1);
            if (!v.isDouble() || d != std::floor(d) || d < 0 || d > 100) {
                fail(error, QStringLiteral("ui.rowStripes: want a whole number 0..100"));
                return std::nullopt;
            }
            t.ui.rowStripes = int(d);
        }
    }
    return t;
}

QByteArray serialize(const Theme &t)
{
    QJsonObject o;
    o.insert(QLatin1String("format"), QLatin1String(kFormat));
    o.insert(QLatin1String("version"), kVersion);
    o.insert(QLatin1String("name"), t.name);
    if (!t.basedOn.isEmpty()) {
        o.insert(QLatin1String("basedOn"), t.basedOn);
    }
    QJsonObject p;
    for (int i = 0; i < RoleCount; ++i) {
        p.insert(QLatin1String(kRoleKeys[i]), hex(t.roles[size_t(i)]));
    }
    o.insert(QLatin1String("palette"), p);
    if (t.terminal) {
        QJsonObject to;
        QJsonArray a;
        for (std::uint32_t c : t.terminal->ansi) {
            a.append(hex(c));
        }
        to.insert(QLatin1String("ansi"), a);
        to.insert(QLatin1String("cursor"), hex(t.terminal->cursor));
        to.insert(QLatin1String("selection"), hex(t.terminal->selection));
        to.insert(QLatin1String("selectionText"), hex(t.terminal->selectionText));
        if (!t.terminal->cursorShape.isEmpty()) {
            to.insert(QLatin1String("cursorShape"), t.terminal->cursorShape);
        }
        if (t.terminal->cursorBlink) {
            to.insert(QLatin1String("cursorBlink"), *t.terminal->cursorBlink);
        }
        o.insert(QLatin1String("terminal"), to);
    }
    QJsonObject f;
    if (!t.fonts.ui.isEmpty()) f.insert(QLatin1String("ui"), t.fonts.ui);
    if (t.fonts.uiSize > 0) f.insert(QLatin1String("uiSize"), t.fonts.uiSize);
    if (!t.fonts.terminal.isEmpty()) f.insert(QLatin1String("terminal"), t.fonts.terminal);
    if (t.fonts.terminalSize > 0) f.insert(QLatin1String("terminalSize"), t.fonts.terminalSize);
    if (!f.isEmpty()) {
        o.insert(QLatin1String("fonts"), f);
    }
    if (t.ui.rowStripes) {
        o.insert(QLatin1String("ui"), QJsonObject{{QLatin1String("rowStripes"), *t.ui.rowStripes}});
    }
    return QJsonDocument(o).toJson(QJsonDocument::Indented);
}

std::optional<Theme> load(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        fail(error, QStringLiteral("can't read %1: %2").arg(QFileInfo(path).fileName(), f.errorString()));
        return std::nullopt;
    }
    return parse(f.read(kMaxFileBytes + 1), error);
}

bool save(const Theme &t, const QString &path, QString *error)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(serialize(t)) < 0 || !f.commit()) {
        return fail(error, QStringLiteral("can't write %1: %2").arg(QFileInfo(path).fileName(), f.errorString()));
    }
    return true;
}

QString fileStem(const QString &name)
{
    QString s;
    for (const QChar c : name.toLower()) {
        if ((c >= QLatin1Char('a') && c <= QLatin1Char('z')) || (c >= QLatin1Char('0') && c <= QLatin1Char('9'))) {
            s += c;
        } else if (!s.isEmpty() && !s.endsWith(QLatin1Char('-'))) {
            s += QLatin1Char('-');
        }
    }
    while (s.endsWith(QLatin1Char('-'))) {
        s.chop(1);
    }
    return s.isEmpty() ? QStringLiteral("theme") : s.left(48);
}

QString themesDir(const QString &configHome, const QString &app)
{
    return configHome + QLatin1Char('/') + app + QStringLiteral("/themes");
}

QList<Entry> scan(const QString &dir)
{
    QList<Entry> out;
    const QString suffix = QLatin1String(kSuffix);
    for (const QFileInfo &fi : QDir(dir).entryInfoList({QLatin1Char('*') + suffix}, QDir::Files, QDir::Name)) {
        if (std::optional<Theme> t = load(fi.absoluteFilePath())) {
            out.append({fi.absoluteFilePath(), fi.fileName().chopped(suffix.size()), *t});
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Entry &a, const Entry &b) {
        return QString::localeAwareCompare(a.theme.name, b.theme.name) < 0;
    });
    return out;
}

QString uniquePath(const QString &dir, const QString &name)
{
    const QString stem = fileStem(name);
    QString path = dir + QLatin1Char('/') + stem + QLatin1String(kSuffix);
    for (int n = 2; QFileInfo::exists(path); ++n) {
        path = dir + QLatin1Char('/') + stem + QLatin1Char('-') + QString::number(n) + QLatin1String(kSuffix);
    }
    return path;
}

} // namespace sbj::theme
