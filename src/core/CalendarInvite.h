#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

namespace zmail {

struct CalendarAttendee
{
    QString name;    // CN, or the address when there is none
    QString address;
    QString status;  // PARTSTAT: ACCEPTED, DECLINED, TENTATIVE, NEEDS-ACTION, "" = not said
};

// The first event of an iCalendar object (RFC 5545), as much of it as the
// preview shows. Read only: zmail displays invitations, it does not answer
// them or keep a calendar.
struct CalendarEvent
{
    enum class Kind { Event, Invitation, Cancelled, Reply };

    bool valid = false;  // a VEVENT with a start was found
    QString method;      // REQUEST, CANCEL, REPLY, PUBLISH, ... (upper case)
    QString summary;
    QString location;
    QString description;
    QString status;      // CONFIRMED, TENTATIVE, CANCELLED
    QDateTime start;
    QDateTime end;       // invalid = no end given
    bool allDay = false; // start/end are dates; end is the last day (inclusive)
    QString organizerName;
    QString organizerAddress;
    QList<CalendarAttendee> attendees;
    QString recurrence;  // "Repeats weekly", "" = once

    Kind kind() const;
    QString kindText() const;       // "Invitation", "Cancelled event", ...
    // "Tuesday, October 13, 2026 · 2:00 PM – 3:00 PM CDT", in local time.
    QString whenText() const;
    QString organizerText() const;  // "Ada Lovelace <ada@example.org>"
    // "Ada (accepted), Grace, and 3 more": at most `max` named.
    QString attendeesText(int max = 6) const;
};

namespace CalendarInvite {

// "" or junk gives an event with valid == false. Input beyond kMaxBytes is
// not read.
CalendarEvent parse(const QString &ics);
inline constexpr int kMaxBytes = 512 * 1024;

// A MIME part that carries one: text/calendar, application/ics, or a file
// named *.ics.
bool isCalendarPart(const QString &mimeType, const QString &fileName);

} // namespace CalendarInvite
} // namespace zmail
