#pragma once

#include <QColor>
#include <QCoreApplication>
#include <QString>

#include <array>

namespace zmail::ui {

// Message flags. Gmail has one star; zmail has a set of coloured flags. The
// colour is zmail's own (kept in the local cache), and any flag also stars
// the message in Gmail, so "flagged" follows you to other devices while the
// colour stays here. A message starred somewhere else shows the Gmail
// colour, yellow.
struct FlagColor
{
    const char *id;   // stored
    const char *name; // shown
    QRgb rgb;
};

inline constexpr std::array<FlagColor, 7> kFlagColors = {{
    {"red", QT_TRANSLATE_NOOP("zmail::ui::Flags", "Red"), 0xffd93025},
    {"orange", QT_TRANSLATE_NOOP("zmail::ui::Flags", "Orange"), 0xffe8710a},
    {"yellow", QT_TRANSLATE_NOOP("zmail::ui::Flags", "Yellow"), 0xffe2a400},
    {"green", QT_TRANSLATE_NOOP("zmail::ui::Flags", "Green"), 0xff188038},
    {"blue", QT_TRANSLATE_NOOP("zmail::ui::Flags", "Blue"), 0xff1a73e8},
    {"purple", QT_TRANSLATE_NOOP("zmail::ui::Flags", "Purple"), 0xff9334e6},
    {"gray", QT_TRANSLATE_NOOP("zmail::ui::Flags", "Gray"), 0xff80868b},
}};
inline constexpr const char *kDefaultFlag = "red";   // the first click on the flag column
inline constexpr const char *kStarredFlag = "yellow"; // starred in Gmail, no colour chosen here

// 1-based position in kFlagColors; 0: not a flag.
inline int flagOrder(const QString &id)
{
    for (std::size_t i = 0; i < kFlagColors.size(); ++i) {
        if (id == QLatin1String(kFlagColors[i].id)) {
            return int(i) + 1;
        }
    }
    return 0;
}

inline QColor flagColor(const QString &id)
{
    const int n = flagOrder(id);
    return n ? QColor::fromRgba(kFlagColors[std::size_t(n - 1)].rgb) : QColor();
}

inline QString flagName(const QString &id)
{
    const int n = flagOrder(id);
    return n ? QCoreApplication::translate("zmail::ui::Flags", kFlagColors[std::size_t(n - 1)].name) : QString();
}

} // namespace zmail::ui
