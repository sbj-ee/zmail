// CalendarInvite on hostile iCalendar text.
#include "FuzzCommon.h"
#include "core/CalendarInvite.h"

extern "C" int LLVMFuzzerInitialize(int *, char ***)
{
    zmail::fuzz::initQt();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const QString ics = QString::fromUtf8(reinterpret_cast<const char *>(data), static_cast<qsizetype>(size));
    const zmail::CalendarEvent ev = zmail::CalendarInvite::parse(ics);
    (void)ev.whenText();
    (void)ev.organizerText();
    (void)ev.attendeesText();
    return 0;
}
