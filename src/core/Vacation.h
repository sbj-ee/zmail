#pragma once

#include <QDate>
#include <QJsonObject>
#include <QString>

namespace zmail {

// Gmail's vacation responder (users.settings.vacation): the automatic reply
// Google sends while you are away. It lives in the account, not in zmail, so
// it keeps answering with zmail closed.
struct VacationSettings
{
    bool enabled = false;
    QString subject;
    QString text;              // the reply, as plain text
    bool contactsOnly = false; // only answer people in my Contacts
    bool domainOnly = false;   // (Workspace accounts) only answer my own organisation
    QDate firstDay;            // invalid = from now
    QDate lastDay;             // invalid = until it is turned off

    // Gmail's times are instants. zmail reads and writes them as UTC
    // midnights, the end being the midnight after the last day.
    static VacationSettings fromJson(const QJsonObject &o);
    QJsonObject toJson() const;

    // "On until October 20", "Off", ...: for the status bar and the dialog.
    QString summary() const;

    bool operator==(const VacationSettings &) const = default;
};

} // namespace zmail
