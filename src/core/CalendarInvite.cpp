#include "CalendarInvite.h"

#include <QCoreApplication>
#include <QHash>
#include <QLocale>
#include <QRegularExpression>
#include <QStringList>
#include <QTimeZone>

namespace zmail {

namespace {
constexpr int kMaxField = 4000;
constexpr int kMaxAttendees = 500;

QString tr(const char *text)
{
    return QCoreApplication::translate("CalendarInvite", text);
}

struct Line
{
    QString name;                   // upper case
    QHash<QString, QString> params; // upper-case keys
    QString value;
};

// "NAME;PARAM=x;OTHER=\"a:b\":value" -> its parts. A ':' or ';' inside a
// quoted parameter value doesn't split.
Line splitLine(const QString &raw)
{
    Line out;
    bool quoted = false;
    qsizetype colon = -1;
    QList<qsizetype> semis;
    for (qsizetype i = 0; i < raw.size(); ++i) {
        const QChar c = raw.at(i);
        if (c == QLatin1Char('"')) {
            quoted = !quoted;
        } else if (!quoted && c == QLatin1Char(';')) {
            semis.append(i);
        } else if (!quoted && c == QLatin1Char(':')) {
            colon = i;
            break;
        }
    }
    if (colon < 0) {
        return out;
    }
    out.value = raw.mid(colon + 1);
    semis.append(colon);
    out.name = raw.left(semis.first()).trimmed().toUpper();
    for (qsizetype i = 0; i + 1 < semis.size(); ++i) {
        const QString param = raw.mid(semis[i] + 1, semis[i + 1] - semis[i] - 1);
        const qsizetype eq = param.indexOf(QLatin1Char('='));
        if (eq <= 0) {
            continue;
        }
        QString v = param.mid(eq + 1).trimmed();
        if (v.size() >= 2 && v.startsWith(QLatin1Char('"')) && v.endsWith(QLatin1Char('"'))) {
            v = v.mid(1, v.size() - 2);
        }
        out.params.insert(param.left(eq).trimmed().toUpper(), v);
    }
    return out;
}

// TEXT values: \n, \N, \\, \; and \, (RFC 5545 §3.3.11).
QString unescapeText(const QString &v)
{
    QString out;
    out.reserve(v.size());
    for (qsizetype i = 0; i < v.size(); ++i) {
        const QChar c = v.at(i);
        if (c == QLatin1Char('\\') && i + 1 < v.size()) {
            const QChar n = v.at(++i);
            out += (n == QLatin1Char('n') || n == QLatin1Char('N')) ? QLatin1Char('\n') : n;
        } else {
            out += c;
        }
    }
    return out.trimmed().left(kMaxField);
}

QString oneLine(const QString &v)
{
    return unescapeText(v).simplified();
}

QString addressOf(const QString &value)
{
    QString v = value.trimmed();
    if (v.startsWith(QLatin1String("mailto:"), Qt::CaseInsensitive)) {
        v = v.mid(7);
    }
    return v.simplified().left(320);
}

QTimeZone zoneFor(const QString &tzid)
{
    if (tzid.isEmpty()) {
        return {};
    }
    QTimeZone tz(tzid.toUtf8());
    if (!tz.isValid()) {
        // Outlook writes Windows names ("Central Standard Time").
        const QByteArray iana = QTimeZone::windowsIdToDefaultIanaId(tzid.toUtf8());
        if (!iana.isEmpty()) {
            tz = QTimeZone(iana);
        }
    }
    return tz;
}

// 20261013, 20261013T140000 (floating, or in TZID) and 20261013T140000Z.
QDateTime parseTime(const Line &l, bool *dateOnly)
{
    const QString v = l.value.trimmed();
    *dateOnly = false;
    if (v.size() == 8 || l.params.value(QStringLiteral("VALUE")).compare(QLatin1String("DATE"), Qt::CaseInsensitive) == 0) {
        const QDate d = QDate::fromString(v.left(8), QStringLiteral("yyyyMMdd"));
        *dateOnly = d.isValid();
        return d.isValid() ? QDateTime(d, QTime(0, 0)) : QDateTime();
    }
    const bool utc = v.endsWith(QLatin1Char('Z'), Qt::CaseInsensitive);
    const QDate d = QDate::fromString(v.left(8), QStringLiteral("yyyyMMdd"));
    const QTime t = QTime::fromString(v.mid(9, 6), QStringLiteral("HHmmss"));
    if (!d.isValid() || !t.isValid() || v.size() < 15 || v.at(8).toUpper() != QLatin1Char('T')) {
        return {};
    }
    if (utc) {
        return QDateTime(d, t, Qt::UTC);
    }
    const QTimeZone tz = zoneFor(l.params.value(QStringLiteral("TZID")));
    return tz.isValid() ? QDateTime(d, t, tz) : QDateTime(d, t); // unknown zone: as written, local
}

// P1D, PT1H30M, P1W, -PT15M: seconds (0 if it doesn't parse).
qint64 parseDuration(const QString &value)
{
    static const QRegularExpression re(
        QStringLiteral("^([+-])?P(?:(\\d+)W)?(?:(\\d+)D)?(?:T(?:(\\d+)H)?(?:(\\d+)M)?(?:(\\d+)S)?)?$"));
    const auto m = re.match(value.trimmed().toUpper());
    if (!m.hasMatch()) {
        return 0;
    }
    const qint64 secs = m.captured(2).toLongLong() * 7 * 86400 + m.captured(3).toLongLong() * 86400 +
                        m.captured(4).toLongLong() * 3600 + m.captured(5).toLongLong() * 60 + m.captured(6).toLongLong();
    return m.captured(1) == QLatin1String("-") ? -secs : secs;
}

QString recurrenceText(const QString &rrule)
{
    QHash<QString, QString> parts;
    for (const QString &p : rrule.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const qsizetype eq = p.indexOf(QLatin1Char('='));
        if (eq > 0) {
            parts.insert(p.left(eq).trimmed().toUpper(), p.mid(eq + 1).trimmed().toUpper());
        }
    }
    const QString freq = parts.value(QStringLiteral("FREQ"));
    const int every = std::max(1, parts.value(QStringLiteral("INTERVAL"), QStringLiteral("1")).toInt());
    struct Unit { const char *freq, *one, *many; };
    static const Unit units[] = {{"DAILY", "Repeats daily", "Repeats every %1 days"},
                                 {"WEEKLY", "Repeats weekly", "Repeats every %1 weeks"},
                                 {"MONTHLY", "Repeats monthly", "Repeats every %1 months"},
                                 {"YEARLY", "Repeats yearly", "Repeats every %1 years"}};
    for (const Unit &u : units) {
        if (freq == QLatin1String(u.freq)) {
            return every == 1 ? tr(u.one) : tr(u.many).arg(every);
        }
    }
    return freq.isEmpty() ? QString() : tr("Repeats");
}

QLocale english()
{
    return QLocale(QLocale::English, QLocale::UnitedStates);
}

QString statusWord(const QString &partstat)
{
    if (partstat == QLatin1String("ACCEPTED")) {
        return tr("accepted");
    }
    if (partstat == QLatin1String("DECLINED")) {
        return tr("declined");
    }
    if (partstat == QLatin1String("TENTATIVE")) {
        return tr("maybe");
    }
    return {};
}
} // namespace

CalendarEvent::Kind CalendarEvent::kind() const
{
    if (method == QLatin1String("CANCEL") || status == QLatin1String("CANCELLED")) {
        return Kind::Cancelled;
    }
    if (method == QLatin1String("REPLY")) {
        return Kind::Reply;
    }
    return method == QLatin1String("REQUEST") ? Kind::Invitation : Kind::Event;
}

QString CalendarEvent::kindText() const
{
    switch (kind()) {
    case Kind::Invitation:
        return tr("Invitation");
    case Kind::Cancelled:
        return tr("Cancelled event");
    case Kind::Reply:
        return tr("Reply to an invitation");
    case Kind::Event:
        break;
    }
    return tr("Event");
}

QString CalendarEvent::whenText() const
{
    if (!start.isValid()) {
        return {};
    }
    const QLocale loc = english();
    const QString dayFormat = QStringLiteral("dddd, MMMM d, yyyy");
    const QString dash = QStringLiteral(" – ");
    if (allDay) {
        const QString first = loc.toString(start.date(), dayFormat);
        if (end.isValid() && end.date() > start.date()) {
            return tr("%1 (all day)").arg(first + dash + loc.toString(end.date(), dayFormat));
        }
        return tr("%1 (all day)").arg(first);
    }
    const QDateTime s = start.toLocalTime();
    const QString timeFormat = QStringLiteral("h:mm AP");
    QString out = loc.toString(s.date(), dayFormat) + QStringLiteral(" · ") + loc.toString(s.time(), timeFormat);
    if (end.isValid() && end > start) {
        const QDateTime e = end.toLocalTime();
        out += dash;
        if (e.date() != s.date()) {
            out += loc.toString(e.date(), dayFormat) + QStringLiteral(" · ");
        }
        out += loc.toString(e.time(), timeFormat);
    }
    return out + QLatin1Char(' ') + s.timeZoneAbbreviation();
}

QString CalendarEvent::organizerText() const
{
    if (organizerName.isEmpty() || organizerName == organizerAddress) {
        return organizerAddress;
    }
    return organizerAddress.isEmpty() ? organizerName : QStringLiteral("%1 <%2>").arg(organizerName, organizerAddress);
}

QString CalendarEvent::attendeesText(int max) const
{
    QStringList names;
    for (const CalendarAttendee &a : attendees) {
        if (names.size() == max) {
            break;
        }
        const QString word = statusWord(a.status);
        names << (word.isEmpty() ? a.name : QStringLiteral("%1 (%2)").arg(a.name, word));
    }
    QString out = names.join(QStringLiteral(", "));
    if (const qsizetype more = attendees.size() - names.size(); more > 0) {
        out += tr(", and %1 more").arg(more);
    }
    return out;
}

namespace CalendarInvite {

bool isCalendarPart(const QString &mimeType, const QString &fileName)
{
    const QString mime = mimeType.toLower();
    return mime == QLatin1String("text/calendar") || mime == QLatin1String("application/ics") ||
           fileName.endsWith(QLatin1String(".ics"), Qt::CaseInsensitive);
}

CalendarEvent parse(const QString &icsIn)
{
    CalendarEvent ev;
    QString ics = icsIn.left(kMaxBytes);
    ics.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    ics.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    // Unfold: a line starting with a space or tab continues the one before.
    ics.replace(QRegularExpression(QStringLiteral("\n[ \t]")), QString());

    QStringList open; // BEGIN stack
    bool seenEvent = false;
    bool endIsDate = false;
    qint64 duration = 0;
    for (const QString &raw : ics.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const Line l = splitLine(raw);
        if (l.name.isEmpty()) {
            continue;
        }
        if (l.name == QLatin1String("BEGIN")) {
            const QString what = l.value.trimmed().toUpper();
            if (what == QLatin1String("VEVENT") && seenEvent) {
                break; // later VEVENTs are exceptions to the first
            }
            open.append(what);
            continue;
        }
        if (l.name == QLatin1String("END")) {
            if (!open.isEmpty() && open.last() == QLatin1String("VEVENT")) {
                seenEvent = true;
            }
            if (!open.isEmpty()) {
                open.removeLast();
            }
            continue;
        }
        const QString where = open.isEmpty() ? QString() : open.last();
        if (where == QLatin1String("VCALENDAR")) {
            if (l.name == QLatin1String("METHOD")) {
                ev.method = l.value.trimmed().toUpper().left(32);
            }
            continue;
        }
        if (where != QLatin1String("VEVENT")) {
            continue; // VTIMEZONE, VALARM, ...
        }
        if (l.name == QLatin1String("SUMMARY")) {
            ev.summary = oneLine(l.value);
        } else if (l.name == QLatin1String("LOCATION")) {
            ev.location = oneLine(l.value);
        } else if (l.name == QLatin1String("DESCRIPTION")) {
            ev.description = unescapeText(l.value);
        } else if (l.name == QLatin1String("STATUS")) {
            ev.status = l.value.trimmed().toUpper().left(32);
        } else if (l.name == QLatin1String("DTSTART")) {
            ev.start = parseTime(l, &ev.allDay);
        } else if (l.name == QLatin1String("DTEND")) {
            ev.end = parseTime(l, &endIsDate);
        } else if (l.name == QLatin1String("DURATION")) {
            duration = parseDuration(l.value);
        } else if (l.name == QLatin1String("RRULE")) {
            ev.recurrence = recurrenceText(l.value);
        } else if (l.name == QLatin1String("ORGANIZER")) {
            ev.organizerAddress = addressOf(l.value);
            ev.organizerName = l.params.value(QStringLiteral("CN")).simplified().left(200);
        } else if (l.name == QLatin1String("ATTENDEE") && ev.attendees.size() < kMaxAttendees) {
            CalendarAttendee a;
            a.address = addressOf(l.value);
            a.name = l.params.value(QStringLiteral("CN")).simplified().left(200);
            if (a.name.isEmpty()) {
                a.name = a.address;
            }
            a.status = l.params.value(QStringLiteral("PARTSTAT")).toUpper();
            if (!a.name.isEmpty()) {
                ev.attendees.append(a);
            }
        }
    }
    if (!ev.start.isValid()) {
        return ev;
    }
    if (!ev.end.isValid() && duration > 0) {
        ev.end = ev.start.addSecs(duration);
        endIsDate = ev.allDay;
    }
    if (ev.allDay && ev.end.isValid()) {
        // A date DTEND is the day after the last one.
        ev.end = endIsDate ? ev.end.addDays(-1) : ev.end;
        if (ev.end < ev.start) {
            ev.end = ev.start;
        }
    }
    ev.valid = true;
    return ev;
}

} // namespace CalendarInvite
} // namespace zmail
