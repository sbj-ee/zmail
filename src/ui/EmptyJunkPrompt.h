#pragma once

#include "core/SyncEngine.h"

#include <QCoreApplication>
#include <QLocale>
#include <QString>

namespace zmail::ui {

// The wording of Right-click Junk / Suspicious > Empty Junk… (the confirmation
// and the result), in one place so the window and the tests agree.
// Every zmail account is Gmail (API or IMAP), where zmail's Delete is "move to
// Trash"; Empty Junk does the same, so nothing is erased for good by it.
struct EmptyJunkPrompt
{
    Q_DECLARE_TR_FUNCTIONS(EmptyJunkPrompt)

public:
    // count: Gmail's figure when `exact`; otherwise zmail's last known one and
    // `why` says why Gmail could not be asked.
    static QString question(int count, bool exact, const QString &why)
    {
        const QString n = QLocale().toString(count);
        if (exact) {
            return count == 1 ? tr("Move the 1 message in Junk / Suspicious to Trash?")
                              : tr("Move all %1 messages in Junk / Suspicious to Trash?").arg(n);
        }
        return tr("Move every message in Junk / Suspicious to Trash?\n\nzmail last counted about %1, but couldn't ask "
                  "Gmail for the number just now (%2), so that may be out of date.")
            .arg(n, why.isEmpty() ? tr("no answer") : why);
    }
    static QString detail(bool exact)
    {
        return tr("%1They go to Trash, as with Delete anywhere else in zmail: nothing is erased permanently by this. "
                  "Gmail empties Trash by itself after 30 days, and you can empty it sooner from Trash.\n\n"
                  "A big folder is moved in batches of %2; you can stop at any time with Stop in the status bar "
                  "(what has moved stays in Trash).")
            .arg(exact ? tr("Gmail's count, asked just now.\n\n") : QString(), QString::number(zmail::SyncEngine::kEmptyJunkBatch));
    }
    static QString result(int deleted, int total, bool cancelled, const QString &error)
    {
        const int left = total - deleted;
        if (!error.isEmpty()) {
            return tr("Junk was only partly emptied: %1 of %2 moved to Trash, %3 still in Junk.\n\nWhat went wrong: %4")
                .arg(deleted).arg(total).arg(left).arg(error);
        }
        if (cancelled) {
            return tr("Stopped: %1 of %2 messages moved to Trash; %3 still in Junk.").arg(deleted).arg(total).arg(left);
        }
        if (total == 0) {
            return tr("Junk is already empty.");
        }
        return deleted == 1 ? tr("Junk emptied: 1 message moved to Trash.")
                            : tr("Junk emptied: %1 messages moved to Trash.").arg(deleted);
    }
};

} // namespace zmail::ui
