#pragma once

#include <QDateTime>
#include <QString>
#include <QList>

namespace zmail {

// Local-time snooze wake presets. Pure functions for unit tests.
struct SnoozePreset
{
    const char *id;   // "laterToday", "tomorrow", "weekend", "nextWeek", "custom"
    QString label;    // UI text (caller translates or passes already-translated)
};

class SnoozeTimes
{
public:
    // Wake times are in the given "now" zone (typically QDateTime::currentDateTime()).
    static QDateTime laterToday(const QDateTime &now);      // now + 3 hours
    static QDateTime tomorrowMorning(const QDateTime &now); // tomorrow 08:00 local
    static QDateTime thisWeekend(const QDateTime &now);     // next Saturday 08:00 (today if Sat before 8)
    static QDateTime nextWeek(const QDateTime &now);        // next Monday 08:00 (skip today if Mon)
    static QDateTime wakeFor(const char *id, const QDateTime &now);

    // Active snooze rows due at or before now (wake_ms <= now.toMSecsSinceEpoch()).
    static bool isDue(qint64 wakeMs, const QDateTime &now);
};

} // namespace zmail
