#include "SnoozeTimes.h"

namespace zmail {

namespace {
QDateTime atHour(QDate date, int hour)
{
    return QDateTime(date, QTime(hour, 0), Qt::LocalTime);
}
} // namespace

QDateTime SnoozeTimes::laterToday(const QDateTime &now)
{
    return now.addSecs(3 * 3600);
}

QDateTime SnoozeTimes::tomorrowMorning(const QDateTime &now)
{
    return atHour(now.date().addDays(1), 8);
}

QDateTime SnoozeTimes::thisWeekend(const QDateTime &now)
{
    // Next Saturday 08:00 local. If today is Saturday and before 08:00, use today.
    const int dow = now.date().dayOfWeek(); // Mon=1 .. Sun=7
    int days = (6 - dow + 7) % 7;           // days until Saturday
    if (days == 0 && now.time() >= QTime(8, 0)) {
        days = 7;
    }
    return atHour(now.date().addDays(days), 8);
}

QDateTime SnoozeTimes::nextWeek(const QDateTime &now)
{
    // Next Monday 08:00 (always at least tomorrow's week if today is Monday after 8,
    // or the upcoming Monday otherwise — never "today").
    const int dow = now.date().dayOfWeek();
    int days = (1 - dow + 7) % 7;
    if (days == 0) {
        days = 7; // today is Monday → next Monday
    }
    return atHour(now.date().addDays(days), 8);
}

QDateTime SnoozeTimes::wakeFor(const char *id, const QDateTime &now)
{
    const QByteArray k(id);
    if (k == "laterToday") return laterToday(now);
    if (k == "tomorrow") return tomorrowMorning(now);
    if (k == "weekend") return thisWeekend(now);
    if (k == "nextWeek") return nextWeek(now);
    return {};
}

bool SnoozeTimes::isDue(qint64 wakeMs, const QDateTime &now)
{
    return wakeMs > 0 && wakeMs <= now.toMSecsSinceEpoch();
}

} // namespace zmail
